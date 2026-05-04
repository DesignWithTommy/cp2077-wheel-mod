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
                IDirectInput8W* di;
                IDirectInputDevice8W* device;
                uint16_t pid;
                uint16_t vid;
                wchar_t  productName[256];
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

                ctx->device = dev;
                ctx->vid    = vid;
                ctx->pid    = pid;
                std::wcsncpy(ctx->productName, inst->tszProductName,
                             sizeof(ctx->productName) / sizeof(wchar_t) - 1);
                return DIENUM_STOP;
            }

            bool TryOpenDevice()
            {
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

                // Probe TrueForce support BEFORE doing anything cooperative-
                // level intrusive. If the wheel isn't TrueForce-capable we
                // fail fast without touching DInput acquisition.
                if (vt_->logiTrueForceSupportedByDirectInputW)
                {
                    log::Info("[gwheel] TFBE.open[4] calling logiTrueForceSupportedByDirectInputW");
                    const bool tfSupported = vt_->logiTrueForceSupportedByDirectInputW(ctx.device);
                    log::InfoF("[gwheel] TFBE.open[4] returned %s", tfSupported ? "true" : "false");
                    if (!tfSupported)
                    {
                        log::Warn("[gwheel] TFBE.open: wheel reports no TrueForce support; aborting");
                        ctx.device->Release();
                        return false;
                    }
                }

                log::Info("[gwheel] TFBE.open[5] SetDataFormat");
                if (FAILED(ctx.device->SetDataFormat(&c_dfDIJoystick2)))
                {
                    log::Warn("[gwheel] TFBE.open[5.x] SetDataFormat failed");
                    ctx.device->Release();
                    return false;
                }

                // Use NON-EXCLUSIVE cooperative level. Exclusive racing with
                // the game's own DInput layer is the leading suspect for
                // the v2.31.2 crash; the TrueForce SDK should manage FFB
                // submission internally without needing exclusive on us.
                log::Info("[gwheel] TFBE.open[6] SetCooperativeLevel(BACKGROUND|NONEXCLUSIVE)");
                if (FAILED(ctx.device->SetCooperativeLevel(hwnd,
                                                           DISCL_BACKGROUND | DISCL_NONEXCLUSIVE)))
                {
                    log::Warn("[gwheel] TFBE.open[6.x] SetCooperativeLevel failed");
                    ctx.device->Release();
                    return false;
                }

                // DO NOT call Acquire here. The TrueForce SDK is expected
                // to manage acquisition itself when we hand off the device.
                // Phase-1-crash-2026-05-03: Acquire-before-handoff is the
                // second leading suspect for the crash; let the SDK do it.

                if (!vt_->logiWheelOpenByDirectInputW)
                {
                    log::Warn("[gwheel] TFBE.open[7.x] logiWheelOpenByDirectInputW not bound");
                    ctx.device->Release();
                    return false;
                }
                log::Info("[gwheel] TFBE.open[7] CALLING logiWheelOpenByDirectInputW (THE risky call)");
                const int handle = vt_->logiWheelOpenByDirectInputW(ctx.device);
                log::InfoF("[gwheel] TFBE.open[7] returned handle=%d", handle);
                if (handle < 0)
                {
                    log::WarnF("[gwheel] TFBE.open[7.x] logiWheelOpenByDirectInputW returned %d", handle);
                    ctx.device->Release();
                    return false;
                }

                // Acquire AFTER the SDK has taken ownership, so Pump can
                // GetDeviceState for axes/buttons. NONEXCLUSIVE means this
                // shouldn't fight game-side acquisition.
                log::Info("[gwheel] TFBE.open[8] Acquire");
                if (FAILED(ctx.device->Acquire()))
                    log::Warn("[gwheel] TFBE.open[8] Acquire failed (input polling may return empty state)");

                dinputDevice_ = ctx.device;
                wheelHandle_  = handle;

                // Capture device info + capabilities.
                {
                    DeviceInfo i{};
                    i.vendor = Vendor::Logitech;
                    i.vid    = ctx.vid;
                    i.pid    = ctx.pid;
                    WideCharToMultiByte(CP_UTF8, 0, ctx.productName, -1,
                                        i.productName, sizeof(i.productName),
                                        nullptr, nullptr);
                    if (vt_->logiWheelGetOperatingRangeDegrees)
                    {
                        double deg = 0.0;
                        if (vt_->logiWheelGetOperatingRangeDegrees(wheelHandle_, &deg))
                            i.operatingRangeDeg = static_cast<int>(deg);
                    }
                    std::lock_guard lk(infoMtx_);
                    info_ = i;
                    if (i.operatingRangeDeg > 0)
                        cachedRangeDeg_.store(i.operatingRangeDeg, std::memory_order_release);
                }

                // Cache max peak torque so PlayConstant can map normalized
                // magnitude to Nm. Falls back to a reasonable estimate if
                // the SDK readback fails.
                if (vt_->logiTrueForceGetMaxPeakTorqueKF)
                {
                    double nm = 0.0;
                    if (vt_->logiTrueForceGetMaxPeakTorqueKF(wheelHandle_, &nm) && nm > 0.0)
                        maxPeakTorqueNm_ = nm;
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
