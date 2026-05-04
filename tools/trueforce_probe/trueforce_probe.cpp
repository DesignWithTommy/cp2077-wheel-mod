// Standalone TrueForce SDK probe.
//
// Loads C:\Program Files\Logi\Trueforce\<latest>\trueforce_sdk_x64.dll,
// calls the same minimal sequence the in-game backend does, and prints
// every observable: dllOpen result, Available() value, Supported(0),
// IsPaused(0)/Resume(0) round-trip, GainKF/GainTF round-trip,
// GetMaxPeakTorqueKF, GetOperatingRangeDegrees.
//
// Purpose: distinguish "the SDK is broken everywhere" vs "the SDK rejects
// our particular CP2077 process." If this tool gets Available()=true while
// the in-game integration gets Available()=false, the issue is a server-
// side process whitelist (lghub_agent.exe gating TrueForce activation
// behind partner-status). If this tool also gets Available()=false, the
// SDK has some other prerequisite we're missing on this whole machine.

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// ---- Function pointer typedefs (best-guess; mirrors trueforce_api.h) -------
using PFN_dllOpen                            = bool   (__cdecl*)();
using PFN_dllClose                           = bool   (__cdecl*)();
using PFN_logiTrueForceAvailable             = bool   (__cdecl*)();
using PFN_logiTrueForceSupported             = bool   (__cdecl*)(int);
using PFN_logiTrueForceIsPaused              = bool   (__cdecl*)(int);
using PFN_logiTrueForceResume                = bool   (__cdecl*)(int);
using PFN_logiTrueForcePause                 = bool   (__cdecl*)(int);
using PFN_logiTrueForceSync                  = bool   (__cdecl*)(int);
using PFN_logiTrueForceSetGainKF             = bool   (__cdecl*)(int, double);
using PFN_logiTrueForceSetGainTF             = bool   (__cdecl*)(int, double);
using PFN_logiTrueForceGetMaxPeakTorqueKF    = bool   (__cdecl*)(int, double*);
using PFN_logiTrueForceGetMaxContinuousTorqueKF
                                              = bool   (__cdecl*)(int, double*);
using PFN_logiTrueForceSetTorqueKF           = bool   (__cdecl*)(int, double);
using PFN_logiTrueForceClearKF               = bool   (__cdecl*)(int);
using PFN_logiTrueForceGetHapticRate         = bool   (__cdecl*)(int, int*);
using PFN_logiTrueForceGetAngleDegrees       = bool   (__cdecl*)(int, double*);
using PFN_logiWheelGetOperatingRangeDegrees  = bool   (__cdecl*)(int, double*);

template <typename T>
T resolve(HMODULE m, const char* name)
{
    T fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    std::printf("  %-44s %s\n", name, fn ? "OK" : "MISSING");
    return fn;
}

// SEH-protect raw call invocations so a wrong signature doesn't kill the
// whole probe (SEH cannot live in functions with C++ object cleanup, so
// each variant gets its own free function).
struct R { bool survived; bool result; DWORD code; };
struct RD { bool survived; double value; bool result; DWORD code; };

R seh_call_b(void* fn) {
    R r{false, false, 0};
    if (!fn) return r;
    using F = bool(__cdecl*)();
    __try { r.result = reinterpret_cast<F>(fn)(); r.survived = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { r.code = GetExceptionCode(); }
    return r;
}
R seh_call_b_i(void* fn, int a) {
    R r{false, false, 0};
    if (!fn) return r;
    using F = bool(__cdecl*)(int);
    __try { r.result = reinterpret_cast<F>(fn)(a); r.survived = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { r.code = GetExceptionCode(); }
    return r;
}
R seh_call_b_id(void* fn, int a, double v) {
    R r{false, false, 0};
    if (!fn) return r;
    using F = bool(__cdecl*)(int, double);
    __try { r.result = reinterpret_cast<F>(fn)(a, v); r.survived = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { r.code = GetExceptionCode(); }
    return r;
}
RD seh_call_b_idp(void* fn, int a) {
    RD r{false, 0.0, false, 0};
    if (!fn) return r;
    using F = bool(__cdecl*)(int, double*);
    __try { r.result = reinterpret_cast<F>(fn)(a, &r.value); r.survived = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { r.code = GetExceptionCode(); }
    return r;
}

void print_b(const char* label, R r) {
    if (!r.survived)
        std::printf("  %-44s CRASHED code=0x%08lX\n", label, r.code);
    else
        std::printf("  %-44s %s\n", label, r.result ? "true" : "false");
}
void print_bd(const char* label, RD r) {
    if (!r.survived)
        std::printf("  %-44s CRASHED code=0x%08lX\n", label, r.code);
    else
        std::printf("  %-44s %s value=%.4f\n", label,
                    r.result ? "OK" : "FAIL", r.value);
}

std::wstring find_dll()
{
    for (const wchar_t* base : {
            L"C:\\Program Files\\Logi\\Trueforce",
            L"C:\\Program Files (x86)\\Logi\\Trueforce" })
    {
        std::error_code ec;
        if (!fs::exists(base, ec)) continue;
        for (auto& d : fs::directory_iterator(base, ec))
        {
            if (!d.is_directory(ec)) continue;
            auto candidate = d.path() / L"trueforce_sdk_x64.dll";
            if (fs::exists(candidate, ec))
                return candidate.wstring();
        }
    }
    return {};
}

int wmain()
{
    std::printf("=== TrueForce standalone probe ===\n");

    auto dll = find_dll();
    if (dll.empty()) { std::printf("FAIL: trueforce_sdk_x64.dll not found.\n"); return 1; }
    char dllUtf8[1024]{};
    WideCharToMultiByte(CP_UTF8, 0, dll.c_str(), -1, dllUtf8, sizeof(dllUtf8), nullptr, nullptr);
    std::printf("DLL: %s\n", dllUtf8);

    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) { std::printf("FAIL: LoadLibrary failed (GetLastError=%lu)\n", GetLastError()); return 1; }
    std::printf("LoadLibrary OK (HMODULE=%p)\n\n", m);

    std::printf("--- Resolving exports ---\n");
    auto pDllOpen        = resolve<PFN_dllOpen>                  (m, "dllOpen");
    auto pDllClose       = resolve<PFN_dllClose>                 (m, "dllClose");
    auto pAvailable      = resolve<PFN_logiTrueForceAvailable>   (m, "logiTrueForceAvailable");
    auto pSupported      = resolve<PFN_logiTrueForceSupported>   (m, "logiTrueForceSupported");
    auto pIsPaused       = resolve<PFN_logiTrueForceIsPaused>    (m, "logiTrueForceIsPaused");
    auto pResume         = resolve<PFN_logiTrueForceResume>      (m, "logiTrueForceResume");
    auto pSync           = resolve<PFN_logiTrueForceSync>        (m, "logiTrueForceSync");
    auto pSetGainKF      = resolve<PFN_logiTrueForceSetGainKF>   (m, "logiTrueForceSetGainKF");
    auto pSetGainTF      = resolve<PFN_logiTrueForceSetGainTF>   (m, "logiTrueForceSetGainTF");
    auto pGetMaxPeak     = resolve<PFN_logiTrueForceGetMaxPeakTorqueKF>(m, "logiTrueForceGetMaxPeakTorqueKF");
    auto pSetTorqueKF    = resolve<PFN_logiTrueForceSetTorqueKF> (m, "logiTrueForceSetTorqueKF");
    auto pGetRange       = resolve<PFN_logiWheelGetOperatingRangeDegrees>(m, "logiWheelGetOperatingRangeDegrees");
    auto pGetAngle       = resolve<PFN_logiTrueForceGetAngleDegrees>(m, "logiTrueForceGetAngleDegrees");

    std::printf("\n--- Calling dllOpen ---\n");
    auto r = seh_call_b(reinterpret_cast<void*>(pDllOpen));
    print_b("dllOpen()", r);
    std::printf("\n--- Polling Available() with 200ms steps for 5s ---\n");
    for (int i = 0; i < 25; ++i)
    {
        Sleep(200);
        auto av = seh_call_b(reinterpret_cast<void*>(pAvailable));
        std::printf("  t=%4d ms: Available() = %s%s\n",
                    (i + 1) * 200,
                    av.survived ? (av.result ? "true" : "false") : "CRASHED",
                    !av.survived ? "" : (av.result ? "  <-- CONNECTED!" : ""));
        if (av.survived && av.result) break;
    }

    std::printf("\n--- Probe state with wheel index 0 ---\n");
    print_b ("logiTrueForceAvailable()",            seh_call_b(reinterpret_cast<void*>(pAvailable)));
    print_b ("logiTrueForceSupported(0)",           seh_call_b_i(reinterpret_cast<void*>(pSupported), 0));
    print_b ("logiTrueForceIsPaused(0)",            seh_call_b_i(reinterpret_cast<void*>(pIsPaused), 0));
    print_b ("logiTrueForceResume(0)",              seh_call_b_i(reinterpret_cast<void*>(pResume), 0));
    print_b ("logiTrueForceIsPaused(0) post-Resume",seh_call_b_i(reinterpret_cast<void*>(pIsPaused), 0));
    print_b ("logiTrueForceSync(0)",                seh_call_b_i(reinterpret_cast<void*>(pSync), 0));
    print_b ("logiTrueForceSetGainKF(0, 1.0)",      seh_call_b_id(reinterpret_cast<void*>(pSetGainKF), 0, 1.0));
    print_b ("logiTrueForceSetGainTF(0, 1.0)",      seh_call_b_id(reinterpret_cast<void*>(pSetGainTF), 0, 1.0));
    print_bd("logiTrueForceGetMaxPeakTorqueKF(0)",  seh_call_b_idp(reinterpret_cast<void*>(pGetMaxPeak), 0));
    print_bd("logiWheelGetOperatingRangeDegrees(0)",seh_call_b_idp(reinterpret_cast<void*>(pGetRange), 0));
    print_bd("logiTrueForceGetAngleDegrees(0)",     seh_call_b_idp(reinterpret_cast<void*>(pGetAngle), 0));

    std::printf("\n--- Attempting an actual force write ---\n");
    print_b ("logiTrueForceSetTorqueKF(0, 1.0)",    seh_call_b_id(reinterpret_cast<void*>(pSetTorqueKF), 0, 1.0));
    Sleep(500);
    print_b ("logiTrueForceSetTorqueKF(0, 0.0)",    seh_call_b_id(reinterpret_cast<void*>(pSetTorqueKF), 0, 0.0));

    std::printf("\n--- dllClose ---\n");
    if (pDllClose) print_b("dllClose()", seh_call_b(reinterpret_cast<void*>(pDllClose)));

    FreeLibrary(m);
    std::printf("\nDone.\n");
    return 0;
}
