#include "wheel_backend_logi_trueforce.h"
#include "logi_common.h"
#include "logging.h"
#include "device_table.h"
#include "trueforce_loader.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace gwheel::wheel_backend
{
    namespace
    {
        // ====================================================================
        // SEH probe helpers
        //
        // The W-suffixed exports (logiTrueForceSupportedByDirectInputW and
        // logiWheelOpenByDirectInputW) crash hard on first call when handed
        // an IDirectInputDevice8W*. Without official Logitech headers, the
        // actual argument shape is unknown. SEH-wrap each variant attempt
        // so one wrong guess doesn't kill the game; iterate through plausible
        // argument types until one survives.
        //
        // Variants tried for SupportedByDirectInputW:
        //   A. IDirectInputDevice8W*           (the obvious COM-pointer reading)
        //   B. const GUID* (guidInstance)      (DInput's per-device GUID)
        //   C. const GUID* (guidProduct)       (VID/PID-derived product GUID)
        //   D. const wchar_t* (productName)    (W-suffix often = wide string)
        //
        // The function pointer typedef declares parameter A; for B/C/D we
        // reinterpret_cast the function pointer to a different signature and
        // call. This is safe at the call site (function pointers are just
        // addresses), modulo whatever the SDK actually expects.
        // ====================================================================

        struct SehResult
        {
            bool  survived       = false;
            bool  result         = false;
            DWORD exceptionCode  = 0;
            int   intResult      = -1;
        };

        // Pass IDirectInputDevice8W* (literal interface pointer).
        SehResult Seh_SupportedByDInput_AsDevice(
            void* fn, IDirectInputDevice8W* dev)
        {
            using PFN = bool (__cdecl*)(IDirectInputDevice8W*);
            SehResult r{};
            __try { r.result = reinterpret_cast<PFN>(fn)(dev); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.exceptionCode = GetExceptionCode(); }
            return r;
        }

        // Pass a pointer to a GUID.
        SehResult Seh_SupportedByDInput_AsGuidPtr(void* fn, const GUID* g)
        {
            using PFN = bool (__cdecl*)(const GUID*);
            SehResult r{};
            __try { r.result = reinterpret_cast<PFN>(fn)(g); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.exceptionCode = GetExceptionCode(); }
            return r;
        }

        // Pass a wide-char string (e.g. friendly name or device path).
        SehResult Seh_SupportedByDInput_AsWideStr(void* fn, const wchar_t* s)
        {
            using PFN = bool (__cdecl*)(const wchar_t*);
            SehResult r{};
            __try { r.result = reinterpret_cast<PFN>(fn)(s); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.exceptionCode = GetExceptionCode(); }
            return r;
        }

        // SEH-wrapped open call for each candidate signature shape. Returns
        // the int wheel handle on survive (>=0 success, <0 SDK-reported error)
        // or -1 with survived=false on AV.
        SehResult Seh_OpenByDInput_AsDevice(void* fn, IDirectInputDevice8W* dev)
        {
            using PFN = int (__cdecl*)(IDirectInputDevice8W*);
            SehResult r{};
            r.intResult = -1;
            __try { r.intResult = reinterpret_cast<PFN>(fn)(dev); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.exceptionCode = GetExceptionCode(); }
            return r;
        }

        SehResult Seh_OpenByDInput_AsGuidPtr(void* fn, const GUID* g)
        {
            using PFN = int (__cdecl*)(const GUID*);
            SehResult r{};
            r.intResult = -1;
            __try { r.intResult = reinterpret_cast<PFN>(fn)(g); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.exceptionCode = GetExceptionCode(); }
            return r;
        }

        SehResult Seh_OpenByDInput_AsWideStr(void* fn, const wchar_t* s)
        {
            using PFN = int (__cdecl*)(const wchar_t*);
            SehResult r{};
            r.intResult = -1;
            __try { r.intResult = reinterpret_cast<PFN>(fn)(s); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.exceptionCode = GetExceptionCode(); }
            return r;
        }

        // SEH-wrapped no-arg → bool. Used to probe logiTrueForceAvailable().
        SehResult Seh_BoolNoArg(void* fn)
        {
            using PFN = bool (__cdecl*)();
            SehResult r{};
            if (!fn) return r;
            __try { r.result = reinterpret_cast<PFN>(fn)(); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.exceptionCode = GetExceptionCode(); }
            return r;
        }

        // Generic SEH-wrapped int-arg → bool call (e.g. logiTrueForceSupported(int)).
        SehResult Seh_BoolByInt(void* fn, int idx)
        {
            using PFN = bool (__cdecl*)(int);
            SehResult r{};
            __try { r.result = reinterpret_cast<PFN>(fn)(idx); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.exceptionCode = GetExceptionCode(); }
            return r;
        }

        // SEH-wrapped (int wheelHandle, double value) → bool. Used to probe
        // SetTorqueKF / SetDamping / SetGainKF / SetGainTF.
        SehResult Seh_BoolByIntDouble(void* fn, int idx, double value)
        {
            using PFN = bool (__cdecl*)(int, double);
            SehResult r{};
            __try { r.result = reinterpret_cast<PFN>(fn)(idx, value); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.exceptionCode = GetExceptionCode(); }
            return r;
        }

        // SEH-wrapped (int wheelHandle, int* outValue) → bool. Some Get*
        // functions follow this pattern.
        SehResult Seh_BoolByIntIntPtr(void* fn, int idx, int* outValue)
        {
            using PFN = bool (__cdecl*)(int, int*);
            SehResult r{};
            __try { r.result = reinterpret_cast<PFN>(fn)(idx, outValue); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.exceptionCode = GetExceptionCode(); }
            return r;
        }

        // SEH-wrapped (int wheelHandle, double* outValue) → bool. Used to probe
        // GetMaxPeakTorqueKF / GetDamping / GetTorqueKF.
        SehResult Seh_BoolByIntDoublePtr(void* fn, int idx, double* outValue)
        {
            using PFN = bool (__cdecl*)(int, double*);
            SehResult r{};
            __try { r.result = reinterpret_cast<PFN>(fn)(idx, outValue); r.survived = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { r.exceptionCode = GetExceptionCode(); }
            return r;
        }

        // ---- TrueForceBackend ---------------------------------------------
        //
        // Drives the TrueForce SDK directly. Owns its own IDirectInputDevice8W
        // (the legacy SDK doesn't expose a device handle, so we can't share).
        //
        // Function signatures consumed here are best-guess from the v1.3.11
        // export-table dump (see trueforce_api.h notes). Wrong-shape calls
        // will crash; the loader's strict export-binding guard means a
        // missing export turns the whole backend off cleanly via IsActive,
        // but signature errors only show up at call time. Until we have
        // licensed headers, treat any first-time-fired primitive as suspect.

        class TrueForceBackend final : public IWheelBackend
        {
        public:
            ~TrueForceBackend() override { Close(); }

            // ---- Identification --------------------------------------------

            Vendor      GetVendor() const      override { return Vendor::Logitech; }
            BackendKind GetKind() const        override { return BackendKind::LogitechTrueForce; }
            const char* GetBackendName() const override { return "Logitech (TrueForce SDK)"; }

            // ---- Lifecycle -------------------------------------------------

            bool Init() override
            {
                if (!trueforce::IsActive())
                {
                    log::Warn("[gwheel] TrueForceBackend::Init - trueforce::IsActive()=false; cannot proceed");
                    return false;
                }
                vt_ = trueforce::Vtable();
                if (!vt_)
                {
                    log::Warn("[gwheel] TrueForceBackend::Init - vtable nullptr; cannot proceed");
                    return false;
                }
                log::Info("[gwheel] TrueForceBackend::Init - vtable bound, deferring DInput open to first Pump");
                return true;
            }

            bool Open() override
            {
                // Like the legacy backend, Open is lazy: first Pump that
                // finds a usable game window opens the DInput device and
                // calls logiWheelOpenByDirectInputW.
                return true;
            }

            void Close() override
            {
                ready_.store(false, std::memory_order_release);
                if (wheelHandle_ >= 0 && vt_ && vt_->logiWheelClose)
                {
                    vt_->logiWheelClose(wheelHandle_);
                    wheelHandle_ = -1;
                }
                if (dinputDevice_)
                {
                    dinputDevice_->Unacquire();
                    dinputDevice_->Release();
                    dinputDevice_ = nullptr;
                }
                if (dinput_)
                {
                    dinput_->Release();
                    dinput_ = nullptr;
                }
            }

            bool IsOpen() const override { return ready_.load(std::memory_order_acquire); }

            // ---- Pump ------------------------------------------------------

            void Pump() override
            {
                if (!vt_) return;

                if (!ready_.load(std::memory_order_acquire))
                {
                    if (!TryOpenDevice()) return;
                }

                if (dinputDevice_)
                {
                    HRESULT hr = dinputDevice_->Poll();
                    if (FAILED(hr))
                    {
                        // Lost device — re-acquire on the next tick.
                        dinputDevice_->Acquire();
                        return;
                    }
                    DIJOYSTATE2 state{};
                    hr = dinputDevice_->GetDeviceState(sizeof(state), &state);
                    if (SUCCEEDED(hr))
                    {
                        wheel::Snapshot s;
                        s.connected = true;
                        logi_common::PopulateSnapshotFromDInput(state, s);
                        std::lock_guard lk(snapMtx_);
                        snap_ = s;
                    }
                }

                RefreshOperatingRange();
            }

            const wheel::Snapshot& CurrentSnapshot() const override
            {
                static thread_local wheel::Snapshot tl;
                std::lock_guard lk(snapMtx_);
                tl = snap_;
                return tl;
            }

            const DeviceInfo&   GetDeviceInfo() const   override
            {
                static thread_local DeviceInfo tl;
                std::lock_guard lk(infoMtx_);
                tl = info_;
                return tl;
            }

            const Capabilities& GetCapabilities() const override { return caps_; }

            // ---- FFB primitives -------------------------------------------
            //
            // KF = kinesthetic. Our normalized -1..+1 magnitude maps to
            // -maxPeakTorqueKF..+maxPeakTorqueKF Newton-meters, scaled by
            // globalGain. STILL BEST-GUESS on signature; observed crash means
            // this is where to start corrections.

            void PlayConstant(float magnitudeSigned01) override
            {
                if (!ready_.load() || !vt_ || !vt_->logiTrueForceSetTorqueKF) return;
                const double torque = std::clamp(magnitudeSigned01, -1.f, 1.f)
                                    * maxPeakTorqueNm_
                                    * std::clamp(globalGain_.load(std::memory_order_relaxed), 0.f, 1.f);
                vt_->logiTrueForceSetTorqueKF(wheelHandle_, torque);
            }

            void StopConstant() override
            {
                if (!ready_.load() || !vt_) return;
                if (vt_->logiTrueForceClearKF)
                    vt_->logiTrueForceClearKF(wheelHandle_);
                else if (vt_->logiTrueForceSetTorqueKF)
                    vt_->logiTrueForceSetTorqueKF(wheelHandle_, 0.0);
            }

            // Spring is implemented via SetTorqueKFPiecewise on TrueForce, but
            // the segment format is unknown until we have headers. Capability
            // declared FALSE so the FFB pipeline takes the BeamNG-style
            // (constForce-only) path on TrueForce, matching ffbUseSpring=false.
            // Until we validate the piecewise format, leaving these as no-ops.
            void PlaySpring(float /*coefficient01*/) override {}
            void StopSpring() override {}

            void PlayDamper(float coefficient01) override
            {
                if (!ready_.load() || !vt_ || !vt_->logiTrueForceSetDamping) return;
                const double c = std::clamp(coefficient01, 0.f, 1.f);
                vt_->logiTrueForceSetDamping(wheelHandle_, c);
            }

            void StopDamper() override
            {
                if (!ready_.load() || !vt_ || !vt_->logiTrueForceSetDamping) return;
                vt_->logiTrueForceSetDamping(wheelHandle_, 0.0);
            }

            // Periodic (LogiPlaySurfaceEffect SINE) doesn't have a direct
            // TrueForce analog. The TF channel is the upgrade path: real
            // signals will feed via TfFeedSample. Capability declared FALSE.
            void PlayPeriodic(float /*magnitude01*/, int /*periodMs*/) override {}
            void StopPeriodic() override {}

            void SetAirborne(bool airborne) override
            {
                // No native airborne shake; "airborne" semantically means
                // "wheel disconnected from road feel" — clear all KF torque
                // + damping. The pipeline is responsible for resuming
                // forces when grounded, which it does via PlayConstant etc.
                if (!ready_.load() || !vt_) return;
                if (airborne)
                {
                    if (vt_->logiTrueForceClearKF) vt_->logiTrueForceClearKF(wheelHandle_);
                    if (vt_->logiTrueForceSetDamping) vt_->logiTrueForceSetDamping(wheelHandle_, 0.0);
                }
                // off: no-op; pipeline resumes normal forces
            }

            void StopAll() override
            {
                if (!ready_.load() || !vt_) return;
                if (vt_->logiTrueForceClearKF)    vt_->logiTrueForceClearKF(wheelHandle_);
                if (vt_->logiTrueForceClearTF)    vt_->logiTrueForceClearTF(wheelHandle_);
                if (vt_->logiTrueForceSetDamping) vt_->logiTrueForceSetDamping(wheelHandle_, 0.0);
            }

            // ---- TF channel (the headline feature) ------------------------

            void TfFeedSample(float sample) override
            {
                if (!ready_.load() || !vt_ || !vt_->logiTrueForceSetTorqueTFfloat) return;
                vt_->logiTrueForceSetTorqueTFfloat(wheelHandle_, sample);
            }

            void TfClearStream() override
            {
                if (!ready_.load() || !vt_ || !vt_->logiTrueForceClearTF) return;
                vt_->logiTrueForceClearTF(wheelHandle_);
            }

            void TfSetGain(float gain01) override
            {
                if (!ready_.load() || !vt_ || !vt_->logiTrueForceSetGainTF) return;
                vt_->logiTrueForceSetGainTF(wheelHandle_, std::clamp(gain01, 0.f, 1.f));
            }

            int GetHapticRateHz() const override
            {
                if (!ready_.load() || !vt_ || !vt_->logiTrueForceGetHapticRate) return 0;
                int hz = 0;
                vt_->logiTrueForceGetHapticRate(wheelHandle_, &hz);
                return hz;
            }

            // ---- LEDs ------------------------------------------------------

            void PlayLeds(float currentRpm, float firstLedRpm, float redlineRpm) override
            {
                if (!ready_.load() || !vt_ || !vt_->logiWheelPlayLeds) return;
                if (!(redlineRpm > firstLedRpm) || redlineRpm <= 0.f)
                {
                    SetLedLevel(0.f);
                    return;
                }
                if (currentRpm < firstLedRpm)
                {
                    SetLedLevel(0.f);
                    return;
                }
                vt_->logiWheelPlayLeds(wheelHandle_, currentRpm, firstLedRpm, redlineRpm);
            }

            void ClearLeds() override
            {
                SetLedLevel(0.f);
            }

            void SetLedLevel(float level01) override
            {
                if (!ready_.load() || !vt_ || !vt_->logiWheelPlayLeds) return;
                // Same G923 idle-indicator quirk handling as the legacy
                // backend: dark-bar branch uses a strictly-below-firstLed
                // currentRpm to override the firmware default.
                if (level01 <= 0.005f)
                    vt_->logiWheelPlayLeds(wheelHandle_, 0.f, 100.f, 200.f);
                else
                    vt_->logiWheelPlayLeds(wheelHandle_, std::clamp(level01, 0.f, 1.f), 0.f, 1.f);
            }

            // ---- Configuration --------------------------------------------

            void SetOperatingRangeDeg(int degrees) override
            {
                if (!ready_.load() || !vt_ || !vt_->logiWheelSetOperatingRangeDegrees) return;
                vt_->logiWheelSetOperatingRangeDegrees(wheelHandle_,
                                                       static_cast<double>(std::clamp(degrees, 40, 1080)));
            }

            void SetGlobalGain(float gain01) override
            {
                gain01 = std::clamp(gain01, 0.f, 1.f);
                globalGain_.store(gain01, std::memory_order_relaxed);
                if (!ready_.load() || !vt_) return;
                if (vt_->logiTrueForceSetGainKF)
                    vt_->logiTrueForceSetGainKF(wheelHandle_, gain01);
                if (vt_->logiTrueForceSetGainTF)
                    vt_->logiTrueForceSetGainTF(wheelHandle_, gain01);
            }

            // ---- Readback --------------------------------------------------

            float GetWheelAngleDeg() const override
            {
                if (!ready_.load() || !vt_ || !vt_->logiTrueForceGetAngleDegrees)
                    return std::numeric_limits<float>::quiet_NaN();
                double deg = 0.0;
                if (!vt_->logiTrueForceGetAngleDegrees(wheelHandle_, &deg))
                    return std::numeric_limits<float>::quiet_NaN();
                return static_cast<float>(deg);
            }

            float GetWheelAngularVelocityDeg() const override
            {
                if (!ready_.load() || !vt_ || !vt_->logiTrueForceGetAngularVelocityDegrees)
                    return std::numeric_limits<float>::quiet_NaN();
                double dps = 0.0;
                if (!vt_->logiTrueForceGetAngularVelocityDegrees(wheelHandle_, &dps))
                    return std::numeric_limits<float>::quiet_NaN();
                return static_cast<float>(dps);
            }

            int GetOperatingRangeDeg() const override
            {
                return cachedRangeDeg_.load(std::memory_order_acquire);
            }

            bool HasFFB() const override
            {
                return ready_.load(std::memory_order_acquire);
            }

        private:
            // ---- Open: enumerate, open, register with TrueForce -----------

            struct EnumCtx
            {
                IDirectInput8W*       di;
                IDirectInputDevice8W* device;
                uint16_t              pid;
                uint16_t              vid;
                GUID                  guidInstance;
                GUID                  guidProduct;
                wchar_t               productName[256];
            };

            static BOOL CALLBACK EnumDevicesCallback(LPCDIDEVICEINSTANCEW inst, LPVOID ud)
            {
                auto* ctx = reinterpret_cast<EnumCtx*>(ud);
                const uint16_t vid = static_cast<uint16_t>(inst->guidProduct.Data1 & 0xFFFF);
                const uint16_t pid = static_cast<uint16_t>(inst->guidProduct.Data1 >> 16);
                if (vid != static_cast<uint16_t>(kLogitechVid)) return DIENUM_CONTINUE;
                if (!LookupByPid(pid)) return DIENUM_CONTINUE;

                IDirectInputDevice8W* dev = nullptr;
                if (FAILED(ctx->di->CreateDevice(inst->guidInstance, &dev, nullptr)) || !dev)
                    return DIENUM_CONTINUE;

                ctx->device       = dev;
                ctx->vid          = vid;
                ctx->pid          = pid;
                ctx->guidInstance = inst->guidInstance;
                ctx->guidProduct  = inst->guidProduct;
                std::wcsncpy(ctx->productName, inst->tszProductName,
                             sizeof(ctx->productName) / sizeof(wchar_t) - 1);
                return DIENUM_STOP;
            }

            bool TryOpenDevice()
            {
                if (permanentFailure_.load(std::memory_order_acquire))
                    return false;

                HWND hwnd = logi_common::FindOwnGameWindow();
                const uint32_t n = openAttempts_.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool verbose = (n == 1) || (n % 1250 == 0);

                if (!hwnd)
                {
                    if (verbose)
                        log::Info("[gwheel] TrueForceBackend: waiting for game window");
                    return false;
                }

                log::Info("[gwheel] TFBE.open[1] game window acquired; creating DInput");

                if (!dinput_)
                {
                    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr),
                                                  DIRECTINPUT_VERSION,
                                                  IID_IDirectInput8W,
                                                  reinterpret_cast<LPVOID*>(&dinput_),
                                                  nullptr))
                        || !dinput_)
                    {
                        log::Warn("[gwheel] TFBE.open[1.x] DirectInput8Create failed");
                        return false;
                    }
                }

                log::Info("[gwheel] TFBE.open[2] enumerating devices");
                EnumCtx ctx{ dinput_, nullptr, 0, 0, {} };
                dinput_->EnumDevices(DI8DEVCLASS_GAMECTRL, &EnumDevicesCallback,
                                     &ctx, DIEDFL_ATTACHEDONLY);
                if (!ctx.device)
                {
                    if (verbose)
                        log::Warn("[gwheel] TFBE.open[2.x] no Logitech wheel attached");
                    return false;
                }
                log::InfoF("[gwheel] TFBE.open[3] device created pid=0x%04X", ctx.pid);
                {
                    char nameUtf8[256] = {};
                    WideCharToMultiByte(CP_UTF8, 0, ctx.productName, -1,
                                        nameUtf8, sizeof(nameUtf8), nullptr, nullptr);
                    log::InfoF("[gwheel] TFBE.open[3]   productName=\"%s\"", nameUtf8);
                }

                log::Info("[gwheel] TFBE.open[4] SetDataFormat");
                if (FAILED(ctx.device->SetDataFormat(&c_dfDIJoystick2)))
                {
                    log::Warn("[gwheel] TFBE.open[4.x] SetDataFormat failed");
                    ctx.device->Release();
                    return false;
                }

                log::Info("[gwheel] TFBE.open[5] SetCooperativeLevel(BACKGROUND|NONEXCLUSIVE)");
                if (FAILED(ctx.device->SetCooperativeLevel(hwnd,
                                                           DISCL_BACKGROUND | DISCL_NONEXCLUSIVE)))
                {
                    log::Warn("[gwheel] TFBE.open[5.x] SetCooperativeLevel failed");
                    ctx.device->Release();
                    return false;
                }

                // Set signed [-32768, +32767] range on every axis. Default
                // DInput axis range follows the HID descriptor, which on the
                // G923 publishes the wheel rotation as UNSIGNED 0..32767 —
                // that left center at the midpoint of NormalizeSteer's output
                // (~0.5) so the game saw a permanent half-right turn that
                // could only be intensified, never relieved. The legacy SDK
                // applies the same range internally; we have to do it
                // explicitly because we own this DInput device.
                DIPROPRANGE range{};
                range.diph.dwSize       = sizeof(DIPROPRANGE);
                range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
                range.diph.dwObj        = 0;
                range.diph.dwHow        = DIPH_DEVICE;
                range.lMin              = -32768;
                range.lMax              = 32767;
                if (FAILED(ctx.device->SetProperty(DIPROP_RANGE, &range.diph)))
                    log::Warn("[gwheel] TFBE.open[5] SetProperty(DIPROP_RANGE) failed; "
                              "axis values may be off-center (see G923 unsigned-axis bug)");
                else
                    log::Info("[gwheel] TFBE.open[5] axis range set to [-32768, +32767]");

                if (FAILED(ctx.device->Acquire()))
                    log::Warn("[gwheel] TFBE.open[5] Acquire failed (input polling may return empty state)");

                // ---- Skip OpenByDirectInputW entirely -----------------------
                //
                // 2026-05-04 diagnostic finding: TrueForce auto-detects
                // connected wheels at dllOpen time and assigns them indices
                // starting at 0. logiTrueForceSupported(0) returned true on
                // first launch with a G923 attached, BEFORE we ever called
                // OpenByDirectInputW. That gives us a valid wheelHandle
                // without needing the broken-ABI ByDirectInput exports at all.
                //
                // The DInput device we just created above is for OUR input
                // polling (axes/buttons via GetDeviceState in Pump). The SDK
                // has its own internal DInput for FFB submission.
                if (!vt_->logiTrueForceSupported)
                {
                    log::Warn("[gwheel] TFBE.open[6.x] logiTrueForceSupported not bound");
                    ctx.device->Unacquire();
                    ctx.device->Release();
                    permanentFailure_.store(true, std::memory_order_release);
                    return false;
                }

                log::Info("[gwheel] TFBE.open[6] verifying logiTrueForceSupported(0) (the index-based path)");
                auto supRes = Seh_BoolByInt(reinterpret_cast<void*>(vt_->logiTrueForceSupported), 0);
                if (!supRes.survived)
                {
                    log::WarnF("[gwheel] TFBE.open[6.x] logiTrueForceSupported(0) crashed code=0x%08lX",
                               supRes.exceptionCode);
                    ctx.device->Unacquire();
                    ctx.device->Release();
                    permanentFailure_.store(true, std::memory_order_release);
                    return false;
                }
                log::InfoF("[gwheel] TFBE.open[6] logiTrueForceSupported(0) = %s",
                           supRes.result ? "true" : "false");
                if (!supRes.result)
                {
                    log::Warn("[gwheel] TFBE.open[6.x] SDK reports no TrueForce wheel at index 0");
                    ctx.device->Unacquire();
                    ctx.device->Release();
                    permanentFailure_.store(true, std::memory_order_release);
                    return false;
                }

                const int handle = 0;
                log::InfoF("[gwheel] TFBE.open[7] using wheel handle=%d (SDK auto-detected)", handle);

                dinputDevice_ = ctx.device;
                wheelHandle_  = handle;

                // ---- Post-open SDK probe battery ----
                //
                // 2026-05-04 finding: TrueForceBackend opened cleanly and
                // FFB pipeline computed values, but user felt nothing.
                // Hypotheses being tested:
                //   1. SDK starts in paused state (IsPaused → Resume)
                //   2. Default gain is 0 (SetGainKF / SetGainTF set to 1)
                //   3. GetMaxPeakTorqueKF signature wrong, silently returns garbage
                //   4. SetTorqueKF signature wrong, silently no-ops

                // Probe IsPaused / Resume. Both are int-only-arg shapes that
                // we already know don't crash for index 0 (Supported worked).
                if (vt_->logiTrueForceIsPaused)
                {
                    auto r = Seh_BoolByInt(reinterpret_cast<void*>(vt_->logiTrueForceIsPaused), 0);
                    if (!r.survived)
                        log::WarnF("[gwheel] TFBE.open[8] IsPaused(0) crashed code=0x%08lX", r.exceptionCode);
                    else
                        log::InfoF("[gwheel] TFBE.open[8] IsPaused(0) = %s",
                                   r.result ? "true (paused — expected to suppress FFB)" : "false (unpaused)");
                }
                if (vt_->logiTrueForceResume)
                {
                    auto r = Seh_BoolByInt(reinterpret_cast<void*>(vt_->logiTrueForceResume), 0);
                    if (!r.survived)
                        log::WarnF("[gwheel] TFBE.open[8] Resume(0) crashed code=0x%08lX", r.exceptionCode);
                    else
                        log::InfoF("[gwheel] TFBE.open[8] Resume(0) returned %s",
                                   r.result ? "true" : "false");
                }
                if (vt_->logiTrueForceIsPaused)
                {
                    auto r = Seh_BoolByInt(reinterpret_cast<void*>(vt_->logiTrueForceIsPaused), 0);
                    if (r.survived)
                        log::InfoF("[gwheel] TFBE.open[8] IsPaused(0) after Resume = %s",
                                   r.result ? "true (UNEXPECTED — Resume failed?)" : "false");
                }

                // Probe SetGainKF/TF at maximum gain. Default may be 0.
                if (vt_->logiTrueForceSetGainKF)
                {
                    auto r = Seh_BoolByIntDouble(
                        reinterpret_cast<void*>(vt_->logiTrueForceSetGainKF), 0, 1.0);
                    log::InfoF("[gwheel] TFBE.open[8] SetGainKF(0, 1.0) -> %s%s",
                               r.survived ? "SURVIVED" : "CRASHED",
                               r.survived ? (r.result ? " result=true" : " result=false") : "");
                    if (!r.survived) log::WarnF("[gwheel] TFBE.open[8]   code=0x%08lX", r.exceptionCode);
                }
                if (vt_->logiTrueForceSetGainTF)
                {
                    auto r = Seh_BoolByIntDouble(
                        reinterpret_cast<void*>(vt_->logiTrueForceSetGainTF), 0, 1.0);
                    log::InfoF("[gwheel] TFBE.open[8] SetGainTF(0, 1.0) -> %s%s",
                               r.survived ? "SURVIVED" : "CRASHED",
                               r.survived ? (r.result ? " result=true" : " result=false") : "");
                    if (!r.survived) log::WarnF("[gwheel] TFBE.open[8]   code=0x%08lX", r.exceptionCode);
                }

                // Probe Available + recheck after Resume. Reuses Seh_TFAvailable
                // pattern but inline; the loader's helpers live in a different
                // translation unit so we duplicate the SEH wrap here.
                {
                    auto r = Seh_BoolNoArg(reinterpret_cast<void*>(vt_->logiTrueForceAvailable));
                    if (r.survived)
                        log::InfoF("[gwheel] TFBE.open[8] Available() post-Resume = %s",
                                   r.result ? "true" : "false");
                }

                // Capture device info + capabilities.
                {
                    DeviceInfo i{};
                    i.vendor = Vendor::Logitech;
                    i.vid    = ctx.vid;
                    i.pid    = ctx.pid;
                    WideCharToMultiByte(CP_UTF8, 0, ctx.productName, -1,
                                        i.productName, sizeof(i.productName),
                                        nullptr, nullptr);
                    // SEH-wrap GetOperatingRangeDegrees — its signature is also
                    // a guess, and the previous run showed it apparently
                    // returned no useful value (range=0 deg in the open log).
                    if (vt_->logiWheelGetOperatingRangeDegrees)
                    {
                        double deg = 0.0;
                        auto r = Seh_BoolByIntDoublePtr(
                            reinterpret_cast<void*>(vt_->logiWheelGetOperatingRangeDegrees),
                            wheelHandle_, &deg);
                        log::InfoF("[gwheel] TFBE.open[8] GetOperatingRangeDegrees -> %s deg=%.1f",
                                   r.survived ? (r.result ? "OK" : "FAIL") : "CRASHED", deg);
                        if (r.survived && r.result && deg > 0.0)
                            i.operatingRangeDeg = static_cast<int>(deg);
                    }
                    std::lock_guard lk(infoMtx_);
                    info_ = i;
                    if (i.operatingRangeDeg > 0)
                        cachedRangeDeg_.store(i.operatingRangeDeg, std::memory_order_release);
                }

                // Cache max peak torque (SEH-protected this time).
                if (vt_->logiTrueForceGetMaxPeakTorqueKF)
                {
                    double nm = 0.0;
                    auto r = Seh_BoolByIntDoublePtr(
                        reinterpret_cast<void*>(vt_->logiTrueForceGetMaxPeakTorqueKF),
                        wheelHandle_, &nm);
                    log::InfoF("[gwheel] TFBE.open[8] GetMaxPeakTorqueKF -> %s nm=%.3f",
                               r.survived ? (r.result ? "OK" : "FAIL") : "CRASHED", nm);
                    if (r.survived && r.result && nm > 0.0) maxPeakTorqueNm_ = nm;
                }
                if (maxPeakTorqueNm_ <= 0.0)
                    maxPeakTorqueNm_ = 2.5; // G923 spec is ~2.3 Nm peak; conservative default

                // Capabilities. ffbSpring + ffbPeriodic are FALSE pending
                // header validation (see PlaySpring / PlayPeriodic above).
                Capabilities c{};
                c.ffbConstant            = true;
                c.ffbSpring              = false;
                c.ffbDamper              = vt_->logiTrueForceSetDamping != nullptr;
                c.ffbPeriodic            = false;
                c.ffbAirborne            = true;
                c.ffbJolt                = true;
                c.vibrotactile           = vt_->logiTrueForceSetTorqueTFfloat != nullptr;
                c.revLeds                = vt_->logiWheelPlayLeds != nullptr;
                c.angleReadback          = vt_->logiTrueForceGetAngleDegrees != nullptr;
                c.angularVelocityReadback= vt_->logiTrueForceGetAngularVelocityDegrees != nullptr;
                c.operatingRangeSet      = vt_->logiWheelSetOperatingRangeDegrees != nullptr;
                c.globalGainSet          = vt_->logiTrueForceSetGainKF != nullptr;
                c.hasRightCluster        = false;  // populated downstream from device_table
                if (const auto* mi = LookupByPid(ctx.pid))
                    c.hasRightCluster = mi->has_right_cluster;
                caps_ = c;

                {
                    char nameUtf8[256] = {};
                    WideCharToMultiByte(CP_UTF8, 0, ctx.productName, -1, nameUtf8,
                                        sizeof(nameUtf8), nullptr, nullptr);
                    log::InfoF("[gwheel] TrueForceBackend: opened \"%s\" pid=0x%04X handle=%d range=%d deg maxPeakKF=%.2fNm",
                               nameUtf8, ctx.pid, handle, info_.operatingRangeDeg, maxPeakTorqueNm_);
                }

                ready_.store(true, std::memory_order_release);
                return true;
            }

            void RefreshOperatingRange()
            {
                // Sample every ~1s at 250 Hz pump.
                if ((++pumpTicks_ % 250) != 0) return;
                if (!vt_ || !vt_->logiWheelGetOperatingRangeDegrees) return;

                double deg = 0.0;
                if (!vt_->logiWheelGetOperatingRangeDegrees(wheelHandle_, &deg) || deg <= 0.0)
                    return;
                const int actual = static_cast<int>(deg);
                const int prev = cachedRangeDeg_.exchange(actual, std::memory_order_acq_rel);
                if (prev != actual)
                    log::InfoF("[gwheel] TrueForce range: %d -> %d deg", prev, actual);
            }

            // ---- State -----------------------------------------------------

            const trueforce::api::Vtable* vt_ = nullptr;
            IDirectInput8W*               dinput_       = nullptr;
            IDirectInputDevice8W*         dinputDevice_ = nullptr;
            int                           wheelHandle_  = -1;
            double                        maxPeakTorqueNm_ = 0.0;

            std::atomic<bool>     ready_{false};
            // Latched after a signature-probe pass returns surviveMask=0 so
            // we don't retry-storm. SEH-caught AVs leave the SDK's internal
            // state corrupted; one pass per session is the safe ceiling.
            std::atomic<bool>     permanentFailure_{false};
            std::atomic<float>    globalGain_{1.f};
            std::atomic<int>      cachedRangeDeg_{540};
            std::atomic<uint32_t> openAttempts_{0};
            uint64_t              pumpTicks_{0};

            mutable std::mutex snapMtx_;
            wheel::Snapshot    snap_{};

            mutable std::mutex infoMtx_;
            DeviceInfo         info_{};

            Capabilities       caps_{};
        };
    }

    std::unique_ptr<IWheelBackend> CreateLogitechTrueForceBackend()
    {
        return std::make_unique<TrueForceBackend>();
    }
}
