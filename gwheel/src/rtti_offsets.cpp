#include "rtti_offsets.h"
#include "logging.h"

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/RTTISystem.hpp>
#include <RED4ext/RTTITypes.hpp>
#include <RED4ext/CName.hpp>
#include <RED4ext/Scripting/CProperty.hpp>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace gwheel::rtti_offsets
{
    namespace
    {
        struct State
        {
            std::atomic<bool>     ready{false};
            std::atomic<uint32_t> vehicleBaseObject_physics{0};
            std::atomic<uint32_t> vehicleBaseObject_physicsData{0};
            std::atomic<uint32_t> physicsData_velocity{0};
            std::atomic<uint32_t> physicsData_angularVelocity{0};
            std::atomic<uint32_t> wheeledPhysics_turnRate{0};
            std::atomic<uint32_t> wheeledPhysics_maxWheelTurnDeg{0};
            std::atomic<uint32_t> wheeledPhysics_slipAngleCurveScale{0};
            std::atomic<uint32_t> wheeledPhysics_numDriveWheels{0};
            std::atomic<uint32_t> wheeledPhysics_frontBackWheelDistance{0};

            // Runtime-probe results: where physicsData / physics live on
            // vehicleBaseObject, discovered by back-pointer correlation.
            // Zero = not yet resolved (use fallback).
            std::atomic<uint32_t> probed_physicsDataOffset{0};
            std::atomic<uint32_t> probed_physicsOffset{0};
            std::atomic<bool>     probeDone{false};
        };

        State& S() { static State s; return s; }

        // Resolve a single (className, fieldName) pair and log the result.
        // Returns the field's valueOffset or 0 if unresolvable.
        uint32_t Resolve(RED4ext::CRTTISystem* rtti,
                         const char* className,
                         const char* fieldName)
        {
            auto* cls = rtti->GetClass(RED4ext::CName(className));
            if (!cls)
            {
                log::WarnF("[gwheel:rtti-off] class '%s' not found — cannot resolve '%s.%s'",
                           className, className, fieldName);
                return 0;
            }
            auto* prop = cls->GetProperty(RED4ext::CName(fieldName));
            if (!prop)
            {
                log::WarnF("[gwheel:rtti-off] field '%s.%s' not found on this build — check DumpClassProperties output",
                           className, fieldName);
                return 0;
            }
            const uint32_t off = prop->valueOffset;
            const char* typeName = prop->type ? prop->type->GetName().ToString() : "?";
            log::InfoF("[gwheel:rtti-off] %s.%s -> offset=0x%X type=%s",
                       className, fieldName, off, typeName ? typeName : "?");
            return off;
        }

        // Pick the first resolvable (className, fieldName) pair from a list.
        // Useful when the game has used multiple names for the same field
        // across versions, or when a field lives on a parent/subclass with a
        // differently-named RTTI class in different builds.
        uint32_t ResolveFirst(RED4ext::CRTTISystem* rtti,
                              std::initializer_list<std::pair<const char*, const char*>> candidates)
        {
            for (const auto& [cls, field] : candidates)
            {
                const uint32_t off = Resolve(rtti, cls, field);
                if (off != 0) return off;
            }
            return 0;
        }
    }

    void Init()
    {
        auto& st = S();
        if (st.ready.load(std::memory_order_acquire)) return;

        auto* rtti = RED4ext::CRTTISystem::Get();
        if (!rtti)
        {
            log::Warn("[gwheel:rtti-off] RTTI system unavailable — all offsets will fall back to hardcoded values");
            return;
        }

        log::Info("[gwheel:rtti-off] ======== resolving struct offsets from game RTTI ========");

        // vehicle::BaseObject — has `physics` and `physicsData` handles.
        st.vehicleBaseObject_physics.store(
            ResolveFirst(rtti, {{"vehicleBaseObject", "physics"},
                                {"vehicleBaseObject", "Physics"}}),
            std::memory_order_release);
        st.vehicleBaseObject_physicsData.store(
            ResolveFirst(rtti, {{"vehicleBaseObject", "physicsData"},
                                {"vehicleBaseObject", "PhysicsData"}}),
            std::memory_order_release);

        // vehicle::PhysicsData — velocity + angularVelocity.
        st.physicsData_velocity.store(
            ResolveFirst(rtti, {{"vehiclePhysicsData", "velocity"}}),
            std::memory_order_release);
        st.physicsData_angularVelocity.store(
            ResolveFirst(rtti, {{"vehiclePhysicsData", "angularVelocity"}}),
            std::memory_order_release);

        // vehicle::WheeledPhysics — per-car handling tuning.
        st.wheeledPhysics_turnRate.store(
            ResolveFirst(rtti, {{"vehicleWheeledPhysics", "turnRate"}}),
            std::memory_order_release);
        st.wheeledPhysics_maxWheelTurnDeg.store(
            ResolveFirst(rtti, {{"vehicleWheeledPhysics", "maxWheelTurnDeg"}}),
            std::memory_order_release);
        st.wheeledPhysics_slipAngleCurveScale.store(
            ResolveFirst(rtti, {{"vehicleWheeledPhysics", "slipAngleCurveScale"}}),
            std::memory_order_release);
        st.wheeledPhysics_numDriveWheels.store(
            ResolveFirst(rtti, {{"vehicleWheeledPhysics", "numDriveWheels"}}),
            std::memory_order_release);
        st.wheeledPhysics_frontBackWheelDistance.store(
            ResolveFirst(rtti, {{"vehicleWheeledPhysics", "frontBackWheelDistance"}}),
            std::memory_order_release);

        log::Info("[gwheel:rtti-off] ======== dumping full property lists (for discovery) ========");
        DumpClassProperties("vehicleBaseObject");
        DumpClassProperties("vehiclePhysicsData");
        DumpClassProperties("vehicleWheeledPhysics");
        DumpClassProperties("vehiclePersistentDataPS");
        DumpClassProperties("vehicleWheelRuntimePSData");
        // Camera / action discovery sweeps were here on 2026-04-24 to find
        // direct-method callables for RearViewCamera / RadioMenu / Siren.
        // Results captured in the build log; the only lasting use was
        // wiring up `RadioNext` -> NextRadioReceiverStation. Sweep removed
        // from the startup path because it produces ~5 KB of one-time
        // output on every launch. The helpers (DumpClassesContaining /
        // DumpFunctionsContaining / DumpClassFunctions) remain available
        // in the header for ad-hoc reuse if we go hunting for more
        // direct-action paths later.
        log::Info("[gwheel:rtti-off] ======== offset resolution complete ========");

        st.ready.store(true, std::memory_order_release);
    }

    bool IsReady() { return S().ready.load(std::memory_order_acquire); }

    uint32_t VehicleBaseObject_physicsOffset()          { return S().vehicleBaseObject_physics.load(std::memory_order_acquire); }
    uint32_t VehicleBaseObject_physicsDataOffset()      { return S().vehicleBaseObject_physicsData.load(std::memory_order_acquire); }
    uint32_t PhysicsData_velocityOffset()               { return S().physicsData_velocity.load(std::memory_order_acquire); }
    uint32_t PhysicsData_angularVelocityOffset()        { return S().physicsData_angularVelocity.load(std::memory_order_acquire); }
    uint32_t WheeledPhysics_turnRateOffset()            { return S().wheeledPhysics_turnRate.load(std::memory_order_acquire); }
    uint32_t WheeledPhysics_maxWheelTurnDegOffset()     { return S().wheeledPhysics_maxWheelTurnDeg.load(std::memory_order_acquire); }
    uint32_t WheeledPhysics_slipAngleCurveScaleOffset() { return S().wheeledPhysics_slipAngleCurveScale.load(std::memory_order_acquire); }
    uint32_t WheeledPhysics_numDriveWheelsOffset()      { return S().wheeledPhysics_numDriveWheels.load(std::memory_order_acquire); }
    uint32_t WheeledPhysics_frontBackWheelDistanceOffset() { return S().wheeledPhysics_frontBackWheelDistance.load(std::memory_order_acquire); }

    namespace
    {
        // Cheap "is this pointer plausibly a valid heap allocation" check.
        // We VirtualQuery it and make sure it's committed, readable, and
        // not code/shared memory. Returns false for null / misaligned / bad.
        bool LooksLikeValidHeapPtr(void* p)
        {
            if (!p) return false;
            if (reinterpret_cast<uintptr_t>(p) < 0x10000) return false; // low memory / small ints
            if (reinterpret_cast<uintptr_t>(p) & 0x7) return false; // must be 8-byte aligned
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
            if (mbi.State != MEM_COMMIT) return false;
            const DWORD rw = PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
            if (!(mbi.Protect & rw)) return false;
            return true;
        }

        // Look for the pointer `needle` anywhere in the 8-byte-aligned slots
        // of `[base, base+range)`. Returns the offset of the first match, or
        // -1 if not found. Cheap linear scan; range is always small.
        std::ptrdiff_t FindPointerIn(void* base, std::ptrdiff_t range, void* needle)
        {
            auto* p = static_cast<const uintptr_t*>(base);
            const auto target = reinterpret_cast<uintptr_t>(needle);
            const std::ptrdiff_t slots = range / sizeof(uintptr_t);
            for (std::ptrdiff_t i = 0; i < slots; ++i)
            {
                if (p[i] == target)
                    return i * static_cast<std::ptrdiff_t>(sizeof(uintptr_t));
            }
            return -1;
        }
    }

    bool ProbeStructOffsets(void* vehicle)
    {
        auto& st = S();
        if (st.probeDone.load(std::memory_order_acquire)) return true;
        if (!vehicle) return false;

        // Candidate outer offsets to probe on vehicleBaseObject. LTBF's
        // fork had physics@0x2C8 and physicsData@0x2D0 — we scan a generous
        // window around that, covering +/- patch drift.
        constexpr std::ptrdiff_t kScanStart = 0x200;
        constexpr std::ptrdiff_t kScanEnd   = 0x400;
        // Inner range: back-pointer on PhysicsData was @0x180 in LTBF fork,
        // on Physics @0x60. Scan a superset range inside each candidate.
        constexpr std::ptrdiff_t kInnerStart = 0x00;
        constexpr std::ptrdiff_t kInnerEnd   = 0x200;

        log::InfoF("[gwheel:rtti-off] probing struct offsets from live vehicle=%p", vehicle);

        struct Hit { std::ptrdiff_t outerOff; std::ptrdiff_t backOff; void* inner; };
        Hit hits[8] = {};
        int hitCount = 0;

        auto* bytes = static_cast<char*>(vehicle);
        for (std::ptrdiff_t o = kScanStart; o + static_cast<std::ptrdiff_t>(sizeof(void*)) <= kScanEnd;
             o += sizeof(void*))
        {
            void* cand = *reinterpret_cast<void**>(bytes + o);
            if (!LooksLikeValidHeapPtr(cand)) continue;

            const std::ptrdiff_t backOff =
                FindPointerIn(cand, kInnerEnd - kInnerStart, vehicle);
            if (backOff < 0) continue;

            if (hitCount < static_cast<int>(std::size(hits)))
            {
                hits[hitCount++] = { o, kInnerStart + backOff, cand };
                log::InfoF("[gwheel:rtti-off]   candidate: outer=0x%03X -> inner=%p with back-ref @0x%03X",
                           static_cast<unsigned>(o), cand, static_cast<unsigned>(kInnerStart + backOff));
            }
        }

        if (hitCount == 0)
        {
            log::Warn("[gwheel:rtti-off] probe: no correlated pointers found — falling back to hardcoded offsets");
            st.probeDone.store(true, std::memory_order_release); // don't re-probe every tick
            return false;
        }

        // Classify hits by back-pointer-offset signature. From LTBF's SDK
        // fork the signatures were stable:
        //   PhysicsData.vehicle (back-ref) @ 0x180
        //   Physics.parent (back-ref)      @ 0x060
        // We match each candidate to the nearest signature within a
        // tolerance. Pointers with back-refs far from any signature
        // (e.g. chassis @0x050, movement @0x020) are rejected.
        constexpr std::ptrdiff_t kSigPhysicsData = 0x180;
        constexpr std::ptrdiff_t kSigPhysics     = 0x060;
        constexpr std::ptrdiff_t kSigTolerance   = 0x18;

        int physicsDataIdx = -1;
        int physicsIdx     = -1;
        std::ptrdiff_t bestPDDelta = kSigTolerance + 1;
        std::ptrdiff_t bestPhDelta = kSigTolerance + 1;

        for (int i = 0; i < hitCount; ++i)
        {
            const auto bo = hits[i].backOff;
            const auto pdDelta = std::abs(bo - kSigPhysicsData);
            const auto phDelta = std::abs(bo - kSigPhysics);
            if (pdDelta <= kSigTolerance && pdDelta < bestPDDelta)
            {
                physicsDataIdx = i;
                bestPDDelta = pdDelta;
            }
            if (phDelta <= kSigTolerance && phDelta < bestPhDelta)
            {
                physicsIdx = i;
                bestPhDelta = phDelta;
            }
        }

        if (physicsDataIdx >= 0)
        {
            const auto& h = hits[physicsDataIdx];
            st.probed_physicsDataOffset.store(static_cast<uint32_t>(h.outerOff),
                                              std::memory_order_release);
            log::InfoF("[gwheel:rtti-off] probe: physicsData -> outer=0x%03X (back-ref @0x%03X, inner=%p)",
                       static_cast<unsigned>(h.outerOff),
                       static_cast<unsigned>(h.backOff), h.inner);
        }
        if (physicsIdx >= 0 && physicsIdx != physicsDataIdx)
        {
            const auto& h = hits[physicsIdx];
            st.probed_physicsOffset.store(static_cast<uint32_t>(h.outerOff),
                                          std::memory_order_release);
            log::InfoF("[gwheel:rtti-off] probe: physics     -> outer=0x%03X (back-ref @0x%03X, inner=%p)",
                       static_cast<unsigned>(h.outerOff),
                       static_cast<unsigned>(h.backOff), h.inner);
        }

        // Populate the existing "vehicleBaseObject_*" accessors so the rest
        // of the plugin sees the probed values transparently.
        if (auto o = st.probed_physicsDataOffset.load(std::memory_order_acquire); o != 0)
            st.vehicleBaseObject_physicsData.store(o, std::memory_order_release);
        if (auto o = st.probed_physicsOffset.load(std::memory_order_acquire); o != 0)
            st.vehicleBaseObject_physics.store(o, std::memory_order_release);

        st.probeDone.store(true, std::memory_order_release);
        return (physicsDataIdx >= 0 || physicsIdx >= 0);
    }

    bool StructProbeDone() { return S().probeDone.load(std::memory_order_acquire); }

    void ProbeInnerWheeledFields(void* wp)
    {
        if (!wp) return;
        auto* bytes = static_cast<const char*>(wp);
        constexpr std::ptrdiff_t kScanEnd = 0x200;

        log::InfoF("[gwheel:rtti-off] ---- WheeledPhysics inner-field scan at %p ----", wp);

        // numDriveWheels: uint32 == 2 or 4. Highly distinctive — random
        // memory rarely hits either exact value aligned to uint32.
        {
            bool any = false;
            log::Info("[gwheel:rtti-off]   numDriveWheels candidates (uint32 == 2 or 4):");
            for (std::ptrdiff_t off = 0; off + 4 <= kScanEnd; off += 4)
            {
                const uint32_t v = *reinterpret_cast<const uint32_t*>(bytes + off);
                if (v == 2 || v == 4)
                {
                    log::InfoF("[gwheel:rtti-off]     +0x%03X = %u", static_cast<unsigned>(off), v);
                    any = true;
                }
            }
            if (!any) log::Info("[gwheel:rtti-off]     (none)");
        }

        // wheelbase: float in [1.0, 5.0] m. Multiple hits typical; cross-
        // vehicle consistency picks the winner.
        {
            bool any = false;
            log::Info("[gwheel:rtti-off]   wheelbase candidates (float in [1.0, 5.0]):");
            for (std::ptrdiff_t off = 0; off + 4 <= kScanEnd; off += 4)
            {
                const float f = *reinterpret_cast<const float*>(bytes + off);
                if (std::isfinite(f) && f >= 1.0f && f <= 5.0f)
                {
                    log::InfoF("[gwheel:rtti-off]     +0x%03X = %.3f", static_cast<unsigned>(off), f);
                    any = true;
                }
            }
            if (!any) log::Info("[gwheel:rtti-off]     (none)");
        }

        // turnRate: float in [0.5, 100.0]. Broader — includes rad/s and
        // deg/s interpretations. Cross-reference against numDriveWheels
        // and wheelbase hits to narrow.
        {
            bool any = false;
            log::Info("[gwheel:rtti-off]   turnRate candidates (float in [0.5, 100.0]):");
            for (std::ptrdiff_t off = 0; off + 4 <= kScanEnd; off += 4)
            {
                const float f = *reinterpret_cast<const float*>(bytes + off);
                if (std::isfinite(f) && f >= 0.5f && f <= 100.0f)
                {
                    log::InfoF("[gwheel:rtti-off]     +0x%03X = %.3f", static_cast<unsigned>(off), f);
                    any = true;
                }
            }
            if (!any) log::Info("[gwheel:rtti-off]     (none)");
        }

        log::Info("[gwheel:rtti-off] ---- end inner-field scan ----");
    }

    namespace
    {
        // Slot ids and their human-readable tags. Per-kind, each slot latches
        // independently. Operator diffs the per-slot full-float dumps after
        // the session to identify which offsets carry which live signal.
        enum ProbeSlot : int
        {
            Slot_Idle = 0,
            Slot_LeftCorner,
            Slot_RightCorner,
            Slot_HardBrake,
            Slot_HardAccel,
            Slot_Count
        };

        constexpr const char* kSlotName[Slot_Count] = {
            "idle", "left-corner", "right-corner", "hard-brake", "hard-accel",
        };

        // Per-kind latch state. The detour thread is the only writer.
        struct ProbeLatches
        {
            bool fired[Slot_Count] = {};
        };

        ProbeLatches& BikeLatches() { static ProbeLatches s; return s; }
        ProbeLatches& CarLatches()  { static ProbeLatches s; return s; }

        // Bike/car-derived live fields sit AFTER the WheeledPhysics base.
        // 1.52 SDK puts the boundary at 0xDE0; BikePhysics extends to 0xE80
        // (+0xA0), CarPhysics to 0xF50 (+0x170). 2.31 likely close. Scan
        // [0x800, kindEnd) — past the WheeledPhysics base, capping at each
        // derived class's known-1.52 size with a small margin.
        //
        // CRASH FIX (2026-05-02): the previous range [0xC00, 0x1200) overran
        // the heap allocation when a new vehicle was summoned with a tighter
        // page layout, hitting an unmapped page → access violation. The
        // VirtualQuery wrapper below now caps every scan at the live region
        // boundary regardless of the requested range, so any future range
        // bumps are safe.
        constexpr std::ptrdiff_t kBikeScanStart = 0x800;
        constexpr std::ptrdiff_t kBikeScanEnd   = 0xE80;
        constexpr std::ptrdiff_t kCarScanStart  = 0x800;
        constexpr std::ptrdiff_t kCarScanEnd    = 0xF50;

        // Cap [scanStart, scanEnd) at the boundary of the heap region that
        // contains `base`. VirtualQuery returns the start + size of the
        // committed region; reads past `base + (regionStart + regionSize)`
        // can crash. Returns the largest safe end offset, never larger than
        // requested. If VirtualQuery fails or the page isn't readable,
        // returns scanStart (so the scan loop is a no-op).
        std::ptrdiff_t SafeScanEnd(const void* base,
                                   std::ptrdiff_t scanStart,
                                   std::ptrdiff_t scanEnd)
        {
            if (!base) return scanStart;
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(base, &mbi, sizeof(mbi)) == 0) return scanStart;
            if (mbi.State != MEM_COMMIT) return scanStart;
            const DWORD rw = PAGE_READWRITE | PAGE_READONLY
                           | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
            if (!(mbi.Protect & rw)) return scanStart;

            const auto baseAddr   = reinterpret_cast<uintptr_t>(base);
            const auto regionEnd  = reinterpret_cast<uintptr_t>(mbi.BaseAddress)
                                  + mbi.RegionSize;
            if (regionEnd <= baseAddr) return scanStart;
            const std::ptrdiff_t maxOffset =
                static_cast<std::ptrdiff_t>(regionEnd - baseAddr);
            return std::min(scanEnd, maxOffset);
        }

        // Trigger thresholds. Tight enough to fire only on clear regimes:
        //   - Idle: nearly stationary, rim within deadband, no pedal input.
        //   - Cornering: yaw rate exceeds threshold AND driver steer agrees
        //     in sign (filters out yaw bursts from terrain bumps).
        //   - HardBrake / HardAccel: pedal saturated, speed regime sensible.
        constexpr float kIdleSpeedMax       = 1.0f;   // m/s
        constexpr float kIdleSteerAbs       = 0.05f;
        constexpr float kCornerYawAbs       = 0.4f;   // rad/s
        constexpr float kCornerSteerAbs     = 0.20f;
        constexpr float kCornerSpeedMin     = 4.0f;   // m/s
        constexpr float kBrakeMin           = 0.8f;
        constexpr float kBrakeSpeedMin      = 5.0f;   // m/s
        constexpr float kAccelMin           = 0.8f;
        constexpr float kAccelMaxBrake      = 0.05f;
        constexpr float kAccelSpeedMin      = 1.0f;

        // Pick the slot whose predicate is satisfied AND hasn't latched.
        // Returns Slot_Count when nothing applies. Order matters when multiple
        // predicates would match — Idle wins under low-input idle, corner
        // wins over brake when both apply, etc. — but in practice the gating
        // is tight enough that overlap is rare.
        int PickSlot(const InnerProbeState& s, const ProbeLatches& latches)
        {
            // Idle: stationary, no rim deflection, no pedal pressed.
            if (!latches.fired[Slot_Idle]
                && s.isOnGround
                && s.speedMps < kIdleSpeedMax
                && std::fabs(s.steerNorm) < kIdleSteerAbs
                && s.throttle < 0.05f && s.brake < 0.05f)
                return Slot_Idle;

            // Left corner: yaw signed negative + steer signed negative.
            // Sign convention: angVel.z is the world-space yaw rate. On a
            // bike or car turning left, the chassis yaws CCW (negative z in
            // CDPR's convention). Driver steer: negative = left.
            const bool yawLeft  = s.yawSigned < -kCornerYawAbs;
            const bool yawRight = s.yawSigned >  kCornerYawAbs;
            const bool steerLeft  = s.steerNorm < -kCornerSteerAbs;
            const bool steerRight = s.steerNorm >  kCornerSteerAbs;
            const bool moving   = s.isOnGround && s.speedMps > kCornerSpeedMin;

            if (!latches.fired[Slot_LeftCorner]  && moving && yawLeft  && steerLeft)
                return Slot_LeftCorner;
            if (!latches.fired[Slot_RightCorner] && moving && yawRight && steerRight)
                return Slot_RightCorner;

            if (!latches.fired[Slot_HardBrake]
                && s.isOnGround
                && s.brake > kBrakeMin
                && s.speedMps > kBrakeSpeedMin)
                return Slot_HardBrake;

            if (!latches.fired[Slot_HardAccel]
                && s.isOnGround
                && s.throttle > kAccelMin
                && s.brake < kAccelMaxBrake
                && s.speedMps > kAccelSpeedMin)
                return Slot_HardAccel;

            return Slot_Count;
        }

        // Full float dump (no domain filter) over [scanStart, scanEnd).
        // Output is one row per 4 fields so a 1.7KB region produces ~50
        // log lines. The operator diffs slots after the session — a packed
        // dump is far more compact than per-field rows for that workflow.
        void DumpAllFloats(const char* tag, const char* slotName,
                           const InnerProbeState& s, const void* base,
                           std::ptrdiff_t scanStart, std::ptrdiff_t scanEnd)
        {
            if (!base) return;
            const std::ptrdiff_t safeEnd = SafeScanEnd(base, scanStart, scanEnd);
            log::InfoF("[gwheel:rtti-off] ---- %s @ %s (speed=%.2f yaw=%+.3f steer=%+.3f thr=%.2f br=%.2f) ----",
                       tag, slotName,
                       s.speedMps, s.yawSigned, s.steerNorm, s.throttle, s.brake);
            log::InfoF("[gwheel:rtti-off] %s @ %s scan [0x%03X..0x%03X) base=%p (capped from 0x%03X)",
                       tag, slotName,
                       static_cast<unsigned>(scanStart),
                       static_cast<unsigned>(safeEnd),
                       base,
                       static_cast<unsigned>(scanEnd));
            scanEnd = safeEnd;
            if (scanEnd <= scanStart)
            {
                log::InfoF("[gwheel:rtti-off]   (region too small — skipping)");
                log::InfoF("[gwheel:rtti-off] ---- %s @ %s end ----", tag, slotName);
                return;
            }

            const auto* bytes = static_cast<const char*>(base);
            char line[256];
            int  inLine = 0;
            int  written = 0;
            line[0] = '\0';
            for (std::ptrdiff_t off = scanStart; off + 4 <= scanEnd; off += 4)
            {
                const float v = *reinterpret_cast<const float*>(bytes + off);
                if (!std::isfinite(v)) continue;
                const int n = std::snprintf(line + written, sizeof(line) - written,
                                            "%s+0x%03X=%+.4f",
                                            inLine == 0 ? "" : "  ",
                                            static_cast<unsigned>(off), v);
                if (n <= 0 || n >= static_cast<int>(sizeof(line) - written)) break;
                written += n;
                if (++inLine >= 4)
                {
                    log::InfoF("[gwheel:rtti-off]   %s", line);
                    line[0] = '\0';
                    written = 0;
                    inLine = 0;
                }
            }
            if (inLine > 0)
                log::InfoF("[gwheel:rtti-off]   %s", line);
            log::InfoF("[gwheel:rtti-off] ---- %s @ %s end ----", tag, slotName);
        }

        void TriggerSlot(const char* tag, const void* base,
                         std::ptrdiff_t scanStart, std::ptrdiff_t scanEnd,
                         ProbeLatches& latches, const InnerProbeState& s)
        {
            if (!base) return;
            const int slot = PickSlot(s, latches);
            if (slot >= Slot_Count) return;
            DumpAllFloats(tag, kSlotName[slot], s, base, scanStart, scanEnd);
            latches.fired[slot] = true;

            // Once-only summary when all slots have fired so the operator
            // sees a clean "kind complete" line in the log.
            bool allFired = true;
            for (bool b : latches.fired) { if (!b) { allFired = false; break; } }
            if (allFired)
                log::InfoF("[gwheel:rtti-off] **** %s probe: all slots captured ****", tag);
        }
    }

    void TriggerBikeProbeSlot(const void* bp, const InnerProbeState& st)
    {
        TriggerSlot("BikePhysics", bp, kBikeScanStart, kBikeScanEnd,
                    BikeLatches(), st);
    }

    void TriggerCarProbeSlot(const void* cp, const InnerProbeState& st)
    {
        TriggerSlot("CarPhysics", cp, kCarScanStart, kCarScanEnd,
                    CarLatches(), st);
    }

    void DumpClassProperties(const char* className)
    {
        auto* rtti = RED4ext::CRTTISystem::Get();
        if (!rtti || !className)
        {
            log::Warn("[gwheel:rtti-off] DumpClassProperties: RTTI or class name unavailable");
            return;
        }

        auto* cls = rtti->GetClass(RED4ext::CName(className));
        if (!cls)
        {
            log::WarnF("[gwheel:rtti-off] DumpClassProperties: class '%s' not found", className);
            return;
        }

        RED4ext::DynArray<RED4ext::CProperty*> props;
        cls->GetProperties(props);
        const uint32_t n = props.Size();
        log::InfoF("[gwheel:rtti-off] --- %s (size=0x%X, %u properties) ---", className, cls->size, n);
        for (uint32_t i = 0; i < n; ++i)
        {
            auto* p = props[i];
            if (!p) continue;
            const char* name = p->name.ToString();
            const char* typeName = p->type ? p->type->GetName().ToString() : "?";
            log::InfoF("[gwheel:rtti-off]   0x%04X  %s : %s",
                       p->valueOffset, name ? name : "?", typeName ? typeName : "?");
        }
    }

    namespace
    {
        // Lowercase ASCII compare-substring. Cheap, allocation-free.
        bool ContainsCI(const char* haystack, const char* needle)
        {
            if (!haystack || !needle) return false;
            for (const char* p = haystack; *p; ++p)
            {
                size_t i = 0;
                while (needle[i] && p[i])
                {
                    char a = p[i], b = needle[i];
                    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                    if (a != b) break;
                    ++i;
                }
                if (!needle[i]) return true;
            }
            return false;
        }

        // Format one function signature: name(arg0:T0, arg1:T1, ...) -> ret.
        // Writes into `out` (size cap). Truncation OK — diagnostic only.
        void FormatFunctionSig(const RED4ext::CBaseFunction* fn, char* out, size_t cap)
        {
            if (!fn || !out || cap == 0) return;
            out[0] = '\0';
            const char* nm = fn->fullName.ToString();
            int written = std::snprintf(out, cap, "%s(", nm ? nm : "?");
            const uint32_t nParams = fn->params.Size();
            for (uint32_t i = 0; i < nParams; ++i)
            {
                auto* p = fn->params[i];
                if (!p) continue;
                const char* pn = p->name.ToString();
                const char* pt = p->type ? p->type->GetName().ToString() : "?";
                written += std::snprintf(out + written,
                                         (static_cast<size_t>(written) < cap) ? cap - static_cast<size_t>(written) : 0,
                                         "%s%s:%s",
                                         (i ? ", " : ""), pn ? pn : "?", pt ? pt : "?");
                if (static_cast<size_t>(written) + 1 >= cap) break;
            }
            const char* rt = (fn->returnType && fn->returnType->type)
                                ? fn->returnType->type->GetName().ToString()
                                : "void";
            std::snprintf(out + written,
                          (static_cast<size_t>(written) < cap) ? cap - static_cast<size_t>(written) : 0,
                          ") -> %s", rt ? rt : "void");
        }
    }

    void DumpClassFunctions(const char* className)
    {
        auto* rtti = RED4ext::CRTTISystem::Get();
        if (!rtti || !className)
        {
            log::Warn("[gwheel:rtti-off] DumpClassFunctions: RTTI or class name unavailable");
            return;
        }

        auto* cls = rtti->GetClass(RED4ext::CName(className));
        if (!cls)
        {
            log::WarnF("[gwheel:rtti-off] DumpClassFunctions: class '%s' not found", className);
            return;
        }

        const uint32_t fns = cls->funcs.Size();
        const uint32_t sfs = cls->staticFuncs.Size();
        log::InfoF("[gwheel:rtti-off] --- %s methods (%u instance, %u static) ---",
                   className, fns, sfs);
        char buf[1024];
        for (uint32_t i = 0; i < fns; ++i)
        {
            auto* f = cls->funcs[i];
            if (!f) continue;
            FormatFunctionSig(f, buf, sizeof(buf));
            log::InfoF("[gwheel:rtti-off]   inst %s", buf);
        }
        for (uint32_t i = 0; i < sfs; ++i)
        {
            auto* f = cls->staticFuncs[i];
            if (!f) continue;
            FormatFunctionSig(f, buf, sizeof(buf));
            log::InfoF("[gwheel:rtti-off]   stat %s", buf);
        }
    }

    void DumpClassesContaining(std::initializer_list<const char*> subs)
    {
        auto* rtti = RED4ext::CRTTISystem::Get();
        if (!rtti)
        {
            log::Warn("[gwheel:rtti-off] DumpClassesContaining: RTTI unavailable");
            return;
        }

        log::Info("[gwheel:rtti-off] === sweeping RTTI classes for substring matches ===");
        RED4ext::DynArray<RED4ext::CClass*> classes;
        rtti->GetClasses(nullptr, classes, nullptr, true);
        const uint32_t n = classes.Size();
        size_t hits = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            auto* cls = classes[i];
            if (!cls) continue;
            const char* nm = cls->GetName().ToString();
            if (!nm) continue;
            for (const char* s : subs)
            {
                if (ContainsCI(nm, s))
                {
                    log::InfoF("[gwheel:rtti-off]   class %s (size=0x%X, props=%u, funcs=%u)",
                               nm, cls->size, cls->props.Size(), cls->funcs.Size());
                    ++hits;
                    break;
                }
            }
        }
        log::InfoF("[gwheel:rtti-off] === sweep complete (%zu matches across %u classes) ===",
                   hits, n);
    }

    void DumpFunctionsContaining(std::initializer_list<const char*> subs)
    {
        auto* rtti = RED4ext::CRTTISystem::Get();
        if (!rtti)
        {
            log::Warn("[gwheel:rtti-off] DumpFunctionsContaining: RTTI unavailable");
            return;
        }

        log::Info("[gwheel:rtti-off] === sweeping RTTI methods for substring matches ===");
        RED4ext::DynArray<RED4ext::CBaseFunction*> fns;
        rtti->GetClassFunctions(fns);
        const uint32_t n = fns.Size();
        size_t hits = 0;
        char buf[1024];
        for (uint32_t i = 0; i < n; ++i)
        {
            auto* f = fns[i];
            if (!f) continue;
            const char* fn = f->fullName.ToString();
            if (!fn) continue;
            for (const char* s : subs)
            {
                if (ContainsCI(fn, s))
                {
                    FormatFunctionSig(f, buf, sizeof(buf));
                    log::InfoF("[gwheel:rtti-off]   %s", buf);
                    ++hits;
                    break;
                }
            }
        }
        log::InfoF("[gwheel:rtti-off] === function sweep complete (%zu matches across %u methods) ===",
                   hits, n);
    }
}
