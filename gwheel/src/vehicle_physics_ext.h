#pragma once

// Dynamic, RTTI-resolved accessors for vehicle physics state.
//
// Most physics class names CDPR uses (vehicleWheeledPhysics / vehicleCarPhysics
// / vehicleBikePhysics / physicsVehiclePhysicsInsert1) are NOT reflection-
// registered on 2.31 — every CRTTISystem::GetClass on those names returns
// null. The redesigned FFB pipeline reads authored physics from TweakDB
// record classes (which ARE RTTI-registered); see gwheel/src/tweakdb_physics.
// What survives in this file is just the small set of pointer/scalar reads
// against classes that DO resolve through RTTI (vehicleBaseObject,
// vehiclePhysicsData) plus the validated 2.31 hardcoded inner offsets for
// vehicleWheeledPhysics that were triangulated by ProbeInnerWheeledFields.

#include "rtti_offsets.h"
#include "logging.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>

namespace gwheel::vehicle_ext
{
    // Hardcoded-fallback offsets. Outer offsets (physics / physicsData on
    // vehicleBaseObject) and PhysicsData inner offsets come from LTBF's SDK
    // fork (new-types branch, game 1.52-1.61) and have held steady into
    // 2.31 — the runtime back-pointer probe in rtti_offsets.cpp validates
    // them each session. Inner WheeledPhysics offsets drifted for 2.31 and
    // were re-discovered via the ProbeInnerWheeledFields scanner —
    // triangulated across car/bike/truck by matching each field to its
    // expected value domain (wheelbase ∈ [1,5]m, numDriveWheels ∈ {2,4},
    // etc.). If CDPR patches the struct layout again, re-enable the inner
    // probe on next vehicle mount and update these constants.
    namespace fallback
    {
        // Outer (vehicleBaseObject) — stable across 1.52 → 2.31.
        inline constexpr std::ptrdiff_t kVehicleBaseObject_physics      = 0x2C8;
        inline constexpr std::ptrdiff_t kVehicleBaseObject_physicsData  = 0x2D0;

        // PhysicsData (base Physics layout) — stable across 1.52 → 2.31.
        inline constexpr std::ptrdiff_t kPhysicsData_velocity           = 0x18;
        inline constexpr std::ptrdiff_t kPhysicsData_angularVelocity    = 0x24;

        // WheeledPhysics inner fields — triangulated for 2.31 (build 5294808).
        // Re-probe if these break on a future patch.
        inline constexpr std::ptrdiff_t kWheeledPhysics_numDriveWheels         = 0xE0;  // was 0xD8 in 1.52-fork
        inline constexpr std::ptrdiff_t kWheeledPhysics_frontBackWheelDistance = 0xE4;  // was 0xDC
        inline constexpr std::ptrdiff_t kWheeledPhysics_turnRate               = 0x114; // was 0xF4
        inline constexpr std::ptrdiff_t kWheeledPhysics_maxWheelTurnDeg        = 0x1A4; // was 0xC10
        inline constexpr std::ptrdiff_t kWheeledPhysics_slipAngleCurveScale    = 0xC78; // still valid; varied per-car in probe
    }

    // Bike-derived field offsets (BikePhysics extends WheeledPhysics).
    // Triangulated 2026-05-02 from event-triggered slot dumps in
    // [0x800, 0xE80) — see rtti_offsets::TriggerBikeProbeSlot. These
    // sit just past the WheeledPhysics base layout; the SDK 1.52 fork
    // had them at the equivalent 0xE10+ region post-WheeledPhysics.
    namespace fallback_bike
    {
        // turnInput: live float clamped to [-1, +1], tracks driver steer.
        // Verified: idle=0.0, left-corner@steer=-1.0 -> -1.0,
        // right-corner@steer=+1.0 -> +1.0.
        inline constexpr std::ptrdiff_t kBikePhysics_turnInput        = 0xE1C;

        // bikeCurrentTilt: live signed float, lean angle in radians.
        // Verified: idle=0.0, swings to -0.5/+0.5 in cornering.
        // Maps to the SDK 1.52 `tiltRelated` field.
        inline constexpr std::ptrdiff_t kBikePhysics_currentTilt      = 0xE2C;

        // bikeTransversalForce: live signed float, lateral cornering force
        // (newtons). Sign flips with cornering direction. Magnitudes
        // observed up to ±20 N in moderate corners, more under hard lean.
        inline constexpr std::ptrdiff_t kBikePhysics_transversalForce = 0xE3C;
    }

    // Car-derived field offsets (CarPhysics extends WheeledPhysics).
    // Triangulated 2026-05-02 from event-triggered slot dumps. Lives in
    // the same post-WheeledPhysics region as the bike fields. The 1.52
    // SDK had veh_bank_body_l_r/_f_b at 0xE48/0xE4C; in 2.31 those slots
    // read zero across all driving regimes — the live LR signal landed
    // at 0xE38 instead (1.52 had `affectsTurnX` there). Magnitude/sign
    // pattern matches a normalized chassis-roll direction.
    namespace fallback_car
    {
        // carBankBodyLR: live signed float, normalized lateral roll.
        // Verified: idle=0.0, left-corner@steer=-0.21 -> -0.81,
        // right-corner@steer=+0.22 -> +0.79. Magnitude ~0.8 at full
        // cornering load, bounded near ±1.
        inline constexpr std::ptrdiff_t kCarPhysics_bankBodyLR    = 0xE38;

        // carCurrTurnInput: live signed float, tracks driver steer.
        // Verified to match input within ~0.02. Useful as a sanity
        // check rather than a feedback term — we already have driver
        // steer in the FfbDriverState.
        inline constexpr std::ptrdiff_t kCarPhysics_currTurnInput = 0xE44;
    }

    // ---- Domain validation -----------------------------------------------
    //
    // Reader contract: every physics field read goes through this validator.
    // It clamps nothing (the caller decides whether to substitute a default),
    // but logs a one-time warning per (kind, field) when the read value
    // falls outside the declared physical-plausibility domain. That means a
    // wrong-offset RTTI hit, a wrong-offset fallback, or a mid-air null
    // pointer all surface as a single grep in the log.
    //
    // Warn-once is keyed off the field-name pointer to avoid log spam — each
    // string-literal name has a unique address, and the bit gets latched on
    // first failure. The intent is "tell me on first bad read; suppress the
    // next 10,000".
    template <typename T>
    inline bool ValidateInDomain(const char* fieldName, T value, T lo, T hi,
                                 bool fromFallback = false)
    {
        if (std::isfinite(static_cast<double>(value)) && value >= lo && value <= hi)
            return true;
        // Latch + log once per field-name pointer. The static map-via-CName
        // would be cleaner but adds dependency weight; this hash table is
        // small and lock-free.
        static std::atomic<const char*> seen[64] = {};
        for (auto& slot : seen)
        {
            const char* cur = slot.load(std::memory_order_acquire);
            if (cur == fieldName) return false; // already warned
            if (cur == nullptr)
            {
                const char* expected = nullptr;
                if (slot.compare_exchange_strong(expected, fieldName,
                                                 std::memory_order_acq_rel))
                {
                    char vbuf[64];
                    std::snprintf(vbuf, sizeof(vbuf), "%g",
                                  static_cast<double>(value));
                    log::WarnF("[gwheel:physics] domain-fail %s: read=%s expected=[%g, %g] (%s)",
                               fieldName ? fieldName : "?",
                               vbuf,
                               static_cast<double>(lo),
                               static_cast<double>(hi),
                               fromFallback ? "fallback offset" : "rtti offset");
                    return false;
                }
                // Lost race; continue scanning for this field.
            }
        }
        // Table full; warn anyway (rare).
        log::WarnF("[gwheel:physics] domain-fail %s (table full)",
                   fieldName ? fieldName : "?");
        return false;
    }

    // Return the runtime offset if RTTI resolved it, else the fallback.
    inline uint32_t ResolvedOr(uint32_t rttiOffset, std::ptrdiff_t fallback)
    {
        return rttiOffset ? rttiOffset : static_cast<uint32_t>(fallback);
    }

    // --- pointer-field accessors -------------------------------------------
    // physics and physicsData are both pointers in the vehicle struct. Read
    // them through the resolved offset. Can return nullptr if the vehicle's
    // physics chain isn't set up yet (brief window during mount).

    inline void* GetPhysicsRaw(void* vehicle)
    {
        const uint32_t off = ResolvedOr(rtti_offsets::VehicleBaseObject_physicsOffset(),
                                        fallback::kVehicleBaseObject_physics);
        return *reinterpret_cast<void**>(static_cast<char*>(vehicle) + off);
    }

    inline void* GetPhysicsDataRaw(void* vehicle)
    {
        const uint32_t off = ResolvedOr(rtti_offsets::VehicleBaseObject_physicsDataOffset(),
                                        fallback::kVehicleBaseObject_physicsData);
        return *reinterpret_cast<void**>(static_cast<char*>(vehicle) + off);
    }

    // --- PhysicsData scalar reads ------------------------------------------

    // Reads Vector3 velocity from physicsData into out[3]. Returns true on
    // success, false if physicsData is null.
    inline bool ReadVelocity(void* vehicle, float out[3])
    {
        void* pd = GetPhysicsDataRaw(vehicle);
        if (!pd) return false;
        const uint32_t off = ResolvedOr(rtti_offsets::PhysicsData_velocityOffset(),
                                        fallback::kPhysicsData_velocity);
        const float* src = reinterpret_cast<const float*>(static_cast<char*>(pd) + off);
        out[0] = src[0]; out[1] = src[1]; out[2] = src[2];
        return true;
    }

    inline bool ReadAngularVelocity(void* vehicle, float out[3])
    {
        void* pd = GetPhysicsDataRaw(vehicle);
        if (!pd) return false;
        const uint32_t off = ResolvedOr(rtti_offsets::PhysicsData_angularVelocityOffset(),
                                        fallback::kPhysicsData_angularVelocity);
        const float* src = reinterpret_cast<const float*>(static_cast<char*>(pd) + off);
        out[0] = src[0]; out[1] = src[1]; out[2] = src[2];
        return true;
    }

    // --- WheeledPhysics scalar reads ---------------------------------------

    struct WheeledPhysicsSnapshot
    {
        float    turnRate               = 0.f;
        float    maxWheelTurnDeg        = 0.f;
        float    slipAngleCurveScale    = 0.f;
        uint32_t numDriveWheels         = 0;
        float    frontBackWheelDistance = 0.f;
        bool     valid                  = false;
    };

    // --- BikePhysics / CarPhysics live-state reads ---------------------------
    //
    // Bike and car derived classes are NOT RTTI-visible. We read the live
    // signals via hardcoded offsets validated by domain checks. Each helper
    // returns a small struct with a `valid` flag — false when the physics
    // pointer is null, when any read falls outside the field's plausible
    // domain (caught by ValidateInDomain), or when the chassis kind doesn't
    // match. Callers fall back to the proxy heuristics in that case.

    struct BikeLiveFields
    {
        float currentTilt       = 0.f;  // radians, signed
        float transversalForce  = 0.f;  // newtons, signed
        float turnInput         = 0.f;  // [-1, +1], signed
        bool  valid             = false;
    };

    struct CarLiveFields
    {
        float bankBodyLR        = 0.f;  // signed normalized roll direction
        float currTurnInput     = 0.f;  // [-1, +1], signed; tracks driver
        bool  valid             = false;
    };

    // Sanity check on a heap-allocated game pointer. Catches:
    //   - null
    //   - small-int sentinels (anything below 0x10000 is reserved / invalid)
    //   - all-ones sentinel (0xFFFFFFFFFFFFFFFF — written by CDPR's allocator
    //     into freed-pointer slots during teardown; quit-time crash 2026-05-02)
    //   - high-half-canonical addresses (Win64 user-space tops out below
    //     0x00007FFFFFFFFFFF)
    // Cheap range check, no syscall — safe to call from the per-tick hot path.
    inline bool LooksLikeUserPtr(const void* p)
    {
        const auto v = reinterpret_cast<uintptr_t>(p);
        return v >= 0x10000 && v < 0x00007FFFFFFFFFFFull;
    }

    inline BikeLiveFields ReadBikeLiveFields(void* vehicle)
    {
        BikeLiveFields out;
        if (!LooksLikeUserPtr(vehicle)) return out;
        void* bp = GetPhysicsRaw(vehicle);
        if (!LooksLikeUserPtr(bp)) return out;
        const auto* bytes = static_cast<const char*>(bp);

        const float tilt = *reinterpret_cast<const float*>(
            bytes + fallback_bike::kBikePhysics_currentTilt);
        const float force = *reinterpret_cast<const float*>(
            bytes + fallback_bike::kBikePhysics_transversalForce);
        const float input = *reinterpret_cast<const float*>(
            bytes + fallback_bike::kBikePhysics_turnInput);

        // Domain bounds: tilt up to ~1.5 rad covers the most extreme bike
        // lean. transversalForce can reach hundreds of N at speed, so cap
        // generously. turnInput is clamped to [-1, +1] by the game.
        const bool tiltOk  = ValidateInDomain("bikeCurrentTilt",      tilt,  -1.5f, 1.5f, true);
        const bool forceOk = ValidateInDomain("bikeTransversalForce", force, -2000.f, 2000.f, true);
        const bool inputOk = ValidateInDomain("bikeTurnInput",        input, -1.05f, 1.05f, true);
        if (!tiltOk || !forceOk || !inputOk) return out;

        out.currentTilt      = tilt;
        out.transversalForce = force;
        out.turnInput        = input;
        out.valid            = true;
        return out;
    }

    inline CarLiveFields ReadCarLiveFields(void* vehicle)
    {
        CarLiveFields out;
        if (!LooksLikeUserPtr(vehicle)) return out;
        void* cp = GetPhysicsRaw(vehicle);
        if (!LooksLikeUserPtr(cp)) return out;
        const auto* bytes = static_cast<const char*>(cp);

        const float bank  = *reinterpret_cast<const float*>(
            bytes + fallback_car::kCarPhysics_bankBodyLR);
        const float input = *reinterpret_cast<const float*>(
            bytes + fallback_car::kCarPhysics_currTurnInput);

        // bankBodyLR observed up to ±0.85 in normal play; allow ±2 headroom.
        const bool bankOk  = ValidateInDomain("carBankBodyLR",    bank,  -2.f, 2.f, true);
        const bool inputOk = ValidateInDomain("carCurrTurnInput", input, -1.05f, 1.05f, true);
        if (!bankOk || !inputOk) return out;

        out.bankBodyLR    = bank;
        out.currTurnInput = input;
        out.valid         = true;
        return out;
    }

    inline WheeledPhysicsSnapshot ReadWheeledPhysics(void* vehicle)
    {
        WheeledPhysicsSnapshot s;
        void* wp = GetPhysicsRaw(vehicle);
        if (!wp) return s;
        const auto at = [&](uint32_t off) -> const char* {
            return static_cast<const char*>(wp) + off;
        };
        s.turnRate               = *reinterpret_cast<const float*>(at(ResolvedOr(
            rtti_offsets::WheeledPhysics_turnRateOffset(),
            fallback::kWheeledPhysics_turnRate)));
        s.maxWheelTurnDeg        = *reinterpret_cast<const float*>(at(ResolvedOr(
            rtti_offsets::WheeledPhysics_maxWheelTurnDegOffset(),
            fallback::kWheeledPhysics_maxWheelTurnDeg)));
        s.slipAngleCurveScale    = *reinterpret_cast<const float*>(at(ResolvedOr(
            rtti_offsets::WheeledPhysics_slipAngleCurveScaleOffset(),
            fallback::kWheeledPhysics_slipAngleCurveScale)));
        s.numDriveWheels         = *reinterpret_cast<const uint32_t*>(at(ResolvedOr(
            rtti_offsets::WheeledPhysics_numDriveWheelsOffset(),
            fallback::kWheeledPhysics_numDriveWheels)));
        s.frontBackWheelDistance = *reinterpret_cast<const float*>(at(ResolvedOr(
            rtti_offsets::WheeledPhysics_frontBackWheelDistanceOffset(),
            fallback::kWheeledPhysics_frontBackWheelDistance)));
        s.valid = true;
        return s;
    }
}
