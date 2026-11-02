#include "plugin.h"
#include "logging.h"
#include "wheel.h"
#include "sources.h"
#include "input_bindings.h"
#include "vehicle_hook.h"
#include "kbd_hook.h"
#include "config.h"
#include "rtti.h"
#include "rtti_dump.h"
#include "audio_monitor.h"
#include "led.h"
#include "mod_settings_seed.h"
#include <cmath>



#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace direct_wheel
{
    namespace
    {
        std::atomic<bool> g_pumpRunning{false};
        std::thread       g_pumpThread;

        // Convert the Logi SDK's wheel::Snapshot into the canonical
        // sources::Frame that the rest of the plugin consumes. Today this
        // is a straight field copy; if we move buttons+POV to RawInput
        // later, this becomes the merge point between the two readers.
        sources::Frame BuildFrame(const wheel::Snapshot& s)
        {
            sources::Frame f;
            f.connected       = s.connected;
            f.axes.steer      = s.steer;
            f.axes.throttle   = s.throttle;
            f.axes.brake      = s.brake;
            f.axes.clutch     = s.clutch;
            f.digital.buttons = s.buttons;
            f.digital.pov     = s.pov;

            // ChoomLink: inverts are applied once, in wheel.cpp ReadSnapshot. Applying them here too undid them:
            // a G920 pedals read 0.00 there but reached the car as gas 1.00 + brake 1.00 (Nov 1 22:10), a burnout.

            // Apply deadzone
            float deadzonePct = config::Current().input.steeringDeadzoneDegrees / 450.0f; // Assume 900 deg wheel
            if (std::fabs(f.axes.steer) <= deadzonePct)
            {
                f.axes.steer = 0.0f;
            }
            else
            {
                float sign = f.axes.steer > 0.0f ? 1.0f : -1.0f;
                f.axes.steer = sign * ((std::fabs(f.axes.steer) - deadzonePct) / (1.0f - deadzonePct));
            }

            // Clutch-as-brake: the G923's brake pedal is physically stiff; the
            // softer clutch pedal is easier to modulate. CP2077 has no manual
            // transmission so the clutch axis is otherwise ignored. When the
            // toggle is on, whichever pedal is pressed deeper drives braking.
            if (config::Current().input.clutchAsBrake)
                f.axes.brake = std::max(f.axes.brake, f.axes.clutch);

            return f;
        }

        void PumpLoop()
        {
            using namespace std::chrono_literals;
            log::Info("[direct_wheel] pump thread started (250 Hz)");

            while (g_pumpRunning.load(std::memory_order_acquire))
            {
                wheel::Pump();
                const sources::Frame frame = BuildFrame(wheel::CurrentSnapshot());
                sources::Publish(frame);
                if (frame.connected)
                    input_bindings::OnTick(frame);
                std::this_thread::sleep_for(4ms);
            }
            log::Info("[direct_wheel] pump thread stopped");
        }
    }

    PluginContext& Ctx()
    {
        static PluginContext ctx;
        return ctx;
    }

    void OnLoad(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
    {
        auto& ctx = Ctx();
        ctx.handle = aHandle;
        ctx.sdk = aSdk;

        log::InfoF("[direct_wheel] ========================================");
        log::InfoF("[direct_wheel] loaded v%s", kVersionString);
        log::InfoF("[direct_wheel] ========================================");

        log::Info("[direct_wheel] step 1/9: loading config");
        config::Load();

        // MUST run before mod_settings reads its user.ini in ProcessScriptData.
        // All RED4ext plugin OnLoads complete before script processing begins,
        // so this ordering is safe regardless of plugin load order.
        log::Info("[direct_wheel] step 2/9: probing attached wheel + seeding mod_settings/user.ini");
        mod_settings_seed::Run();

        log::Info("[direct_wheel] step 3/9: registering redscript natives");
        rtti::Register();

        log::Info("[direct_wheel] step 4/9: initializing Logitech SDK wheel layer (deferred)");
        wheel::Init();

        log::Info("[direct_wheel] step 5/9: installing vehicle-input detour (hash-resolved)");
        vehicle_hook::Init();

        log::Info("[direct_wheel] step 6/9: installing on-foot keyboard hook (G HUB injection filter)");
        kbd_hook::Install();

        log::Info("[direct_wheel] step 7/9: starting 250 Hz pump thread");
        g_pumpRunning.store(true, std::memory_order_release);
        g_pumpThread = std::thread(PumpLoop);

        log::Info("[direct_wheel] step 8/9: starting WASAPI loopback audio monitor");
        audio_monitor::Init();

        log::Info("[direct_wheel] step 9/9: starting rev-strip LED controller");
        led::Init();

        log::InfoF("[direct_wheel] ready: hook=%s kbdhook=%s",
                   vehicle_hook::IsInstalled() ? "installed" : "not-installed",
                   kbd_hook::IsInstalled() ? "installed" : "not-installed");
    }

    void OnUnload()
    {
        log::Info("[direct_wheel] unloading");

        // Shut LED + audio down before the pump / SDK so the controller
        // isn't still calling LogiPlayLeds as we tear the SDK down.
        led::Shutdown();
        audio_monitor::Shutdown();

        g_pumpRunning.store(false, std::memory_order_release);
        if (g_pumpThread.joinable()) g_pumpThread.join();

        kbd_hook::Uninstall();
        vehicle_hook::Shutdown();
        wheel::Shutdown();

        auto& ctx = Ctx();
        ctx.handle = {};
        ctx.sdk = nullptr;
    }
}
