#include "led.h"
#include "wheel.h"
#include "audio_monitor.h"
#include "sources.h"
#include "config.h"
#include "logging.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace gwheel::led
{
    namespace
    {
        std::atomic<bool> g_running{false};
        std::thread       g_thread;

        // Light smoothing on the RPM signal so the LED bar looks like it's
        // following a tachometer rather than teleporting with every
        // Blackboard callback. alpha=0.35 at 30 Hz ≈ 80 ms time constant —
        // responsive enough that revving the engine still snaps the bar
        // toward redline, slow enough that micro-jitter on the RPM value
        // doesn't strobe segments.
        constexpr float kRpmEnvelopeAlpha = 0.35f;

        // First-LED threshold lands 60% of the way from idle to the
        // current gear's shift point. Below this the bar is fully dark;
        // above this it ramps green→amber→red→blink as RPM climbs to
        // the shift point. 0.6 chosen so cruise sits in the dark range
        // and only an actual approach to upshift lights the strip.
        constexpr float kFirstLedFraction = 0.6f;

        void Tick(bool& ledsDarkened, float& rpmState)
        {
            // The startup handshake owns the bar while it runs its own
            // sweep / flashes / breath pulse. Don't fight it from here;
            // hand the bar back once the flag drops.
            if (wheel::IsHandshakeActive())
            {
                ledsDarkened = false;
                return;
            }

            // Outside of a vehicle (main menu, on-foot, dialogue, menus)
            // the bar stays dark regardless of anything else.
            if (!sources::InVehicle())
            {
                if (!ledsDarkened)
                {
                    wheel::ClearLeds();
                    ledsDarkened = true;
                }
                rpmState = 0.f;  // envelope resets so next mount starts clean
                return;
            }

            const auto cfg = config::Current();

            if (!cfg.led.enabled)
            {
                if (!ledsDarkened)
                {
                    wheel::ClearLeds();
                    ledsDarkened = true;
                }
                return;
            }
            ledsDarkened = false;

            // Mode selection is driven by the game's own radio state
            // (VehicleDef.VehRadioState, pushed by the redscript
            // Blackboard listener) — NOT by audio-amplitude heuristics.
            // When the radio is on AND the user opted into the music-
            // visualizer mode, the bar tracks the WASAPI-loopback audio
            // level. When off, the bar shows real engine RPM scaled by
            // the current gear's shift point.
            const bool visualize = cfg.led.visualizerWhileMusic
                                   && sources::RadioActive();

            if (visualize)
            {
                // Audio monitor's CurrentLevel() is the dynamic-range-
                // normalised WASAPI signal in [0..1]. Send through the
                // legacy normalized path; the music branch has no notion
                // of RPM units.
                rpmState = 0.f;
                wheel::PlayLeds(std::clamp(audio_monitor::CurrentLevel(), 0.f, 1.f));
                return;
            }

            // Per-gear rev-strip path. Look up the active gear's shift
            // point on every tick: the redscript Int listener writes
            // currentGear on every transmission change, so this read is
            // always coherent with the engine state.
            const float target = sources::EngineRpmRaw();
            rpmState += kRpmEnvelopeAlpha * (target - rpmState);

            const int   gear     = sources::CurrentGear();
            const float idleRpm  = sources::IdleRpm();
            const float shiftRpm = sources::ShiftRpmForGear(gear);

            const float firstLedRpm = idleRpm + kFirstLedFraction * (shiftRpm - idleRpm);
            wheel::PlayLedsRaw(rpmState, firstLedRpm, shiftRpm);
        }

        void Loop()
        {
            using namespace std::chrono_literals;
            log::Info("[gwheel:led] controller thread started (30 Hz)");
            bool  ledsDarkened = false;
            float rpmState     = 0.f;
            while (g_running.load(std::memory_order_acquire))
            {
                Tick(ledsDarkened, rpmState);
                std::this_thread::sleep_for(33ms);
            }
            // Clean exit — leave the LEDs dark so they don't latch on
            // whatever the last value was when the game closes.
            wheel::ClearLeds();
            log::Info("[gwheel:led] controller thread stopped");
        }
    }

    void Init()
    {
        if (g_running.exchange(true, std::memory_order_acq_rel)) return;
        g_thread = std::thread(Loop);
    }

    void Shutdown()
    {
        if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
        if (g_thread.joinable()) g_thread.join();
    }
}
