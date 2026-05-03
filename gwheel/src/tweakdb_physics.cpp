#include "tweakdb_physics.h"
#include "rtti_offsets.h"
#include "vehicle_physics_ext.h"
#include "logging.h"

#include <RED4ext/RTTISystem.hpp>
#include <RED4ext/CName.hpp>
#include <RED4ext/Handle.hpp>
#include <RED4ext/Scripting/IScriptable.hpp>
#include <RED4ext/Scripting/Utils.hpp>
#include <RED4ext/Scripting/Natives/gamedataTweakDBRecord.hpp>

#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace gwheel::tweakdb_physics
{
    namespace
    {
        struct State
        {
            std::atomic<bool> ready{false};

            // Cached record-class pointers. Subsequent steps cache each
            // class's getter-method `CClassFunction*` by name (TweakDB
            // record fields are exposed as zero-arg instance methods, not
            // CProperty entries).
            std::atomic<RED4ext::CClass*> vehicleRecord{nullptr};
            std::atomic<RED4ext::CClass*> driveModelData{nullptr};
            std::atomic<RED4ext::CClass*> bikeDriveModelData{nullptr};
            std::atomic<RED4ext::CClass*> wheelDrivingPreset{nullptr};
            std::atomic<RED4ext::CClass*> wheelDimensionsPreset{nullptr};
            std::atomic<RED4ext::CClass*> wheelRolePreset{nullptr};
            std::atomic<RED4ext::CClass*> wheelDimensionsSetup{nullptr};
            std::atomic<RED4ext::CClass*> wheelsFrictionMap{nullptr};
            std::atomic<RED4ext::CClass*> wheelDrivingSetup2{nullptr};
            std::atomic<RED4ext::CClass*> wheelDrivingSetup4{nullptr};

            // Cached chain-entry getter function pointers. Resolved once,
            // atomically. Null = lookup hasn't happened yet OR already
            // failed (warn-once latch).
            std::atomic<RED4ext::CBaseFunction*> fnGetRecord{nullptr};

            // gamedataVehicle_Record FK getters.
            std::atomic<RED4ext::CBaseFunction*> fnVehDriveModelDataHandle{nullptr};
            std::atomic<RED4ext::CBaseFunction*> fnBikeDriveModelDataHandle{nullptr};
            std::atomic<RED4ext::CBaseFunction*> fnVehWheelDimensionsSetupHandle{nullptr};

            // gamedataVehicleDriveModelData_Record FK getters. Inherited
            // by Bike/Tank derived classes — one resolved pointer works
            // for all three kinds.
            std::atomic<RED4ext::CBaseFunction*> fnWheelSetupHandle{nullptr};
            std::atomic<RED4ext::CBaseFunction*> fnWheelsFrictionMapHandle{nullptr};

            // WheelsFrictionMap_Record + WheelDimensionsSetup_Record FK getters.
            std::atomic<RED4ext::CBaseFunction*> fnDefaultFrictionPresetHandle{nullptr};
            std::atomic<RED4ext::CBaseFunction*> fnFrontDimensionsHandle{nullptr};
            std::atomic<RED4ext::CBaseFunction*> fnBackDimensionsHandle{nullptr};

            // Per-slot WheelRole getters. _4_Record (cars/tanks): RF/RB/LF/LB.
            // _2_Record (bikes): F/B. Stored as 4 slots; slots 2/3 stay null
            // for bike's 2-wheel record. Slot 0 maps to the front-most wheel
            // in both layouts so generic indexing works.
            std::atomic<RED4ext::CBaseFunction*> fnSetup4_RF{nullptr};
            std::atomic<RED4ext::CBaseFunction*> fnSetup4_RB{nullptr};
            std::atomic<RED4ext::CBaseFunction*> fnSetup4_LF{nullptr};
            std::atomic<RED4ext::CBaseFunction*> fnSetup4_LB{nullptr};
            std::atomic<RED4ext::CBaseFunction*> fnSetup2_F{nullptr};
            std::atomic<RED4ext::CBaseFunction*> fnSetup2_B{nullptr};

            std::atomic<bool> chainGettersResolved{false};

            // ---- Field-getter cache --------------------------------------
            //
            // CDPR exposes TweakDB record fields as zero-arg instance methods
            // returning the typed flat value. We cache CBaseFunction* per
            // (recordClass, fieldName) once at first chain resolution, then
            // invoke via RED4ext::ExecuteFunction(handle, fn, &out) per tick.
            //
            // Separated by record class so the unused subtype caches stay
            // null on builds that don't ship that subtype.

            // gamedataVehicleDriveModelData_Record. Inherited by Bike/Tank
            // derived classes — invocation works on a Bike or Tank handle
            // for the base-class fields.
            std::atomic<RED4ext::CBaseFunction*> getDM_slipAngleCurveScale{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_slipAngleMinSpeedThreshold{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_slipRatioCurveScale{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_slipRatioMinSpeedThreshold{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_forwardWeightTransferFactor{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_sideWeightTransferFactor{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_airResistanceFactor{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_brakingFrictionFactor{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_handbrakeBrakingTorque{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_slopeTractionReductionBegin{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_slopeTractionReductionMax{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_slopeTractionReductionFactor{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_turningRollFactor{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_turnUpdateBaseSpeedThreshold{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_turnUpdateMidSpeedThreshold{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_turnUpdateMaxSpeedThreshold{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_turnUpdateMidSpeedTurnMul{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_turnUpdateMaxSpeedTurnMul{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_turnUpdateMidSpeedTurnChangeMul{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_turnUpdateMaxSpeedTurnChangeMul{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_maxWheelTurnDeg{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_wheelTurnMaxAddPerSecond{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_wheelTurnMaxSubPerSecond{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_perfectSteeringFactor{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_differentialOvershootFactor{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_bankBodyFBTanMultiplier{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_bankBodyLRTanMultiplier{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_bodyFriction{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDM_totalMass{nullptr};       // RTTI name: Total_mass
            std::atomic<RED4ext::CBaseFunction*> getDM_chassisMass{nullptr};     // RTTI name: Chassis_mass

            // gamedataBikeDriveModelData_Record (bike-only fields).
            std::atomic<RED4ext::CBaseFunction*> getBike_maxTilt{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getBike_tiltSpeed{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getBike_tiltCustomSpeed{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getBike_tiltReturnSpeed{nullptr};

            // gamedataVehicleWheelDimensionsPreset_Record (per-axle).
            std::atomic<RED4ext::CBaseFunction*> getDim_tireRadius{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getDim_tireWidth{nullptr};

            // gamedataVehicleWheelsFrictionPreset_Record (per-surface).
            std::atomic<RED4ext::CBaseFunction*> getFp_frictionLatMultiplier{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getFp_frictionLongMultiplier{nullptr};

            // gamedataVehicleWheelRole_Record (per-wheel role flags).
            std::atomic<RED4ext::CBaseFunction*> getRole_isDrive{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getRole_isMainBrake{nullptr};
            std::atomic<RED4ext::CBaseFunction*> getRole_isHandBrake{nullptr};

            // Per-vehicle resolution cache, keyed by TweakDBID hash. Insert
            // is rare (once per unique vehicle the player ever mounts);
            // lookup is the per-tick path. Mutex-guarded because vehicle
            // hooks fire from the game's update thread but we read on
            // first-mount which can race with ongoing reads.
            std::mutex resolvedMu;
            std::unordered_map<uint64_t, ResolvedRecords> resolvedByRecordID;

            // Per-vehicle-POINTER cache, sitting in front of the recordID
            // cache above. The recordID cache requires resolving the
            // recordID first via ExecuteFunction(vehicle, GetRecord, ...) —
            // a Handle<>-returning getter call that leaks/over-decrements
            // record handles per call (see project-bike-mount-crash memory
            // entry, 2026-05-02). Three remounts of the same bike triggered
            // three GetRecord calls; the third dereferenced a freed handle
            // at Cyberpunk2077.exe+0x72512B reading 0xFFFFFFFFFFFFFFFF.
            //
            // This cache short-circuits BEFORE the GetRecord call: same
            // vehicle pointer mounted N times = exactly one GetRecord call
            // total, regardless of N. Bounded by the count of unique
            // vehicles the player mounts in one game session (~handful),
            // not by the count of mount events (unbounded across play).
            std::mutex resolvedByPtrMu;
            std::unordered_map<void*, ResolvedRecords> resolvedByVehiclePtr;
        };

        State& S() { static State s; return s; }

        // Resolve a single TweakDB record class by RTTI name. Logs success
        // or `class not found`. Returns the class pointer or nullptr.
        RED4ext::CClass* ResolveRecordClass(RED4ext::CRTTISystem* rtti,
                                            const char* className)
        {
            auto* cls = rtti->GetClass(RED4ext::CName(className));
            if (!cls)
            {
                log::WarnF("[gwheel:tweakdb] record class '%s' not found in RTTI",
                           className);
                return nullptr;
            }
            log::InfoF("[gwheel:tweakdb] record class %s resolved (size=0x%X, props=%u)",
                       className, cls->size, cls->props.Size());
            return cls;
        }

        // Resolve a method on a class by name. Logs the result. Returns
        // the function pointer or nullptr.
        RED4ext::CBaseFunction* ResolveMethod(RED4ext::CClass* cls,
                                              const char* className,
                                              const char* methodName)
        {
            if (!cls)
            {
                log::WarnF("[gwheel:tweakdb] method lookup '%s::%s' skipped — class unresolved",
                           className, methodName);
                return nullptr;
            }
            auto* fn = cls->GetFunction(RED4ext::CName(methodName));
            if (!fn)
            {
                log::WarnF("[gwheel:tweakdb] method '%s::%s' not found", className, methodName);
                return nullptr;
            }
            log::InfoF("[gwheel:tweakdb] method %s::%s -> %p", className, methodName, fn);
            return fn;
        }

        // Lazily resolve all chain-entry getter pointers on first use.
        void EnsureChainGettersResolved()
        {
            auto& st = S();
            if (st.chainGettersResolved.load(std::memory_order_acquire)) return;

            auto* rtti = RED4ext::CRTTISystem::Get();
            if (!rtti)
            {
                log::Warn("[gwheel:tweakdb] EnsureChainGetters: RTTI unavailable");
                st.chainGettersResolved.store(true, std::memory_order_release);
                return;
            }

            // vehicleBaseObject::GetRecord — returns the gamedataVehicle_Record
            // handle directly (avoids the TweakDB::TryGetRecord step).
            if (auto* vbo = rtti->GetClass(RED4ext::CName("vehicleBaseObject")))
            {
                st.fnGetRecord.store(
                    ResolveMethod(vbo, "vehicleBaseObject", "GetRecord"),
                    std::memory_order_release);
            }

            // gamedataVehicle_Record subtype-FK getters. Exactly one of the
            // three drive-model getters resolves to a non-null handle on a
            // given vehicle; that identifies the chassis kind.
            auto* vr = st.vehicleRecord.load(std::memory_order_acquire);
            st.fnVehDriveModelDataHandle.store(
                ResolveMethod(vr, "gamedataVehicle_Record", "VehDriveModelDataHandle"),
                std::memory_order_release);
            st.fnBikeDriveModelDataHandle.store(
                ResolveMethod(vr, "gamedataVehicle_Record", "BikeDriveModelDataHandle"),
                std::memory_order_release);
            st.fnVehWheelDimensionsSetupHandle.store(
                ResolveMethod(vr, "gamedataVehicle_Record", "VehWheelDimensionsSetupHandle"),
                std::memory_order_release);

            // gamedataVehicleDriveModelData_Record FK getters. Inherited by
            // Bike/Tank derived classes; one resolved pointer covers all
            // three kinds.
            auto* dm = st.driveModelData.load(std::memory_order_acquire);
            st.fnWheelSetupHandle.store(
                ResolveMethod(dm, "gamedataVehicleDriveModelData_Record", "WheelSetupHandle"),
                std::memory_order_release);
            st.fnWheelsFrictionMapHandle.store(
                ResolveMethod(dm, "gamedataVehicleDriveModelData_Record", "WheelsFrictionMapHandle"),
                std::memory_order_release);

            // WheelsFrictionMap.DefaultFrictionPresetHandle -> the chassis-
            // wide WheelDrivingPreset. The chain entry CDPR actually uses
            // for spring/swaybar/tire-friction values.
            auto* fm = st.wheelsFrictionMap.load(std::memory_order_acquire);
            st.fnDefaultFrictionPresetHandle.store(
                ResolveMethod(fm, "gamedataVehicleWheelsFrictionMap_Record",
                              "DefaultFrictionPresetHandle"),
                std::memory_order_release);

            // WheelDimensionsSetup.{Front,Back}PresetHandle -> per-axle
            // WheelDimensionsPreset. CDPR's data is per-axle, not per-wheel.
            auto* ds = st.wheelDimensionsSetup.load(std::memory_order_acquire);
            st.fnFrontDimensionsHandle.store(
                ResolveMethod(ds, "gamedataVehicleWheelDimensionsSetup_Record",
                              "FrontPresetHandle"),
                std::memory_order_release);
            st.fnBackDimensionsHandle.store(
                ResolveMethod(ds, "gamedataVehicleWheelDimensionsSetup_Record",
                              "BackPresetHandle"),
                std::memory_order_release);

            // Per-slot wheel-role getters on each WheelDrivingSetup variant.
            auto* setup4 = st.wheelDrivingSetup4.load(std::memory_order_acquire);
            st.fnSetup4_RF.store(ResolveMethod(setup4,
                "gamedataVehicleWheelDrivingSetup_4_Record", "RFHandle"),
                std::memory_order_release);
            st.fnSetup4_RB.store(ResolveMethod(setup4,
                "gamedataVehicleWheelDrivingSetup_4_Record", "RBHandle"),
                std::memory_order_release);
            st.fnSetup4_LF.store(ResolveMethod(setup4,
                "gamedataVehicleWheelDrivingSetup_4_Record", "LFHandle"),
                std::memory_order_release);
            st.fnSetup4_LB.store(ResolveMethod(setup4,
                "gamedataVehicleWheelDrivingSetup_4_Record", "LBHandle"),
                std::memory_order_release);
            auto* setup2 = st.wheelDrivingSetup2.load(std::memory_order_acquire);
            st.fnSetup2_F.store(ResolveMethod(setup2,
                "gamedataVehicleWheelDrivingSetup_2_Record", "FHandle"),
                std::memory_order_release);
            st.fnSetup2_B.store(ResolveMethod(setup2,
                "gamedataVehicleWheelDrivingSetup_2_Record", "BHandle"),
                std::memory_order_release);

            // Field-getter resolution. Looked up once on each record class.
            // Names match the live RTTI dump (verified 2026-04-30 / 05-01).
            //
            // DriveModelData chassis-wide (62 methods total — we read 30).
            #define GWHEEL_RESOLVE_DM(field, name) \
                st.getDM_##field.store(ResolveMethod(dm, \
                    "gamedataVehicleDriveModelData_Record", name), \
                    std::memory_order_release)
            GWHEEL_RESOLVE_DM(slipAngleCurveScale,           "SlipAngleCurveScale");
            GWHEEL_RESOLVE_DM(slipAngleMinSpeedThreshold,    "SlipAngleMinSpeedThreshold");
            GWHEEL_RESOLVE_DM(slipRatioCurveScale,           "SlipRatioCurveScale");
            GWHEEL_RESOLVE_DM(slipRatioMinSpeedThreshold,    "SlipRatioMinSpeedThreshold");
            GWHEEL_RESOLVE_DM(forwardWeightTransferFactor,   "ForwardWeightTransferFactor");
            GWHEEL_RESOLVE_DM(sideWeightTransferFactor,      "SideWeightTransferFactor");
            GWHEEL_RESOLVE_DM(airResistanceFactor,           "AirResistanceFactor");
            GWHEEL_RESOLVE_DM(brakingFrictionFactor,         "BrakingFrictionFactor");
            GWHEEL_RESOLVE_DM(handbrakeBrakingTorque,        "HandbrakeBrakingTorque");
            GWHEEL_RESOLVE_DM(slopeTractionReductionBegin,   "SlopeTractionReductionBegin");
            GWHEEL_RESOLVE_DM(slopeTractionReductionMax,     "SlopeTractionReductionMax");
            GWHEEL_RESOLVE_DM(slopeTractionReductionFactor,  "SlopeTractionReductionFactor");
            GWHEEL_RESOLVE_DM(turningRollFactor,             "TurningRollFactor");
            GWHEEL_RESOLVE_DM(turnUpdateBaseSpeedThreshold,  "TurnUpdateBaseSpeedThreshold");
            GWHEEL_RESOLVE_DM(turnUpdateMidSpeedThreshold,   "TurnUpdateMidSpeedThreshold");
            GWHEEL_RESOLVE_DM(turnUpdateMaxSpeedThreshold,   "TurnUpdateMaxSpeedThreshold");
            GWHEEL_RESOLVE_DM(turnUpdateMidSpeedTurnMul,     "TurnUpdateMidSpeedTurnMul");
            GWHEEL_RESOLVE_DM(turnUpdateMaxSpeedTurnMul,     "TurnUpdateMaxSpeedTurnMul");
            GWHEEL_RESOLVE_DM(turnUpdateMidSpeedTurnChangeMul, "TurnUpdateMidSpeedTurnChangeMul");
            GWHEEL_RESOLVE_DM(turnUpdateMaxSpeedTurnChangeMul, "TurnUpdateMaxSpeedTurnChangeMul");
            GWHEEL_RESOLVE_DM(maxWheelTurnDeg,               "MaxWheelTurnDeg");
            GWHEEL_RESOLVE_DM(wheelTurnMaxAddPerSecond,      "WheelTurnMaxAddPerSecond");
            GWHEEL_RESOLVE_DM(wheelTurnMaxSubPerSecond,      "WheelTurnMaxSubPerSecond");
            GWHEEL_RESOLVE_DM(perfectSteeringFactor,         "PerfectSteeringFactor");
            GWHEEL_RESOLVE_DM(differentialOvershootFactor,   "DifferentialOvershootFactor");
            GWHEEL_RESOLVE_DM(bankBodyFBTanMultiplier,       "BankBodyFBTanMultiplier");
            GWHEEL_RESOLVE_DM(bankBodyLRTanMultiplier,       "BankBodyLRTanMultiplier");
            GWHEEL_RESOLVE_DM(bodyFriction,                  "BodyFriction");
            GWHEEL_RESOLVE_DM(totalMass,                     "Total_mass");
            GWHEEL_RESOLVE_DM(chassisMass,                   "Chassis_mass");
            #undef GWHEEL_RESOLVE_DM

            // BikeDriveModelData (12 methods — we read 4).
            auto* bikeDm = st.bikeDriveModelData.load(std::memory_order_acquire);
            #define GWHEEL_RESOLVE_BIKE(field, name) \
                st.getBike_##field.store(ResolveMethod(bikeDm, \
                    "gamedataBikeDriveModelData_Record", name), \
                    std::memory_order_release)
            GWHEEL_RESOLVE_BIKE(maxTilt,         "BikeMaxTilt");
            GWHEEL_RESOLVE_BIKE(tiltSpeed,       "BikeTiltSpeed");
            GWHEEL_RESOLVE_BIKE(tiltCustomSpeed, "BikeTiltCustomSpeed");
            GWHEEL_RESOLVE_BIKE(tiltReturnSpeed, "BikeTiltReturnSpeed");
            #undef GWHEEL_RESOLVE_BIKE

            // WheelDimensionsPreset (4 methods — we read 2: tireRadius, tireWidth).
            auto* dim = st.wheelDimensionsPreset.load(std::memory_order_acquire);
            st.getDim_tireRadius.store(ResolveMethod(dim,
                "gamedataVehicleWheelDimensionsPreset_Record", "TireRadius"),
                std::memory_order_release);
            st.getDim_tireWidth.store(ResolveMethod(dim,
                "gamedataVehicleWheelDimensionsPreset_Record", "TireWidth"),
                std::memory_order_release);

            // WheelsFrictionPreset (10 methods — we read 2: lat / long mul).
            auto* fp = rtti->GetClass(RED4ext::CName("gamedataVehicleWheelsFrictionPreset_Record"));
            st.getFp_frictionLatMultiplier.store(ResolveMethod(fp,
                "gamedataVehicleWheelsFrictionPreset_Record", "FrictionLatMultiplier"),
                std::memory_order_release);
            st.getFp_frictionLongMultiplier.store(ResolveMethod(fp,
                "gamedataVehicleWheelsFrictionPreset_Record", "FrictionLongMultiplier"),
                std::memory_order_release);

            // WheelRole (3 methods — all of them).
            auto* role = st.wheelRolePreset.load(std::memory_order_acquire);
            st.getRole_isDrive.store(ResolveMethod(role,
                "gamedataVehicleWheelRole_Record", "IsDrive"),
                std::memory_order_release);
            st.getRole_isMainBrake.store(ResolveMethod(role,
                "gamedataVehicleWheelRole_Record", "IsMainBrake"),
                std::memory_order_release);
            st.getRole_isHandBrake.store(ResolveMethod(role,
                "gamedataVehicleWheelRole_Record", "IsHandBrake"),
                std::memory_order_release);

            st.chainGettersResolved.store(true, std::memory_order_release);
        }

        // Try one of the three subtype FK getters. Writes the returned
        // record handle into outHandle (raw pointer; the Handle<> wrapper
        // owns a refcount we don't touch — record handles are long-lived
        // game objects). Returns true if the handle is non-null.
        bool TryGetSubtypeHandle(RED4ext::IScriptable* vehicleRecord,
                                 RED4ext::CBaseFunction* fn,
                                 RED4ext::IScriptable*& outHandle)
        {
            outHandle = nullptr;
            if (!vehicleRecord || !fn) return false;
            RED4ext::Handle<RED4ext::IScriptable> handle;
            if (!RED4ext::ExecuteFunction(vehicleRecord, fn, &handle))
                return false;
            if (!handle.instance) return false;
            outHandle = handle.instance;
            return true;
        }

        // Typed-getter invocation. RTTI getters on TweakDB records take
        // zero args and return the typed flat value via the out-pointer.
        // Returns 0 / false on null inputs or invocation failure.
        inline float CallFloatGetter(RED4ext::IScriptable* self,
                                     RED4ext::CBaseFunction* fn)
        {
            if (!self || !fn) return 0.f;
            float out = 0.f;
            if (!RED4ext::ExecuteFunction(self, fn, &out)) return 0.f;
            return out;
        }

        inline bool CallBoolGetter(RED4ext::IScriptable* self,
                                   RED4ext::CBaseFunction* fn)
        {
            if (!self || !fn) return false;
            bool out = false;
            if (!RED4ext::ExecuteFunction(self, fn, &out)) return false;
            return out;
        }
    }

    void Init()
    {
        auto& st = S();
        if (st.ready.load(std::memory_order_acquire)) return;

        auto* rtti = RED4ext::CRTTISystem::Get();
        if (!rtti)
        {
            log::Warn("[gwheel:tweakdb] RTTI system unavailable — record classes will not be resolved");
            return;
        }

        log::Info("[gwheel:tweakdb] ======== resolving TweakDB record classes ========");

        st.vehicleRecord.store(
            ResolveRecordClass(rtti, "gamedataVehicle_Record"),
            std::memory_order_release);
        st.driveModelData.store(
            ResolveRecordClass(rtti, "gamedataVehicleDriveModelData_Record"),
            std::memory_order_release);
        st.bikeDriveModelData.store(
            ResolveRecordClass(rtti, "gamedataBikeDriveModelData_Record"),
            std::memory_order_release);
        st.wheelDrivingPreset.store(
            ResolveRecordClass(rtti, "gamedataVehicleWheelDrivingPreset_Record"),
            std::memory_order_release);
        st.wheelDimensionsPreset.store(
            ResolveRecordClass(rtti, "gamedataVehicleWheelDimensionsPreset_Record"),
            std::memory_order_release);
        st.wheelRolePreset.store(
            ResolveRecordClass(rtti, "gamedataVehicleWheelRole_Record"),
            std::memory_order_release);
        st.wheelDimensionsSetup.store(
            ResolveRecordClass(rtti, "gamedataVehicleWheelDimensionsSetup_Record"),
            std::memory_order_release);
        st.wheelsFrictionMap.store(
            ResolveRecordClass(rtti, "gamedataVehicleWheelsFrictionMap_Record"),
            std::memory_order_release);
        st.wheelDrivingSetup2.store(
            ResolveRecordClass(rtti, "gamedataVehicleWheelDrivingSetup_2_Record"),
            std::memory_order_release);
        st.wheelDrivingSetup4.store(
            ResolveRecordClass(rtti, "gamedataVehicleWheelDrivingSetup_4_Record"),
            std::memory_order_release);

        log::Info("[gwheel:tweakdb] ======== record-class resolution complete ========");

        // Mark ready iff at least the base DriveModelData class resolved —
        // bike/tank derived classes can be missing on builds that don't
        // ship those subtypes, and the wheel preset chain is best-effort
        // until per-vehicle resolution lands.
        st.ready.store(st.driveModelData.load(std::memory_order_acquire) != nullptr,
                       std::memory_order_release);
    }

    bool IsReady() { return S().ready.load(std::memory_order_acquire); }

    ResolvedRecords ResolvePerVehicle(void* vehicle)
    {
        ResolvedRecords result{};
        if (!vehicle) return result;

        auto& st = S();
        if (!st.ready.load(std::memory_order_acquire)) return result;

        // Per-vehicle-pointer cache hit = no GetRecord call at all. Same
        // bike mounted N times → N-1 calls skipped. See note on
        // resolvedByVehiclePtr above for the crash this prevents.
        {
            std::lock_guard<std::mutex> lk(st.resolvedByPtrMu);
            auto it = st.resolvedByVehiclePtr.find(vehicle);
            if (it != st.resolvedByVehiclePtr.end())
                return it->second;
        }

        EnsureChainGettersResolved();

        // Step 1: vehicle -> recordID (TweakDBID).
        auto* fnGetRecord = st.fnGetRecord.load(std::memory_order_acquire);
        if (!fnGetRecord) return result;

        // GetRecord returns a Handle<gamedataVehicle_Record>. The handle's
        // `instance` is a pointer to the long-lived record object; we don't
        // own it.
        RED4ext::Handle<RED4ext::IScriptable> vehicleRecHandle;
        if (!RED4ext::ExecuteFunction(vehicle, fnGetRecord, &vehicleRecHandle))
        {
            log::WarnF("[gwheel:tweakdb] GetRecord on vehicle=%p returned false", vehicle);
            return result;
        }
        auto* vehicleRecord = vehicleRecHandle.instance;
        if (!vehicleRecord)
        {
            // Pre-mount race; the vehicle's record isn't wired yet. Caller
            // should retry on a later tick.
            return result;
        }

        // Use the record's own TweakDBID as the cache key. The record handle
        // is a `gamedataTweakDBRecord` derivative; its `recordID` field is
        // at the start of the TweakDBRecord layout (offset 0x40 per SDK).
        auto* tdbRecord = reinterpret_cast<RED4ext::gamedataTweakDBRecord*>(vehicleRecord);
        const uint64_t recordIDHash = tdbRecord->recordID.value;
        result.vehicleRecordID = recordIDHash;

        // Cache lookup. Hot path on subsequent ticks for the same vehicle.
        // (Distinct from the per-pointer cache up top — same recordID can
        // back two distinct vehicle pointers, e.g. quest-spawned vs garage
        // instances of the same model. Each pointer gets its own pointer-
        // cache entry but shares the recordID-cache walk.)
        {
            std::lock_guard<std::mutex> lk(st.resolvedMu);
            auto it = st.resolvedByRecordID.find(recordIDHash);
            if (it != st.resolvedByRecordID.end())
            {
                // Promote into the pointer cache so the next mount of this
                // exact pointer skips GetRecord.
                std::lock_guard<std::mutex> lk2(st.resolvedByPtrMu);
                st.resolvedByVehiclePtr[vehicle] = it->second;
                return it->second;
            }
        }

        // Step 2: vehicleRecord -> DriveModelData handle. Try subtype FKs in
        // order of specificity (Bike, Tank, then base car). Empirically the
        // base game wires exactly one subtype FK per vehicle; the others
        // resolve to a null Handle.
        RED4ext::IScriptable* dmHandle = nullptr;
        ChassisKind kind = ChassisKind::Unknown;

        if (TryGetSubtypeHandle(vehicleRecord,
                                st.fnBikeDriveModelDataHandle.load(std::memory_order_acquire),
                                dmHandle))
        {
            kind = ChassisKind::Bike;
        }
        else if (TryGetSubtypeHandle(vehicleRecord,
                                     st.fnVehDriveModelDataHandle.load(std::memory_order_acquire),
                                     dmHandle))
        {
            kind = ChassisKind::Car;
        }
        // Tanks (which would resolve TankDriveModelDataHandle) fall through
        // to ChassisKind::Unknown and the FFB pipeline bails on them.

        result.kind = kind;
        result.driveModelData = dmHandle;

        // Confirm via record's RTTI class — sanity check against subtype
        // dispatch above. If the resolved record's class doesn't match the
        // kind we picked, log it (suggests a CDPR data-shape change).
        const char* dmClassName = "?";
        if (dmHandle && dmHandle->GetType())
            dmClassName = dmHandle->GetType()->GetName().ToString();

        const char* kindStr = (kind == ChassisKind::Car)  ? "Car"
                            : (kind == ChassisKind::Bike) ? "Bike"
                            :                               "Unknown";

        // Step 3: dmHandle -> WheelSetup record + WheelsFrictionMap record.
        // WheelsFrictionMap.DefaultFrictionPresetHandle -> chassis-wide
        // WheelsFrictionPreset (per-surface tire-road friction scalars,
        // NOT spring/swaybar). The 24-method WheelDrivingPreset_Record
        // exists in RTTI but isn't reachable from this chain.
        RED4ext::IScriptable* wsHandle = nullptr;
        RED4ext::IScriptable* fmHandle = nullptr;
        RED4ext::IScriptable* fpHandle = nullptr;
        if (dmHandle)
        {
            (void)TryGetSubtypeHandle(dmHandle,
                st.fnWheelSetupHandle.load(std::memory_order_acquire), wsHandle);
            (void)TryGetSubtypeHandle(dmHandle,
                st.fnWheelsFrictionMapHandle.load(std::memory_order_acquire), fmHandle);
        }
        if (fmHandle)
        {
            (void)TryGetSubtypeHandle(fmHandle,
                st.fnDefaultFrictionPresetHandle.load(std::memory_order_acquire), fpHandle);
        }
        result.wheelSetup        = wsHandle;
        result.wheelsFrictionMap = fmHandle;
        result.frictionPreset    = fpHandle;

        // Step 4: vehicleRecord -> VehWheelDimensionsSetup -> Front/Back
        // DimensionsPreset. Per-axle, not per-wheel — CDPR's data shape.
        RED4ext::IScriptable* dimSetup = nullptr;
        RED4ext::IScriptable* frontDim = nullptr;
        RED4ext::IScriptable* backDim  = nullptr;
        (void)TryGetSubtypeHandle(vehicleRecord,
            st.fnVehWheelDimensionsSetupHandle.load(std::memory_order_acquire), dimSetup);
        if (dimSetup)
        {
            (void)TryGetSubtypeHandle(dimSetup,
                st.fnFrontDimensionsHandle.load(std::memory_order_acquire), frontDim);
            (void)TryGetSubtypeHandle(dimSetup,
                st.fnBackDimensionsHandle.load(std::memory_order_acquire), backDim);
        }
        result.dimensionsSetup = dimSetup;
        result.frontDimensions = frontDim;
        result.backDimensions  = backDim;

        // Step 5: per-slot WheelRole records via wheelSetup. Setup_4
        // (cars/tanks) exposes RF/RB/LF/LB; Setup_2 (bikes) exposes F/B.
        // We dispatch by the resolved record's RTTI class.
        if (wsHandle && wsHandle->GetType())
        {
            auto* wsCls   = wsHandle->GetType();
            auto* setup4  = st.wheelDrivingSetup4.load(std::memory_order_acquire);
            auto* setup2  = st.wheelDrivingSetup2.load(std::memory_order_acquire);
            if (wsCls == setup4)
            {
                (void)TryGetSubtypeHandle(wsHandle,
                    st.fnSetup4_RF.load(std::memory_order_acquire), result.wheelRole[0]);
                (void)TryGetSubtypeHandle(wsHandle,
                    st.fnSetup4_RB.load(std::memory_order_acquire), result.wheelRole[1]);
                (void)TryGetSubtypeHandle(wsHandle,
                    st.fnSetup4_LF.load(std::memory_order_acquire), result.wheelRole[2]);
                (void)TryGetSubtypeHandle(wsHandle,
                    st.fnSetup4_LB.load(std::memory_order_acquire), result.wheelRole[3]);
                result.wheelCount = 4;
            }
            else if (wsCls == setup2)
            {
                (void)TryGetSubtypeHandle(wsHandle,
                    st.fnSetup2_F.load(std::memory_order_acquire), result.wheelRole[0]);
                (void)TryGetSubtypeHandle(wsHandle,
                    st.fnSetup2_B.load(std::memory_order_acquire), result.wheelRole[1]);
                result.wheelCount = 2;
            }
        }

        const char* wsClassName = (wsHandle && wsHandle->GetType())
            ? wsHandle->GetType()->GetName().ToString() : "?";
        const char* fpClassName = (fpHandle && fpHandle->GetType())
            ? fpHandle->GetType()->GetName().ToString() : "?";

        // First-encounter log per unique vehicle record.
        log::InfoF("[gwheel:tweakdb] resolved vehicle: recordID=%016llX kind=%s wheels=%u",
                   static_cast<unsigned long long>(recordIDHash), kindStr, result.wheelCount);
        log::InfoF("[gwheel:tweakdb]   driveModelData=%p (%s)", dmHandle, dmClassName);
        log::InfoF("[gwheel:tweakdb]   wheelSetup=%p (%s) frictionMap=%p frictionPreset=%p (%s)",
                   wsHandle, wsClassName, fmHandle, fpHandle, fpClassName);
        log::InfoF("[gwheel:tweakdb]   dimensionsSetup=%p frontDim=%p backDim=%p",
                   dimSetup, frontDim, backDim);
        log::InfoF("[gwheel:tweakdb]   wheelRole[0..3]=%p %p %p %p",
                   result.wheelRole[0], result.wheelRole[1],
                   result.wheelRole[2], result.wheelRole[3]);

        {
            std::lock_guard<std::mutex> lk(st.resolvedMu);
            st.resolvedByRecordID[recordIDHash] = result;
        }
        // Also stash by raw vehicle pointer so subsequent mounts of the
        // SAME vehicle skip the GetRecord call entirely (the only
        // remaining Handle<>-returning getter call in this resolver).
        {
            std::lock_guard<std::mutex> lk(st.resolvedByPtrMu);
            st.resolvedByVehiclePtr[vehicle] = result;
        }
        return result;
    }

    namespace
    {
        // Authored-snapshot cache, keyed by the same vehicleRecordID hash
        // that ResolvedRecords uses. TweakDB authored values don't change
        // during a session, so we resolve them once per unique vehicle and
        // copy from this cache on every subsequent tick. Without this cache
        // the per-tick RTTI invocation count was ~60 ExecuteFunction calls
        // × 250 Hz pump = ~15 000 RTTI calls/sec, which empirically crashed
        // the game on Galena mount (2026-05-01).
        struct AuthoredSnapshotCache
        {
            std::mutex mu;
            std::unordered_map<uint64_t, PerCarPhysics> byRecordID;
        };
        AuthoredSnapshotCache& AuthoredCache()
        {
            static AuthoredSnapshotCache c;
            return c;
        }
    }

    PerCarPhysics ReadPerCarPhysics(void* vehicle)
    {
        PerCarPhysics out{};
        if (!vehicle) return out;

        // Resolve the record chain (cached after first call per vehicle).
        const ResolvedRecords rr = ResolvePerVehicle(vehicle);
        out.kind            = rr.kind;
        out.vehicleRecordID = rr.vehicleRecordID;
        out.chainResolved   = (rr.driveModelData != nullptr);
        out.wheelCount      = rr.wheelCount;

        auto& st = S();

        // ---- Cached authored snapshot (lift from cache if present) ------
        // First call per unique vehicle resolves all field-getters; every
        // subsequent call is a memcpy. Cache key is the vehicleRecordID.
        bool cachedHit = false;
        if (out.chainResolved && rr.vehicleRecordID != 0)
        {
            auto& cache = AuthoredCache();
            std::lock_guard<std::mutex> lk(cache.mu);
            auto it = cache.byRecordID.find(rr.vehicleRecordID);
            if (it != cache.byRecordID.end())
            {
                // Copy authored fields. Live state slots get overwritten
                // below by the per-tick PhysicsData read, so we don't care
                // what stale live values landed in the cached entry.
                out          = it->second;
                out.kind     = rr.kind;
                out.vehicleRecordID = rr.vehicleRecordID;
                out.chainResolved   = true;
                out.wheelCount      = rr.wheelCount;
                cachedHit = true;
            }
        }

        // ---- Live state (PhysicsData) ----------------------------------
        if (vehicle_ext::ReadVelocity(vehicle, out.vel))
        {
            float ang[3] = {};
            (void)vehicle_ext::ReadAngularVelocity(vehicle, ang);
            out.angVel[0] = ang[0]; out.angVel[1] = ang[1]; out.angVel[2] = ang[2];
            out.speedMps         = std::sqrt(out.vel[0] * out.vel[0]
                                           + out.vel[1] * out.vel[1]
                                           + out.vel[2] * out.vel[2]);
            out.verticalSpeedMps = out.vel[2];
            // Yaw rate is the Z component of angular velocity in world space.
            out.yawMagRadPerSec  = std::abs(out.angVel[2]);
            out.liveStateValid   = true;
        }

        // ---- Authored values (TweakDB record getters) ------------------
        // Skip if we already lifted these from the cache above. Otherwise
        // do the ~60 RED4ext::ExecuteFunction calls once and cache the
        // result for every subsequent tick.
        if (!cachedHit)
        {
        // DriveModelData base-class fields work on Bike handles via
        // RTTI inheritance, so both kinds share the bulk of the reads.
        auto* dm = rr.driveModelData;
        if (dm)
        {
            out.slipAngleCurveScale            = CallFloatGetter(dm, st.getDM_slipAngleCurveScale.load(std::memory_order_acquire));
            out.slipAngleMinSpeedThreshold     = CallFloatGetter(dm, st.getDM_slipAngleMinSpeedThreshold.load(std::memory_order_acquire));
            out.slipRatioCurveScale            = CallFloatGetter(dm, st.getDM_slipRatioCurveScale.load(std::memory_order_acquire));
            out.slipRatioMinSpeedThreshold     = CallFloatGetter(dm, st.getDM_slipRatioMinSpeedThreshold.load(std::memory_order_acquire));
            out.forwardWeightTransferFactor    = CallFloatGetter(dm, st.getDM_forwardWeightTransferFactor.load(std::memory_order_acquire));
            out.sideWeightTransferFactor       = CallFloatGetter(dm, st.getDM_sideWeightTransferFactor.load(std::memory_order_acquire));
            out.airResistanceFactor            = CallFloatGetter(dm, st.getDM_airResistanceFactor.load(std::memory_order_acquire));
            out.brakingFrictionFactor          = CallFloatGetter(dm, st.getDM_brakingFrictionFactor.load(std::memory_order_acquire));
            out.handbrakeBrakingTorque         = CallFloatGetter(dm, st.getDM_handbrakeBrakingTorque.load(std::memory_order_acquire));
            out.slopeTractionReductionBegin    = CallFloatGetter(dm, st.getDM_slopeTractionReductionBegin.load(std::memory_order_acquire));
            out.slopeTractionReductionMax      = CallFloatGetter(dm, st.getDM_slopeTractionReductionMax.load(std::memory_order_acquire));
            out.slopeTractionReductionFactor   = CallFloatGetter(dm, st.getDM_slopeTractionReductionFactor.load(std::memory_order_acquire));
            out.turningRollFactor              = CallFloatGetter(dm, st.getDM_turningRollFactor.load(std::memory_order_acquire));
            out.turnUpdateBaseSpeedThreshold   = CallFloatGetter(dm, st.getDM_turnUpdateBaseSpeedThreshold.load(std::memory_order_acquire));
            out.turnUpdateMidSpeedThreshold    = CallFloatGetter(dm, st.getDM_turnUpdateMidSpeedThreshold.load(std::memory_order_acquire));
            out.turnUpdateMaxSpeedThreshold    = CallFloatGetter(dm, st.getDM_turnUpdateMaxSpeedThreshold.load(std::memory_order_acquire));
            out.turnUpdateMidSpeedTurnMul      = CallFloatGetter(dm, st.getDM_turnUpdateMidSpeedTurnMul.load(std::memory_order_acquire));
            out.turnUpdateMaxSpeedTurnMul      = CallFloatGetter(dm, st.getDM_turnUpdateMaxSpeedTurnMul.load(std::memory_order_acquire));
            out.turnUpdateMidSpeedTurnChangeMul = CallFloatGetter(dm, st.getDM_turnUpdateMidSpeedTurnChangeMul.load(std::memory_order_acquire));
            out.turnUpdateMaxSpeedTurnChangeMul = CallFloatGetter(dm, st.getDM_turnUpdateMaxSpeedTurnChangeMul.load(std::memory_order_acquire));
            out.maxWheelTurnDeg                = CallFloatGetter(dm, st.getDM_maxWheelTurnDeg.load(std::memory_order_acquire));
            out.wheelTurnMaxAddPerSecond       = CallFloatGetter(dm, st.getDM_wheelTurnMaxAddPerSecond.load(std::memory_order_acquire));
            out.wheelTurnMaxSubPerSecond       = CallFloatGetter(dm, st.getDM_wheelTurnMaxSubPerSecond.load(std::memory_order_acquire));
            out.perfectSteeringFactor          = CallFloatGetter(dm, st.getDM_perfectSteeringFactor.load(std::memory_order_acquire));
            out.differentialOvershootFactor    = CallFloatGetter(dm, st.getDM_differentialOvershootFactor.load(std::memory_order_acquire));
            out.bankBodyFBTanMultiplier        = CallFloatGetter(dm, st.getDM_bankBodyFBTanMultiplier.load(std::memory_order_acquire));
            out.bankBodyLRTanMultiplier        = CallFloatGetter(dm, st.getDM_bankBodyLRTanMultiplier.load(std::memory_order_acquire));
            out.bodyFriction                   = CallFloatGetter(dm, st.getDM_bodyFriction.load(std::memory_order_acquire));
            out.totalMass                      = CallFloatGetter(dm, st.getDM_totalMass.load(std::memory_order_acquire));
            out.chassisMass                    = CallFloatGetter(dm, st.getDM_chassisMass.load(std::memory_order_acquire));
        }

        // Bike-only fields. The same DriveModelData* IS the bike record
        // (gamedataBikeDriveModelData_Record derives from base), so we can
        // invoke bike-class getters directly on the same handle.
        if (dm && rr.kind == ChassisKind::Bike)
        {
            out.bikeMaxTilt         = CallFloatGetter(dm, st.getBike_maxTilt.load(std::memory_order_acquire));
            out.bikeTiltSpeed       = CallFloatGetter(dm, st.getBike_tiltSpeed.load(std::memory_order_acquire));
            out.bikeTiltCustomSpeed = CallFloatGetter(dm, st.getBike_tiltCustomSpeed.load(std::memory_order_acquire));
            out.bikeTiltReturnSpeed = CallFloatGetter(dm, st.getBike_tiltReturnSpeed.load(std::memory_order_acquire));
        }

        // Per-axle dimensions.
        if (rr.frontDimensions)
        {
            out.frontTireRadius = CallFloatGetter(rr.frontDimensions, st.getDim_tireRadius.load(std::memory_order_acquire));
            out.frontTireWidth  = CallFloatGetter(rr.frontDimensions, st.getDim_tireWidth.load(std::memory_order_acquire));
        }
        if (rr.backDimensions)
        {
            out.backTireRadius = CallFloatGetter(rr.backDimensions, st.getDim_tireRadius.load(std::memory_order_acquire));
            out.backTireWidth  = CallFloatGetter(rr.backDimensions, st.getDim_tireWidth.load(std::memory_order_acquire));
        }

        // Surface friction (chassis-wide default; per-surface lookup is a
        // future capability via FrictionMap.Surfaces[]).
        if (rr.frictionPreset)
        {
            out.frictionLatMultiplier  = CallFloatGetter(rr.frictionPreset, st.getFp_frictionLatMultiplier.load(std::memory_order_acquire));
            out.frictionLongMultiplier = CallFloatGetter(rr.frictionPreset, st.getFp_frictionLongMultiplier.load(std::memory_order_acquire));
        }

        // Per-wheel role flags.
        for (uint32_t i = 0; i < 4; ++i)
        {
            auto* role = rr.wheelRole[i];
            if (!role) continue;
            out.wheels[i].isDrive     = CallBoolGetter(role, st.getRole_isDrive.load(std::memory_order_acquire));
            out.wheels[i].isMainBrake = CallBoolGetter(role, st.getRole_isMainBrake.load(std::memory_order_acquire));
            out.wheels[i].isHandBrake = CallBoolGetter(role, st.getRole_isHandBrake.load(std::memory_order_acquire));
        }

        // Persist the authored values into the cache so subsequent ticks
        // skip the ~60 ExecuteFunction calls. Live state (vel/angVel/etc)
        // gets overwritten on every read, so the stale-live-state values
        // copied alongside the authored fields don't matter.
        if (out.chainResolved && rr.vehicleRecordID != 0)
        {
            auto& cache = AuthoredCache();
            std::lock_guard<std::mutex> lk(cache.mu);
            cache.byRecordID[rr.vehicleRecordID] = out;
        }
        } // end if (!cachedHit)

        return out;
    }
}
