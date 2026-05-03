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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gwheel::input_bindings
{
    namespace
    {
        // ---------------- Action enum (internal) -------------------------------
        //
        // The Action enum is the internal dispatch identifier. The user-facing
        // name strings (the keys redscript pushes through GWheel_SetActionBinding)
        // are stored in kActionNames at parallel indices. The enum integer values
        // are NOT persisted anywhere — only the action-name string is — so this
        // enum may be reordered freely without breaking saved configs.
        //
        // CameraCycleBackward is the historical C++ name for the "rear-view
        // camera" tap action. Redscript calls it "RearViewCamera"; the name
        // table maps that string to this enum slot so dispatch resolves.
        enum Action : int32_t
        {
            None = 0,

            Horn,
            Headlights,
            Handbrake,
            Autodrive,
            ExitVehicle,
            CallVehicle,

            CameraCycleForward,
            CameraCycleBackward,    // user-facing name: "RearViewCamera"
            CameraReset,

            ShootPrimary,
            ShootSecondary,
            ShootTertiary,

            NextWeapon,
            PrevWeapon,
            WeaponSlot1,
            WeaponSlot2,
            SwitchWeapons,
            HolsterWeapon,

            RadioMenu,
            RadioNext,

            UseConsumable,
            IconicCyberware,
            QuickSave,

            OpenMap,
            OpenJournal,
            OpenInventory,
            OpenPhone,
            OpenPerks,
            OpenCrafting,
            Pause,

            MenuConfirm,
            MenuCancel,
            MenuUp,
            MenuDown,
            MenuLeft,
            MenuRight,

            kActionCount
        };

        // Order MUST match the Action enum positions one-to-one. Strings are
        // the keys redscript uses; lookups go through ActionFromName.
        constexpr std::array<const char*, kActionCount> kActionNames = {{
            "None",
            "Horn",
            "Headlights",
            "Handbrake",
            "Autodrive",
            "ExitVehicle",
            "CallVehicle",
            "CameraCycleForward",
            "RearViewCamera",
            "CameraReset",
            "ShootPrimary",
            "ShootSecondary",
            "ShootTertiary",
            "NextWeapon",
            "PrevWeapon",
            "WeaponSlot1",
            "WeaponSlot2",
            "SwitchWeapons",
            "HolsterWeapon",
            "RadioMenu",
            "RadioNext",
            "UseConsumable",
            "IconicCyberware",
            "QuickSave",
            "OpenMap",
            "OpenJournal",
            "OpenInventory",
            "OpenPhone",
            "OpenPerks",
            "OpenCrafting",
            "Pause",
            "MenuConfirm",
            "MenuCancel",
            "MenuUp",
            "MenuDown",
            "MenuLeft",
            "MenuRight",
        }};

        int32_t ActionFromName(const std::string& name)
        {
            for (int32_t i = 0; i < kActionCount; ++i)
                if (name == kActionNames[i]) return i;
            return Action::None;
        }

        // ---------------- Dispatch table --------------------------------------
        //
        // How each Action translates to a Windows input event. Indexed by
        // Action value. VehicleNative bypasses keyboard/mouse entirely: it
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
        //               driving. Reserved for menu-opening actions and the
        //               CallVehicle summon-from-foot affordance.
        // VehicleOnly = suppress on-foot. CP2077's bound keyboard keys mean
        //               different things when V is walking (Space=jump,
        //               V=summon, F=interact, G=grenade, etc.), so firing
        //               them off a wheel button produces unintended side-
        //               effects. Default for every action that isn't
        //               explicitly about opening a menu.
        enum class ActionScope : uint8_t { AnyContext, VehicleOnly };

        struct DispatchEntry
        {
            DispatchKind kind          = DispatchKind::None;
            DispatchMode mode          = DispatchMode::Tap;
            ActionScope  scope         = ActionScope::VehicleOnly;
            WORD         vk            = 0;
            DWORD        mouseDownFlag = 0;
            DWORD        mouseUpFlag   = 0;
            DWORD        wheelDelta    = 0;
            const char*  label         = "";
            const char*  vehicleMethodName = nullptr;
        };

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
        constexpr DispatchEntry KH(WORD vk, const char* lbl)
        {
            return { DispatchKind::Keyboard, DispatchMode::Hold, ActionScope::VehicleOnly, vk, 0, 0, 0, lbl };
        }
        constexpr DispatchEntry MH(DWORD down, DWORD up, const char* lbl)
        {
            return { DispatchKind::MouseButton, DispatchMode::Hold, ActionScope::VehicleOnly, 0, down, up, 0, lbl };
        }
        constexpr DispatchEntry KA(WORD vk, const char* lbl)
        {
            return { DispatchKind::Keyboard, DispatchMode::Tap, ActionScope::AnyContext, vk, 0, 0, 0, lbl };
        }
        constexpr DispatchEntry KAH(WORD vk, const char* lbl)
        {
            return { DispatchKind::Keyboard, DispatchMode::Hold, ActionScope::AnyContext, vk, 0, 0, 0, lbl };
        }
        constexpr DispatchEntry VN(const char* methodName, const char* lbl)
        {
            return { DispatchKind::VehicleNative, DispatchMode::Tap, ActionScope::VehicleOnly,
                     0, 0, 0, 0, lbl, methodName };
        }

        // Order MUST match the Action enum, indexed by Action value.
        constexpr std::array<DispatchEntry, kActionCount> kDispatch = {{
            /* 0  None                */ { DispatchKind::None, DispatchMode::Tap, ActionScope::AnyContext, 0, 0, 0, 0, "None" },

            /* 1  Horn                */ KH('Z',        "Horn (Z, hold)"),
            // Headlights is Hold, not Tap: CP2077's IK_V is shared by four
            // mappings (Vehicle_CycleLights, Vehicle_Siren, CallVehicle,
            // CloseQuickHackPanel). A 30 ms tap pulse wasn't enough in
            // practice; mirroring the physical button gives the game the
            // full press duration to resolve the right meaning.
            /* 2  Headlights          */ KH('V',        "Headlights (V, hold)"),
            /* 3  Handbrake           */ KH(VK_SPACE,   "Handbrake (Space, hold)"),
            // Autodrive needs a sustained G press; tap doesn't engage.
            /* 4  Autodrive           */ KH('G',        "Autodrive (G, hold)"),
            // ExitVehicle uses hold-to-exit on F.
            /* 5  ExitVehicle         */ KH('F',        "ExitVehicle (F, hold)"),
            /* 6  CallVehicle         */ KA('V',        "CallVehicle (V)"),

            /* 7  CameraCycleForward  */ K('Q',         "CameraCycleFwd (Q)"),
            /* 8  CameraCycleBackward */ MH(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, "Rear-view camera (MMB, hold)"),
            /* 9  CameraReset         */ K('C',         "CameraReset (C)"),

            /* 10 ShootPrimary        */ MH(MOUSEEVENTF_LEFTDOWN,  MOUSEEVENTF_LEFTUP,  "ShootPrimary (LMB, hold)"),
            /* 11 ShootSecondary      */ MH(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, "ShootSecondary (RMB, hold)"),
            /* 12 ShootTertiary       */ KH(VK_LCONTROL,"ShootTertiary (LCtrl, hold)"),

            /* 13 NextWeapon          */ W(+WHEEL_DELTA,"NextWeapon (MWheelUp)"),
            /* 14 PrevWeapon          */ W(-WHEEL_DELTA,"PrevWeapon (MWheelDown)"),
            /* 15 WeaponSlot1         */ K('1',         "WeaponSlot1 (1)"),
            /* 16 WeaponSlot2         */ K('2',         "WeaponSlot2 (2)"),
            /* 17 SwitchWeapons       */ KH(VK_MENU,    "SwitchWeapons (Alt, hold for wheel)"),
            /* 18 HolsterWeapon       */ K('B',         "HolsterWeapon (B)"),

            // RadioMenu: tap cycles stations, hold opens the radio wheel —
            // mirror the press duration so both gestures work.
            /* 19 RadioMenu           */ KH('R',        "RadioMenu (R, hold)"),
            // RadioNext: direct-method call on vehicleBaseObject, immune
            // to CP2077 keybind changes.
            /* 20 RadioNext           */ VN("NextRadioReceiverStation", "RadioNext (direct)"),

            /* 21 UseConsumable       */ K('X',         "UseConsumable (X)"),
            /* 22 IconicCyberware     */ K('E',         "IconicCyberware (E)"),
            /* 23 QuickSave           */ KA(VK_F5,      "QuickSave (F5)"),

            /* 24 OpenMap             */ KA('M',        "OpenMap (M)"),
            /* 25 OpenJournal         */ KA('J',        "OpenJournal (J)"),
            /* 26 OpenInventory       */ KA('I',        "OpenInventory (I)"),
            // OpenPhone: tap answers a call, hold opens the phone UI.
            /* 27 OpenPhone           */ KAH('T',       "OpenPhone (T, hold)"),
            /* 28 OpenPerks           */ KA('P',        "OpenPerks (P)"),
            /* 29 OpenCrafting        */ KA('K',        "OpenCrafting (K)"),
            /* 30 Pause               */ KA(VK_ESCAPE,  "Pause (Esc)"),

            /* 31 MenuConfirm         */ KA(VK_RETURN,  "MenuConfirm (Enter)"),
            /* 32 MenuCancel          */ KA(VK_ESCAPE,  "MenuCancel (Esc)"),
            /* 33 MenuUp              */ KA(VK_UP,      "MenuUp (Up arrow)"),
            /* 34 MenuDown            */ KA(VK_DOWN,    "MenuDown (Down arrow)"),
            /* 35 MenuLeft            */ KA(VK_LEFT,    "MenuLeft (Left arrow)"),
            /* 36 MenuRight           */ KA(VK_RIGHT,   "MenuRight (Right arrow)"),
        }};

        const char* ActionLabel(int32_t action)
        {
            if (action < 0 || action >= kActionCount) return "?";
            return kDispatch[static_cast<size_t>(action)].label;
        }

        // ---------------- Parsed bindings -------------------------------------
        //
        // Each ActionEntry holds the parsed sources for one action. The user's
        // raw token string is parsed once at SetActionBinding time so OnTick
        // doesn't pay the parsing cost on every pump tick (250 Hz).
        //
        // Only wheel-button and wheel-pov tokens reach this layer. Axis
        // tokens are handled by axis_resolver. Keyboard / mouse / gamepad
        // tokens are skipped: the user is already physically pressing the
        // key, so re-injecting it would create a feedback loop.
        //
        // Multi-source: a single binding string can hold multiple `|`-delimited
        // tokens (cap 2 from the UI side). All sources OR together for press
        // detection — pressing either bound paddle fires the action.
        struct ParsedSource
        {
            enum class Kind : uint8_t { WheelButton, WheelPov };
            Kind     kind  = Kind::WheelButton;
            uint32_t value = 0;  // button index OR POV degrees
        };

        struct ActionEntry
        {
            int32_t                   action = Action::None;
            std::vector<ParsedSource> sources;
        };

        // ---------------- State -----------------------------------------------

        enum class CaptureKind : uint8_t { None, Button, Axis };

        struct State
        {
            std::mutex mtx;

            // Action-keyed bindings, parsed once at update time.
            std::unordered_map<std::string, ActionEntry> bindings;

            // Edge-detection state, owned by the pump thread.
            uint32_t prevButtons = 0;
            DWORD    prevPov     = 0xFFFFFFFF;
            bool     havePrev    = false;

            // Capture mode for the listening UI.
            CaptureKind captureKind        = CaptureKind::None;
            uint32_t    captureButtonsBase = 0;
            DWORD       capturePovBase     = 0xFFFFFFFF;
            float       captureAxesBase[wheel::Axis_kCount] = {0};
            std::string captured;

            // Axis-capture leadership tracking. Picking the first axis to
            // cross threshold loses to noise: a foot resting on the throttle
            // pedal can swing lY by 0.5 in a single frame and beat any
            // intentional wheel rotation. Instead we track the cumulative
            // max |delta| per axis across the entire capture, name the axis
            // with the highest cumulative max as the "leader," and only
            // commit once that leader has held its lead for a settling
            // window (so a brief blip doesn't lock in the wrong choice).
            float       captureAxisMax[wheel::Axis_kCount] = {0};
            int         captureLeader        = -1;  // -1 = no leader yet
            uint64_t    captureTickCount     = 0;
            uint64_t    captureLeaderSince   = 0;   // tick when leader took over
        };

        State& S() { static State s; return s; }

        // ---------------- Token parser ----------------------------------------
        //
        // Tagged tokens recognised here:
        //   "wheel:N"       — DInput button index N (decimal)
        //   "wheel-pov:DEG" — POV[0] direction in raw degrees (0/9000/18000/27000)
        // Anything else (axis, keyboard, mouse, gamepad, empty, malformed)
        // is silently ignored.
        bool ParseToken(const char* data, size_t len, ParsedSource& out)
        {
            constexpr const char btnPrefix[] = "wheel:";
            constexpr const char povPrefix[] = "wheel-pov:";
            constexpr size_t btnLen = sizeof(btnPrefix) - 1;
            constexpr size_t povLen = sizeof(povPrefix) - 1;

            if (len > btnLen && std::memcmp(data, btnPrefix, btnLen) == 0
                // Disambiguate from "wheel-axis:" / "wheel-pov:": the next
                // char after "wheel:" must be a digit.
                && std::isdigit(static_cast<unsigned char>(data[btnLen])))
            {
                std::string s(data + btnLen, len - btnLen);
                char* endp = nullptr;
                long v = std::strtol(s.c_str(), &endp, 10);
                if (endp == s.c_str()) return false;
                if (v < 0 || v > 31) return false;
                out.kind = ParsedSource::Kind::WheelButton;
                out.value = static_cast<uint32_t>(v);
                return true;
            }

            if (len > povLen && std::memcmp(data, povPrefix, povLen) == 0)
            {
                std::string s(data + povLen, len - povLen);
                char* endp = nullptr;
                long v = std::strtol(s.c_str(), &endp, 10);
                if (endp == s.c_str()) return false;
                out.kind = ParsedSource::Kind::WheelPov;
                out.value = static_cast<uint32_t>(v);
                return true;
            }

            return false;
        }

        // Walk pipe-delimited tokens. Empty tokens (consecutive pipes,
        // leading/trailing) are skipped.
        template <typename F>
        void ForEachToken(const std::string& s, F&& fn)
        {
            size_t i = 0;
            while (i < s.size())
            {
                size_t j = s.find('|', i);
                if (j == std::string::npos) j = s.size();
                if (j > i) fn(s.data() + i, j - i);
                i = j + 1;
            }
        }

        // ---------------- Dispatch -------------------------------------------

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

        RED4ext::CBaseFunction* ResolveVehicleMethod(const char* methodName)
        {
            if (!methodName) return nullptr;
            static std::mutex sMtx;
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
                if (down)
                {
                    in.type = INPUT_MOUSE;
                    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
                    in.mi.mouseData = e.wheelDelta;
                    FireInput(in, e, true);
                }
                break;
            case DispatchKind::VehicleNative:
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

            if (e.scope == ActionScope::VehicleOnly && !sources::InVehicle())
            {
                if (rising)
                    log::DebugF("[gwheel:bind] on-foot: %s suppressed", e.label);
                return;
            }

            if (e.mode == DispatchMode::Tap)
            {
                // 30 ms separates DOWN and UP so CP2077's per-tick input
                // sampler sees the key as held across at least one frame.
                if (!rising) return;
                LogForeground(e.label);
                FireOne(e, true);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                FireOne(e, false);
                log::InfoF("[gwheel:bind] dispatch action=%d %s TAP", action, e.label);
            }
            else
            {
                if (rising) LogForeground(e.label);
                FireOne(e, rising);
                log::InfoF("[gwheel:bind] dispatch action=%d %s %s",
                           action, e.label, rising ? "DOWN" : "UP");
            }
        }

        bool IsSourceActive(const ParsedSource& src, uint32_t buttons, DWORD pov)
        {
            switch (src.kind)
            {
            case ParsedSource::Kind::WheelButton:
                return (buttons & (1u << src.value)) != 0;
            case ParsedSource::Kind::WheelPov:
                return pov == src.value;
            }
            return false;
        }

        bool AnySourceActive(const std::vector<ParsedSource>& sources,
                             uint32_t buttons, DWORD pov)
        {
            for (const auto& s : sources)
                if (IsSourceActive(s, buttons, pov)) return true;
            return false;
        }
    }

    // ---------------- Public API ------------------------------------------

    void SetActionBinding(const std::string& actionName, const std::string& value)
    {
        if (actionName.empty()) return;
        const int32_t action = ActionFromName(actionName);
        if (action == Action::None)
        {
            // Action names that don't appear in kActionNames are still valid
            // axis-only keys (SteerAxis / ThrottleAxis / BrakeAxis) — those
            // route through axis_resolver, not here. Silently ignore so the
            // shared SetActionBinding native can fan out to both modules.
            return;
        }

        auto& st = S();
        std::lock_guard lk(st.mtx);

        if (value.empty())
        {
            st.bindings.erase(actionName);
            return;
        }

        ActionEntry entry;
        entry.action = action;

        // Walk all `|`-delimited tokens. Wheel-button / wheel-pov tokens
        // become ParsedSource entries; axis / keyboard tokens are skipped
        // (handled elsewhere or not handled at all).
        ForEachToken(value, [&](const char* tok, size_t len) {
            ParsedSource src;
            if (ParseToken(tok, len, src))
                entry.sources.push_back(src);
        });

        st.bindings[actionName] = entry;

        if (!entry.sources.empty())
        {
            log::InfoF("[gwheel:bind] %s -> %zu source(s) (%s)",
                       actionName.c_str(), entry.sources.size(),
                       ActionLabel(action));
        }
        else
        {
            log::DebugF("[gwheel:bind] %s -> '%s' (no wheel source; ignored at button layer)",
                        actionName.c_str(), value.c_str());
        }
    }

    void Clear()
    {
        auto& st = S();
        std::lock_guard lk(st.mtx);
        st.bindings.clear();
    }

    // Synthesize a VK_F24 keypress to wake the listening UI when capture
    // completes. The redscript-side OnKeyBindingEvent handler recognizes
    // n"IK_F24" while listening, drains the captured value via the input
    // provider, commits it, and exits listening.
    //
    // Why this and not a DelayCallback poll loop: CP2077's in-game pause
    // menu freezes the DelaySystem dispatcher entirely, even with
    // isAffectedByTimeDilation=false. Keyboard events, by contrast, do
    // dispatch during pause (Esc / arrow keys / Enter all work). So a
    // synthetic keypress is the most reliable signal channel from C++
    // (running on its own pump thread) into redscript.
    //
    // Tagged with kbd_hook::kExtraInfoTag so the LL keyboard hook lets
    // it pass through (the hook would otherwise filter our own SendInput
    // events the same way it filters G HUB's).
    void FireCaptureWakeup()
    {
        INPUT ev[2] = {};
        ev[0].type           = INPUT_KEYBOARD;
        ev[0].ki.wVk         = VK_F24;
        ev[0].ki.dwExtraInfo = kbd_hook::kExtraInfoTag;
        ev[1].type           = INPUT_KEYBOARD;
        ev[1].ki.wVk         = VK_F24;
        ev[1].ki.dwFlags     = KEYEVENTF_KEYUP;
        ev[1].ki.dwExtraInfo = kbd_hook::kExtraInfoTag;
        SendInput(2, ev, sizeof(INPUT));
    }

    void OnTick(const sources::Frame& frame)
    {
        auto& st = S();

        const uint32_t buttons = frame.digital.buttons;
        const DWORD    pov     = static_cast<DWORD>(frame.digital.pov);

        // Tracks whether THIS tick filled the captured slot (false→true
        // transition) and whether capture is still armed. Both are set
        // inside the lock so we can release the mutex before SendInput.
        bool captureWokeThisTick = false;
        bool captureStillArmed   = false;

        // Capture-mode handling runs ahead of the action dispatcher and
        // runs even when input is globally disabled, so the bind UI can
        // capture inputs while config.input.enabled is false.
        {
            std::lock_guard lk(st.mtx);
            const bool wasCapturedBefore = !st.captured.empty();
            if (st.captureKind != CaptureKind::None && st.captured.empty())
            {
                if (st.captureKind == CaptureKind::Button)
                {
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
                    // Tunable: how far an axis must move from its baseline
                    // to even be considered. 0.25 is comfortable for both
                    // a half-pressed pedal and a ~50° turn on a 180° wheel.
                    constexpr float    kAxisDeviationThreshold = 0.25f;
                    // Tunable: how many pump ticks (~250 Hz) the leader must
                    // hold its lead before we commit. ~50 ticks = 200 ms.
                    // Long enough to outlast a transient blip, short enough
                    // not to feel laggy.
                    constexpr uint64_t kLeaderStabilityTicks   = 50;

                    const wheel::Snapshot snap = wheel::CurrentSnapshot();
                    ++st.captureTickCount;

                    // Update cumulative max |delta| per axis. Find the
                    // current leader (axis with the largest cumulative max
                    // that is also above threshold).
                    int   leaderIdx = -1;
                    float leaderMax = 0.f;
                    for (int i = 0; i < wheel::Axis_kCount; ++i)
                    {
                        const float d = std::fabs(snap.axes[i] - st.captureAxesBase[i]);
                        if (d > st.captureAxisMax[i]) st.captureAxisMax[i] = d;
                        if (st.captureAxisMax[i] >= kAxisDeviationThreshold &&
                            st.captureAxisMax[i] > leaderMax)
                        {
                            leaderIdx = i;
                            leaderMax = st.captureAxisMax[i];
                        }
                    }

                    // Reset the stability counter every time a new axis
                    // takes the lead. The user is still moving things; wait
                    // for the dust to settle.
                    if (leaderIdx != st.captureLeader)
                    {
                        st.captureLeader      = leaderIdx;
                        st.captureLeaderSince = st.captureTickCount;
                    }

                    if (st.captureLeader >= 0 &&
                        st.captureTickCount - st.captureLeaderSince >= kLeaderStabilityTicks)
                    {
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "wheel-axis:%s",
                                      wheel::AxisName(static_cast<wheel::Axis>(st.captureLeader)));
                        st.captured = buf;
                        log::InfoF("[gwheel:capture] axis caught: %s (maxDelta=%.2f, stable for %llu ticks)",
                                   buf, leaderMax,
                                   static_cast<unsigned long long>(
                                       st.captureTickCount - st.captureLeaderSince));
                    }
                }

                if (!st.captured.empty() && st.captureKind == CaptureKind::Button)
                {
                    log::InfoF("[gwheel:capture] button caught: %s", st.captured.c_str());
                }
            }
            captureWokeThisTick = !wasCapturedBefore && !st.captured.empty();
            captureStillArmed   = (st.captureKind != CaptureKind::None);

            if (captureStillArmed)
            {
                // Update edge-prev so the dispatcher resumes cleanly when
                // capture ends (stale prev would fire spurious edges on
                // the first tick after capture release).
                st.prevButtons = buttons;
                st.prevPov     = pov;
                st.havePrev    = true;
            }
        }

        // Mutex released. Fire the wake-up keypress outside the lock so we
        // never hold the mutex across SendInput (which can block briefly).
        if (captureWokeThisTick)
        {
            FireCaptureWakeup();
        }

        // If we were inside capture mode, the dispatcher must not run
        // (suppress wheel-button → game-action dispatch during binding).
        if (captureStillArmed)
        {
            return;
        }

        if (!config::Current().input.enabled) return;

        if (!st.havePrev)
        {
            std::lock_guard lk(st.mtx);
            st.prevButtons = buttons;
            st.prevPov     = pov;
            st.havePrev    = true;
            return;
        }

        // Snapshot the bindings so we don't hold the lock across SendInput
        // (Dispatch can sleep 30 ms for tap actions). The map is small
        // enough that copying once per tick is cheap.
        std::vector<std::pair<std::string, ActionEntry>> snapshot;
        uint32_t prevButtons;
        DWORD    prevPov;
        {
            std::lock_guard lk(st.mtx);
            snapshot.reserve(st.bindings.size());
            for (const auto& kv : st.bindings) snapshot.push_back(kv);
            prevButtons = st.prevButtons;
            prevPov     = st.prevPov;
        }

        for (const auto& [name, entry] : snapshot)
        {
            if (entry.sources.empty()) continue;
            const bool pressed    = AnySourceActive(entry.sources, buttons,     pov);
            const bool wasPressed = AnySourceActive(entry.sources, prevButtons, prevPov);
            if (pressed != wasPressed)
            {
                log::InfoF("[gwheel:edge] %s (%zu src) %s -> action=%d %s",
                           name.c_str(), entry.sources.size(),
                           pressed ? "PRESS" : "RELEASE",
                           entry.action, ActionLabel(entry.action));
                Dispatch(entry.action, pressed);
            }
        }

        {
            std::lock_guard lk(st.mtx);
            st.prevButtons = buttons;
            st.prevPov     = pov;
        }
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
        st.captureButtonsBase = snap.buttons;
        st.capturePovBase     = snap.pov;
        for (int i = 0; i < wheel::Axis_kCount; ++i)
        {
            st.captureAxesBase[i] = snap.axes[i];
            st.captureAxisMax[i]  = 0.f;
        }
        st.captureLeader      = -1;
        st.captureTickCount   = 0;
        st.captureLeaderSince = 0;
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
