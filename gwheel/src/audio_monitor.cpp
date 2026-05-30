#include "audio_monitor.h"
#include "config.h"
#include "logging.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace gwheel::audio_monitor
{
    namespace
    {
        std::atomic<bool>   g_running{false};
        std::atomic<float>  g_level{0.f};
        std::thread         g_thread;

        // Per-hook record. The instanceId is the LPWSTR returned by
        // IAudioSessionControl2::GetSessionInstanceIdentifier — a stable
        // identifier for that logical session. We dedupe re-attach calls
        // against this string instead of the IAudioSessionControl* pointer
        // because the COM enumerator returns a NEW interface pointer per
        // GetSession() call for the same logical session, which made the
        // old pointer-based dedup never match — every 250 ms re-poll
        // re-registered the listener and re-pushed the session, growing
        // both the vector and the listener's per-session callback list
        // without bound. (Repro: city driving, 30+ minutes, hitches escalate
        // and the process eventually crashes.)
        struct HookedSession
        {
            IAudioSessionControl* ctrl = nullptr;
            std::wstring          instanceId;
        };

        // Live per-app session volume scalar [0..1]. Updated instantly
        // on Game Bar / Volume Mixer changes via IAudioSessionEvents,
        // not polled — so when the user drags the slider, compensation
        // tracks within a single capture tick (10 ms) instead of
        // waiting for a 500 ms poll round-trip.
        std::atomic<float> g_volumeScalar{1.f};

        // Rolling window for dynamic-range normalisation. At 10ms
        // chunks, 300 chunks = 3 seconds — long enough to stretch a
        // quiet intro to full scale, short enough to feel reactive.
        constexpr int   kWindowChunks  = 300;

        // Asymmetric envelope smoothing on the per-chunk RMS. Attack
        // responds fast to transients (bass hits punch), release decays
        // fast enough that quiet valleys between beats remain visible
        // instead of filling in.
        constexpr float kAttackAlpha   = 0.35f;
        constexpr float kReleaseAlpha  = 0.15f;

        template <typename T>
        void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

        // -----------------------------------------------------------------
        // Bass-band IIR bandpass filter.
        //
        // Two short-lived attempts to remove this filter (2026-04-24,
        // first to "fix" sparse-music dark stretches, then again with
        // full-band RMS) both made the visualizer track engine + dialogue
        // + SFX more than the music. Result was a bar that pulsed with
        // the world rather than the song — the user reported "completely
        // unrelated to what I'm hearing." Bass-band is a deliberate
        // engine/dialogue rejection: kick drums and bass lines have
        // sharp transients in 40-160 Hz where engine sits too, but
        // music's transients are *steeper* (single-cycle attacks) and
        // win the rolling-min/peak normalisation downstream. The trade-
        // off: bass-light tracks (sparse vocal, ambient) under-drive
        // the bar. We accept that over the alternative.
        //
        // Transposed Direct Form II. Coefficients designed once at init
        // time from the WASAPI mix-format sample rate.
        struct Biquad
        {
            float b0{0.f}, b1{0.f}, b2{0.f}, a1{0.f}, a2{0.f};
            float z1{0.f}, z2{0.f};

            void DesignBandpass(float sampleRate, float centerHz, float q)
            {
                const float omega = 2.f * 3.14159265f * centerHz / sampleRate;
                const float sinw  = std::sin(omega);
                const float cosw  = std::cos(omega);
                const float alpha = sinw / (2.f * q);
                const float a0    = 1.f + alpha;
                b0 =  alpha / a0;
                b1 =  0.f;
                b2 = -alpha / a0;
                a1 = -2.f * cosw / a0;
                a2 = (1.f - alpha) / a0;
                z1 = z2 = 0.f;
            }

            float Process(float x)
            {
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }

            void Reset() { z1 = z2 = 0.f; }
        };

        // 80 Hz center, Q=0.7 → pass band ~55-115 Hz at -3 dB. Designed
        // lazily at format-discovery time so we adapt to whatever sample
        // rate WASAPI hands us (typically 44100 or 48000).
        Biquad g_bassFilter{};
        bool   g_bassFilterDesigned = false;

        bool IsFloatFormat(const WAVEFORMATEX* fmt)
        {
            if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
            if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                && fmt->cbSize >= 22)
            {
                const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
                return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            }
            return false;
        }

        bool IsPcm16Format(const WAVEFORMATEX* fmt)
        {
            if (fmt->wBitsPerSample != 16) return false;
            if (fmt->wFormatTag == WAVE_FORMAT_PCM) return true;
            if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                && fmt->cbSize >= 22)
            {
                const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
                return ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
            }
            return false;
        }

        // Compute mean-square (not RMS — sqrt comes after accumulation
        // across all chunks in the packet). Each frame is mono-averaged
        // across channels then run through the bass-band bandpass; the
        // filtered sample is what gets squared. End result is energy in
        // ~55-115 Hz where music kick/bass lives. Filter state (biquad
        // delays) persists across chunks via file-scope g_bassFilter, so
        // cross-packet continuity is correct.
        double ChunkMeanSquare(const BYTE* data, uint32_t numFrames, const WAVEFORMATEX* fmt)
        {
            if (numFrames == 0) return 0.0;
            const uint32_t ch = fmt->nChannels;
            if (ch == 0) return 0.0;

            double sumSq = 0.0;

            if (IsFloatFormat(fmt))
            {
                const float* samples = reinterpret_cast<const float*>(data);
                for (uint32_t f = 0; f < numFrames; ++f)
                {
                    float mono = 0.f;
                    for (uint32_t c = 0; c < ch; ++c) mono += samples[f * ch + c];
                    mono /= static_cast<float>(ch);
                    const float y = g_bassFilter.Process(mono);
                    sumSq += static_cast<double>(y) * y;
                }
            }
            else if (IsPcm16Format(fmt))
            {
                const int16_t* samples = reinterpret_cast<const int16_t*>(data);
                for (uint32_t f = 0; f < numFrames; ++f)
                {
                    float mono = 0.f;
                    for (uint32_t c = 0; c < ch; ++c)
                        mono += static_cast<float>(samples[f * ch + c]) / 32768.f;
                    mono /= static_cast<float>(ch);
                    const float y = g_bassFilter.Process(mono);
                    sumSq += static_cast<double>(y) * y;
                }
            }
            // Unknown format → treat as silent. Modern Windows mix format
            // is effectively always float32, so this path is rare.

            return sumSq / static_cast<double>(numFrames);
        }

        // Case-insensitive lookup of a running process by its exe name
        // (e.g. "Spotify.exe"). Returns 0 if no match is found. We use
        // Toolhelp32 rather than WTSEnumerateProcesses so we can match
        // without needing session-query privileges.
        DWORD FindProcessIdByName(const std::string& name)
        {
            if (name.empty()) return 0;

            // Widen ASCII-ish name for Process32FirstW. Full UTF-8 isn't
            // required — exe filenames on Windows are typically ASCII.
            std::wstring wname;
            wname.reserve(name.size());
            for (char c : name) wname.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));

            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE) return 0;

            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            DWORD found = 0;
            if (Process32FirstW(snap, &entry))
            {
                do {
                    if (_wcsicmp(entry.szExeFile, wname.c_str()) == 0)
                    {
                        found = entry.th32ProcessID;
                        break;
                    }
                } while (Process32NextW(snap, &entry));
            }
            CloseHandle(snap);
            return found;
        }

        // IAudioSessionEvents listener that pushes per-app volume
        // changes straight into g_volumeScalar. Game Bar / Volume
        // Mixer fire OnSimpleVolumeChanged synchronously when the
        // user drags a slider, giving us instant compensation
        // updates with zero per-tick overhead.
        struct SessionVolumeListener : public IAudioSessionEvents
        {
            LONG m_refs = 1;

            ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
            ULONG STDMETHODCALLTYPE Release() override
            {
                const LONG n = InterlockedDecrement(&m_refs);
                if (n == 0) delete this;
                return static_cast<ULONG>(n);
            }
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
            {
                if (!ppv) return E_POINTER;
                if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionEvents))
                {
                    *ppv = static_cast<IAudioSessionEvents*>(this);
                    AddRef();
                    return S_OK;
                }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }

            HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float newVolume, BOOL newMute, LPCGUID) override
            {
                const float v = (newMute || newVolume <= 0.f) ? 0.f : newVolume;
                g_volumeScalar.store(v, std::memory_order_release);
                return S_OK;
            }

            // Unused but required by the interface.
            HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID)        override { return S_OK; }
            HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID)           override { return S_OK; }
            HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float*, DWORD, LPCGUID) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID)      override { return S_OK; }
            HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState)             override { return S_OK; }
            HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override { return S_OK; }
        };

        // Resolver: find ALL audio sessions for the given PID on the
        // default render endpoint, register a SessionVolumeListener on
        // each, seed g_volumeScalar with the most-attenuated current
        // volume, and append every successfully-hooked session into
        // outSessions. Caller calls UnregisterAudioSessionNotification
        // on each at shutdown.
        //
        // CP2077 (via Wwise) creates multiple audio sessions per
        // process and Game Bar's per-app slider drives all of them in
        // lockstep. Hooking only one means slider drags routed
        // through a different session would not fire OnSimpleVolumeChanged
        // on us — which manifested as ~1 s delay before the poll
        // safety net re-read the truth. Hooking all of them gives
        // single-tick latency regardless of which session the slider
        // event happens to land on.
        //
        // Re-callable: dedupes by IAudioSessionControl2 instance
        // identifier (stable LPWSTR per logical session) — NOT by
        // IAudioSessionControl* interface pointer, which is freshly
        // allocated on each enumeration and never matches across calls.
        // The pointer-based dedup that lived here previously caused a
        // per-poll re-registration leak: every 250 ms each session got
        // pushed onto outSessions again and `listener` got appended to
        // its internal callback list again, both growing without bound
        // for the entire session. Diagnosed via city-driving crashes
        // 2026-05-05.
        void AttachSessionVolumeListenersAll(DWORD targetPid,
                                             SessionVolumeListener* listener,
                                             std::vector<HookedSession>& outSessions)
        {
            IMMDeviceEnumerator*     enumerator = nullptr;
            IMMDevice*               device     = nullptr;
            IAudioSessionManager2*   mgr        = nullptr;
            IAudioSessionEnumerator* sessions   = nullptr;

            auto cleanup = [&]() {
                SafeRelease(sessions);
                SafeRelease(mgr);
                SafeRelease(device);
                SafeRelease(enumerator);
            };

            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                          reinterpret_cast<void**>(&enumerator));
            if (FAILED(hr)) { cleanup(); return; }

            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            if (FAILED(hr)) { cleanup(); return; }

            hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&mgr));
            if (FAILED(hr)) { cleanup(); return; }

            hr = mgr->GetSessionEnumerator(&sessions);
            if (FAILED(hr) || !sessions) { cleanup(); return; }

            float bestScalar = -1.f;
            int   count      = 0;
            sessions->GetCount(&count);
            int newAttachments = 0;
            for (int i = 0; i < count; ++i)
            {
                IAudioSessionControl* ctrl = nullptr;
                if (FAILED(sessions->GetSession(i, &ctrl)) || !ctrl) continue;

                IAudioSessionControl2* ctrl2 = nullptr;
                if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                   reinterpret_cast<void**>(&ctrl2))) && ctrl2)
                {
                    DWORD pid = 0;
                    if (SUCCEEDED(ctrl2->GetProcessId(&pid)) && pid == targetPid)
                    {
                        ISimpleAudioVolume* vol = nullptr;
                        if (SUCCEEDED(ctrl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                           reinterpret_cast<void**>(&vol))) && vol)
                        {
                            float v = 1.f; BOOL muted = FALSE;
                            vol->GetMasterVolume(&v);
                            vol->GetMute(&muted);
                            vol->Release();
                            const float effective = (muted || v <= 0.f) ? 0.f : v;
                            if (bestScalar < 0.f || effective < bestScalar)
                                bestScalar = effective;
                        }

                        // Stable per-session identifier. CoTaskMemFree the
                        // returned LPWSTR after we've copied it into a
                        // wstring. If the call fails (older Windows or COM
                        // hiccup) we fall back to skipping this session
                        // rather than risking a re-registration leak — a
                        // single missed session just means slider events
                        // on it bounce through the 250 ms poll safety net.
                        LPWSTR rawId = nullptr;
                        std::wstring instanceId;
                        if (SUCCEEDED(ctrl2->GetSessionInstanceIdentifier(&rawId)) && rawId)
                        {
                            instanceId = rawId;
                            CoTaskMemFree(rawId);
                        }

                        if (!instanceId.empty())
                        {
                            bool already = false;
                            for (const auto& h : outSessions)
                            {
                                if (h.instanceId == instanceId) { already = true; break; }
                            }
                            if (!already
                                && SUCCEEDED(ctrl->RegisterAudioSessionNotification(listener)))
                            {
                                HookedSession h;
                                h.ctrl       = ctrl;
                                h.instanceId = std::move(instanceId);
                                outSessions.push_back(std::move(h));
                                ctrl = nullptr;  // ownership transferred
                                ++newAttachments;
                            }
                        }
                    }
                    ctrl2->Release();
                }
                if (ctrl) ctrl->Release();
            }

            if (bestScalar >= 0.f)
                g_volumeScalar.store(bestScalar, std::memory_order_release);

            if (newAttachments > 0)
            {
                log::InfoF("[gwheel:audio] attached %d new session-volume listener(s) (total now %zu)",
                           newAttachments, outSessions.size());
            }

            cleanup();
        }

        // Polling re-read for the safety-net path. Walks every audio
        // session belonging to targetPid (the game can have multiple
        // Wwise sessions; Game Bar's slider drives all of them in
        // lockstep) and uses the LOWEST non-muted volume found, which
        // matches what the user effectively hears. Stores into
        // g_volumeScalar. Silent if no session is found.
        void PollSessionVolume(DWORD targetPid)
        {
            IMMDeviceEnumerator*     enumerator = nullptr;
            IMMDevice*               device     = nullptr;
            IAudioSessionManager2*   mgr        = nullptr;
            IAudioSessionEnumerator* sessions   = nullptr;

            auto cleanup = [&]() {
                SafeRelease(sessions);
                SafeRelease(mgr);
                SafeRelease(device);
                SafeRelease(enumerator);
            };

            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                          reinterpret_cast<void**>(&enumerator));
            if (FAILED(hr)) { cleanup(); return; }

            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            if (FAILED(hr)) { cleanup(); return; }

            hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&mgr));
            if (FAILED(hr)) { cleanup(); return; }

            hr = mgr->GetSessionEnumerator(&sessions);
            if (FAILED(hr) || !sessions) { cleanup(); return; }

            float bestScalar = -1.f;  // sentinel: no matching session yet
            int   count      = 0;
            sessions->GetCount(&count);
            for (int i = 0; i < count; ++i)
            {
                IAudioSessionControl* ctrl = nullptr;
                if (FAILED(sessions->GetSession(i, &ctrl)) || !ctrl) continue;

                IAudioSessionControl2* ctrl2 = nullptr;
                if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                   reinterpret_cast<void**>(&ctrl2))) && ctrl2)
                {
                    DWORD pid = 0;
                    if (SUCCEEDED(ctrl2->GetProcessId(&pid)) && pid == targetPid)
                    {
                        ISimpleAudioVolume* vol = nullptr;
                        if (SUCCEEDED(ctrl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                           reinterpret_cast<void**>(&vol))) && vol)
                        {
                            float v = 1.f; BOOL muted = FALSE;
                            vol->GetMasterVolume(&v);
                            vol->GetMute(&muted);
                            vol->Release();
                            const float effective = (muted || v <= 0.f) ? 0.f : v;
                            if (bestScalar < 0.f || effective < bestScalar)
                                bestScalar = effective;
                        }
                    }
                    ctrl2->Release();
                }
                ctrl->Release();
            }

            if (bestScalar >= 0.f)
                g_volumeScalar.store(bestScalar, std::memory_order_release);

            cleanup();
        }

        // Minimal IActivateAudioInterfaceCompletionHandler. The per-process
        // activation API is async; we marshal the completion onto an event
        // and wait synchronously on the init thread.
        struct ActivationHandler : public IActivateAudioInterfaceCompletionHandler
        {
            LONG            m_refs   = 1;
            HANDLE          m_event  = nullptr;
            HRESULT         m_result = E_FAIL;
            IAudioClient*   m_client = nullptr;

            ActivationHandler() { m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
            virtual ~ActivationHandler()
            {
                if (m_client) m_client->Release();
                if (m_event)  CloseHandle(m_event);
            }

            ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
            ULONG STDMETHODCALLTYPE Release() override
            {
                const LONG n = InterlockedDecrement(&m_refs);
                if (n == 0) delete this;
                return static_cast<ULONG>(n);
            }
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
            {
                if (!ppv) return E_POINTER;
                if (riid == __uuidof(IUnknown) ||
                    riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
                    riid == __uuidof(IAgileObject))
                {
                    *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
                    AddRef();
                    return S_OK;
                }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }

            HRESULT STDMETHODCALLTYPE ActivateCompleted(
                IActivateAudioInterfaceAsyncOperation* op) override
            {
                HRESULT actResult = E_UNEXPECTED;
                IUnknown* unk = nullptr;
                m_result = op->GetActivateResult(&actResult, &unk);
                if (SUCCEEDED(m_result)) m_result = actResult;
                if (SUCCEEDED(m_result) && unk)
                    unk->QueryInterface(__uuidof(IAudioClient),
                                        reinterpret_cast<void**>(&m_client));
                if (unk) unk->Release();
                SetEvent(m_event);
                return S_OK;
            }
        };

        // Open a per-process loopback capture attached to `pid`. On
        // success returns a started IAudioClient + its manually-specified
        // PCM format. Windows' per-process API requires a known integer-
        // PCM format (won't honour GetMixFormat), so we ask for the same
        // 16-bit 44.1 kHz stereo shape the official MS ApplicationLoopback
        // sample uses. ChunkMeanSquare handles this format path natively.
        bool OpenPerProcessLoopback(DWORD pid,
                                    IAudioClient** outClient,
                                    WAVEFORMATEX** outFormat)
        {
            AUDIOCLIENT_ACTIVATION_PARAMS params{};
            params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
            params.ProcessLoopbackParams.TargetProcessId  = pid;
            params.ProcessLoopbackParams.ProcessLoopbackMode =
                PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

            PROPVARIANT pv{};
            pv.vt              = VT_BLOB;
            pv.blob.cbSize     = sizeof(params);
            pv.blob.pBlobData  = reinterpret_cast<BYTE*>(&params);

            auto* handler = new ActivationHandler();
            IActivateAudioInterfaceAsyncOperation* op = nullptr;
            HRESULT hr = ActivateAudioInterfaceAsync(
                VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                __uuidof(IAudioClient),
                &pv,
                handler,
                &op);

            if (FAILED(hr))
            {
                log::WarnF("[gwheel:audio] ActivateAudioInterfaceAsync failed hr=0x%08lX", hr);
                handler->Release();
                if (op) op->Release();
                return false;
            }

            const bool signalled = WaitForSingleObject(handler->m_event, 5000) == WAIT_OBJECT_0;
            HRESULT result = handler->m_result;
            IAudioClient* client = nullptr;
            if (signalled)
            {
                client = handler->m_client;
                handler->m_client = nullptr;
            }
            handler->Release();
            if (op) op->Release();

            if (!signalled || FAILED(result) || !client)
            {
                log::WarnF("[gwheel:audio] per-process activation did not complete (signalled=%d, hr=0x%08lX)",
                           signalled ? 1 : 0, result);
                if (client) client->Release();
                return false;
            }

            auto* fmt = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
            if (!fmt) { client->Release(); return false; }
            *fmt = {};
            fmt->wFormatTag      = WAVE_FORMAT_PCM;
            fmt->nChannels       = 2;
            fmt->nSamplesPerSec  = 44100;
            fmt->wBitsPerSample  = 16;
            fmt->nBlockAlign     = static_cast<WORD>(fmt->nChannels * fmt->wBitsPerSample / 8);
            fmt->nAvgBytesPerSec = fmt->nSamplesPerSec * fmt->nBlockAlign;
            fmt->cbSize          = 0;

            constexpr REFERENCE_TIME kHnsBuffer = 2'000'000; // 200 ms
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    kHnsBuffer, 0, fmt, nullptr);
            if (FAILED(hr))
            {
                log::WarnF("[gwheel:audio] per-process IAudioClient::Initialize failed hr=0x%08lX", hr);
                CoTaskMemFree(fmt);
                client->Release();
                return false;
            }

            *outClient = client;
            *outFormat = fmt;
            return true;
        }

        void CaptureLoop()
        {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool comOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
            if (!comOk)
            {
                log::WarnF("[gwheel:audio] CoInitializeEx failed (hr=0x%08lX) — visualizer disabled", hr);
                return;
            }

            IMMDeviceEnumerator* enumerator = nullptr;
            IMMDevice*           device     = nullptr;
            IAudioClient*        client     = nullptr;
            IAudioCaptureClient* capture    = nullptr;
            WAVEFORMATEX*        mixFormat  = nullptr;

            auto cleanup = [&]() {
                if (client) client->Stop();
                SafeRelease(capture);
                SafeRelease(client);
                SafeRelease(device);
                SafeRelease(enumerator);
                if (mixFormat) { CoTaskMemFree(mixFormat); mixFormat = nullptr; }
                if (comOk) CoUninitialize();
            };

            // Pick a capture source. Default (empty processName) is
            // per-process loopback against our OWN process — gwheel.dll
            // is loaded inside Cyberpunk2077.exe, so GetCurrentProcessId()
            // *is* the game's PID. That gives us a clean tap of just the
            // game's audio tree (radio + engine + SFX, but nothing else
            // on the system). Without this, the system-wide loopback
            // mixed in any other app the user had making noise — Spotify
            // tabs, Discord, browser auto-play — and the visualizer
            // appeared to "react to a different song" because it
            // literally was. Non-empty processName is an advanced
            // override for capturing some other app's audio (e.g.
            // streaming a separate Spotify session). System-wide
            // loopback only happens as a last-resort fallback if
            // per-process activation fails outright.
            const auto cfg = config::Current();
            bool  perProcess  = false;
            DWORD attachedPid = 0;  // tracked so the capture loop can poll session volume
            {
                DWORD pid = 0;
                std::string targetLabel;
                if (cfg.music.processName.empty())
                {
                    pid = GetCurrentProcessId();
                    targetLabel = "self (game process)";
                }
                else
                {
                    pid = FindProcessIdByName(cfg.music.processName);
                    targetLabel = cfg.music.processName;
                    if (pid == 0)
                    {
                        log::WarnF("[gwheel:audio] music.processName=\"%s\" not running — falling back to system loopback",
                                   cfg.music.processName.c_str());
                    }
                }

                if (pid != 0)
                {
                    if (OpenPerProcessLoopback(pid, &client, &mixFormat))
                    {
                        log::InfoF("[gwheel:audio] per-process loopback attached to %s (pid=%u, 44100 Hz 16-bit stereo)",
                                   targetLabel.c_str(), pid);
                        perProcess  = true;
                        attachedPid = pid;
                    }
                    else
                    {
                        log::WarnF("[gwheel:audio] per-process loopback failed for %s (pid=%u) — falling back to system loopback",
                                   targetLabel.c_str(), pid);
                    }
                }
            }

            if (!perProcess)
            {
                hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator));
                if (FAILED(hr)) {
                    log::WarnF("[gwheel:audio] CoCreateInstance(MMDeviceEnumerator) failed hr=0x%08lX", hr);
                    cleanup(); return;
                }

                hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
                if (FAILED(hr)) {
                    log::WarnF("[gwheel:audio] GetDefaultAudioEndpoint failed hr=0x%08lX", hr);
                    cleanup(); return;
                }

                hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(&client));
                if (FAILED(hr)) {
                    log::WarnF("[gwheel:audio] IAudioClient::Activate failed hr=0x%08lX", hr);
                    cleanup(); return;
                }

                hr = client->GetMixFormat(&mixFormat);
                if (FAILED(hr) || !mixFormat) {
                    log::WarnF("[gwheel:audio] GetMixFormat failed hr=0x%08lX", hr);
                    cleanup(); return;
                }

                // Loopback captures must be shared-mode and do not support
                // event-driven callbacks. Allocate a 200ms ring buffer and
                // poll on a 10ms timer below.
                constexpr REFERENCE_TIME kHnsBuffer = 2'000'000; // 200 ms in 100ns units
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_LOOPBACK,
                                        kHnsBuffer, 0, mixFormat, nullptr);
                if (FAILED(hr)) {
                    log::WarnF("[gwheel:audio] IAudioClient::Initialize failed hr=0x%08lX", hr);
                    cleanup(); return;
                }
            }

            hr = client->GetService(__uuidof(IAudioCaptureClient),
                                    reinterpret_cast<void**>(&capture));
            if (FAILED(hr)) {
                log::WarnF("[gwheel:audio] GetService(IAudioCaptureClient) failed hr=0x%08lX", hr);
                cleanup(); return;
            }

            hr = client->Start();
            if (FAILED(hr)) {
                log::WarnF("[gwheel:audio] IAudioClient::Start failed hr=0x%08lX", hr);
                cleanup(); return;
            }

            log::InfoF("[gwheel:audio] WASAPI %s loopback started (%u Hz, %u ch, tag=0x%04X)",
                       perProcess ? "per-process" : "system",
                       mixFormat->nSamplesPerSec, mixFormat->nChannels, mixFormat->wFormatTag);

            // Design the bass-band bandpass once the sample rate is known.
            g_bassFilter.DesignBandpass(
                static_cast<float>(mixFormat->nSamplesPerSec), 80.f, 0.7f);
            g_bassFilterDesigned = true;
            log::Info("[gwheel:audio] bass-band bandpass filter designed "
                      "(80 Hz center, Q=0.7)");

            std::deque<float> rollingBuf;
            float smoothed = 0.f;

            // Periodic level log counter. At 10ms chunks, 500 = 5 seconds.
            int logCounter = 0;

            // Empty-tick run length. WASAPI per-process loopback stops
            // emitting packets when the captured tree goes silent —
            // GetNextPacketSize returns 0 indefinitely. We use this to
            // (a) decay the envelope toward 0 so the bar doesn't freeze
            // on the last value, (b) shrink the rolling window's stale
            // peak, and (c) restart the stream entirely if the silence
            // run gets long enough to suggest a dropped capture.
            int emptyTicks = 0;
            constexpr int kEmptyTicksDecay   = 50;   // 0.5 s → start decaying
            constexpr int kEmptyTicksRestart = 500;  // 5.0 s → tear down + restart

            // Per-app session volume compensation. Xbox Game Bar /
            // Windows Volume Mixer adjusts ISimpleAudioVolume on the
            // captured process's session, and per-process loopback
            // sees audio AFTER that fader. The listener registered
            // below pushes volume changes straight into g_volumeScalar
            // synchronously when the user drags a slider — no polling,
            // no per-tick rescan, single capture-tick latency.
            // 1% cutoff. Below that the user is essentially muted and
            // we'd amplify the digital noise floor 100×+ — no useful
            // signal. Above 1% we always compensate so e.g. a 5%
            // slider position keeps the bar pulsing on real music.
            constexpr float kMinCompensationScalar = 0.01f;

            SessionVolumeListener*       listener = nullptr;
            std::vector<HookedSession>   hookedSessions;
            if (perProcess && attachedPid != 0)
            {
                listener = new SessionVolumeListener();
                AttachSessionVolumeListenersAll(attachedPid, listener, hookedSessions);
                if (hookedSessions.empty())
                {
                    log::WarnF("[gwheel:audio] no session-volume listeners attached yet for pid=%u — will keep retrying via poll",
                               attachedPid);
                }
                else
                {
                    log::InfoF("[gwheel:audio] session-volume listeners attached on %zu sessions (instant Game Bar slider compensation)",
                               hookedSessions.size());
                }
            }

            // Polling safety net. The event listeners give instant
            // updates the moment a slider event reaches any of the
            // hooked sessions, but CP2077 can spawn new sessions
            // mid-game and we need to re-hook those plus catch any
            // event that slipped through. 250 ms feels effectively
            // instant to the user without burning CPU.
            int pollCounter = 0;
            constexpr int kPollTicks = 25;  // 0.25 s

            while (g_running.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

                if (perProcess && attachedPid != 0 && ++pollCounter >= kPollTicks)
                {
                    pollCounter = 0;
                    // Authoritative re-read of the lowest-active
                    // session volume. Also opportunistically hooks any
                    // new sessions that have appeared since the last
                    // pass (idempotent for already-hooked sessions).
                    if (listener)
                        AttachSessionVolumeListenersAll(attachedPid, listener, hookedSessions);
                    else
                        PollSessionVolume(attachedPid);
                }

                const float volumeScalar = g_volumeScalar.load(std::memory_order_acquire);

                // Drain every packet available since the last tick.
                double sumSq = 0.0;
                uint32_t totalFrames = 0;

                UINT32 packetFrames = 0;
                HRESULT ph = capture->GetNextPacketSize(&packetFrames);
                if (FAILED(ph)) break;

                while (packetFrames > 0)
                {
                    BYTE*  data   = nullptr;
                    UINT32 frames = 0;
                    DWORD  flags  = 0;
                    ph = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                    if (FAILED(ph)) break;

                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && frames > 0 && data)
                    {
                        const double ms = ChunkMeanSquare(data, frames, mixFormat);
                        sumSq       += ms * static_cast<double>(frames);
                        totalFrames += frames;
                    }

                    capture->ReleaseBuffer(frames);
                    ph = capture->GetNextPacketSize(&packetFrames);
                    if (FAILED(ph)) break;
                }

                float chunkRms = (totalFrames > 0)
                    ? static_cast<float>(std::sqrt(sumSq / static_cast<double>(totalFrames)))
                    : 0.f;

                // Undo the per-app fader so the visualizer responds to
                // the music's actual dynamics, not Game Bar's slider
                // position. Skip compensation when volume is near zero
                // (would amplify the noise floor by 20×+).
                if (perProcess && volumeScalar >= kMinCompensationScalar)
                    chunkRms /= volumeScalar;

                // Track empty-tick runs so we can tell silence from a
                // dropped stream. A tick counts as empty if no non-silent
                // frames came back this round.
                if (totalFrames == 0) ++emptyTicks;
                else                  emptyTicks = 0;

                // Stream-restart watchdog. If the loopback stops giving
                // us packets for 5 seconds straight, the stream has
                // effectively died (per-process loopback is known to do
                // this on long silence). Tear down and re-Start so the
                // next packet of audio actually reaches us.
                if (emptyTicks >= kEmptyTicksRestart)
                {
                    log::Warn("[gwheel:audio] no packets for 5s — restarting capture stream");
                    if (client) client->Stop();
                    HRESULT rh = client ? client->Reset()  : E_FAIL;
                    HRESULT sh = client ? client->Start()  : E_FAIL;
                    if (FAILED(rh) || FAILED(sh))
                    {
                        log::WarnF("[gwheel:audio] capture restart failed (reset hr=0x%08lX, start hr=0x%08lX)", rh, sh);
                        break;
                    }
                    g_bassFilter.Reset();
                    smoothed = 0.f;
                    rollingBuf.clear();
                    emptyTicks = 0;
                    continue;
                }

                // Asymmetric envelope.
                const float alpha = (chunkRms > smoothed) ? kAttackAlpha : kReleaseAlpha;
                smoothed += alpha * (chunkRms - smoothed);

                // Rolling min + max over the recent window. The pair is
                // the dynamic range we stretch across the LED bar, so
                // "quietest recent moment" lands on dark and "loudest
                // recent moment" lands on full.
                rollingBuf.push_back(smoothed);
                while (static_cast<int>(rollingBuf.size()) > kWindowChunks)
                    rollingBuf.pop_front();
                float rollingPeak = 0.f;
                float rollingMin  = std::numeric_limits<float>::max();
                for (float v : rollingBuf)
                {
                    if (v > rollingPeak) rollingPeak = v;
                    if (v < rollingMin)  rollingMin  = v;
                }
                if (rollingBuf.empty()) rollingMin = 0.f;

                // Long-silence peak decay. Once we've been idle for
                // 0.5 s+ the rolling peak is stale — calibrated to a
                // song that's no longer playing. Bleed it down so the
                // first packet of a new song doesn't get crushed
                // against the old loud peak (which would render as
                // a stuck-dim bar for ~3 s until the window flushes).
                if (emptyTicks >= kEmptyTicksDecay)
                {
                    const float decayPerTick = 0.985f;
                    for (float& v : rollingBuf) v *= decayPerTick;
                }

                // Dynamic-range normalisation: stretch the recent
                // window's quietest-to-loudest span across [0..1]. A
                // tiny-range guard avoids amplifying the noise floor
                // into a jittery full-scale bar when the signal is
                // essentially silent. The LED controller decides when
                // to consume this level (only while the radio is on
                // per the game's Blackboard), so we don't need any
                // music-vs-silence classification here.
                const float range = rollingPeak - rollingMin;
                float level = 0.f;
                if (range > 0.0005f)
                    level = std::clamp((smoothed - rollingMin) / range, 0.f, 1.f);
                g_level.store(level, std::memory_order_release);

                // Periodic telemetry. Gated on the FFB debug-log toggle
                // so release logs stay quiet.
                if (++logCounter >= 500 && log::DebugEnabled()) {
                    logCounter = 0;
                    log::DebugF("[gwheel:audio] rms=%.5f smoothed=%.5f min=%.5f peak=%.5f range=%.5f vol=%.2f level=%.2f",
                                chunkRms, smoothed, rollingMin, rollingPeak, range, volumeScalar, level);
                }
            }

            log::Info("[gwheel:audio] WASAPI loopback stopping");

            if (listener)
            {
                for (auto& h : hookedSessions)
                {
                    if (h.ctrl)
                    {
                        h.ctrl->UnregisterAudioSessionNotification(listener);
                        h.ctrl->Release();
                        h.ctrl = nullptr;
                    }
                }
                hookedSessions.clear();
                listener->Release();
                listener = nullptr;
            }
            g_volumeScalar.store(1.f, std::memory_order_release);

            cleanup();
        }
    }

    void Init()
    {
        if (g_running.exchange(true, std::memory_order_acq_rel)) return;
        g_thread = std::thread(CaptureLoop);
    }

    void Shutdown()
    {
        if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
        if (g_thread.joinable()) g_thread.join();
        g_level.store(0.f, std::memory_order_release);
    }

    float CurrentLevel() { return g_level.load(std::memory_order_acquire); }
}
