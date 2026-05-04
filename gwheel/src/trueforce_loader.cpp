#include "trueforce_loader.h"
#include "config.h"
#include "logging.h"
#include "trueforce_api.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace gwheel::trueforce
{
    namespace
    {
        constexpr wchar_t kDllName[] = L"trueforce_sdk_x64.dll";

        struct State
        {
            std::atomic<bool> active{false};
            HMODULE           hMod = nullptr;
            api::Vtable       vt;
            std::wstring      resolvedPath;
        };

        State& S()
        {
            static State s;
            return s;
        }

        // Read a string value from HKLM\<subkey>\<name>. Returns empty on
        // any failure; caller decides what to do with that.
        std::wstring ReadHklmString(const wchar_t* subkey, const wchar_t* name)
        {
            HKEY hKey = nullptr;
            // KEY_WOW64_64KEY: G HUB writes its install location under the
            // 64-bit hive even though the registry redirector might otherwise
            // route us elsewhere when this DLL is loaded into a 64-bit host.
            LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0,
                                    KEY_READ | KEY_WOW64_64KEY, &hKey);
            if (rc != ERROR_SUCCESS) return {};

            DWORD type = 0;
            DWORD cb = 0;
            rc = RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &cb);
            if (rc != ERROR_SUCCESS || type != REG_SZ || cb == 0)
            {
                RegCloseKey(hKey);
                return {};
            }

            std::wstring out(cb / sizeof(wchar_t), L'\0');
            rc = RegQueryValueExW(hKey, name, nullptr, &type,
                                  reinterpret_cast<LPBYTE>(out.data()), &cb);
            RegCloseKey(hKey);
            if (rc != ERROR_SUCCESS) return {};

            // Trim trailing NULs that the registry tends to leave.
            while (!out.empty() && out.back() == L'\0') out.pop_back();
            return out;
        }

        std::wstring ExpandEnv(const wchar_t* var)
        {
            wchar_t buf[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) return {};
            return std::wstring(buf, n);
        }

        std::string Utf8(const std::wstring& w)
        {
            if (w.empty()) return {};
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                                        nullptr, 0, nullptr, nullptr);
            if (n <= 1) return {};
            std::string out(static_cast<size_t>(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                                out.data(), n, nullptr, nullptr);
            return out;
        }

        // Walk one level under a Logi\Trueforce install root for the
        // version-named subdirectory holding the DLL. Modern G HUB lays
        // these out as e.g. `Logi\Trueforce\1_3_11\trueforce_sdk_x64.dll`
        // — version subfolder is underscore-separated and changes with
        // every TrueForce SDK push. If multiple versions are installed
        // (uncommon), descending lexicographic order picks the highest
        // for the typical `MAJOR_MINOR_PATCH` shape.
        std::wstring FindInVersionedRoot(const std::wstring& root)
        {
            if (root.empty()) return {};
            std::error_code ec;
            std::filesystem::path base(root);
            if (!std::filesystem::exists(base, ec)) return {};

            std::vector<std::filesystem::path> dirs;
            for (auto& entry : std::filesystem::directory_iterator(base, ec))
            {
                if (ec) break;
                if (entry.is_directory(ec)) dirs.push_back(entry.path());
            }
            // Highest version first.
            std::sort(dirs.begin(), dirs.end(),
                      [](const auto& a, const auto& b) { return a > b; });

            for (const auto& dir : dirs)
            {
                auto candidate = dir / kDllName;
                if (std::filesystem::exists(candidate, ec))
                    return candidate.wstring();
            }
            return {};
        }

        // Build the candidate-path list in priority order. First hit wins.
        //
        // Modern G HUB (verified on 2026-05-03 against trueforce_sdk
        // extension v1.3.11) deploys the DLL via trueforce_manager.exe
        // to `%ProgramFiles%\Logi\Trueforce\<version>\` per the manifest
        // at ProgramData\LGHUB\depots\<id>\trueforce_sdk\manifest.json.
        // Older fallback paths cover G HUB layouts that bundled the DLL
        // directly under the LGHUB install dir.
        std::vector<std::wstring> BuildCandidatePaths()
        {
            std::vector<std::wstring> paths;

            // 1) Canonical Logi\Trueforce install (current G HUB).
            //    Walk the versioned subdirectory tree.
            for (auto base : {
                     L"C:\\Program Files\\Logi\\Trueforce",
                     L"C:\\Program Files (x86)\\Logi\\Trueforce",
                 })
            {
                if (auto hit = FindInVersionedRoot(base); !hit.empty())
                {
                    paths.push_back(hit);
                    break;
                }
            }
            // %ProgramFiles%-relative variant in case the install is on
            // a non-C: drive.
            if (auto pf = ExpandEnv(L"ProgramFiles"); !pf.empty())
            {
                if (auto hit = FindInVersionedRoot(pf + L"\\Logi\\Trueforce");
                    !hit.empty())
                    paths.push_back(hit);
            }
            if (auto pf86 = ExpandEnv(L"ProgramFiles(x86)"); !pf86.empty())
            {
                if (auto hit = FindInVersionedRoot(pf86 + L"\\Logi\\Trueforce");
                    !hit.empty())
                    paths.push_back(hit);
            }

            // 2) Legacy LGHUB install path (older G HUB releases that
            //    shipped the DLL directly under the LGHUB folder).
            for (auto subkey : {
                     L"SOFTWARE\\Logitech\\LGHUB",
                     L"SOFTWARE\\WOW6432Node\\Logitech\\LGHUB",
                 })
            {
                auto v = ReadHklmString(subkey, L"InstallLocation");
                if (v.empty())
                    v = ReadHklmString(subkey, L"InstallDir");
                if (!v.empty())
                {
                    if (v.back() != L'\\' && v.back() != L'/') v.push_back(L'\\');
                    paths.push_back(v + kDllName);
                }
            }
            for (auto base : {
                     L"C:\\Program Files\\LGHUB\\",
                     L"C:\\Program Files (x86)\\LGHUB\\",
                 })
            {
                paths.emplace_back(std::wstring(base) + kDllName);
            }

            return paths;
        }

        // Bind every export the wheel backend will use. Names are
        // confirmed against the v1.3.11 export dump; signatures are
        // best-guess until we have official headers (mismatched
        // signatures will manifest as crashes the first time a given
        // export is called).
        bool ResolveExports(HMODULE hMod, api::Vtable& vt)
        {
            int missing = 0;
            auto bind = [&](auto& slot, const char* name)
            {
                using T = std::remove_reference_t<decltype(slot)>;
                slot = reinterpret_cast<T>(
                    reinterpret_cast<void*>(GetProcAddress(hMod, name)));
                if (!slot)
                {
                    ++missing;
                    log::DebugF("[gwheel] trueforce: export '%s' not found", name);
                }
            };

            // Lifecycle
            bind(vt.dllOpen,                     "dllOpen");
            bind(vt.dllClose,                    "dllClose");
            bind(vt.logiWheelOpenByDirectInputW, "logiWheelOpenByDirectInputW");
            bind(vt.logiWheelClose,              "logiWheelClose");
            bind(vt.logiWheelSdkHasControl,      "logiWheelSdkHasControl");
            bind(vt.logiWheelGetVersion,         "logiWheelGetVersion");
            bind(vt.logiWheelGetCoreLibraryVersion, "logiWheelGetCoreLibraryVersion");

            // Capability
            bind(vt.logiTrueForceAvailable,             "logiTrueForceAvailable");
            bind(vt.logiTrueForceSupported,             "logiTrueForceSupported");
            bind(vt.logiTrueForceSupportedByDirectInputW,
                 "logiTrueForceSupportedByDirectInputW");
            bind(vt.logiWheelSupportedByDirectInputW,
                 "logiWheelSupportedByDirectInputW");

            // Force mode
            bind(vt.logiWheelGetForceMode, "logiWheelGetForceMode");
            bind(vt.logiWheelSetForceMode, "logiWheelSetForceMode");

            // Range
            bind(vt.logiWheelSetOperatingRangeDegrees,
                 "logiWheelSetOperatingRangeDegrees");
            bind(vt.logiWheelGetOperatingRangeDegrees,
                 "logiWheelGetOperatingRangeDegrees");

            // Wheel state
            bind(vt.logiTrueForceGetAngleDegrees,
                 "logiTrueForceGetAngleDegrees");
            bind(vt.logiTrueForceGetAngularVelocityDegrees,
                 "logiTrueForceGetAngularVelocityDegrees");

            // Pause/sync
            bind(vt.logiTrueForcePause,         "logiTrueForcePause");
            bind(vt.logiTrueForceResume,        "logiTrueForceResume");
            bind(vt.logiTrueForceIsPaused,      "logiTrueForceIsPaused");
            bind(vt.logiTrueForceSync,          "logiTrueForceSync");
            bind(vt.logiTrueForceGetHapticRate, "logiTrueForceGetHapticRate");

            // KF
            bind(vt.logiTrueForceSetTorqueKF,             "logiTrueForceSetTorqueKF");
            bind(vt.logiTrueForceClearKF,                 "logiTrueForceClearKF");
            bind(vt.logiTrueForceSetGainKF,               "logiTrueForceSetGainKF");
            bind(vt.logiTrueForceGetMaxPeakTorqueKF,      "logiTrueForceGetMaxPeakTorqueKF");
            bind(vt.logiTrueForceGetMaxContinuousTorqueKF, "logiTrueForceGetMaxContinuousTorqueKF");
            bind(vt.logiTrueForceSetTorqueKFPiecewise,    "logiTrueForceSetTorqueKFPiecewise");

            // TF
            bind(vt.logiTrueForceSetTorqueTFfloat,  "logiTrueForceSetTorqueTFfloat");
            bind(vt.logiTrueForceSetTorqueTFdouble, "logiTrueForceSetTorqueTFdouble");
            bind(vt.logiTrueForceSetTorqueTFint16,  "logiTrueForceSetTorqueTFint16");
            bind(vt.logiTrueForceSetStreamTF,       "logiTrueForceSetStreamTF");
            bind(vt.logiTrueForceClearTF,           "logiTrueForceClearTF");
            bind(vt.logiTrueForceSetGainTF,         "logiTrueForceSetGainTF");
            bind(vt.logiTrueForceGetTorqueTFRateBounds, "logiTrueForceGetTorqueTFRateBounds");

            // Damping
            bind(vt.logiTrueForceSetDamping,    "logiTrueForceSetDamping");
            bind(vt.logiTrueForceGetDamping,    "logiTrueForceGetDamping");
            bind(vt.logiTrueForceGetDampingMax, "logiTrueForceGetDampingMax");

            // LEDs
            bind(vt.logiWheelPlayLeds,   "logiWheelPlayLeds");
            bind(vt.logiWheelSetRpmLeds, "logiWheelSetRpmLeds");

            log::InfoF("[gwheel] trueforce: bound exports (missing=%d)", missing);

            // Minimum required for backend selection to succeed: we must
            // be able to open the wheel, set torque on KF, set TF, and
            // probe support. Other exports are nice-to-haves.
            return vt.dllOpen
                && vt.logiWheelOpenByDirectInputW
                && vt.logiWheelClose
                && vt.logiTrueForceSupportedByDirectInputW
                && vt.logiTrueForceSetTorqueKF
                && vt.logiTrueForceSetTorqueTFfloat;
        }

        // ====================================================================
        // SEH probe helpers
        //
        // Wrap each potentially-fatal SDK call in __try/__except so an access
        // violation (signature mismatch, ABI mismatch, etc.) produces a logged
        // failure instead of taking the whole game down. Each helper lives in
        // its own free function — MSVC SEH cannot coexist with C++ object
        // destructors in the same function body.
        //
        // The probe battery in DiagnosticProbe() exercises only the no-arg /
        // int-arg exports (no DInput pointers) so those should never crash.
        // If they DO crash, the DLL is fundamentally incompatible with our
        // call shape and there's no point trying the ByDirectInput variants.
        // ====================================================================

        struct ProbeResult
        {
            bool  survived       = false;
            DWORD exceptionCode  = 0;
        };

        ProbeResult Seh_TFAvailable(const api::Vtable& vt)
        {
            ProbeResult r{};
            if (!vt.logiTrueForceAvailable) { r.survived = false; return r; }
            __try
            {
                const bool ok = vt.logiTrueForceAvailable();
                log::InfoF("[gwheel] TF.probe: logiTrueForceAvailable() = %s",
                           ok ? "true" : "false");
                r.survived = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                r.exceptionCode = GetExceptionCode();
            }
            return r;
        }

        ProbeResult Seh_TFSupportedByIndex(const api::Vtable& vt, int idx)
        {
            ProbeResult r{};
            if (!vt.logiTrueForceSupported) { r.survived = false; return r; }
            __try
            {
                const bool ok = vt.logiTrueForceSupported(idx);
                log::InfoF("[gwheel] TF.probe: logiTrueForceSupported(%d) = %s",
                           idx, ok ? "true" : "false");
                r.survived = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                r.exceptionCode = GetExceptionCode();
            }
            return r;
        }

        ProbeResult Seh_WheelGetVersion(const api::Vtable& vt)
        {
            ProbeResult r{};
            if (!vt.logiWheelGetVersion) { r.survived = false; return r; }
            __try
            {
                const int v = vt.logiWheelGetVersion();
                log::InfoF("[gwheel] TF.probe: logiWheelGetVersion() = %d", v);
                r.survived = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                r.exceptionCode = GetExceptionCode();
            }
            return r;
        }

        ProbeResult Seh_CoreLibVersion(const api::Vtable& vt)
        {
            ProbeResult r{};
            if (!vt.logiWheelGetCoreLibraryVersion) { r.survived = false; return r; }
            __try
            {
                const int v = vt.logiWheelGetCoreLibraryVersion();
                log::InfoF("[gwheel] TF.probe: logiWheelGetCoreLibraryVersion() = %d", v);
                r.survived = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                r.exceptionCode = GetExceptionCode();
            }
            return r;
        }

        // Plain SEH wrapper for a no-arg bool function. Used to call
        // dllOpen variants and Available() without C++ object-unwinding
        // restrictions in the calling function.
        struct OpenProbeResult { bool survived; bool result; DWORD code; };
        OpenProbeResult Seh_CallNoArg(void* fn)
        {
            OpenProbeResult r{false, false, 0};
            if (!fn) return r;
            using PFN = bool (__cdecl*)();
            __try { r.result = reinterpret_cast<PFN>(fn)(); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.code = GetExceptionCode(); }
            return r;
        }

        void RunDiagnosticProbe(const api::Vtable& vt)
        {
            log::Info("[gwheel] TF.probe: running diagnostic battery (SEH-protected, no DInput pointers)");

            auto report = [](const char* name, ProbeResult r) {
                if (r.survived)
                    log::InfoF("[gwheel] TF.probe:   %-40s SURVIVED", name);
                else
                    log::WarnF("[gwheel] TF.probe:   %-40s CRASHED (code=0x%08lX)",
                               name, r.exceptionCode);
            };

            // Probe battery is now narrowed to the calls we KNOW survive
            // (run 2026-05-04 found logiTrueForceAvailable + Supported(0)
            // safe; Supported(1), logiWheelGetVersion(), and
            // logiWheelGetCoreLibraryVersion all AV with signature TBD).
            // Calling the crashing variants again would just corrupt SDK
            // state for no diagnostic gain.
            report("logiTrueForceAvailable()",        Seh_TFAvailable(vt));
            report("logiTrueForceSupported(0)",       Seh_TFSupportedByIndex(vt, 0));

            log::Info("[gwheel] TF.probe: battery complete");
        }
    }

    void Initialize()
    {
        auto& st = S();
        if (st.active.load(std::memory_order_acquire))
            return;

        const auto cfg = config::Current();
        if (!cfg.trueForce.enabled)
        {
            log::Info("[gwheel] trueforce: disabled in config "
                      "(trueForce.enabled = false) — skipping probe");
            return;
        }

        log::Info("[gwheel] trueforce: probing G HUB installation for "
                  "trueforce_sdk_x64.dll");

        const auto candidates = BuildCandidatePaths();
        std::wstring picked;
        for (const auto& p : candidates)
        {
            std::error_code ec;
            const bool exists = std::filesystem::exists(p, ec);
            log::DebugF("[gwheel] trueforce: candidate %s -> %s",
                        Utf8(p).c_str(),
                        exists ? "exists" : "missing");
            if (exists) { picked = p; break; }
        }

        if (picked.empty())
        {
            log::Warn("[gwheel] trueforce: DLL not found in any known G HUB "
                      "location — feature disabled. Install or repair G HUB, "
                      "or set trueForce.enabled=false in config.json to "
                      "silence this probe.");
            return;
        }

        st.hMod = LoadLibraryW(picked.c_str());
        if (!st.hMod)
        {
            log::WarnF("[gwheel] trueforce: LoadLibrary failed for %s "
                       "(GetLastError=%lu) — feature disabled",
                       Utf8(picked).c_str(), GetLastError());
            return;
        }

        st.resolvedPath = picked;
        log::InfoF("[gwheel] trueforce: loaded DLL from %s",
                   Utf8(picked).c_str());

        if (!ResolveExports(st.hMod, st.vt))
        {
            log::Warn("[gwheel] trueforce: export resolution incomplete "
                      "(symbol names pending SDK access). DLL is loaded "
                      "but per-signal calls are no-ops.");
            return;
        }

        // ---- dllOpen variant probe ----
        //
        // 2026-05-04 finding: dllOpen() returned true, but every subsequent
        // SDK call runs in a "disconnected" state (Resume returns true but
        // IsPaused stays true; Get* returns OK but with zeroed outputs).
        // The named pipe `\\.\pipe\logi.trueforce.connect` is alive and
        // lghub_agent.exe is hosting it, so the server side is fine. Our
        // dllOpen() must not be establishing the pipe connection.
        //
        // Hypothesis: dllOpen takes argument(s) we're not passing. Probe
        // each plausible signature under SEH, picking the variant that
        // results in Available() flipping to true.
        bool dllOpenSucceeded = false;
        if (st.vt.dllOpen)
        {
            void* fnOpen = reinterpret_cast<void*>(st.vt.dllOpen);

            auto openR = Seh_CallNoArg(fnOpen);
            log::InfoF("[gwheel] trueforce: dllOpen() no-arg -> %s%s",
                       openR.survived ? "SURVIVED" : "CRASHED",
                       openR.survived ? (openR.result ? " result=true" : " result=false") : "");
            if (!openR.survived)
                log::WarnF("[gwheel] trueforce:   code=0x%08lX", openR.code);

            if (openR.survived && st.vt.logiTrueForceAvailable)
            {
                auto avR = Seh_CallNoArg(reinterpret_cast<void*>(st.vt.logiTrueForceAvailable));
                log::InfoF("[gwheel] trueforce:   Available() after no-arg dllOpen = %s",
                           avR.survived ? (avR.result ? "true" : "false") : "CRASHED");
                if (avR.survived && avR.result) dllOpenSucceeded = true;
            }
        }

        // 2026-05-04 forensic finding: dllOpen returns true immediately,
        // but LocalClientImpl::start() inside it posts a named-pipe
        // connection attempt to \\.\pipe\logi.trueforce.connect that
        // completes asynchronously on a worker thread. If Available()
        // is checked too soon, it returns false even when the connection
        // would eventually succeed. Poll Available() for up to 2 seconds
        // to give the async connect a chance.
        if (!dllOpenSucceeded && st.vt.logiTrueForceAvailable)
        {
            log::Info("[gwheel] trueforce: polling Available() for up to 2s "
                      "(async pipe connect may be in flight)");
            for (int i = 0; i < 20; ++i)
            {
                Sleep(100);
                auto avR = Seh_CallNoArg(reinterpret_cast<void*>(st.vt.logiTrueForceAvailable));
                if (avR.survived && avR.result)
                {
                    log::InfoF("[gwheel] trueforce:   Available() flipped to true after %d ms", (i + 1) * 100);
                    dllOpenSucceeded = true;
                    break;
                }
            }
            if (!dllOpenSucceeded)
                log::Info("[gwheel] trueforce:   Available() still false after 2s; pipe handshake not completing");
        }

        if (!dllOpenSucceeded)
        {
            log::Warn("[gwheel] trueforce: dllOpen returned true but Available()=false. "
                      "The SDK loaded into our process but never established a working "
                      "connection to lghub_agent.exe over the \\\\.\\pipe\\logi.trueforce.connect "
                      "named pipe. Pipe IS reachable from arbitrary processes (verified via "
                      "PowerShell). Likely cause: server-side process whitelist (lghub_agent.exe "
                      "may only accept connections from TrueForce-registered game executables) "
                      "or a handshake protocol step we cannot reproduce without official Logitech "
                      "headers. FFB will not be felt this session even though calls return true.");
        }

        st.active.store(true, std::memory_order_release);
        log::InfoF("[gwheel] trueforce: active (vtable bound, dllOpen %s)",
                   dllOpenSucceeded ? "connected" : "loaded-but-disconnected");

        // Run the SEH-protected diagnostic battery.
        RunDiagnosticProbe(st.vt);
    }

    void Shutdown()
    {
        auto& st = S();
        if (st.active.load(std::memory_order_acquire) && st.vt.dllClose)
            st.vt.dllClose();
        if (st.hMod)
        {
            FreeLibrary(st.hMod);
            st.hMod = nullptr;
        }
        st.vt = {};
        st.active.store(false, std::memory_order_release);
    }

    bool IsActive()
    {
        return S().active.load(std::memory_order_acquire);
    }

    const api::Vtable* Vtable()
    {
        auto& st = S();
        if (!st.active.load(std::memory_order_acquire)) return nullptr;
        return &st.vt;
    }
}
