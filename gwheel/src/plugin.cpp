#include "plugin.h"
#include "logging.h"
#include "wheel.h"
#include "sources.h"
#include "axis_resolver.h"
#include "input_bindings.h"
#include "vehicle_hook.h"
#include "kbd_hook.h"
#include "config.h"
#include "rtti.h"
#include "rtti_dump.h"
#include "audio_monitor.h"
#include "led.h"
#include "mod_settings_seed.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace gwheel
{
    namespace
    {
        std::atomic<bool> g_pumpRunning{false};
        std::thread       g_pumpThread;

        // Convert the Logi SDK's wheel::Snapshot into the canonical
        // sources::Frame that the rest of the plugin consumes. Steering /
        // throttle / brake are routed through axis_resolver against the
        // user's configured action bindings. An unbound action resolves
        // to 0, which means a fresh install with empty actionBindings
        // produces a still wheel and dead pedals until the user binds
        // them via Mod Settings.
        sources::Frame BuildFrame(const wheel::Snapshot& s)
        {
            sources::Frame f;
            f.connected       = s.connected;
            f.axes.steer      = axis_resolver::Resolve("SteerAxis",    s, axis_resolver::Combine::SignedFirst);
            f.axes.throttle   = axis_resolver::Resolve("ThrottleAxis", s, axis_resolver::Combine::UnsignedMax);
            f.axes.brake      = axis_resolver::Resolve("BrakeAxis",    s, axis_resolver::Combine::UnsignedMax);
            f.digital.buttons = s.buttons;
            f.digital.pov     = s.pov;
            return f;
        }

        void PumpLoop()
        {
            using namespace std::chrono_literals;
            log::Info("[gwheel] pump thread started (250 Hz)");
            while (g_pumpRunning.load(std::memory_order_acquire))
            {
                wheel::Pump();
                const sources::Frame frame = BuildFrame(wheel::CurrentSnapshot());
                sources::Publish(frame);
                if (frame.connected)
                    input_bindings::OnTick(frame);
                std::this_thread::sleep_for(4ms);
            }
            log::Info("[gwheel] pump thread stopped");
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

        log::InfoF("[gwheel] ========================================");
        log::InfoF("[gwheel] loaded v%s", kVersionString);
        log::InfoF("[gwheel] ========================================");

        log::Info("[gwheel] step 1/9: loading config");
        config::Load();

        // MUST run before mod_settings reads its user.ini in ProcessScriptData.
        // All RED4ext plugin OnLoads complete before script processing begins,
        // so this ordering is safe regardless of plugin load order.
        log::Info("[gwheel] step 2/9: probing attached wheel + seeding mod_settings/user.ini");
        mod_settings_seed::Run();

        log::Info("[gwheel] step 3/9: registering redscript natives");
        rtti::Register();

        log::Info("[gwheel] step 4/9: initializing Logitech SDK wheel layer (deferred)");
        wheel::Init();

        log::Info("[gwheel] step 5/9: installing vehicle-input detour (hash-resolved)");
        vehicle_hook::Init();

        log::Info("[gwheel] step 6/9: installing on-foot keyboard hook (G HUB injection filter)");
        kbd_hook::Install();

        log::Info("[gwheel] step 7/9: starting 250 Hz pump thread");
        g_pumpRunning.store(true, std::memory_order_release);
        g_pumpThread = std::thread(PumpLoop);

        log::Info("[gwheel] step 8/9: starting WASAPI loopback audio monitor");
        audio_monitor::Init();

        log::Info("[gwheel] step 9/9: starting rev-strip LED controller");
        led::Init();

        log::InfoF("[gwheel] ready: hook=%s kbdhook=%s",
                   vehicle_hook::IsInstalled() ? "installed" : "not-installed",
                   kbd_hook::IsInstalled() ? "installed" : "not-installed");
    }

    void OnUnload()
    {
        log::Info("[gwheel] unloading");

        // Detach the vehicle-input detour FIRST so the game's
        // UpdateVehicleCameraInput stops re-entering us while the rest
        // of the plugin is mid-teardown. The detour reads sources::Frame
        // and writes axes into the vehicle — leaving it live until the
        // end means a game-thread call could land on half-torn-down
        // state when the player quits while in a vehicle.
        vehicle_hook::Shutdown();

        // Then shut LED + audio + pump. LED first so the controller
        // isn't still calling LogiPlayLeds as we tear the SDK down.
        led::Shutdown();
        audio_monitor::Shutdown();

        g_pumpRunning.store(false, std::memory_order_release);
        if (g_pumpThread.joinable()) g_pumpThread.join();

        // Defensive zeroing of telemetry sources. Threads that read
        // these are already joined; this just ensures the state is
        // clean if the DLL is later reloaded (RED4ext supports hot-
        // reload of plugins) without leaning on static destruction
        // ordering.
        sources::SetInVehicle(false);
        sources::SetEngineRpmRaw(0.f);
        sources::SetCurrentGear(0);
        sources::ClearEngineProfile();
        sources::SetRadioActive(false);

        kbd_hook::Uninstall();
        wheel::Shutdown();

        auto& ctx = Ctx();
        ctx.handle = {};
        ctx.sdk = nullptr;
    }
}
