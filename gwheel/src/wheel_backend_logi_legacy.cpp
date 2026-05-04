#include "wheel_backend_logi_legacy.h"
#include "logi_common.h"
#include "logging.h"
#include "device_table.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <LogitechSteeringWheelLib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>

namespace gwheel::wheel_backend
{
    namespace
    {
        // ---- LegacyBackend -------------------------------------------------
        //
        // Wraps the Logitech Steering Wheel SDK (LogitechSteeringWheelLib).
        // Single-instance: the SDK itself is process-global, so only one of
        // these can be live at a time. Created by CreateLogitechLegacyBackend()
        // and held by the wheel.cpp dispatch shim.
        //
        // SDK init is deferred — Init() returns true immediately; the actual
        // LogiSteeringInitialize call happens on the first Pump() that finds
        // a usable game window. This matches the legacy behavior where the
        // game window may not exist yet at plugin load time.

        class LegacyBackend final : public IWheelBackend
        {
        public:
            // ---- Identification -------------------------------------------

            Vendor      GetVendor()      const override { return Vendor::Logitech; }
            BackendKind GetKind()        const override { return BackendKind::LogitechLegacy; }
            const char* GetBackendName() const override { return "Logitech (legacy SDK)"; }

            // ---- Lifecycle -------------------------------------------------

            bool Init() override
            {
                log::Info("[gwheel] LegacyBackend::Init - SDK init is deferred to the pump thread");
                return true;
            }

            bool Open() override
            {
                // The legacy backend opens lazily inside Pump (TryInitSdk +
                // TryBindController), retrying each tick until the game
                // window exists and the SDK reports a connected wheel.
                return true;
            }

            void Close() override
            {
                ready_.store(false, std::memory_order_release);
                if (sdkInitialized_.exchange(false))
                {
                    const int idx = index_.load(std::memory_order_acquire);
                    if (idx >= 0)
                    {
                        LogiStopSpringForce(idx);
                        LogiStopDamperForce(idx);
                        LogiStopConstantForce(idx);
                        LogiStopSurfaceEffect(idx);
                        LogiStopCarAirborne(idx);
                        LogiPlayLeds(idx, 0.f, 100.f, 200.f);
                    }
                    // Deliberately NOT calling LogiSteeringShutdown(). The
                    // shipped LogitechSteeringWheelEnginesWrapper.dll (SDK
                    // 8.75.30) was compiled DEBUG against MSVCR120D and is
                    // documented to crash inside its own teardown when called
                    // at process exit. Forces above are stopped; OS reclaims
                    // SDK state on process exit a fraction later.
                    log::Info("[gwheel] LogiSteeringShutdown skipped (known SDK debug-DLL crash on teardown)");
                }
                index_.store(-1, std::memory_order_release);
            }

            bool IsOpen() const override { return ready_.load(std::memory_order_acquire); }

            // ---- Pump ------------------------------------------------------

            void Pump() override
            {
                if (!sdkInitialized_.load(std::memory_order_acquire))
                {
                    if (!TryInitSdk()) return;
                }

                LogiUpdate();

                if (!ready_.load(std::memory_order_acquire))
                {
                    if (!TryBindController()) return;
                }

                const int idx = index_.load(std::memory_order_acquire);
                if (idx < 0) return;

                if (!LogiIsConnected(idx))
                {
                    if (ready_.exchange(false))
                        log::Warn("[gwheel] wheel disconnected; will re-bind when it returns");
                    return;
                }

                RefreshGHubRange(idx);

                const DIJOYSTATE2* raw = LogiGetState(idx);
                if (!raw) return;

                wheel::Snapshot s;
                s.connected = true;
                logi_common::PopulateSnapshotFromDInput(*raw, s);

                {
                    std::lock_guard lk(snapMtx_);
                    snap_ = s;
                }
            }

            const wheel::Snapshot& CurrentSnapshot() const override
            {
                static thread_local wheel::Snapshot tl;
                std::lock_guard lk(snapMtx_);
                tl = snap_;
                return tl;
            }

            const DeviceInfo&   GetDeviceInfo()   const override
            {
                static thread_local DeviceInfo tl;
                std::lock_guard lk(infoMtx_);
                tl = info_;
                return tl;
            }

            const Capabilities& GetCapabilities() const override { return caps_; }

            // ---- FFB primitives -------------------------------------------

            void PlayConstant(float magnitudeSigned01) override
            {
                if (!ready_.load() || !hasFFB_.load()) return;
                const int idx = index_.load(std::memory_order_acquire);
                const int pct = ScaleSigned100(magnitudeSigned01);
                if (!LogiPlayConstantForce(idx, pct))
                    log::DebugF("[gwheel] LogiPlayConstantForce(%d) returned false", pct);
                else if (log::DebugEnabled())
                    log::DebugF("[gwheel] constant=%d%%", pct);
            }
            void StopConstant() override
            {
                const int idx = index_.load(std::memory_order_acquire);
                if (idx >= 0) LogiStopConstantForce(idx);
            }
            void PlaySpring(float coefficient01) override
            {
                if (!ready_.load() || !hasFFB_.load()) return;
                const int idx = index_.load(std::memory_order_acquire);
                const int pct = ScaleUnsigned100(coefficient01);
                if (!LogiPlaySpringForce(idx, 0, 100, pct))
                    log::DebugF("[gwheel] LogiPlaySpringForce(coef=%d) returned false", pct);
                else if (log::DebugEnabled())
                    log::DebugF("[gwheel] spring=%d%%", pct);
            }
            void StopSpring() override
            {
                const int idx = index_.load(std::memory_order_acquire);
                if (idx >= 0) LogiStopSpringForce(idx);
            }
            void PlayDamper(float coefficient01) override
            {
                if (!ready_.load() || !hasFFB_.load()) return;
                const int idx = index_.load(std::memory_order_acquire);
                const int pct = ScaleUnsigned100(coefficient01);
                if (!LogiPlayDamperForce(idx, pct))
                    log::DebugF("[gwheel] LogiPlayDamperForce(%d) returned false", pct);
                else if (log::DebugEnabled())
                    log::DebugF("[gwheel] damper=%d%%", pct);
            }
            void StopDamper() override
            {
                const int idx = index_.load(std::memory_order_acquire);
                if (idx >= 0) LogiStopDamperForce(idx);
            }
            void PlayPeriodic(float magnitude01, int periodMs) override
            {
                if (!ready_.load() || !hasFFB_.load()) return;
                const int idx = index_.load(std::memory_order_acquire);
                const int pct = ScaleUnsigned100(magnitude01);
                if (!LogiPlaySurfaceEffect(idx, LOGI_PERIODICTYPE_SINE, pct, periodMs))
                    log::DebugF("[gwheel] LogiPlaySurfaceEffect(SINE, %d, %dms) returned false", pct, periodMs);
                else if (log::DebugEnabled())
                    log::DebugF("[gwheel] surface=%d%% @ %dms", pct, periodMs);
            }
            void StopPeriodic() override
            {
                const int idx = index_.load(std::memory_order_acquire);
                if (idx >= 0) LogiStopSurfaceEffect(idx);
            }
            void SetAirborne(bool airborne) override
            {
                if (!ready_.load() || !hasFFB_.load()) return;
                const int idx = index_.load(std::memory_order_acquire);
                if (idx < 0) return;
                if (airborne)
                {
                    if (!LogiPlayCarAirborne(idx))
                        log::Debug("[gwheel] LogiPlayCarAirborne returned false");
                    else if (log::DebugEnabled())
                        log::Debug("[gwheel] airborne ON");
                }
                else
                {
                    LogiStopCarAirborne(idx);
                }
            }
            void StopAll() override
            {
                const int idx = index_.load(std::memory_order_acquire);
                if (idx < 0) return;
                LogiStopConstantForce(idx);
                LogiStopDamperForce(idx);
                LogiStopSpringForce(idx);
                LogiStopSurfaceEffect(idx);
                LogiStopCarAirborne(idx);
            }

            // ---- TF channel: not supported on legacy backend --------------

            void TfFeedSample(float)        override {}
            void TfClearStream()            override {}
            void TfSetGain(float)           override {}
            int  GetHapticRateHz() const    override { return 0; }

            // ---- LEDs ------------------------------------------------------

            void PlayLeds(float currentRpm, float firstLedRpm, float redlineRpm) override
            {
                if (!ready_.load()) return;
                const int idx = index_.load(std::memory_order_acquire);
                if (idx < 0) return;
                // Degenerate profile (redline <= firstLed or both zero) =
                // hasn't been pushed yet; fall through to dark-bar branch
                // rather than feeding LogiPlayLeds a zero-width window.
                if (!(redlineRpm > firstLedRpm) || redlineRpm <= 0.f)
                {
                    WriteLeds(idx, 0.f);
                    return;
                }
                if (currentRpm < firstLedRpm)
                {
                    WriteLeds(idx, 0.f);
                    return;
                }
                LogiPlayLeds(idx, currentRpm, firstLedRpm, redlineRpm);
            }

            void ClearLeds() override
            {
                if (!ready_.load()) return;
                const int idx = index_.load(std::memory_order_acquire);
                if (idx < 0) return;
                WriteLeds(idx, 0.f);
            }

            void SetLedLevel(float level01) override
            {
                if (!ready_.load()) return;
                const int idx = index_.load(std::memory_order_acquire);
                if (idx < 0) return;
                WriteLeds(idx, level01);
            }

            // ---- Configuration --------------------------------------------

            void SetOperatingRangeDeg(int degrees) override
            {
                const int idx = index_.load(std::memory_order_acquire);
                if (idx < 0) return;
                LogiSetOperatingRange(idx, std::clamp(degrees, 40, 1080));
            }
            void SetGlobalGain(float gain01) override
            {
                // LogiSetPreferredControllerProperties is broken on recent
                // G HUB versions (always returns FAILED). We apply gain as a
                // per-effect multiplier instead — captured in globalStrength_,
                // applied by ScaleSigned100 / ScaleUnsigned100.
                globalStrength_.store(std::clamp(gain01, 0.f, 1.f),
                                      std::memory_order_relaxed);
            }

            // ---- Readback --------------------------------------------------

            float GetWheelAngleDeg() const override
            {
                return std::numeric_limits<float>::quiet_NaN();
            }
            float GetWheelAngularVelocityDeg() const override
            {
                return std::numeric_limits<float>::quiet_NaN();
            }
            int  GetOperatingRangeDeg() const override
            {
                return ghubRangeDeg_.load(std::memory_order_acquire);
            }
            bool HasFFB() const override
            {
                return hasFFB_.load(std::memory_order_acquire);
            }

            // ---- Public for handshake (called via static_cast) -------------

            int  WheelIndex() const { return index_.load(std::memory_order_acquire); }

        private:
            // ---- Helpers ---------------------------------------------------

            int FindWheelIndex()
            {
                for (int i = 0; i < LOGI_MAX_CONTROLLERS; ++i)
                {
                    if (!LogiIsConnected(i)) continue;
                    if (LogiIsDeviceConnected(i, LOGI_DEVICE_TYPE_WHEEL)) return i;
                }
                for (int i = 0; i < LOGI_MAX_CONTROLLERS; ++i)
                {
                    if (!LogiIsConnected(i)) continue;
                    if (LogiIsManufacturerConnected(i, LOGI_MANUFACTURER_LOGITECH)) return i;
                }
                return -1;
            }

            void LogSdkVersion()
            {
                int major = 0, minor = 0, build = 0;
                if (LogiSteeringGetSdkVersion(&major, &minor, &build))
                {
                    std::lock_guard lk(infoMtx_);
                    info_.sdkMajor = major;
                    info_.sdkMinor = minor;
                    info_.sdkBuild = build;
                    log::InfoF("[gwheel] Logitech Steering Wheel SDK v%d.%d build %d",
                               major, minor, build);
                }
                else
                {
                    log::Warn("[gwheel] LogiSteeringGetSdkVersion failed; continuing anyway");
                }
            }

            void CaptureCaps(int idx)
            {
                DeviceInfo i{};
                {
                    std::lock_guard lk(infoMtx_);
                    i = info_;
                }

                wchar_t friendly[256] = {};
                if (LogiGetFriendlyProductName(idx, friendly, 256))
                {
                    WideCharToMultiByte(CP_UTF8, 0, friendly, -1,
                                        i.productName, sizeof(i.productName),
                                        nullptr, nullptr);
                }
                else
                {
                    std::snprintf(i.productName, sizeof(i.productName),
                                  "Logitech wheel (slot %d)", idx);
                }

                const bool ffb = LogiHasForceFeedback(idx);
                int range = 0;
                if (LogiGetOperatingRange(idx, range)) i.operatingRangeDeg = range;
                i.vendor = Vendor::Logitech;

                {
                    std::lock_guard lk(infoMtx_);
                    info_ = i;
                }

                // Capabilities are fixed for the legacy backend.
                Capabilities c{};
                c.ffbConstant            = ffb;
                c.ffbSpring              = ffb;
                c.ffbDamper              = ffb;
                c.ffbPeriodic            = ffb;
                c.ffbAirborne            = ffb;
                c.ffbJolt                = ffb;
                c.vibrotactile           = false; // legacy SDK has no TF channel
                c.revLeds                = true;  // probed via device_table downstream
                c.angleReadback          = false;
                c.angularVelocityReadback= false;
                c.operatingRangeSet      = true;
                c.globalGainSet          = false; // properties API broken; we use per-effect mul
                c.hasRightCluster        = false; // populated downstream from device_table
                caps_ = c;

                hasFFB_.store(ffb, std::memory_order_release);
            }

            // Single entry point for every LED write. Handles the G923
            // "idle-indicator" quirk: with firstLedOn=0 and currentRPM=0,
            // the wheel firmware leaves the outermost green LEDs lit as a
            // ready marker. Pushing currentRPM strictly below firstLedOn
            // (here 100 < 200) guarantees the bar goes fully dark.
            static void WriteLeds(int idx, float value)
            {
                if (value <= 0.005f)
                    LogiPlayLeds(idx, 0.f, 100.f, 200.f);
                else
                    LogiPlayLeds(idx, std::clamp(value, 0.f, 1.f), 0.f, 1.f);
            }

            int ScaleSigned100(float v) const
            {
                const float mul = std::clamp(globalStrength_.load(std::memory_order_relaxed), 0.f, 1.f);
                return std::clamp(static_cast<int>(std::lround(std::clamp(v * mul, -1.f, 1.f) * 100.f)),
                                  -100, 100);
            }
            int ScaleUnsigned100(float v) const
            {
                const float mul = std::clamp(globalStrength_.load(std::memory_order_relaxed), 0.f, 1.f);
                return std::clamp(static_cast<int>(std::lround(std::clamp(v * mul, 0.f, 1.f) * 100.f)),
                                  0, 100);
            }

            bool TryInitSdk()
            {
                if (sdkInitialized_.load(std::memory_order_acquire)) return true;

                HWND hwnd = logi_common::FindOwnGameWindow();
                const uint32_t n = initAttempts_.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool verbose = (n == 1) || (n % 1250 == 0);

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
                                   "Common causes: G HUB not running, wheel unplugged, or another exclusive client.",
                                   n);
                    }
                    return false;
                }

                sdkInitialized_.store(true, std::memory_order_release);
                LogSdkVersion();
                return true;
            }

            bool TryBindController()
            {
                if (ready_.load(std::memory_order_acquire)) return true;

                const int idx = FindWheelIndex();
                if (idx < 0)
                {
                    const uint32_t n = initAttempts_.load(std::memory_order_relaxed);
                    if (n == 1 || (n % 250) == 0)
                        log::Warn("[gwheel] no Logitech wheel detected by the SDK yet - waiting");
                    return false;
                }

                index_.store(idx, std::memory_order_release);
                CaptureCaps(idx);

                DeviceInfo info;
                {
                    std::lock_guard lk(infoMtx_);
                    info = info_;
                }
                log::InfoF("[gwheel] wheel bound at SDK slot %d: \"%s\" (FFB=%s range=%d deg)",
                           idx, info.productName,
                           HasFFB() ? "yes" : "no",
                           info.operatingRangeDeg);

                if (info.operatingRangeDeg > 0)
                    ghubRangeDeg_.store(info.operatingRangeDeg, std::memory_order_release);

                ready_.store(true, std::memory_order_release);
                return true;
            }

            // FFB-scale-from-range matches the formula in ApplyFfbFromPhysics
            // (anchor 180 deg = 1.0, clamp 0.3..1.5).
            static float RangeToFfbScale(int rangeDeg)
            {
                const int clamped = std::clamp(rangeDeg, 40, 1080);
                return std::clamp(180.f / static_cast<float>(clamped), 0.3f, 1.5f);
            }

            void RefreshGHubRange(int idx)
            {
                // Pump runs at ~250 Hz; sample every 1s (≈250 ticks).
                if ((++pumpTicks_ % 250) != 0) return;

                int actual = 0;
                if (!LogiGetOperatingRange(idx, actual) || actual <= 0) return;

                const int prev = ghubRangeDeg_.exchange(actual, std::memory_order_acq_rel);
                if (prev != actual)
                {
                    log::InfoF("[gwheel] operating range (G HUB): %d -> %d deg "
                               "(FFB scale: %.2f -> %.2f)",
                               prev, actual, RangeToFfbScale(prev), RangeToFfbScale(actual));
                }
            }

            // ---- State -----------------------------------------------------

            std::atomic<bool>  sdkInitialized_{false};
            std::atomic<bool>  ready_{false};
            std::atomic<int>   index_{-1};
            std::atomic<bool>  hasFFB_{false};
            std::atomic<float> globalStrength_{1.f};
            std::atomic<uint32_t> initAttempts_{0};
            std::atomic<int>   ghubRangeDeg_{540};
            uint64_t           pumpTicks_{0};

            mutable std::mutex snapMtx_;
            wheel::Snapshot    snap_{};

            mutable std::mutex infoMtx_;
            DeviceInfo         info_{};

            Capabilities       caps_{};
        };
    }

    std::unique_ptr<IWheelBackend> CreateLogitechLegacyBackend()
    {
        return std::make_unique<LegacyBackend>();
    }
}
