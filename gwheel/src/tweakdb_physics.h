#pragma once

// TweakDB-resolved physics — record handle resolver + typed accessors.
//
// CDPR ships authored vehicle physics as TweakDB records: every per-vehicle
// slip curve, weight-transfer factor, turn-update threshold, per-wheel
// spring + friction value, etc. lives on `gamedataVehicleDriveModelData_Record`
// (and its Bike-derived class) plus the wheel-setup chain rooted at the
// `wheelSetup` foreign-key. Tanks (Basilisk) are out of scope: a wheel is
// not a useful input while aiming the main gun, and tanks see negligible
// play time. These record classes ARE reflection-
// registered on 2.31 — unlike the native `vehicleWheeledPhysics` /
// `CarPhysics` / `BikePhysics` C++ structs which are NOT (verified empirically
// 2026-04-30: every CRTTISystem::GetClass on those names returns null).
//
// Field access path (verified 2026-04-30): TweakDB record fields are RTTI
// instance methods (zero-arg getters returning the typed flat value), NOT
// CProperty entries. We cache `RED4ext::CClassFunction*` per (recordClass,
// fieldName) pair on init and invoke via `RED4ext::ExecuteFunction(handle,
// fn, &out)` on demand.
//
// Reading authored physics from TweakDB has three concrete advantages over
// offset-based reads of the native classes:
//
//   1. Stable schema. TweakDBIDs and field names are part of CDPR's data
//      contract with the modding ecosystem; they don't drift between patches
//      the way struct layouts do.
//   2. Per-vehicle character lands for free. Each `Vehicle.Vehicles.<name>`
//      record points at a distinct DriveModelData record. We follow the
//      chain on mount and inherit per-car authored values without per-car
//      heuristics.
//   3. Subclass dispatch is record-typed. `gamedataBikeDriveModelData_Record`
//      derives from the base record class. The base `gamedataVehicle_Record`
//      exposes `VehDriveModelDataHandle` (cars) and `BikeDriveModelDataHandle`
//      (bikes); whichever resolves to non-null identifies the kind. Tanks
//      have a separate `TankDriveModelDataHandle` which we do not query —
//      they fall through to `ChassisKind::Unknown` and FFB bails on them.

#include <cstdint>

namespace RED4ext { struct IScriptable; }

namespace gwheel::tweakdb_physics
{
    enum class ChassisKind : uint8_t
    {
        Unknown = 0,
        Car     = 1,
        Bike    = 2,
    };

    // Records resolved for one mounted vehicle. Caller treats record handles
    // as opaque; the typed accessor layer (Step 3+) reads named fields off
    // them. Discovery (2026-05-01) revealed CDPR's actual data shape:
    // dimensions are per-axle (Front/Back), driving preset is chassis-wide
    // off WheelsFrictionMap.DefaultFrictionPreset, per-wheel records exist
    // only as WheelRole (role flags) in the WheelDrivingSetup chain.
    //
    // Slot mapping: wheelRole[0..3] indices follow the wheel-setup record's
    // RTTI method order. For cars (Setup_4): 0=RF, 1=RB, 2=LF, 3=LB. For
    // bikes (Setup_2): 0=F, 1=B; slots 2/3 stay null. Tanks resolve to
    // ChassisKind::Unknown (out of scope) — no chain walked, no fields read.
    struct ResolvedRecords
    {
        ChassisKind             kind = ChassisKind::Unknown;
        uint64_t                vehicleRecordID = 0;          // TweakDBID hash
        RED4ext::IScriptable*   driveModelData = nullptr;     // gamedata*DriveModelData_Record handle
        RED4ext::IScriptable*   wheelSetup = nullptr;         // gamedataVehicleWheelDrivingSetup_(2|4)_Record handle
        RED4ext::IScriptable*   wheelsFrictionMap = nullptr;  // gamedataVehicleWheelsFrictionMap_Record handle
        RED4ext::IScriptable*   frictionPreset = nullptr;     // gamedataVehicleWheelsFrictionPreset_Record handle (DefaultFrictionPreset; per-surface friction scalars)
        RED4ext::IScriptable*   dimensionsSetup = nullptr;    // gamedataVehicleWheelDimensionsSetup_Record handle
        RED4ext::IScriptable*   frontDimensions = nullptr;    // gamedataVehicleWheelDimensionsPreset_Record handle (front axle)
        RED4ext::IScriptable*   backDimensions = nullptr;     // gamedataVehicleWheelDimensionsPreset_Record handle (back axle)
        RED4ext::IScriptable*   wheelRole[4] = {};            // gamedataVehicleWheelRole_Record handles per slot
        uint32_t                wheelCount = 0;               // 4 for cars, 2 for bikes
    };

    // Resolve every TweakDB record class we care about, log each lookup,
    // and emit a one-time DumpClassProperties for each record class so
    // first-encounter field-name discovery is captured in the log.
    //
    // Safe to call multiple times; subsequent calls are no-ops. Must be
    // called after RTTI registration completes (typically from the same
    // PostRegisterTypes callback that wires gwheel::rtti_offsets::Init).
    void Init();

    // True once Init has resolved at least the base DriveModelData class.
    bool IsReady();

    // Resolve the record chain for this vehicle. Cached per vehicleRecordID;
    // first-encounter calls walk the FK chain + log the result, subsequent
    // calls return the cached snapshot. Returns a `ResolvedRecords` whose
    // `kind` is `Unknown` and whose handles are null on failure (logged).
    //
    // The vehicle pointer is a `vehicleBaseObject*`. Must be called from
    // a thread that can safely invoke RTTI methods on a live game object.
    ResolvedRecords ResolvePerVehicle(void* vehicle);

    // Per-tick physics snapshot composing TweakDB authored values + live
    // state. Authored values come from invoking field-getter RTTI methods
    // on the resolved record handles (cached once per plugin lifetime);
    // live state comes from the existing PhysicsData offset probe.
    //
    // Plan revision (verified against 2026-05-01 schema): authored physics
    // for cars/bikes consists of (a) chassis-wide DriveModelData scalars,
    // (b) per-axle WheelDimensions (front/back tire radius+width), (c) per-
    // surface FrictionPreset multipliers, (d) per-wheel role flags. The
    // 24-method WheelDrivingPreset (springs/swaybar) the plan called for
    // is NOT in the chain CDPR's runtime physics walks; per-wheel mass /
    // spring / swaybar values aren't authored at our access layer.
    // Tanks are out of scope (see header doc).
    struct WheelRoleFlags
    {
        bool isDrive     = false;
        bool isMainBrake = false;
        bool isHandBrake = false;
    };

    struct PerCarPhysics
    {
        ChassisKind kind = ChassisKind::Unknown;
        uint64_t    vehicleRecordID = 0;
        bool        chainResolved   = false;
        bool        liveStateValid  = false;

        // Live state (PhysicsData inner reads).
        float vel[3]            = {};
        float angVel[3]         = {};
        float speedMps          = 0.f;
        float verticalSpeedMps  = 0.f;
        float yawMagRadPerSec   = 0.f;

        // Authored chassis-wide (gamedataVehicleDriveModelData_Record).
        float slipAngleCurveScale          = 0.f;
        float slipAngleMinSpeedThreshold   = 0.f;
        float slipRatioCurveScale          = 0.f;
        float slipRatioMinSpeedThreshold   = 0.f;
        float forwardWeightTransferFactor  = 0.f;
        float sideWeightTransferFactor     = 0.f;
        float airResistanceFactor          = 0.f;
        float brakingFrictionFactor        = 0.f;
        float handbrakeBrakingTorque       = 0.f;
        float slopeTractionReductionBegin  = 0.f;
        float slopeTractionReductionMax    = 0.f;
        float slopeTractionReductionFactor = 0.f;
        float turningRollFactor            = 0.f;
        float turnUpdateBaseSpeedThreshold = 0.f;
        float turnUpdateMidSpeedThreshold  = 0.f;
        float turnUpdateMaxSpeedThreshold  = 0.f;
        float turnUpdateMidSpeedTurnMul       = 0.f;
        float turnUpdateMaxSpeedTurnMul       = 0.f;
        float turnUpdateMidSpeedTurnChangeMul = 0.f;
        float turnUpdateMaxSpeedTurnChangeMul = 0.f;
        float maxWheelTurnDeg              = 0.f;
        float wheelTurnMaxAddPerSecond     = 0.f;
        float wheelTurnMaxSubPerSecond     = 0.f;
        float perfectSteeringFactor        = 0.f;
        float differentialOvershootFactor  = 0.f;
        float bankBodyFBTanMultiplier      = 0.f;
        float bankBodyLRTanMultiplier      = 0.f;
        float bodyFriction                 = 0.f;
        float totalMass                    = 0.f;
        float chassisMass                  = 0.f;

        // Bike-only authored (gamedataBikeDriveModelData_Record).
        float bikeMaxTilt          = 0.f;
        float bikeTiltSpeed        = 0.f;
        float bikeTiltCustomSpeed  = 0.f;
        float bikeTiltReturnSpeed  = 0.f;

        // Bike-only live (BikePhysics offsets, see vehicle_physics_ext::fallback_bike).
        // Replaces the lateralVelocityMps/yaw-derived proxies in ApplyFfbFromPhysics.
        // Populated per-tick from vehicle_hook when kind == Bike.
        float bikeCurrentTilt        = 0.f;  // radians, signed
        float bikeTransversalForce   = 0.f;  // newtons, signed
        bool  bikeLiveValid          = false;

        // Car-only live (CarPhysics offsets, see vehicle_physics_ext::fallback_car).
        // Populated per-tick from vehicle_hook when kind == Car. Drives the
        // lean-into-corner FFB term in the car branch.
        float carBankBodyLR          = 0.f;  // signed roll direction (~±0.8 at full corner)
        bool  carLiveValid           = false;

        // Per-axle dimensions (gamedataVehicleWheelDimensionsPreset_Record).
        float frontTireRadius = 0.f;
        float frontTireWidth  = 0.f;
        float backTireRadius  = 0.f;
        float backTireWidth   = 0.f;

        // Default-surface friction multipliers (FrictionPreset). When we
        // wire surface-aware FFB later, these get re-resolved per detected
        // surface from the FrictionMap.Surfaces[] array.
        float frictionLatMultiplier  = 0.f;
        float frictionLongMultiplier = 0.f;

        // Per-wheel role flags. Slot 0..3 follows the WheelDrivingSetup
        // record order: car -> RF, RB, LF, LB. Bike -> F, B (slots 2/3 zeroed).
        WheelRoleFlags wheels[4] = {};
        uint32_t       wheelCount = 0;
    };

    // Read a per-vehicle physics snapshot. Resolves (and caches) the record
    // chain on first call per unique vehicle, invokes every field-getter
    // method, and reads live state from the PhysicsData struct. First-tick
    // log dump per unique vehicle is emitted from the caller (vehicle_hook).
    PerCarPhysics ReadPerCarPhysics(void* vehicle);
}
