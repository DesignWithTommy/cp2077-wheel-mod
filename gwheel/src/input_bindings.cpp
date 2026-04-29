#include "input_bindings.h"
#include "config.h"
#include "logging.h"
#include "sources.h"
#include "kbd_hook.h"
#include "vehicle_hook.h"
#include "wheel.h"

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/RTTISystem.hpp>
#include <RED4ext/Scripting/Functions.hpp>
#include <RED4ext/Scripting/Utils.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace gwheel::input_bindings
{
    namespace
    {
        // How a PhysicalInput is derived from a wheel::Snapshot. Unmapped
        // = this physical control doesn't exist on the current wheel model;
        // the slot is held in the UI for uniformity but edges never fire.
        enum class Source { Unmapped, Button, PovDirection };
        struct InputSourceMap
        {
            Source   source = Source::Unmapped;
            uint32_t value  = 0; // button index (Button) or POV raw value (PovDirection)
        };

        constexpr InputSourceMap UM()          { return { Source::Unmapped, 0 }; }
        constexpr InputSourceMap BT(uint32_t v){ return { Source::Button, v }; }
        constexpr InputSourceMap PV(uint32_t v){ return { Source::PovDirection, v }; }

        using DeviceLayout = std::array<InputSourceMap, kCount>;

        // ------------------------------------------------------------------
        // G923 Xbox — VERIFIED empirically 2026-04-21 via tools/input_probe
        // on Grant's hardware. Ground truth.
        // ------------------------------------------------------------------
        constexpr DeviceLayout kG923XboxLayout = {{
            BT(5),         // PaddleLeft
            BT(4),         // PaddleRight
            PV(0),         // DpadUp
            PV(18000),     // DpadDown
            PV(27000),     // DpadLeft
            PV(9000),      // DpadRight
            BT(0),         // A
            BT(1),         // B
            BT(2),         // X
            BT(3),         // Y
            BT(6),         // Start
            BT(7),         // Select
            BT(9),         // LSB
            BT(8),         // RSB
            BT(18),        // Plus
            BT(19),        // Minus
            BT(22),        // ScrollClick
            BT(20),        // ScrollCW
            BT(21),        // ScrollCCW
            BT(10),        // Xbox
        }};

        // ------------------------------------------------------------------
        // G923 PS / G920 — UNVERIFIED. Physically identical button count
        // to G923 Xbox; most likely same DInput indices. If bindings feel
        // wrong on these wheels, re-run tools/input_probe.exe and submit
        // corrected indices. "Xbox"/"PS" button labels differ physically
        // but map to the same DInput slots.
        // ------------------------------------------------------------------
        constexpr DeviceLayout kG923PSLayout  = kG923XboxLayout;
        constexpr DeviceLayout kG920Layout    = kG923XboxLayout;

        // ------------------------------------------------------------------
        // G29 — UNVERIFIED. Same physical control count as G923 (the G923
        // is effectively a G29 with Trueforce). DInput layout is almost
        // certainly identical. Labels differ (Cross/Circle/Square/Triangle
        // for the PS variant) but slots are the same.
        // ------------------------------------------------------------------
        constexpr DeviceLayout kG29Layout = kG923XboxLayout;

        // ------------------------------------------------------------------
        // G27 — UNVERIFIED. Missing many controls the G923 has: no scroll
        // wheel, no +/- buttons, no Xbox button. The G27 has a 6-speed
        // shifter + reverse as discrete buttons, but we don't expose those
        // as PhysicalInputs yet (would require expanding PhysicalInput and
        // the UI). Face buttons are labeled 1-4 (no ABXY letters).
        // Paddle indices are known from community DIEM configs.
        // ------------------------------------------------------------------
        constexpr DeviceLayout kG27Layout = {{
            BT(5),         // PaddleLeft
            BT(4),         // PaddleRight
            PV(0),         // DpadUp
            PV(18000),     // DpadDown
            PV(27000),     // DpadLeft
            PV(9000),      // DpadRight
            BT(0),         // "A" (G27 button 1)
            BT(1),         // "B" (G27 button 2)
            BT(2),         // "X" (G27 button 3)
            BT(3),         // "Y" (G27 button 4)
            BT(6),         // Start
            BT(7),         // Select
            UM(),          // LSB (not present on G27)
            UM(),          // RSB (not present on G27)
            UM(),          // Plus
            UM(),          // Minus
            UM(),          // ScrollClick
            UM(),          // ScrollCW
            UM(),          // ScrollCCW
            UM(),          // Xbox
        }};

        // ------------------------------------------------------------------
        // G25 — UNVERIFIED. Very similar to G27 but the D-pad might be
        // buttons instead of a POV hat. Minimal face buttons. Probably
        // works basically like G27 for our purposes; users of this wheel
        // are rare enough that we'll correct if anyone reports issues.
        // ------------------------------------------------------------------
        constexpr DeviceLayout kG25Layout = kG27Layout;

        // ------------------------------------------------------------------
        // Driving Force GT / MOMO / Wingman — UNVERIFIED. Older wheels
        // with minimal controls. Fall back to G27 layout (paddles + basic
        // face buttons). Most slots will be Unmapped.
        // ------------------------------------------------------------------
        constexpr DeviceLayout kDrivingForceLayout = kG27Layout;

        // ------------------------------------------------------------------
        // Device registry. Order matters: first friendly-name substring
        // match wins, so list more-specific names before less-specific.
        // ------------------------------------------------------------------
        struct DeviceEntry
        {
            const char*         nameSubstring; // case-insensitive substring for LogiGetFriendlyProductName
            const char*         label;         // for logs
            const DeviceLayout* layout;
            bool                verified;
        };

        constexpr DeviceEntry kDeviceRegistry[] = {
            { "G923 Racing Wheel for Xbox",        "G923 Xbox",       &kG923XboxLayout,      true  },
            { "G923 Racing Wheel for PlayStation", "G923 PS",         &kG923PSLayout,        false },
            { "G923",                               "G923 (unknown variant)", &kG923XboxLayout, false },
            { "G920",                               "G920",            &kG920Layout,          false },
            { "G29",                                "G29",             &kG29Layout,           false },
            { "G27",                                "G27",             &kG27Layout,           false },
            { "G25",                                "G25",             &kG25Layout,           false },
            { "Driving Force GT",                   "Driving Force GT",&kDrivingForceLayout,  false },
            { "Driving Force",                      "Driving Force",   &kDrivingForceLayout,  false },
            { "MOMO",                               "MOMO",            &kDrivingForceLayout,  false },
            { "Wingman",                            "Wingman",         &kDrivingForceLayout,  false },
        };

        bool ContainsCaseInsensitive(const char* haystack, const char* needle)
        {
            if (!haystack || !needle) return false;
            for (const char* p = haystack; *p; ++p)
            {
                size_t i = 0;
                while (needle[i] && p[i] &&
                       std::tolower(static_cast<unsigned char>(p[i])) ==
                       std::tolower(static_cast<unsigned char>(needle[i])))
                    ++i;
                if (!needle[i]) return true;
            }
            return false;
        }

        // How each Action translates to a Windows input event. Ordered to
        // match the Action enum — array index = Action value.
        // VehicleNative bypasses the keyboard/mouse layer entirely: it
        // resolves a method on `vehicleBaseObject` by name via RTTI and
        // invokes it on the cached player-vehicle pointer. The dispatch is
        // independent of any CP2077 controls-menu binding because no
        // synthetic input event is involved.
        enum class DispatchKind : uint8_t { None, Keyboard, MouseButton, MouseWheel, VehicleNative };

        // Tap = fire DOWN+UP together on rising edge (a quick pulse regardless
        //       of how long the user physically holds). Use for toggles and
        //       one-shot actions — e.g. "press M to open map", "tap MMB for
        //       rear-view camera". Prevents held physical buttons from
        //       triggering the game's long-press behavior (weapon wheel on
        //       held MMB, etc.).
        // Hold = fire DOWN on rise, UP on fall. Use for sustained actions —
        //        handbrake, horn, gunfire, weapon-wheel hold. The virtual
        //        key tracks the wheel button's physical state.
        enum class DispatchMode : uint8_t { Tap, Hold };

        // AnyContext  = dispatch regardless of whether V is on foot or
        //               driving. Reserved for menu-opening actions (Pause,
        //               OpenMap/Journal/Inventory/Phone/Perks/Crafting,
        //               QuickSave), menu-nav fallback actions (Menu*), and
        //               CallVehicle (summon-from-foot is the point).
        // VehicleOnly = suppress on-foot. The bound keyboard keys mean
        //               different things when V is walking (Space=jump,
        //               V=summon, F=interact, G=grenade, etc.), so firing
        //               them off a wheel button produces unintended side-
        //               effects. Default for every action that isn't
        //               explicitly about opening a menu. The in-vehicle
        //               flag is set from the mount event wrappers; see
        //               sources::SetInVehicle.
        enum class ActionScope : uint8_t { AnyContext, VehicleOnly };

        struct DispatchEntry
        {
            DispatchKind kind          = DispatchKind::None;
            DispatchMode mode          = DispatchMode::Tap;
            ActionScope  scope         = ActionScope::VehicleOnly;
            WORD         vk            = 0;    // Keyboard: virtual-key code
            DWORD        mouseDownFlag = 0;    // MouseButton
            DWORD        mouseUpFlag   = 0;
            DWORD        wheelDelta    = 0;    // MouseWheel (signed in low word)
            const char*  label         = "";   // for logging
            // VehicleNative: redscript method name (e.g. "ToggleRadioReceiver")
            // looked up on `vehicleBaseObject` and invoked on the player
            // vehicle. Stored as const char* (not CName) so the table stays
            // constexpr — CName resolution happens lazily at first dispatch.
            // Placed last so existing positional initialisers (K/M/W/KH/etc.)
            // keep compiling without modification.
            const char*  vehicleMethodName = nullptr;
        };

        // Default scope for all constructor helpers is VehicleOnly — on-foot
        // suppression is the rule, AnyContext is the exception. Menu-opening
        // actions (the A-variants) explicitly opt in.
        //
        // Tap-mode constructors (toggles, cycles).
        constexpr DispatchEntry K(WORD vk, const char* lbl)
        {
            return { DispatchKind::Keyboard, DispatchMode::Tap, ActionScope::VehicleOnly, vk, 0, 0, 0, lbl };
        }
        constexpr DispatchEntry M(DWORD down, DWORD up, const char* lbl)
        {
            return { DispatchKind::MouseButton, DispatchMode::Tap, ActionScope::VehicleOnly, 0, down, up, 0, lbl };
        }
        constexpr DispatchEntry W(int delta, const char* lbl)
        {
            return { DispatchKind::MouseWheel, DispatchMode::Tap, ActionScope::VehicleOnly, 0, 0, 0, static_cast<DWORD>(delta), lbl };
        }
        // Hold-mode constructors — sustain while the wheel button is held.
        constexpr DispatchEntry KH(WORD vk, const char* lbl)
        {
            return { DispatchKind::Keyboard, DispatchMode::Hold, ActionScope::VehicleOnly, vk, 0, 0, 0, lbl };
        }
        constexpr DispatchEntry MH(DWORD down, DWORD up, const char* lbl)
        {
            return { DispatchKind::MouseButton, DispatchMode::Hold, ActionScope::VehicleOnly, 0, down, up, 0, lbl };
        }
        // AnyContext variants (fire on foot and in-vehicle). Menu/pause/
        // quicksave/CallVehicle only.
        constexpr DispatchEntry KA(WORD vk, const char* lbl)
        {
            return { DispatchKind::Keyboard, DispatchMode::Tap, ActionScope::AnyContext, vk, 0, 0, 0, lbl };
        }
        // Hold variant of AnyContext keyboard. Used for actions whose CP2077
        // binding has both a tap meaning (briefly press) and a hold meaning
        // (sustained press opens a UI), e.g. T = answer call (tap) / open
        // phone (hold), and where the action is meaningful on foot.
        constexpr DispatchEntry KAH(WORD vk, const char* lbl)
        {
            return { DispatchKind::Keyboard, DispatchMode::Hold, ActionScope::AnyContext, vk, 0, 0, 0, lbl };
        }
        // Direct-method (Tap, VehicleOnly): resolve `methodName` on
        // `vehicleBaseObject` via RTTI and invoke it on the cached player
        // vehicle. CP2077-keybind-immune — no SendInput involved.
        constexpr DispatchEntry VN(const char* methodName, const char* lbl)
        {
            return { DispatchKind::VehicleNative, DispatchMode::Tap, ActionScope::VehicleOnly,
                     0, 0, 0, 0, lbl, methodName };
        }

        // Order MUST match the integer values declared in the Action enum
        // (input_bindings.h). The dispatcher indexes by Action value and
        // expects the entry at index N to describe action N. The grouping
        // comments mirror the enum's so the two are easy to keep aligned.
        constexpr std::array<DispatchEntry, kActionCount> kDispatch = {{
            /* 0  None                */ { DispatchKind::None, DispatchMode::Tap, ActionScope::AnyContext, 0, 0, 0, 0, "None" },

            // --- Driving ----------------------------------------------------
            /* 1  Horn                */ KH('Z',        "Horn (Z, hold)"),
            // Headlights is Hold, not Tap: CP2077's IK_V is shared by four
            // mappings (Vehicle_CycleLights, Vehicle_Siren, CallVehicle,
            // CloseQuickHackPanel). Resolving the right one needs the game's
            // context sampler to see V held across at least one driving-tick,
            // and a 30 ms tap pulse wasn't enough in practice. Mirroring the
            // physical button gives the game the full press duration.
            /* 2  Headlights          */ KH('V',        "Headlights (V, hold)"),
            /* 3  Handbrake           */ KH(VK_SPACE,   "Handbrake (Space, hold)"),
            // Autodrive is Hold, not Tap: CP2077's autoDrive input action
            // requires a sustained G press to engage (verified empirically
            // 2026-04-24 — physical-keyboard tap of G doesn't toggle, but
            // a hold does). The 30 ms tap pulse satisfies most other game
            // actions but not this one. Mirroring the wheel button's
            // physical hold duration onto G makes both gestures work.
            /* 4  Autodrive           */ KH('G',        "Autodrive (G, hold)"),
            // ExitVehicle is Hold, not Tap: CP2077 uses hold-to-exit on F
            // (so you don't punt yourself out mid-drive on a stray press),
            // which means we need the physical wheel button's hold duration
            // to propagate as a real sustained F press. Tap mode would only
            // produce a brief DOWN/UP and never satisfy the hold threshold.
            /* 5  ExitVehicle         */ KH('F',        "ExitVehicle (F, hold)"),
            /* 6  CallVehicle         */ KA('V',        "CallVehicle (V)"),

            // --- Camera -----------------------------------------------------
            /* 7  CameraCycleForward  */ K('Q',         "CameraCycleFwd (Q)"),
            /* 8  CameraCycleBackward */ MH(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, "Rear-view camera (MMB, hold)"),
            /* 9  CameraReset         */ K('C',         "CameraReset (C)"),

            // --- Combat (shooting) -----------------------------------------
            /* 10 ShootPrimary        */ MH(MOUSEEVENTF_LEFTDOWN,  MOUSEEVENTF_LEFTUP,  "ShootPrimary (LMB, hold)"),
            /* 11 ShootSecondary      */ MH(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, "ShootSecondary (RMB, hold)"),
            /* 12 ShootTertiary       */ KH(VK_LCONTROL,"ShootTertiary (LCtrl, hold)"),

            // --- Weapons (selection / cycling) -----------------------------
            /* 13 NextWeapon          */ W(+WHEEL_DELTA,"NextWeapon (MWheelUp)"),
            /* 14 PrevWeapon          */ W(-WHEEL_DELTA,"PrevWeapon (MWheelDown)"),
            /* 15 WeaponSlot1         */ K('1',         "WeaponSlot1 (1)"),
            /* 16 WeaponSlot2         */ K('2',         "WeaponSlot2 (2)"),
            /* 17 SwitchWeapons       */ KH(VK_MENU,    "SwitchWeapons (Alt, hold for wheel)"),
            /* 18 HolsterWeapon       */ K('B',         "HolsterWeapon (B)"),

            // --- Radio ------------------------------------------------------
            // RadioMenu is Hold, not Tap: in CP2077 a brief IK_R tap cycles
            // stations in place, while a sustained hold opens the radio
            // wheel (station-select UI). Mirroring the physical press
            // duration lets both gestures work naturally off a single bind.
            /* 19 RadioMenu           */ KH('R',        "RadioMenu (R, hold)"),
            // RadioNext: direct-method invocation on vehicleBaseObject —
            // no SendInput in the loop, immune to CP2077 keybind changes.
            /* 20 RadioNext           */ VN("NextRadioReceiverStation", "RadioNext (direct)"),

            // --- Gameplay misc ---------------------------------------------
            /* 21 UseConsumable       */ K('X',         "UseConsumable (X)"),
            /* 22 IconicCyberware     */ K('E',         "IconicCyberware (E)"),
            /* 23 QuickSave           */ KA(VK_F5,      "QuickSave (F5)"),

            // --- Menus / fullscreen UI -------------------------------------
            /* 24 OpenMap             */ KA('M',        "OpenMap (M)"),
            /* 25 OpenJournal         */ KA('J',        "OpenJournal (J)"),
            /* 26 OpenInventory       */ KA('I',        "OpenInventory (I)"),
            // OpenPhone is Hold, not Tap: a brief IK_T tap in CP2077 only
            // answers/rejects an incoming call (PhoneInteract / PhoneReject
            // mappings), while a sustained hold opens the phone UI for
            // reading texts and contacts. Mirroring the physical button
            // duration gives both gestures off the same bind.
            /* 27 OpenPhone           */ KAH('T',       "OpenPhone (T, hold)"),
            /* 28 OpenPerks           */ KA('P',        "OpenPerks (P)"),
            /* 29 OpenCrafting        */ KA('K',        "OpenCrafting (K)"),
            /* 30 Pause               */ KA(VK_ESCAPE,  "Pause (Esc)"),

            // --- Menu navigation -------------------------------------------
            /* 31 MenuConfirm         */ KA(VK_RETURN,  "MenuConfirm (Enter)"),
            /* 32 MenuCancel          */ KA(VK_ESCAPE,  "MenuCancel (Esc)"),
            /* 33 MenuUp              */ KA(VK_UP,      "MenuUp (Up arrow)"),
            /* 34 MenuDown            */ KA(VK_DOWN,    "MenuDown (Down arrow)"),
            /* 35 MenuLeft            */ KA(VK_LEFT,    "MenuLeft (Left arrow)"),
            /* 36 MenuRight           */ KA(VK_RIGHT,   "MenuRight (Right arrow)"),
        }};

        // Capture mode (Phase 5c). Armed by BeginCapture, polled by
        // PollLatestCapture. While `armed` is true, OnTick suppresses
        // normal action dispatch and watches for the first qualifying
        // input. The captured string lives in `captured` until it is
        // claimed; PollLatestCapture clears it atomically.
        enum class CaptureKind : uint8_t { None, Button, Axis };

        struct State
        {
            std::mutex              mtx;
            BindingArray            bindings{}; // all-zero = all None
            const DeviceLayout*     layout = &kG923XboxLayout; // default until SetDeviceLayout

            // Edge-detection state, owned by the pump thread.
            uint32_t                prevButtons = 0;
            DWORD                   prevPov     = 0xFFFFFFFF;
            bool                    havePrev    = false;

            CaptureKind             captureKind = CaptureKind::None;
            // Buttons / POV at BeginCapture; rising edges relative to this
            // baseline (rather than the OnTick-rolling prev) are reported.
            // This lets a wheel button that was already held when capture
            // started be ignored until release+repress.
            uint32_t                captureButtonsBase = 0;
            DWORD                   capturePovBase     = 0xFFFFFFFF;
            float                   captureAxesBase[wheel::Axis_kCount] = {0};
            std::string             captured;          // set once, cleared by Poll
        };

        State& S() { static State s; return s; }

        // Log the executable name of the window currently receiving keyboard
        // input. SendInput pushes events to whichever window has focus —
        // after alt-tab, focus transitions can briefly land on another app
        // (overlay, IDE, browser) and our dispatches go there instead of
        // CP2077. Called on every Dispatch rising-edge so a log diff makes
        // the tab-out / focus issue visible.
        void LogForeground(const char* label)
        {
            HWND hwnd = GetForegroundWindow();
            if (!hwnd)
            {
                log::InfoF("[gwheel:bind] foreground during %s: <none>", label);
                return;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            char exe[MAX_PATH] = {};
            if (hProc)
            {
                DWORD sz = MAX_PATH;
                QueryFullProcessImageNameA(hProc, 0, exe, &sz);
                CloseHandle(hProc);
            }
            char title[128] = {};
            GetWindowTextA(hwnd, title, sizeof(title));
            log::InfoF("[gwheel:bind] foreground during %s: hwnd=%p pid=%lu exe=\"%s\" title=\"%s\"",
                       label, (void*)hwnd, pid, exe, title);
        }

        void FireInput(const INPUT& in, const DispatchEntry& e, bool down)
        {
            INPUT local = in;
            // Tag every event with kExtraInfoTag so the LL keyboard hook
            // can distinguish our own injections from G HUB's and pass
            // ours through unmodified.
            if (local.type == INPUT_KEYBOARD)
                local.ki.dwExtraInfo = kbd_hook::kExtraInfoTag;
            else if (local.type == INPUT_MOUSE)
                local.mi.dwExtraInfo = kbd_hook::kExtraInfoTag;
            const UINT n = SendInput(1, &local, sizeof(local));
            if (n != 1)
            {
                log::WarnF("[gwheel:bind] SendInput(%s, %s) returned %u (expected 1)",
                           e.label, down ? "DOWN" : "UP", n);
            }
        }

        // RTTI-resolve `vehicleBaseObject::<methodName>` once and cache the
        // CBaseFunction*. The method names referenced by the dispatch table
        // are static strings so the cache is keyed by pointer identity.
        // Returns nullptr if the lookup fails (logged once per name).
        RED4ext::CBaseFunction* ResolveVehicleMethod(const char* methodName)
        {
            if (!methodName) return nullptr;
            static std::mutex sMtx;
            // Tiny linear cache — there are only a handful of direct-method
            // actions, and a vector or map would be overkill.
            struct Cached { const char* key; RED4ext::CBaseFunction* fn; bool resolved; };
            static std::array<Cached, 16> sCache{};
            std::lock_guard<std::mutex> lk(sMtx);
            for (auto& c : sCache)
            {
                if (c.key == methodName) return c.fn;
                if (!c.key)
                {
                    auto* rtti = RED4ext::CRTTISystem::Get();
                    RED4ext::CBaseFunction* fn = nullptr;
                    if (rtti)
                    {
                        if (auto* cls = rtti->GetClass(RED4ext::CName("vehicleBaseObject")))
                            fn = cls->GetFunction(RED4ext::CName(methodName));
                    }
                    if (!fn)
                        log::WarnF("[gwheel:bind] vehicleBaseObject::%s not found in RTTI — direct-method dispatch will be a no-op",
                                   methodName);
                    else
                        log::InfoF("[gwheel:bind] resolved direct-method vehicleBaseObject::%s = %p",
                                   methodName, fn);
                    c = { methodName, fn, true };
                    return fn;
                }
            }
            log::WarnF("[gwheel:bind] direct-method cache overflow at '%s' — increase sCache size",
                       methodName);
            return nullptr;
        }

        // Invoke the cached vehicleBaseObject method on the player's vehicle.
        // Returns true iff the call dispatched. False on missing vehicle,
        // missing method, or RTTI execution failure (each is logged on the
        // rising edge by the caller so we don't double-log here).
        bool FireVehicleNative(const DispatchEntry& e)
        {
            void* vehicle = vehicle_hook::GetPlayerVehicle();
            if (!vehicle)
            {
                log::DebugF("[gwheel:bind] %s: no player vehicle — skipping direct-method call",
                            e.label);
                return false;
            }
            auto* fn = ResolveVehicleMethod(e.vehicleMethodName);
            if (!fn) return false;

            const bool ok = RED4ext::ExecuteFunction(vehicle, fn, /*aOut=*/nullptr);
            if (!ok)
            {
                log::WarnF("[gwheel:bind] %s: ExecuteFunction returned false (vehicle=%p fn=%p)",
                           e.label, vehicle, fn);
            }
            return ok;
        }

        // Fire a single DOWN or UP event for an action, given the entry.
        void FireOne(const DispatchEntry& e, bool down)
        {
            INPUT in{};
            switch (e.kind)
            {
            case DispatchKind::Keyboard:
                in.type = INPUT_KEYBOARD;
                in.ki.wVk = e.vk;
                in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(e.vk, MAPVK_VK_TO_VSC));
                in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
                FireInput(in, e, down);
                break;
            case DispatchKind::MouseButton:
                in.type = INPUT_MOUSE;
                in.mi.dwFlags = down ? e.mouseDownFlag : e.mouseUpFlag;
                FireInput(in, e, down);
                break;
            case DispatchKind::MouseWheel:
                // Wheel is a pulse event regardless of mode; fire only on DOWN.
                if (down)
                {
                    in.type = INPUT_MOUSE;
                    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
                    in.mi.mouseData = e.wheelDelta;
                    FireInput(in, e, true);
                }
                break;
            case DispatchKind::VehicleNative:
                // Pulse: fire only on rising edge. Direct-method calls don't
                // have a "release" semantic — the vehicle method runs once
                // and the game handles the state transition internally.
                if (down) FireVehicleNative(e);
                break;
            case DispatchKind::None:
            default:
                break;
            }
        }

        void Dispatch(int32_t action, bool rising)
        {
            if (action <= 0 || action >= kActionCount) return;
            const auto& e = kDispatch[static_cast<size_t>(action)];
            if (e.kind == DispatchKind::None) return;

            // On-foot suppression for vehicle-centric actions. CP2077 assigns
            // the same keyboard keys different meanings on foot (V=summon,
            // F=interact, Space=jump, etc.), so dispatching a wheel-bound
            // Headlights / Handbrake / Horn while V is walking produces
            // unintended side-effects. Log only on the rising edge so the
            // suppression is visible but not spammy.
            if (e.scope == ActionScope::VehicleOnly && !sources::InVehicle())
            {
                if (rising)
                    log::DebugF("[gwheel:bind] on-foot: %s suppressed", e.label);
                return;
            }

            if (e.mode == DispatchMode::Tap)
            {
                // Tap: fire DOWN, wait ~2 game frames, fire UP. The gap is
                // essential — CP2077's in-vehicle input sampler polls key
                // state per tick, and DOWN+UP sent back-to-back within the
                // same poll window can be collapsed so the game never sees
                // the key as held. 30 ms covers a 60 Hz frame with margin
                // and is still well under any human-perceptible latency.
                // The pump thread stalls for this interval once per tap,
                // which is fine — taps are user-initiated and rare.
                if (!rising) return;
                LogForeground(e.label);
                FireOne(e, true);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                FireOne(e, false);
                log::InfoF("[gwheel:bind] dispatch action=%d %s TAP", action, e.label);
            }
            else
            {
                // Hold: mirror the physical state.
                if (rising) LogForeground(e.label);
                FireOne(e, rising);
                log::InfoF("[gwheel:bind] dispatch action=%d %s %s",
                           action, e.label, rising ? "DOWN" : "UP");
            }
        }

        bool IsPhysicallyPressed(const DeviceLayout& layout, PhysicalInput p,
                                 uint32_t buttons, DWORD pov)
        {
            const auto& m = layout[p];
            switch (m.source)
            {
            case Source::Button:
                return (buttons & (1u << m.value)) != 0;
            case Source::PovDirection:
                return pov == m.value;
            case Source::Unmapped:
            default:
                return false;
            }
        }

        // Names for PhysicalInput slots, parallel to the enum. Used only for
        // logging so the diagnostic trace reads as "Plus" / "ScrollCW" rather
        // than "input 14" / "input 17".
        constexpr std::array<const char*, kCount> kInputNames = {{
            "PaddleLeft",
            "PaddleRight",
            "DpadUp",
            "DpadDown",
            "DpadLeft",
            "DpadRight",
            "ButtonA",
            "ButtonB",
            "ButtonX",
            "ButtonY",
            "Start",
            "Select",
            "LSB",
            "RSB",
            "Plus",
            "Minus",
            "ScrollClick",
            "ScrollCW",
            "ScrollCCW",
            "Xbox",
        }};

        const char* InputName(int32_t id)
        {
            if (id < 0 || id >= kCount) return "?";
            return kInputNames[static_cast<size_t>(id)];
        }

        const char* ActionLabel(int32_t action)
        {
            if (action < 0 || action >= kActionCount) return "?";
            return kDispatch[static_cast<size_t>(action)].label;
        }
    }

    void SetDeviceLayout(const char* friendlyProductName)
    {
        auto& st = S();
        const DeviceEntry* match = nullptr;
        for (const auto& entry : kDeviceRegistry)
        {
            if (ContainsCaseInsensitive(friendlyProductName, entry.nameSubstring))
            {
                match = &entry;
                break;
            }
        }

        std::lock_guard lk(st.mtx);
        if (match)
        {
            st.layout = match->layout;
            if (match->verified)
            {
                log::InfoF("[gwheel:bind] device layout = %s (verified)", match->label);
            }
            else
            {
                log::WarnF("[gwheel:bind] device layout = %s (UNVERIFIED - bindings may be "
                           "wrong; run tools/input_probe.exe to confirm indices)", match->label);
            }
        }
        else
        {
            st.layout = &kG923XboxLayout;
            log::WarnF("[gwheel:bind] unknown wheel \"%s\"; falling back to G923 Xbox "
                       "layout. Run tools/input_probe.exe to confirm indices.",
                       friendlyProductName ? friendlyProductName : "(null)");
        }
    }

    void ReplaceAll(const BindingArray& bindings)
    {
        auto& st = S();
        {
            std::lock_guard lk(st.mtx);
            st.bindings = bindings;
        }
        // Dump the full binding map on any wholesale replacement so the log
        // shows the user's current assignments after a config-load /
        // settings-push. Makes it trivial to confirm what the UI thinks the
        // binding is vs what the plugin actually received.
        log::Info("[gwheel:bind] binding map (Input -> Action):");
        for (int32_t i = 0; i < kCount; ++i)
        {
            const int32_t a = bindings[static_cast<size_t>(i)];
            log::InfoF("[gwheel:bind]   %-12s -> %d %s",
                       InputName(i), a, ActionLabel(a));
        }
    }

    void Set(int32_t inputId, int32_t action)
    {
        if (inputId < 0 || inputId >= kCount) return;
        if (action  < 0 || action  >= kActionCount) action = 0;
        auto& st = S();
        {
            std::lock_guard lk(st.mtx);
            st.bindings[static_cast<size_t>(inputId)] = action;
        }
        log::InfoF("[gwheel:bind] set %-12s -> %d %s",
                   InputName(inputId), action, ActionLabel(action));
    }

    int32_t Get(int32_t inputId)
    {
        if (inputId < 0 || inputId >= kCount) return 0;
        auto& st = S();
        std::lock_guard lk(st.mtx);
        return st.bindings[static_cast<size_t>(inputId)];
    }


    void OnTick(const sources::Frame& frame)
    {
        auto& st = S();

        const uint32_t buttons = frame.digital.buttons;
        // digital.pov is the DInput POV raw in the low 16 bits. 0xFFFF = center.
        // We compare directly against direction values (0, 9000, 18000, 27000)
        // which never collide with 0xFFFF.
        const DWORD    pov     = static_cast<DWORD>(frame.digital.pov);

        // Phase 5c: capture-mode handling runs ahead of the action dispatcher
        // and runs even when input is globally disabled, so the bind-UI can
        // capture inputs while config.input.enabled is false. While
        // captureKind != None we suppress dispatch and watch for the first
        // qualifying transition; once captured we leave the slot armed
        // until PollLatestCapture / CancelCapture (the UI side closes the
        // window so subsequent presses don't overwrite the capture).
        {
            std::lock_guard lk(st.mtx);
            if (st.captureKind != CaptureKind::None && st.captured.empty())
            {
                if (st.captureKind == CaptureKind::Button)
                {
                    // First button bit that's high now AND was low at
                    // BeginCapture wins. Buttons already held at capture
                    // start are ignored until release+repress, which we
                    // model implicitly here by comparing against the base.
                    const uint32_t rising = buttons & ~st.captureButtonsBase;
                    if (rising != 0)
                    {
                        for (int i = 0; i < 32; ++i)
                        {
                            if (rising & (1u << i))
                            {
                                char buf[32];
                                std::snprintf(buf, sizeof(buf), "wheel:%d", i);
                                st.captured = buf;
                                break;
                            }
                        }
                    }
                    if (st.captured.empty() &&
                        pov != 0xFFFF && pov != 0xFFFFFFFF &&
                        pov != st.capturePovBase)
                    {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "wheel-pov:%lu", static_cast<unsigned long>(pov));
                        st.captured = buf;
                    }
                }
                else if (st.captureKind == CaptureKind::Axis)
                {
                    constexpr float kAxisDeviationThreshold = 0.25f;
                    const wheel::Snapshot snap = wheel::CurrentSnapshot();
                    for (int i = 0; i < wheel::Axis_kCount; ++i)
                    {
                        const float delta = snap.axes[i] - st.captureAxesBase[i];
                        if (std::fabs(delta) >= kAxisDeviationThreshold)
                        {
                            char buf[64];
                            std::snprintf(buf, sizeof(buf), "wheel-axis:%s",
                                          wheel::AxisName(static_cast<wheel::Axis>(i)));
                            st.captured = buf;
                            log::InfoF("[gwheel:capture] axis caught: %s (delta=%.2f)",
                                       buf, delta);
                            break;
                        }
                    }
                }

                if (!st.captured.empty() && st.captureKind == CaptureKind::Button)
                {
                    log::InfoF("[gwheel:capture] button caught: %s", st.captured.c_str());
                }
            }

            if (st.captureKind != CaptureKind::None)
            {
                // Update edge-prev so the dispatcher resumes cleanly when
                // capture ends (stale prev would fire spurious edges on
                // the first tick after capture release).
                st.prevButtons = buttons;
                st.prevPov     = pov;
                st.havePrev    = true;
                return;
            }
        }

        if (!config::Current().input.enabled) return;

        if (!st.havePrev)
        {
            st.prevButtons = buttons;
            st.prevPov     = pov;
            st.havePrev    = true;
            return;
        }

        // Every PhysicalInput falls through to the user's Mod Settings
        // binding. An earlier version hard-overrode the D-pad and A to
        // MenuUp/Down/Left/Right/MenuConfirm on the theory that arrow
        // keys + Enter were silent in CP2077 gameplay, but CP2077 actually
        // binds the arrow keys as secondary vehicle controls (Up/Down =
        // accelerate/decelerate, Left/Right = steer), so the override
        // drove the car whenever the user pressed the D-pad — even if they
        // had set the binding to None. The defaults for D-pad / A are now
        // declared in gwheel_settings.reds as Menu* actions, so menu-nav
        // out of the box is preserved while "None" truly means None.

        // Snapshot the bindings and layout once per tick to avoid holding
        // the lock across SendInput calls.
        BindingArray bindings;
        const DeviceLayout* layout;
        {
            std::lock_guard lk(st.mtx);
            bindings = st.bindings;
            layout   = st.layout;
        }

        for (int32_t i = 0; i < kCount; ++i)
        {
            const PhysicalInput p = static_cast<PhysicalInput>(i);
            const bool pressed    = IsPhysicallyPressed(*layout, p, buttons, pov);
            const bool wasPressed = IsPhysicallyPressed(*layout, p, st.prevButtons, st.prevPov);
            if (pressed != wasPressed)
            {
                const int32_t action = bindings[static_cast<size_t>(i)];

                // Edge log: always fires on any physical state change, even
                // when the binding is None. Lets a "press every button" test
                // prove what the plugin saw physically vs what it would
                // dispatch, independent of whether the game reacts.
                const auto& m = (*layout)[i];
                const char* srcKind = (m.source == Source::Button)       ? "btn"
                                    : (m.source == Source::PovDirection) ? "pov"
                                                                         : "unmapped";
                log::InfoF("[gwheel:edge] %-12s (%s %u) %s -> action=%d %s",
                           InputName(i), srcKind, m.value,
                           pressed ? "PRESS" : "RELEASE",
                           action, ActionLabel(action));

                Dispatch(action, pressed);
            }
        }

        st.prevButtons = buttons;
        st.prevPov     = pov;
    }

    void BeginCapture(const char* typeHint)
    {
        auto& st = S();
        const wheel::Snapshot snap = wheel::CurrentSnapshot();
        std::lock_guard lk(st.mtx);
        st.captured.clear();
        if (typeHint && std::strcmp(typeHint, "axis") == 0)
        {
            st.captureKind = CaptureKind::Axis;
        }
        else
        {
            st.captureKind = CaptureKind::Button;
        }
        // Latch current state so already-held buttons / already-deflected
        // axes don't immediately count as a capture.
        st.captureButtonsBase = snap.buttons;
        st.capturePovBase     = snap.pov;
        for (int i = 0; i < wheel::Axis_kCount; ++i)
            st.captureAxesBase[i] = snap.axes[i];
        log::InfoF("[gwheel:capture] BeginCapture(%s)",
                   st.captureKind == CaptureKind::Axis ? "axis" : "button");
    }

    std::string PollLatestCapture()
    {
        auto& st = S();
        std::lock_guard lk(st.mtx);
        if (st.captured.empty()) return {};
        std::string out = st.captured;
        st.captured.clear();
        // Disarm capture once a value is delivered; the listening UI is
        // responsible for re-arming if multi-shot capture is needed.
        st.captureKind = CaptureKind::None;
        log::InfoF("[gwheel:capture] PollLatestCapture -> %s", out.c_str());
        return out;
    }

    void CancelCapture()
    {
        auto& st = S();
        std::lock_guard lk(st.mtx);
        if (st.captureKind == CaptureKind::None && st.captured.empty()) return;
        st.captureKind = CaptureKind::None;
        st.captured.clear();
        log::Info("[gwheel:capture] CancelCapture");
    }

    bool IsCapturing()
    {
        auto& st = S();
        std::lock_guard lk(st.mtx);
        return st.captureKind != CaptureKind::None;
    }
}
