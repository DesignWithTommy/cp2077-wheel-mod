#include "wheel.h"
#include "logging.h"
#include "device_table.h"
#include "config.h"
#include "input_bindings.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <LogitechSteeringWheelLib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace gwheel::wheel
{
    Axis AxisFromName(const char* name)
    {
        if (!name) return Axis_kCount;
        if (std::strcmp(name, "lX")      == 0) return Axis_lX;
        if (std::strcmp(name, "lY")      == 0) return Axis_lY;
        if (std::strcmp(name, "lZ")      == 0) return Axis_lZ;
        if (std::strcmp(name, "lRx")     == 0) return Axis_lRx;
        if (std::strcmp(name, "lRy")     == 0) return Axis_lRy;
        if (std::strcmp(name, "lRz")     == 0) return Axis_lRz;
        if (std::strcmp(name, "slider0") == 0) return Axis_slider0;
        if (std::strcmp(name, "slider1") == 0) return Axis_slider1;
        return Axis_kCount;
    }

    const char* AxisName(Axis a)
    {
        switch (a)
        {
            case Axis_lX:      return "lX";
            case Axis_lY:      return "lY";
            case Axis_lZ:      return "lZ";
            case Axis_lRx:     return "lRx";
            case Axis_lRy:     return "lRy";
            case Axis_lRz:     return "lRz";
            case Axis_slider0: return "slider0";
            case Axis_slider1: return "slider1";
            default:           return "";
        }
    }

    namespace
    {
        // Normalize a DIJOYSTATE2 LONG axis. The Logi SDK for the G923
        // returns bipolar signed 16-bit values in [-32768, +32767]
        // (verified empirically 2026-04-21 via diagnostic logging).
        //
        // Steer: center=0, full-left=-32768, full-right=+32767.
        // Pedals: idle=+32767, fully-pressed=-32768.
        float NormalizePedal(LONG v)
        {
            constexpr float kMax = 32767.f;
            constexpr float kRange = 65535.f; // kMax - kMin = 32767 - (-32768)
            const float t = (kMax - static_cast<float>(v)) / kRange;
            return std::clamp(t, 0.f, 1.f);
        }

        float NormalizeSteer(LONG v)
        {
            constexpr float kMax = 32767.f;
            return std::clamp(static_cast<float>(v) / kMax, -1.f, 1.f);
        }

        struct State
        {
            std::atomic<bool>  sdkInitialized{false};
            std::atomic<bool>  ready{false};
            std::atomic<int>   index{-1};
            std::atomic<bool>  hasFFB{false};
            std::atomic<float> globalStrength{1.0f};

            std::mutex         snapMtx;
            Snapshot           snap{};

            std::mutex         capsMtx;
            Caps               caps{};

            std::atomic<uint32_t> initAttempts{0};
            std::atomic<bool>     handshakeFired{false};
            std::atomic<bool>     handshakeActive{false};

        };

        State& S() { static State s; return s; }

        // G HUB owns the operating range per-profile. The mod honors it:
        // once per second we read the SDK's current range and cache it
        // into s_ghubRangeDeg, which UpdateCenteringSpring consults to
        // scale FFB magnitude inversely with wheel rotation (so a 900-deg
        // wheel isn't weighed down by SAT meant for a tight 180).
        //
        // Default starting value is a safe 540 — most G HUB profiles land
        // in this range, and the value gets overwritten with the real
        // reading at bind time plus refreshed every second thereafter.
        std::atomic<int> s_ghubRangeDeg{540};

        // Pick the first LOGI_DEVICE_TYPE_WHEEL index that reports connected.
        int FindWheelIndex()
        {
            for (int i = 0; i < LOGI_MAX_CONTROLLERS; ++i)
            {
                if (!LogiIsConnected(i)) continue;
                if (LogiIsDeviceConnected(i, LOGI_DEVICE_TYPE_WHEEL)) return i;
            }
            // Fall back: accept any connected Logitech-manufactured controller.
            for (int i = 0; i < LOGI_MAX_CONTROLLERS; ++i)
            {
                if (!LogiIsConnected(i)) continue;
                if (LogiIsManufacturerConnected(i, LOGI_MANUFACTURER_LOGITECH)) return i;
            }
            return -1;
        }

        // Only accept a top-level window belonging to the game's own process.
        // DirectInput (used by the SDK under the hood) rejects foreign HWNDs.
        HWND FindOwnGameWindow()
        {
            const DWORD myPid = GetCurrentProcessId();
            auto ownedByMe = [&](HWND h) -> bool {
                if (!h || !IsWindow(h)) return false;
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                return pid == myPid;
            };
            HWND h = FindWindowW(L"RED4Engine", nullptr);
            if (ownedByMe(h)) return h;
            h = FindWindowW(L"Cyberpunk 2077", nullptr);
            if (ownedByMe(h)) return h;

            struct Ctx { DWORD pid; HWND hit; } ctx{ myPid, nullptr };
            EnumWindows([](HWND w, LPARAM p) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(p);
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                if (pid != c->pid) return TRUE;
                if (!IsWindowVisible(w)) return TRUE;
                RECT r{};
                if (!GetClientRect(w, &r) || (r.right - r.left) < 400 || (r.bottom - r.top) < 300)
                    return TRUE;
                c->hit = w;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&ctx));
            return ctx.hit;
        }

        void LogSdkVersion()
        {
            auto& st = S();
            int major = 0, minor = 0, build = 0;
            if (LogiSteeringGetSdkVersion(&major, &minor, &build))
            {
                std::lock_guard lk(st.capsMtx);
                st.caps.sdkMajor = major;
                st.caps.sdkMinor = minor;
                st.caps.sdkBuild = build;
                log::InfoF("[gwheel] Logitech Steering Wheel SDK v%d.%d build %d", major, minor, build);
            }
            else
            {
                log::Warn("[gwheel] LogiSteeringGetSdkVersion failed; continuing anyway");
            }
        }

        void CaptureCaps(int idx)
        {
            auto& st = S();
            Caps c{};
            {
                std::lock_guard lk(st.capsMtx);
                c = st.caps;
            }

            wchar_t friendly[256] = {};
            if (LogiGetFriendlyProductName(idx, friendly, 256))
            {
                WideCharToMultiByte(CP_UTF8, 0, friendly, -1,
                                    c.productName, sizeof(c.productName),
                                    nullptr, nullptr);
            }
            else
            {
                std::snprintf(c.productName, sizeof(c.productName),
                              "Logitech wheel (slot %d)", idx);
            }

            c.hasFFB = LogiHasForceFeedback(idx);

            int range = 0;
            if (LogiGetOperatingRange(idx, range)) c.operatingRangeDeg = range;

            // Best-effort model probe against the constants this SDK version
            // actually ships (header tops out at G920; newer wheels report via
            // the friendlier name path).
            const int probes[] = {
                LOGI_MODEL_G920,
                LOGI_MODEL_G29,
                LOGI_MODEL_G27,
                LOGI_MODEL_G25,
                LOGI_MODEL_DRIVING_FORCE_GT,
                LOGI_MODEL_DRIVING_FORCE_PRO,
                LOGI_MODEL_DRIVING_FORCE,
                LOGI_MODEL_MOMO_RACING,
                LOGI_MODEL_MOMO_FORCE,
                LOGI_MODEL_FORMULA_FORCE,
                LOGI_MODEL_FORMULA_FORCE_GP,
            };
            for (int model : probes)
            {
                if (LogiIsModelConnected(idx, model))
                {
                    log::InfoF("[gwheel] SDK model match: LOGI_MODEL id=%d", model);
                    break;
                }
            }

            {
                std::lock_guard lk(st.capsMtx);
                st.caps = c;
            }
        }

        // Single entry point for every LED write. Handles the G923
        // "idle-indicator" quirk: with firstLedOn=0 and currentRPM=0,
        // the wheel firmware leaves the outermost green LEDs lit as a
        // "ready to rev" marker. Pushing currentRPM strictly below
        // firstLedOn (here 100 < 200) guarantees the bar goes fully dark.
        void WriteLeds(int idx, float value)
        {
            if (value <= 0.005f)
                LogiPlayLeds(idx, 0.f, 100.f, 200.f);
            else
                LogiPlayLeds(idx, std::clamp(value, 0.f, 1.f), 0.f, 1.f);
        }

        void FireGwheelHandshake(int idx)
        {
            auto& st = S();
            if (st.handshakeFired.exchange(true)) return;
            std::thread([idx] {
                using namespace std::chrono_literals;
                auto& st = S();

                // Raise the handshake-active flag for the duration so the
                // LED controller (led.cpp) yields the bar to us.
                st.handshakeActive.store(true, std::memory_order_release);

                log::Info("[gwheel] firing gwheel handshake (LED sweep + 4 triplets + centering breath)");

                // Small helper: linear LED ramp from `from` to `to` over
                // `total`. Steps at ~33ms (matches the LED controller's
                // tick rate) for a perceptually smooth sweep without
                // hammering the SDK.
                auto ledSweep = [](int deviceIdx, float from, float to,
                                   std::chrono::milliseconds total) {
                    constexpr auto kStep = std::chrono::milliseconds(33);
                    const int steps = std::max<int>(1, static_cast<int>(total / kStep));
                    for (int i = 1; i <= steps; ++i) {
                        const float t = static_cast<float>(i) / static_cast<float>(steps);
                        const float v = from + (to - from) * t;
                        WriteLeds(deviceIdx, v);
                        std::this_thread::sleep_for(kStep);
                    }
                };

                // --- LED pre-roll: 400ms sweep-up / sweep-down --------
                // Replaces the old 400ms idle sleep; same total duration.
                ledSweep(idx, 0.f, 1.f, 200ms);
                ledSweep(idx, 1.f, 0.f, 200ms);

                // --- Triplets: R L R, L R L, R L R, L R L ------------
                // 300 BPM: quarter note = 200ms. Each beat fires a short
                // force kick AND flashes the LED bar full for the kick
                // duration; the bar goes dark between kicks so the lights
                // track the rhythm instead of blurring together.
                constexpr int kTriplets[2][3] = {
                    { +1, -1, +1 },   // R L R
                    { -1, +1, -1 },   // L R L
                };
                constexpr auto kPulseMs     = 40ms;               // active kick
                constexpr auto kBeatMs      = 200ms;              // quarter @ 300 BPM
                constexpr auto kGapMs       = kBeatMs - kPulseMs; // silence within beat
                constexpr auto kRestMs      = 200ms;              // rest between triplets
                constexpr int  kMagnitude   = 45;                 // percent
                constexpr int  kNumTriplets = 4;

                for (int t = 0; t < kNumTriplets; ++t)
                {
                    for (int b = 0; b < 3; ++b)
                    {
                        LogiPlayConstantForce(idx, kTriplets[t % 2][b] * kMagnitude);
                        WriteLeds(idx, 1.f);
                        std::this_thread::sleep_for(kPulseMs);
                        LogiStopConstantForce(idx);
                        WriteLeds(idx, 0.f);
                        std::this_thread::sleep_for(kGapMs);
                    }
                    std::this_thread::sleep_for(kRestMs);
                }

                // --- Centering hold: 3s spring + LED breath pulse -----
                // Three 1-second breaths (up 500ms, down 500ms, peak
                // 70% so a full-scale finish from the prior flashes
                // reads as "calm" rather than "still alarming"). Total
                // 3000ms matches the original centering hold window.
                LogiStopConstantForce(idx);
                LogiStopDamperForce(idx);
                const bool springOk = LogiPlaySpringForce(idx, 0, 100, 100);
                log::InfoF("[gwheel] handshake centering begin (spring=%d) - holding 3s with LED breath",
                           springOk ? 1 : 0);
                for (int i = 0; i < 3; ++i) {
                    ledSweep(idx, 0.f, 0.7f, 500ms);
                    ledSweep(idx, 0.7f, 0.f, 500ms);
                }
                log::Info("[gwheel] handshake centering end - releasing forces");
                LogiStopSpringForce(idx);
                LogiStopConstantForce(idx);
                LogiStopDamperForce(idx);

                // Clear the bar and hand it back to the LED controller.
                WriteLeds(idx, 0.f);
                st.handshakeActive.store(false, std::memory_order_release);
            }).detach();
        }

        bool TryInitSdk()
        {
            auto& st = S();
            if (st.sdkInitialized.load(std::memory_order_acquire)) return true;

            HWND hwnd = FindOwnGameWindow();
            const uint32_t n = st.initAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
            // Log the per-attempt result only on the first try and every ~5s
            // afterwards (pump thread runs at 250 Hz so ~1250 attempts == 5s).
            const bool verbose = (n == 1) || (n % 1250 == 0);

            // ignoreXInput=true: on the G923 Xbox variant (and any other Logi
            // wheel that exposes an XInput face), CP2077 registers an XInput
            // gamepad which siphons the standard-gamepad bits (ABXY / D-pad /
            // LSB / RSB / paddles as triggers / Start / Back) away from
            // DInput. The Logi SDK reads from DInput, so with ignoreXInput=
            // false we only see the non-XInput extras (scroll rotary, +/-,
            // Xbox button). Passing true forces the SDK to surface the full
            // DInput button set even when an XInput client is active in-
            // process. Standalone (input_probe) saw 20/20 buttons because no
            // XInput client was live; in-game we saw 7/20 because CP2077's
            // XInput layer was intercepting. Verified by tools/input_probe.
            bool ok = false;
            if (hwnd)
            {
                ok = LogiSteeringInitializeWithWindow(true, hwnd);
                if (verbose || ok)
                    log::InfoF("[gwheel] LogiSteeringInitializeWithWindow(ignoreXInput=true, hwnd=0x%p) -> %s",
                               static_cast<void*>(hwnd), ok ? "true" : "false");
            }
            else
            {
                ok = LogiSteeringInitialize(true);
                if (verbose)
                    log::InfoF("[gwheel] LogiSteeringInitialize(ignoreXInput=true) -> %s (no game window yet)",
                               ok ? "true" : "false");
            }

            if (!ok)
            {
                if (verbose)
                {
                    log::WarnF("[gwheel] SDK init did not succeed (attempt %u). "
                               "Will retry each pump tick. Common causes: G HUB not running, "
                               "wheel unplugged, or SDK not permitted while another exclusive client holds it.",
                               n);
                }
                return false;
            }

            st.sdkInitialized.store(true, std::memory_order_release);
            LogSdkVersion();
            return true;
        }

        bool TryBindController()
        {
            auto& st = S();
            if (st.ready.load(std::memory_order_acquire)) return true;

            const int idx = FindWheelIndex();
            if (idx < 0)
            {
                const uint32_t n = st.initAttempts.load(std::memory_order_relaxed);
                if (n == 1 || (n % 250) == 0)
                    log::Warn("[gwheel] no Logitech wheel detected by the SDK yet - waiting");
                return false;
            }

            st.index.store(idx, std::memory_order_release);
            CaptureCaps(idx);

            const Caps snap = GetCaps();
            log::InfoF("[gwheel] wheel bound at SDK slot %d: \"%s\" (FFB=%s range=%d deg)",
                       idx,
                       snap.productName,
                       snap.hasFFB ? "yes" : "no",
                       snap.operatingRangeDeg);

            // Seed the cached G HUB range from the bind-time readback so
            // FFB scaling has a real value before the 1 Hz sampler fires.
            if (snap.operatingRangeDeg > 0)
                s_ghubRangeDeg.store(snap.operatingRangeDeg, std::memory_order_release);

            st.hasFFB.store(snap.hasFFB, std::memory_order_release);
            st.ready.store(true, std::memory_order_release);

            // Pick the per-wheel button/POV layout based on friendly name.
            input_bindings::SetDeviceLayout(snap.productName);

            if (snap.hasFFB)
            {
                if (config::Current().handshake.playOnStart)
                    FireGwheelHandshake(idx);
                else
                    log::Info("[gwheel] gwheel handshake disabled by config (handshake.playOnStart=false)");
            }

            return true;
        }

        // Torque is applied as a per-effect magnitude multiplier via
        // wheel::SetGlobalStrength (wired from config::ApplyDerived).
        // The Logi Properties API is broken on recent G HUB versions —
        // LogiSetPreferredControllerProperties consistently returns
        // FAILED — so we can't use the "proper" route of pushing
        // overallGain to G HUB and letting G HUB scale the output at
        // the driver level. The per-effect multiplier is reliable and
        // composes correctly with G HUB's own TRUEFORCE Torque slider.

        void RefreshGHubRange(int idx)
        {
            // Pump runs at ~250 Hz; sample every 1s (≈250 ticks).
            static uint64_t s_pumpTicks = 0;
            if ((++s_pumpTicks % 250) != 0) return;

            int actual = 0;
            if (!LogiGetOperatingRange(idx, actual) || actual <= 0) return;

            const int prev = s_ghubRangeDeg.exchange(actual, std::memory_order_acq_rel);
            if (prev != actual)
            {
                log::InfoF("[gwheel] operating range (G HUB): %d -> %d deg (FFB auto-scaling to match)",
                           prev, actual);
            }
        }
    }

    // Edge-triggered state for UpdateCenteringSpring, hoisted out of the
    // function so the pump watchdog can reset it when the game pauses and
    // the vehicle detour stops firing. -1 / INT_MIN / false = not playing.
    namespace centering_state
    {
        inline std::atomic<int>  s_lastCoefPct{-1};
        inline std::atomic<int>  s_lastConstPct{INT_MIN};
        inline std::atomic<int>  s_lastDamperPct{-1};
        inline std::atomic<int>  s_lastBumpyPct{-1};
        inline std::atomic<bool> s_airborneOn{false};
        inline std::atomic<uint64_t> s_lastCallTickMs{0};

        // Low-pass-filtered road-surface magnitude (0..1). Risess fast on
        // suspension-activity spikes, decays slowly so a single pothole
        // leaves a brief trail. Reset when the game pauses.
        inline std::atomic<float> s_surfaceEnvelope{0.f};

        // Directional jolt state. TriggerJolt sets these three; the spring
        // update overlays a linearly-decaying force on the active torque
        // until the duration elapses. s_joltDurationMs==0 means no jolt.
        inline std::atomic<float>    s_joltForce{0.f};
        inline std::atomic<uint64_t> s_joltStartMs{0};
        inline std::atomic<uint32_t> s_joltDurationMs{0};

        inline void Reset()
        {
            s_lastCoefPct.store(-1, std::memory_order_release);
            s_lastConstPct.store(INT_MIN, std::memory_order_release);
            s_lastDamperPct.store(-1, std::memory_order_release);
            s_lastBumpyPct.store(-1, std::memory_order_release);
            s_airborneOn.store(false, std::memory_order_release);
            s_surfaceEnvelope.store(0.f, std::memory_order_release);
            s_joltDurationMs.store(0, std::memory_order_release);
        }
    }

    bool Init()
    {
        log::Info("[gwheel] wheel::Init (Logitech SDK) - SDK init is deferred to the pump thread");
        // We do NOT call LogiSteeringInitialize here. At plugin-load time the
        // game window does not exist yet and init can fail. TryInitSdk() runs
        // from the pump thread and retries until it succeeds.
        return true;
    }

    void Shutdown()
    {
        auto& st = S();
        st.ready.store(false, std::memory_order_release);
        if (st.sdkInitialized.exchange(false))
        {
            const int idx = st.index.load(std::memory_order_acquire);
            if (idx >= 0)
            {
                // Always stop any forces we might have kicked off.
                LogiStopSpringForce(idx);
                LogiStopDamperForce(idx);
                LogiStopConstantForce(idx);
                LogiStopSurfaceEffect(idx);
                LogiStopCarAirborne(idx);

                // Operating range is G HUB's to manage; we don't touch it
                // on shutdown. G HUB will keep enforcing whatever its
                // profile says.
            }
            LogiSteeringShutdown();
            log::Info("[gwheel] LogiSteeringShutdown ok");
        }
        st.index.store(-1, std::memory_order_release);
        log::Info("[gwheel] wheel::Shutdown complete");
    }

    bool IsReady() { return S().ready.load(std::memory_order_acquire); }

    const Caps& GetCaps()
    {
        auto& st = S();
        std::lock_guard lk(st.capsMtx);
        static thread_local Caps tl;
        tl = st.caps;
        return tl;
    }

    void Pump()
    {
        auto& st = S();

        if (!st.sdkInitialized.load(std::memory_order_acquire))
        {
            if (!TryInitSdk()) return;
        }

        LogiUpdate();

        if (!st.ready.load(std::memory_order_acquire))
        {
            if (!TryBindController()) return;
        }

        const int idx = st.index.load(std::memory_order_acquire);
        if (idx < 0) return;

        if (!LogiIsConnected(idx))
        {
            if (st.ready.exchange(false))
                log::Warn("[gwheel] wheel disconnected; will re-bind when it returns");
            return;
        }

        RefreshGHubRange(idx);

        // Pause / unmount watchdog. UpdateCenteringSpring heartbeats each
        // time the vehicle detour fires. When the game pauses, the detour
        // stops firing — but already-playing effects (spring, road surface,
        // etc.) keep running until something stops them. If no heartbeat
        // in 200 ms, tear everything down. Re-arms automatically when the
        // detour fires again.
        {
            const uint64_t last = centering_state::s_lastCallTickMs.load(std::memory_order_acquire);
            static std::atomic<bool> s_gracefullyTornDown{false};
            if (last > 0)
            {
                const uint64_t now = GetTickCount64();
                const bool stale  = (now - last) > 200;
                const bool wasTD  = s_gracefullyTornDown.load(std::memory_order_acquire);
                if (stale && !wasTD)
                {
                    StopAll();
                    centering_state::Reset();
                    s_gracefullyTornDown.store(true, std::memory_order_release);
                    log::Info("[gwheel] FFB effects released (gameplay halted — pause / unmount)");
                }
                else if (!stale && wasTD)
                {
                    s_gracefullyTornDown.store(false, std::memory_order_release);
                    log::Info("[gwheel] FFB effects re-arming (gameplay resumed)");
                }
            }
        }

        const DIJOYSTATE2* raw = LogiGetState(idx);
        if (!raw) return;

        Snapshot s;
        s.connected = true;
        s.steer     = NormalizeSteer(raw->lX);
        // Classic Logitech pedal mapping via SDK: Y=throttle, Rz=brake, Slider0=clutch.
        s.throttle  = NormalizePedal(raw->lY);
        s.brake     = NormalizePedal(raw->lRz);
        s.clutch    = NormalizePedal(raw->rglSlider[0]);

        // Phase 5a: also publish all eight DInput axes by index for the
        // binding-driven resolver. Steer axis (lX) keeps its -1..+1 sign;
        // every other axis is normalized as a pedal (0..1, idle->full press)
        // so the resolver can max() across them without sign-aware code.
        s.axes[Axis_lX]      = NormalizeSteer(raw->lX);
        s.axes[Axis_lY]      = NormalizePedal(raw->lY);
        s.axes[Axis_lZ]      = NormalizePedal(raw->lZ);
        s.axes[Axis_lRx]     = NormalizePedal(raw->lRx);
        s.axes[Axis_lRy]     = NormalizePedal(raw->lRy);
        s.axes[Axis_lRz]     = NormalizePedal(raw->lRz);
        s.axes[Axis_slider0] = NormalizePedal(raw->rglSlider[0]);
        s.axes[Axis_slider1] = NormalizePedal(raw->rglSlider[1]);

        s.pov       = static_cast<uint16_t>(raw->rgdwPOV[0] & 0xFFFF);

        uint32_t bits = 0;
        for (int i = 0; i < 32; ++i)
            if (raw->rgbButtons[i] & 0x80) bits |= (1u << i);
        s.buttons = bits;

        {
            std::lock_guard lk(st.snapMtx);
            st.snap = s;
        }
    }

    Snapshot CurrentSnapshot()
    {
        auto& st = S();
        std::lock_guard lk(st.snapMtx);
        return st.snap;
    }

    namespace
    {
        int Scale100(float v)
        {
            const float mul = std::clamp(S().globalStrength.load(std::memory_order_relaxed), 0.f, 1.f);
            return std::clamp(static_cast<int>(std::lround(std::clamp(v * mul, -1.f, 1.f) * 100.f)),
                              -100, 100);
        }

        int ScaleUnsigned100(float v)
        {
            const float mul = std::clamp(S().globalStrength.load(std::memory_order_relaxed), 0.f, 1.f);
            return std::clamp(static_cast<int>(std::lround(std::clamp(v * mul, 0.f, 1.f) * 100.f)),
                              0, 100);
        }
    }

    void PlayConstant(float magnitude)
    {
        auto& st = S();
        if (!st.ready.load() || !st.hasFFB.load()) return;
        const int idx = st.index.load(std::memory_order_acquire);
        const int pct = Scale100(magnitude);
        if (!LogiPlayConstantForce(idx, pct))
            log::DebugF("[gwheel] LogiPlayConstantForce(%d) returned false", pct);
        else if (log::DebugEnabled())
            log::DebugF("[gwheel] constant=%d%%", pct);
    }

    void StopConstant()
    {
        auto& st = S();
        const int idx = st.index.load(std::memory_order_acquire);
        if (idx >= 0) LogiStopConstantForce(idx);
    }

    void PlayDamper(float coefficient)
    {
        auto& st = S();
        if (!st.ready.load() || !st.hasFFB.load()) return;
        const int idx = st.index.load(std::memory_order_acquire);
        const int pct = ScaleUnsigned100(coefficient);
        if (!LogiPlayDamperForce(idx, pct))
            log::DebugF("[gwheel] LogiPlayDamperForce(%d) returned false", pct);
        else if (log::DebugEnabled())
            log::DebugF("[gwheel] damper=%d%%", pct);
    }

    void StopDamper()
    {
        auto& st = S();
        const int idx = st.index.load(std::memory_order_acquire);
        if (idx >= 0) LogiStopDamperForce(idx);
    }

    void PlaySpring(float coefficient)
    {
        auto& st = S();
        if (!st.ready.load() || !st.hasFFB.load()) return;
        const int idx = st.index.load(std::memory_order_acquire);
        const int pct = ScaleUnsigned100(coefficient);
        // offset=0 (centered), saturation=100, coefficient=pct.
        if (!LogiPlaySpringForce(idx, 0, 100, pct))
            log::DebugF("[gwheel] LogiPlaySpringForce(coef=%d) returned false", pct);
        else if (log::DebugEnabled())
            log::DebugF("[gwheel] spring=%d%%", pct);
    }

    void StopSpring()
    {
        auto& st = S();
        const int idx = st.index.load(std::memory_order_acquire);
        if (idx >= 0) LogiStopSpringForce(idx);
    }

    void SetGlobalStrength(float mul)
    {
        S().globalStrength.store(std::clamp(mul, 0.f, 1.f), std::memory_order_relaxed);
    }

    void PlayRoadSurface(float magnitude, int periodMs)
    {
        auto& st = S();
        if (!st.ready.load() || !st.hasFFB.load()) return;
        const int idx = st.index.load(std::memory_order_acquire);
        const int pct = ScaleUnsigned100(magnitude);
        if (!LogiPlaySurfaceEffect(idx, LOGI_PERIODICTYPE_SINE, pct, periodMs))
            log::DebugF("[gwheel] LogiPlaySurfaceEffect(SINE, %d, %dms) returned false", pct, periodMs);
        else if (log::DebugEnabled())
            log::DebugF("[gwheel] surface=%d%% @ %dms", pct, periodMs);
    }

    void StopRoadSurface()
    {
        auto& st = S();
        const int idx = st.index.load(std::memory_order_acquire);
        if (idx >= 0) LogiStopSurfaceEffect(idx);
    }

    void PlayCarAirborne()
    {
        auto& st = S();
        if (!st.ready.load() || !st.hasFFB.load()) return;
        const int idx = st.index.load(std::memory_order_acquire);
        if (!LogiPlayCarAirborne(idx))
            log::Debug("[gwheel] LogiPlayCarAirborne returned false");
        else if (log::DebugEnabled())
            log::Debug("[gwheel] airborne ON");
    }

    void StopCarAirborne()
    {
        auto& st = S();
        const int idx = st.index.load(std::memory_order_acquire);
        if (idx >= 0) LogiStopCarAirborne(idx);
    }

    void StopAll()
    {
        auto& st = S();
        const int idx = st.index.load(std::memory_order_acquire);
        if (idx < 0) return;
        LogiStopConstantForce(idx);
        LogiStopDamperForce(idx);
        LogiStopSpringForce(idx);
        LogiStopSurfaceEffect(idx);
        LogiStopCarAirborne(idx);
    }

    void PlayLeds(float level)
    {
        auto& st = S();
        if (!st.ready.load()) return;
        const int idx = st.index.load(std::memory_order_acquire);
        if (idx < 0) return;
        WriteLeds(idx, level);
    }

    void ClearLeds()
    {
        auto& st = S();
        if (!st.ready.load()) return;
        const int idx = st.index.load(std::memory_order_acquire);
        if (idx < 0) return;
        WriteLeds(idx, 0.f);
    }

    bool IsHandshakeActive()
    {
        return S().handshakeActive.load(std::memory_order_acquire);
    }

    // Surface-driven baseline magnitude for the road-surface SINE.
    // Written by SetSurfaceBaselineMag (called from the redscript
    // material poller via the GWheel_OnWheelMaterial native's C++
    // dispatch), read by UpdateCenteringSpring. 0 = no baseline
    // (asphalt-class); positive = constant hum of that magnitude.
    std::atomic<float> s_surfaceBaselineMag{0.f};

    void SetSurfaceBaselineMag(float mag)
    {
        s_surfaceBaselineMag.store(std::clamp(mag, 0.f, 0.5f), std::memory_order_relaxed);
    }

    void TriggerJolt(float lateralKick, int durationMs)
    {
        using namespace centering_state;
        if (durationMs <= 0) return;
        const float k = std::clamp(lateralKick, -1.f, 1.f);
        // Tiny kicks don't register through the wheel motor's static
        // friction threshold — ignore them rather than burn a jolt slot.
        if (std::fabs(k) < 0.05f)
        {
            if (log::DebugEnabled())
                log::DebugF("[gwheel:ffb] jolt REJECTED (tiny): kick=%+.3f dur=%dms", k, durationMs);
            return;
        }
        s_joltForce.store(k, std::memory_order_release);
        s_joltStartMs.store(GetTickCount64(), std::memory_order_release);
        s_joltDurationMs.store(static_cast<uint32_t>(durationMs),
                               std::memory_order_release);
        if (log::DebugEnabled())
            log::DebugF("[gwheel:ffb] jolt queued: kick=%+.3f dur=%dms", k, durationMs);
    }

    void UpdateCenteringSpring(float absSpeedMps,
                               float angVelMagRad,
                               float suspensionActivity,
                               float lateralVelocityMps,
                               float steer,
                               float throttle,
                               float brake,
                               bool  isReversing,
                               bool  isOnGround,
                               bool  enabled,
                               float stationaryMps,
                               float cruiseMps,
                               float centeringBaseline,
                               int   yawFeedbackPct,
                               float yawRef,
                               int   activeTorqueStrengthPct,
                               bool  debugLog)
    {
        using namespace centering_state;

        // Heartbeat for the pump-thread pause watchdog. If this tick count
        // stops advancing (vehicle detour stopped firing — pause, unmount),
        // the pump will tear down all effects after a short grace period.
        s_lastCallTickMs.store(GetTickCount64(), std::memory_order_release);

        // Reverse is a physical constant: when a car backs up, pneumatic
        // trail flips sign and SAT drops to roughly 30-50% of forward. Not
        // car-dependent and not taste — same ratio for every vehicle.
        constexpr float kReverseMul = 0.4f;

        // Operating-range compensation. A 900-deg wheel turns 5x further
        // than a 180-deg wheel for the same SAT output, so the per-degree
        // force feels overwhelming. Scale all FFB inversely with range,
        // anchored on 180-deg = 1.0. Range is read from G HUB (cached by
        // the pump thread); if the user changes profiles mid-session we
        // pick it up within a second. Clamped to avoid zero and absurd
        // boosts at tight ranges.
        const int ghubRange = std::clamp(s_ghubRangeDeg.load(std::memory_order_acquire),
                                         40, 900);
        const float rangeScale = std::clamp(180.f / static_cast<float>(ghubRange),
                                            0.3f, 1.5f);

        auto& st = S();
        if (!st.ready.load() || !st.hasFFB.load()) return;

        // Helper lambdas so the early-return and airborne paths can tear
        // down the same set of effects without code duplication.
        auto stopContactEffects = [&]() {
            const int prevS = s_lastCoefPct.exchange(-1, std::memory_order_acq_rel);
            if (prevS >= 0) StopSpring();
            const int prevC = s_lastConstPct.exchange(INT_MIN, std::memory_order_acq_rel);
            if (prevC != INT_MIN) StopConstant();
            const int prevD = s_lastDamperPct.exchange(-1, std::memory_order_acq_rel);
            if (prevD >= 0) StopDamper();
            const int prevB = s_lastBumpyPct.exchange(-1, std::memory_order_acq_rel);
            if (prevB >= 0) StopRoadSurface();
        };

        // Airborne: all four wheels off the ground. Real SAT drops to zero
        // (no tire load). Play the SDK's dedicated airborne effect (a brief
        // high-frequency shake) and tear down all contact forces so the
        // wheel doesn't feel weirdly heavy while the car is flying.
        if (enabled && !isOnGround)
        {
            stopContactEffects();
            if (!s_airborneOn.exchange(true, std::memory_order_acq_rel))
            {
                PlayCarAirborne();
                if (debugLog)
                    log::DebugF("[gwheel:ffb] airborne BEGIN (speed=%.2f)", absSpeedMps);
            }
            return;
        }

        // Grounded again: stop airborne effect if it was on.
        if (s_airborneOn.exchange(false, std::memory_order_acq_rel))
        {
            StopCarAirborne();
            if (debugLog)
                log::DebugF("[gwheel:ffb] airborne END (speed=%.2f)", absSpeedMps);
        }

        const bool moving = absSpeedMps > stationaryMps;

        if (!enabled || !moving)
        {
            const int prevS = s_lastCoefPct.load(std::memory_order_acquire);
            const int prevC = s_lastConstPct.load(std::memory_order_acquire);
            const int prevD = s_lastDamperPct.load(std::memory_order_acquire);
            const int prevB = s_lastBumpyPct.load(std::memory_order_acquire);
            stopContactEffects();

            if (debugLog && (prevS >= 0 || prevC != INT_MIN || prevD >= 0 || prevB >= 0))
                log::DebugF("[gwheel:ffb] centering OFF (speed=%.2f reversing=%d enabled=%d) prevS=%d%% prevC=%d%% prevD=%d%% prevB=%d%%",
                            absSpeedMps, isReversing ? 1 : 0, enabled ? 1 : 0, prevS, prevC, prevD, prevB);
            return;
        }

        // --- Speed component: v² normalized to per-car cruise ------------
        // Used by the spring (for static heaviness) and the damper (to scale
        // viscous feel with speed). Active torque uses loadFactor below,
        // which is a more physical lateral-accel proxy.
        const float cruiseSafe = std::max(1.f, cruiseMps);
        const float vRatio     = absSpeedMps / cruiseSafe;
        const float speedSq    = std::clamp(vRatio * vRatio, 0.f, 2.25f);

        // --- Yaw / grip components --------------------------------------
        // yawRatio < 1  — below the car's turnRate, full SAT
        // yawRatio == 1 — at the yaw limit (peak grip, rails)
        // yawRatio > 1  — past the limit (sliding); SAT decays exponentially
        const float yawRefSafe = std::max(0.01f, yawRef);
        const float yawMag     = std::fabs(angVelMagRad);
        const float yawRatio   = yawMag / yawRefSafe;
        const float yawRamp    = std::clamp(yawRatio, 0.f, 1.f);
        const float gripFactor = yawRatio < 1.f
                               ? 1.f
                               : std::clamp(std::exp(-2.f * (yawRatio - 1.f)), 0.f, 1.f);

        // --- Lateral-acceleration proxy for active torque ----------------
        // |yawRate × v| is proportional to real lateral acceleration
        // (m/s² of centripetal force in the tire's frame). Normalize by
        // the car's steady-state limit yawRef×cruise so 1.0 = peak grip;
        // allow 1.5 for transient overshoot (you're loading the tire
        // harder than it can sustain, about to slide).
        //
        // This naturally embeds v² (steady-state yaw scales with v, so
        // yaw×v ≈ v² × tan(steer)/wheelbase) — no separate v² term needed
        // on the active torque.
        const float loadFactor = std::clamp(
            (yawMag * absSpeedMps) / (yawRefSafe * cruiseSafe),
            0.f, 1.5f);

        // --- Fore-aft load transfer --------------------------------------
        // Braking loads the fronts (more normal force → more lateral grip
        // → more SAT). Throttle slightly unloads the fronts. Coefficients
        // are rough but capture the directional feel.
        const float weightMul = std::clamp(1.f + 0.3f * brake - 0.05f * throttle,
                                           0.5f, 1.5f);

        const float reverseMul = isReversing ? kReverseMul : 1.f;

        // --- Passive spring: stiffness modulated by speed² + yaw bonus,
        // entire sum multiplied by gripFactor so a slide/drift drops the
        // spring along with every other SAT-derived force. Real tires
        // produce little lateral force past peak slip, so the wheel
        // should go light — not heavier — during a drift.
        const float baseline  = std::clamp(centeringBaseline, 0.f, 1.f);
        float coef = (baseline * speedSq
                    + yawRamp * (static_cast<float>(yawFeedbackPct) / 100.f))
                   * gripFactor;
        coef *= reverseMul * rangeScale;
        coef = std::clamp(coef, 0.f, 1.f);

        const int coefPct = std::clamp(static_cast<int>(std::lround(coef * 100.f)), 0, 100);
        const int prevS   = s_lastCoefPct.load(std::memory_order_acquire);
        if (prevS < 0 || std::abs(coefPct - prevS) >= 5)
        {
            PlaySpring(coef);
            s_lastCoefPct.store(coefPct, std::memory_order_release);
        }

        // --- Active alignment torque: humped over deflection, driven by load
        // Shape: sqrt(|steer|) × (1 − steer⁴). Peak ~0.67 at |steer|=0.54.
        // Strong force at small deflections (sqrt), rolls off toward full
        // lock (^4 term falls fast past 60%) — tires at the slip limit
        // produce less SAT, not more.
        const float steerMag   = std::fabs(steer);
        const float signSteer  = (steer > 0.f) ? 1.f : (steer < 0.f ? -1.f : 0.f);
        const float steerSq    = steerMag * steerMag;
        const float steerShape = std::sqrt(steerMag) * (1.f - steerSq * steerSq);

        float constForce = -signSteer * steerShape * loadFactor * gripFactor * weightMul
                         * (static_cast<float>(activeTorqueStrengthPct) / 100.f);
        constForce *= reverseMul * rangeScale;

        // --- Slip-angle countersteer nudge --------------------------------
        // Past peak slip real SAT flips sign and pulls the steering wheel
        // toward the direction of travel — the "self-correcting" pull that
        // teaches drivers to countersteer instinctively. The effect scales
        // with slip angle (approximated here by lateral velocity in the
        // car's local frame) and with how much grip has been lost.
        //
        // gripFactor == 1 (grip) → no contribution.
        // gripFactor < 1 (slide) → force proportional to |lateralVel|,
        //   in the direction of lateralVel (positive = sliding rightward
        //   → wheel pulled rightward, which IS countersteer for a
        //   right-side slide; the driver's natural "follow the wheel"
        //   response yields correct opposite-lock steering).
        //
        // 6 m/s of lateral velocity saturates the contribution; cap at
        // 70% so the wheel can still be held / controlled during the
        // biggest drifts. activeTorquePct scales with the rest of the
        // active torque so user tuning affects this uniformly.
        constexpr float kLatVelSaturate = 6.0f;
        constexpr float kCountersteerGain = 0.70f;
        const float deadband   = 0.5f;  // don't trigger below slight slide
        const float slipMag    = std::max(0.f, std::fabs(lateralVelocityMps) - deadband);
        const float slipSign   = (lateralVelocityMps > 0.f) ? 1.f : -1.f;
        const float slipNorm   = std::clamp(slipMag / kLatVelSaturate, 0.f, 1.f);
        const float counterForce = slipSign * slipNorm * (1.f - gripFactor)
                                 * kCountersteerGain
                                 * (static_cast<float>(activeTorqueStrengthPct) / 100.f)
                                 * reverseMul * rangeScale;
        constForce += counterForce;

        // Jolt overlay. A collision / bump event queues a linearly-decaying
        // kick; overlay it onto constForce for the jolt's duration. Only
        // applies while driving (this branch only runs when moving + FFB on),
        // so paused menus / airborne flight don't play phantom jolts.
        {
            const uint64_t jStart = s_joltStartMs.load(std::memory_order_acquire);
            const uint32_t jDur   = s_joltDurationMs.load(std::memory_order_acquire);
            if (jDur > 0)
            {
                const uint64_t now = GetTickCount64();
                if (now >= jStart && now - jStart < jDur)
                {
                    const float t = static_cast<float>(now - jStart)
                                  / static_cast<float>(jDur);
                    const float envelope = 1.f - t; // linear decay 1 → 0
                    constForce += s_joltForce.load(std::memory_order_acquire)
                                * envelope * rangeScale;
                }
                else
                {
                    // Expired — clear so we stop checking.
                    s_joltDurationMs.store(0, std::memory_order_release);
                }
            }
        }

        constForce = std::clamp(constForce, -1.f, 1.f);

        const int constPct = std::clamp(static_cast<int>(std::lround(constForce * 100.f)), -100, 100);
        const int prevC    = s_lastConstPct.load(std::memory_order_acquire);
        if (prevC == INT_MIN || std::abs(constPct - prevC) >= 5)
        {
            if (constPct == 0 && prevC != INT_MIN)
            {
                StopConstant();
            }
            else
            {
                PlayConstant(constForce);
            }
            s_lastConstPct.store(constPct, std::memory_order_release);
        }

        // --- Damper: viscous resistance scaling with speed² -------------
        // Real steering has friction + tire/rack damping that grows with
        // load. At rest we want zero damper (wheel turns freely); at cruise
        // we want enough to kill return-to-center oscillation and give the
        // wheel "weight" in the driver's hands.
        // Damper also respects gripFactor — rack + tire damping comes from
        // loaded tires; in a drift the tires aren't loaded, the wheel
        // should flick easily against countersteer.
        const float damperCoef = std::clamp(speedSq * 0.4f * gripFactor * reverseMul * rangeScale,
                                            0.f, 0.5f);
        const int damperPct    = std::clamp(static_cast<int>(std::lround(damperCoef * 100.f)), 0, 100);
        const int prevD        = s_lastDamperPct.load(std::memory_order_acquire);
        if (prevD < 0 || std::abs(damperPct - prevD) >= 5)
        {
            if (damperPct == 0 && prevD > 0)
            {
                StopDamper();
            }
            else
            {
                PlayDamper(damperCoef);
            }
            s_lastDamperPct.store(damperPct, std::memory_order_release);
        }

        // --- Road surface: driven by suspension activity ----------------
        // Real controller rumble on CP2077 reacts to what's under each
        // wheel — smooth asphalt is silent, cobbles / offroad / speedbumps
        // rumble. We approximate that by watching the vehicle's angular-
        // velocity derivative on the roll+pitch axes (computed by the
        // caller from the per-tick Δω read). Yaw is excluded — that's
        // dominated by steering input, not the road.
        //
        // Envelope: fast attack, slow decay. A pothole spike opens the
        // valve; the vibration trails out over ~400 ms rather than
        // cutting hard. Gate below a small threshold so gentle cruising
        // over perfect tarmac stays silent.
        //
        // Period is 180 ms (SINE @ ~5.5 Hz) — we're modulating amplitude,
        // not frequency. Per-surface variants (dirt, gravel, ice) would
        // modulate the period and are a future pass.
        // Tuning values chosen from 2026-04-23 driving log (1916 samples):
        //   p50=0.023 p75=0.095 p90=0.232 p95=0.337 p99=0.672 max=11.8
        //
        // Envelope shape is deliberately asymmetric: SLOW ATTACK, FAST DECAY.
        // A single transient spike (curb, pothole, steering jerk) barely
        // moves the envelope, so curbs play as a clean directional jolt
        // without a trailing SINE. SUSTAINED activity (driving on gravel,
        // dirt, rough offroad) accumulates over many ticks into a steady
        // background hum. This matches the intuitive separation:
        //   transient events → jolt (short, directional)
        //   continuous terrain → surface hum (low-frequency amplitude)
        //
        // Gate at 0.10 filters cruising noise. Gain 4.0 is needed to reach
        // saturation during sustained activity (since the attack is so
        // small each tick). Decay 0.85 → 10% remaining after ~14 ticks
        // (~55 ms), enough to kill transients cleanly.
        constexpr int   kSurfacePeriodMs = 180;
        constexpr float kActivityGate    = 0.10f; // rad/s change per tick
        constexpr float kActivityGain    = 4.00f; // maps raw Δω to envelope input
        constexpr float kAttackCoef      = 0.08f; // LPF rise coef — slow; requires sustained activity
        constexpr float kDecayCoef       = 0.85f; // LPF fall coef — fast; transient spikes die quickly
        constexpr float kSurfacePeakMag  = 0.50f; // hard ceiling on magnitude

        const float rawActivity = std::max(0.f, suspensionActivity - kActivityGate);
        const float target      = std::clamp(rawActivity * kActivityGain, 0.f, kSurfacePeakMag);
        float env = s_surfaceEnvelope.load(std::memory_order_acquire);
        env = (target > env)
                ? env + (target - env) * kAttackCoef
                : env * kDecayCoef;
        s_surfaceEnvelope.store(env, std::memory_order_release);

        // Blend in the surface-CName-driven baseline. Max() rather than
        // add: on textured surfaces the baseline sets a floor so the
        // wheel always hums; transient suspension spikes still poke
        // above it when they arrive. Asphalt/concrete have baseline=0
        // so this is a no-op on pavement.
        const float surfBaseline = s_surfaceBaselineMag.load(std::memory_order_relaxed);
        const float combined     = std::max(env, surfBaseline);
        const float bumpyMag     = std::clamp(combined * rangeScale, 0.f, kSurfacePeakMag);
        const int bumpyPct   = std::clamp(static_cast<int>(std::lround(bumpyMag * 100.f)), 0, 100);
        const int prevB      = s_lastBumpyPct.load(std::memory_order_acquire);
        if (prevB < 0 || std::abs(bumpyPct - prevB) >= 2)
        {
            if (bumpyPct == 0 && prevB > 0)
            {
                StopRoadSurface();
            }
            else if (bumpyPct > 0)
            {
                PlayRoadSurface(bumpyMag, kSurfacePeriodMs);
            }
            s_lastBumpyPct.store(bumpyPct, std::memory_order_release);
        }

        static std::atomic<uint64_t> s_sampleTicks{0};
        const uint64_t sampleN = s_sampleTicks.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool heartbeat = (sampleN % 120) == 0; // ~2 Hz at 250 Hz pump ≈ every 480ms
        if (debugLog && (heartbeat
                      || prevS < 0 || std::abs(coefPct - prevS) >= 5
                      || prevC == INT_MIN || std::abs(constPct - prevC) >= 5
                      || prevD < 0 || std::abs(damperPct - prevD) >= 5
                      || prevB < 0 || std::abs(bumpyPct - prevB) >= 5))
        {
            log::DebugF("[gwheel:ffb] PUSH speed=%.2f yaw=%.2f susp=%.3f latV=%+.2f steer=%+.2f br=%.2f th=%.2f rev=%d "
                        "cruise=%.1f base=%.2f vSq=%.2f yRatio=%.2f grip=%.2f load=%.2f weight=%.2f "
                        "spring=%d%% active=%+d%% damper=%d%% bumpy=%d%% env=%.3f ctr=%+.2f",
                        absSpeedMps, angVelMagRad, suspensionActivity, lateralVelocityMps,
                        steer, brake, throttle, isReversing ? 1 : 0,
                        cruiseMps, centeringBaseline, speedSq, yawRatio, gripFactor, loadFactor, weightMul,
                        coefPct, constPct, damperPct, bumpyPct, env, counterForce);
        }
    }
}
