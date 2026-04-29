#pragma once

#include "sources.h"

#include <array>
#include <cstdint>
#include <string>

namespace gwheel::input_bindings
{
    // Stable integer IDs for each physical control on the wheel. Order is
    // locked by the config.json schema and by the field order in
    // gwheel_settings.reds — do NOT renumber. New controls append at the
    // end. Count (kCount) is updated as we go.
    enum PhysicalInput : int32_t
    {
        PaddleLeft = 0,
        PaddleRight,
        DpadUp,
        DpadDown,
        DpadLeft,
        DpadRight,
        ButtonA,
        ButtonB,
        ButtonX,
        ButtonY,
        Start,
        Select,
        LSB,
        RSB,
        Plus,
        Minus,
        ScrollClick,
        ScrollCW,
        ScrollCCW,
        Xbox,
        kCount
    };

    // Curated action set. Every value here dispatches to a specific Windows
    // virtual-key or mouse event via SendInput. Keep in sync with the
    // `GWheelAction` enum in gwheel_settings.reds (same values, same order)
    // so Mod Settings dropdown indices round-trip correctly.
    //
    // CameraCycleBackward is actually "rear-view camera" (MMB tap, shows
    // what's behind you) despite the historical name. It's a tap action;
    // holding the physical button does not hold MMB, because the game
    // treats held MMB as tag / weapon wheel which is not what we want.
    // Order is grouped by category (driving / camera / combat / weapons /
    // radio / gameplay / menus / menu-nav) for dropdown legibility — Mod
    // Settings displays enum members in declaration order. Reordering this
    // enum is a *breaking* change for users' persisted bindings: Mod
    // Settings stores selections by integer value, so any reorder remaps
    // every saved binding to whatever sits at its old position. Don't
    // reorder casually. If you must, communicate it as a forced re-bind.
    //
    // RadioToggle and SirenToggle used to live as direct-method actions
    // and were dropped: the radio-receiver toggle works but CP2077 doesn't
    // sync the rev-strip / visualizer state to programmatic radio-state
    // changes, and ToggleSiren returns success but the game's siren state
    // machine ignores direct-method invocation. RadioNext (direct) stays
    // because NextRadioReceiverStation is a clean audio-only event.
    // ZoomIn / ZoomOut also used to live in this enum and were removed
    // (their mouse-wheel dispatch was indistinguishable from NextWeapon /
    // PrevWeapon under CP2077's input mapping).
    enum Action : int32_t
    {
        None = 0,

        // --- Driving ----------------------------------------------------
        Horn,
        Headlights,
        Handbrake,
        Autodrive,
        ExitVehicle,
        CallVehicle,

        // --- Camera -----------------------------------------------------
        CameraCycleForward,
        CameraCycleBackward,
        CameraReset,

        // --- Combat (shooting) ------------------------------------------
        ShootPrimary,
        ShootSecondary,
        ShootTertiary,

        // --- Weapons (selection / cycling) ------------------------------
        NextWeapon,
        PrevWeapon,
        WeaponSlot1,
        WeaponSlot2,
        SwitchWeapons,
        HolsterWeapon,

        // --- Radio ------------------------------------------------------
        RadioMenu,
        RadioNext,        // direct-method, CP2077-keybind-immune

        // --- Gameplay misc ---------------------------------------------
        UseConsumable,
        IconicCyberware,
        // `Tag` (MMB-tap, scan/mark in CP2077) used to live here and was
        // removed: it shared MMB with CameraCycleBackward and routed
        // through the same key the user often rebinds in CP2077, so it
        // was rarely useful and frequently surprising. Append-only past
        // this point still applies — Mod Settings persists by enum
        // member name, so removed names fall back to the field default
        // on next load.
        QuickSave,

        // --- Menus / fullscreen UI -------------------------------------
        OpenMap,
        OpenJournal,
        OpenInventory,
        OpenPhone,
        OpenPerks,
        OpenCrafting,
        Pause,

        // --- Menu navigation -------------------------------------------
        MenuConfirm,
        MenuCancel,
        MenuUp,
        MenuDown,
        MenuLeft,
        MenuRight,

        kActionCount
    };

    using BindingArray = std::array<int32_t, kCount>;

    // Replace the entire binding table at once (called by config on load /
    // per-change). Index = PhysicalInput, value = Action.
    void ReplaceAll(const BindingArray& bindings);

    // Set a single binding. Used by the per-input native. Out-of-range
    // inputId or action silently no-ops.
    void Set(int32_t inputId, int32_t action);

    // Read current binding for an input. Returns 0 (None) if unset.
    int32_t Get(int32_t inputId);

    // Pick the per-device button/POV mapping based on the wheel's friendly
    // product name (from LogiGetFriendlyProductName). Call once at bind
    // time. Logs which layout was matched and whether it is empirically
    // verified. Unknown wheels fall back to the G923 Xbox layout with a
    // warning.
    void SetDeviceLayout(const char* friendlyProductName);

    // Called once per pump tick with the latest input frame. Detects
    // rising/falling edges on all physical inputs, dispatches bound actions
    // via SendInput. Gated internally on config.input.enabled. Uses the
    // in-vehicle context from sources to suppress vehicle-only actions
    // when V is on foot.
    void OnTick(const sources::Frame& frame);

    // ---------- Capture mode ------------------------------------------------
    //
    // Mod Settings' InputBinding primitive (Phase 4) calls these from the
    // listening UI to capture wheel buttons / axis movement as tagged-
    // string bindings. While capture is armed, OnTick suppresses normal
    // action dispatch — pressing a paddle to bind it must NOT also fire
    // the paddle's currently-bound action.
    //
    // Lifecycle (caller-driven):
    //   1. UI calls BeginCapture("button") or BeginCapture("axis").
    //   2. Each frame the UI polls PollLatestCapture(); empty string =
    //      nothing captured yet, non-empty = tagged token (e.g. "wheel:5",
    //      "wheel-axis:lY", "wheel-pov:18000"). The poll consumes the
    //      slot atomically.
    //   3. UI calls CancelCapture() either after PollLatestCapture
    //      returned a value (cleanup), or because the user pressed cancel
    //      / clicked elsewhere.
    //
    // typeHint values:
    //   "button" : capture first rising-edge DInput button or POV-direction
    //              transition. Axis movements are ignored.
    //   "axis"   : capture first axis whose value deviates from its
    //              BeginCapture baseline by more than ~0.25. Buttons / POV
    //              are ignored.
    void BeginCapture(const char* typeHint);
    std::string PollLatestCapture();   // empty = nothing yet
    void CancelCapture();
    bool IsCapturing();
}
