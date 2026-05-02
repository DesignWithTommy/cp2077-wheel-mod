#include "vehicle_hook.h"
#include "plugin.h"
#include "logging.h"
#include "sources.h"
#include "config.h"
#include "wheel.h"
#include "vehicle_physics_ext.h"
#include "rtti_offsets.h"
#include "tweakdb_physics.h"
#include "axis_resolver.h"

#include <RED4ext/Relocation.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>
#include <RED4ext/Scripting/Natives/vehicleBaseObject.hpp>
#include <RED4ext/RTTISystem.hpp>
#include <RED4ext/RTTITypes.hpp>
#include <RED4ext/CName.hpp>
#include <RED4ext/CNamePool.hpp>
#include <RED4ext/Handle.hpp>
#include <RED4ext/ISerializable.hpp>
#include <RED4ext/Scripting/CProperty.hpp>
#include <RED4ext/Scripting/Natives/Generated/Vector4.hpp>
#include <RED4ext/Scripting/Utils.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace gwheel::vehicle_hook
{
    namespace
    {
        // vehicle::BaseObject::UpdateVehicleCameraInput(self)
        // Hash from Let There Be Flight (MIT licensed). Resolved per-patch
        // by RED4ext.dll's address database. Fires per-vehicle per-tick;
        // used here to write wheel axis values into the player vehicle's
        // input struct each frame. Non-player vehicles are skipped so
        // traffic AI keeps driving normally.
        using UpdateVehicleCameraInputFn = void (*)(void*);
        constexpr uint32_t kUpdateVehicleCameraInputHash = 501486464u;

        // Input fields in vehicle::BaseObject for game build 5294808 (CP2077
        // v2.31). The RED4ext SDK struct at vehicleBaseObject.hpp labels
        // +0x264 as `acceleration` and +0x268 as `deceleration`; we overwrite
        // them after vanilla g_original(self) runs so the values land as
        // processed drive commands. The +0x278 steer field sits inside the
        // SDK's unk26C[0x2A3-0x26C] gap (no label), so we keep that one as
        // an empirical constant — found via field-probe sweep 2026-04-21.
        // Re-probe steer if CDPR patches the vehicleBaseObject struct.
        namespace off
        {
            constexpr std::ptrdiff_t kInputSteer = 0x278; // float, [-1..1], += right
        }

        UpdateVehicleCameraInputFn g_original = nullptr;
        void* g_target = nullptr;

        std::atomic<uint64_t> g_fireCount{0};
        std::atomic<uint64_t> g_injectCount{0};
        std::atomic<bool>     g_attached{false};

        // Cached pointer of the player's currently-mounted vehicle. The
        // detour fires for many vehicles each tick (parked cars, visible
        // traffic with active camera updates, etc.); without this filter,
        // our input injection writes to every one of them and remote-drives
        // them all. Set by the redscript mount/unmount event wrappers via
        // GWheel_Set/ClearPlayerVehicle natives. nullptr = no injection.
        std::atomic<void*>    g_playerVehicle{nullptr};

        // Cached RTTI handle for vehicleBaseObject::GetCurrentSpeed, looked
        // up once on first use. Calling it via ExecuteFunction(self, fn, &out)
        // returns forward speed in m/s with sign preserved (negative when
        // reversing). Null means lookup failed and we've logged it once.
        std::atomic<RED4ext::CClassFunction*> g_getCurrentSpeedFn{nullptr};
        std::atomic<bool>                     g_getCurrentSpeedLookupTried{false};

        // Cached RTTI handle for vehicleBaseObject::GetVehiclePS(), which
        // returns a handle to the vehicle's persistent-state component
        // (native type vehiclePersistentDataPS). The PS stores a
        // StaticArray<WheelRuntimePSData, 4> at offset 0x78; each entry
        // begins with a CName for the material currently under that
        // wheel (previousTouchedMaterial). That's the game-authored
        // surface signal (asphalt / dirt / gravel / metal / etc.) that
        // CP2077 drives its controller rumble off.
        std::atomic<RED4ext::CClassFunction*> g_getVehiclePSFn{nullptr};
        std::atomic<bool>                     g_getVehiclePSLookupTried{false};

        // Cached RTTI handle for vehicleBaseObject::GetWorldRight(),
        // inherited from gameObject / entEntity. Returns the vehicle's
        // right-axis unit vector in world space. Dotted with the linear
        // velocity, gives the car-local lateral velocity component —
        // the key input for slip-angle / countersteer FFB.
        std::atomic<RED4ext::CClassFunction*> g_getWorldRightFn{nullptr};
        std::atomic<bool>                     g_getWorldRightLookupTried{false};

        // Per-wheel material hashes from the last tick (CName hashes are
        // uint64_t). Initialised to 0; any non-zero change is logged.
        // Index 0-3 = wheel 0-3 (typically FL / FR / RL / RR).
        std::atomic<uint64_t> g_lastWheelMat[4]{};

        // wheelRuntimeData offset on the handle returned by GetVehiclePS().
        // GetVehiclePS() actually returns a `VehicleComponentPS` instance,
        // which is a SIBLING class of the native `vehiclePersistentDataPS`
        // (both extend `gameComponentPS`) — they are NOT the same type,
        // despite sharing the "PS" conceptual role. VehicleComponentPS
        // does not expose wheelRuntimeData on this build, so once we
        // discover that fact we latch g_surfaceLookupFailed and skip the
        // probe + read entirely. A future pass could walk to the real
        // vehiclePersistentDataPS via the persistency system (lookup by
        // persistentID) or by probing for a direct pointer on the vehicle
        // struct.
        std::atomic<uint32_t> g_psWheelDataOffset{0};
        std::atomic<bool>     g_surfaceLookupFailed{false};
        std::atomic<bool>     g_persistentStateProbeDone{false};
        constexpr std::ptrdiff_t kPS_wheelEntryStride       = 0x18;
        // vehicleBaseObject.persistentState offset (from RTTI dump).
        constexpr std::ptrdiff_t kVehicleBaseObject_persistentState = 0x168;

        // Last vehicle pointer for which we've dumped per-car physics values
        // to the log. Cleared in SetPlayerVehicle when a new vehicle mounts,
        // so every fresh ride prints its turnRate / wheelbase / slip-curve
        // etc. once to the log.
        std::atomic<void*> g_lastPhysicsLoggedVehicle{nullptr};

        // Cached per-car snapshot resolved once per mount.
        //
        // Calling tweakdb_physics::ReadPerCarPhysics every tick was
        // empirically crashing the game (0xC0000005 reading at 0x2BD on
        // Galena mount, 2026-05-01) — bisect proved the per-tick path was
        // the trigger; the once-per-vehicle dump alone is safe. The
        // suspected mechanism is the per-tick ExecuteFunction(GetRecord)
        // invocation inside ResolvePerVehicle leaking or over-decrementing
        // a Handle<gamedataVehicle_Record> refcount; over a few ticks the
        // game ends up dereferencing a freed record at an internal field
        // (0x2BD on null is a small-offset null-deref pattern).
        //
        // Resolution: do the full chain resolution + authored-field reads
        // once per `self` (in the snapshot-dump block) and stash the
        // result here. Per tick we lift the authored values from this
        // stash and overwrite the live-state slots from the already-
        // computed ReadVelocity / ReadAngularVelocity values further down
        // in the FFB block — no second TweakDB chain walk per tick.
        // Cleared on player-vehicle change.
        std::mutex                      g_perCarMu;
        void*                           g_perCarSelf = nullptr;
        tweakdb_physics::PerCarPhysics  g_perCarSnapshot{};

        // Cached per-tick yaw reference (rad/s) derived from the current
        // vehicle's WheeledPhysics::turnRate. Zero means "fall back to the
        // hardcoded default" (e.g. tanks, vehicles without accessible
        // physics, first-tick race before the read succeeds). Updated by
        // the detour each tick.
        std::atomic<float> g_perCarYawRef{0.f};

        // Previous tick's angular velocity, used to compute the suspension-
        // activity signal that drives the road-surface FFB. Only the roll
        // (X) and pitch (Y) axes matter — yaw (Z) tracks steering input
        // rather than the road. Initialised to zero; first-tick delta is
        // effectively the current ω, which settles within a tick.
        std::atomic<float> g_prevAngVelX{0.f};
        std::atomic<float> g_prevAngVelY{0.f};

        // Previous tick's vertical linear velocity (world Z). Drives the
        // OTHER half of the suspension-activity signal: bouncing up/down
        // on dirt/gravel registers as |Δvz| even when chassis rotation
        // barely moves. Smooth asphalt at cruise ≈ 0; rough terrain
        // spikes the signal.
        std::atomic<float> g_prevLinVelZ{0.f};

        // Resolve vehicleBaseObject::GetVehiclePS once, return null on
        // failure (logged once). Pattern mirrors ReadVehicleSpeed below.
        RED4ext::CClassFunction* GetVehiclePSFn()
        {
            auto* fn = g_getVehiclePSFn.load(std::memory_order_acquire);
            if (fn) return fn;
            if (g_getVehiclePSLookupTried.exchange(true, std::memory_order_acq_rel))
                return nullptr;
            auto* rtti = RED4ext::CRTTISystem::Get();
            if (!rtti)
            {
                log::Warn("[gwheel:hook] RTTI unavailable — GetVehiclePS lookup skipped");
                return nullptr;
            }
            auto* cls = rtti->GetClass(RED4ext::CName("vehicleBaseObject"));
            if (!cls)
            {
                log::Warn("[gwheel:hook] RTTI class 'vehicleBaseObject' not found (GetVehiclePS)");
                return nullptr;
            }
            auto* resolved = cls->GetFunction(RED4ext::CName("GetVehiclePS"));
            if (!resolved)
            {
                log::Warn("[gwheel:hook] RTTI method 'vehicleBaseObject::GetVehiclePS' not found — wheel-surface detection disabled");
                return nullptr;
            }
            g_getVehiclePSFn.store(resolved, std::memory_order_release);
            log::InfoF("[gwheel:hook] RTTI resolved: vehicleBaseObject::GetVehiclePS -> %p", resolved);
            return resolved;
        }

        // One-shot diagnostic to identify the PS type we get back and
        // reveal where CNames actually live inside it on this build. The
        // hardcoded offset (0x78) is from SDK headers which are known to
        // drift across game patches. We log the first time a PS is read
        // so we can correlate the SDK layout against live memory.
        std::atomic<bool> g_psDiagnosticLogged{false};

        void LogPSDiagnostic(RED4ext::ISerializable* ps)
        {
            if (!ps) return;
            // Latch per-pointer so repeated probes of the same object
            // are silent but each distinct candidate gets one dump.
            static std::atomic<RED4ext::ISerializable*> s_lastDumped{nullptr};
            if (s_lastDumped.exchange(ps, std::memory_order_acq_rel) == ps) return;

            // RTTI class name of the actual runtime type.
            const char* typeName = "?";
            if (auto* t = ps->GetType())
                typeName = t->GetName().ToString();
            log::InfoF("[gwheel:surface] PS handle type=%s at %p", typeName ? typeName : "?", ps);

            // Dump the first 0x200 bytes as 64-bit words. Rows that look
            // like small hash-distribution numbers (not pointer-range
            // 0x00007FF...) are CName-candidate offsets.
            const auto* bytes = reinterpret_cast<const uint64_t*>(ps);
            for (int off = 0; off < 0x200; off += 0x40)
            {
                log::InfoF("[gwheel:surface]   PS+0x%03X: %016llX %016llX %016llX %016llX %016llX %016llX %016llX %016llX",
                           off,
                           (unsigned long long)bytes[(off + 0x00) / 8],
                           (unsigned long long)bytes[(off + 0x08) / 8],
                           (unsigned long long)bytes[(off + 0x10) / 8],
                           (unsigned long long)bytes[(off + 0x18) / 8],
                           (unsigned long long)bytes[(off + 0x20) / 8],
                           (unsigned long long)bytes[(off + 0x28) / 8],
                           (unsigned long long)bytes[(off + 0x30) / 8],
                           (unsigned long long)bytes[(off + 0x38) / 8]);
            }
        }

        // Walk the given runtime class's property table looking for
        // `wheelRuntimeData`. Returns the valueOffset or 0 if absent.
        // The class hierarchy is walked upward (parent chain) so a
        // field declared on a base class resolves through a subclass.
        uint32_t ResolveWheelDataOffset(RED4ext::CClass* cls)
        {
            RED4ext::CName key("wheelRuntimeData");
            while (cls)
            {
                if (auto* p = cls->GetProperty(key))
                    return p->valueOffset;
                cls = cls->parent;
            }
            return 0;
        }

        // Test a candidate PS pointer for wheelRuntimeData. If the
        // pointer's runtime type (or its parent chain) exposes the
        // wheelRuntimeData property, log + return its offset. Zero if
        // this pointer is the wrong type.
        uint32_t TryResolveWheelDataOnPointer(RED4ext::ISerializable* obj,
                                              const char* sourceLabel)
        {
            if (!obj) return 0;
            auto* type = obj->GetType();
            if (!type) return 0;
            const char* typeName = type->GetName().ToString();
            const uint32_t off = ResolveWheelDataOffset(type);
            log::InfoF("[gwheel:surface] probe via %s: ptr=%p type=%s wheelRuntimeData@=0x%X",
                       sourceLabel, obj, typeName ? typeName : "?", off);
            return off;
        }

        // Probe the vehicle struct's `persistentState` handle (offset
        // 0x168 on vehicleBaseObject, type `handle:gamePersistentState`)
        // for wheelRuntimeData. This is the Path A / back-pointer
        // alternative — GetVehiclePS() on this build returns a sibling
        // `VehicleComponentPS` class that doesn't carry wheel data.
        // Returns the wheelRuntimeData offset if the PS at 0x168 is
        // actually a subclass of vehiclePersistentDataPS; otherwise 0.
        RED4ext::ISerializable* ReadPersistentStateHandle(void* self)
        {
            if (!self) return nullptr;
            auto* bytes = reinterpret_cast<const char*>(self);
            // Handle<T> is {T* instance, RefCntPtr<...> rc}; the first
            // 8 bytes are the raw instance pointer.
            return *reinterpret_cast<RED4ext::ISerializable* const*>(
                bytes + kVehicleBaseObject_persistentState);
        }

        // Where the currently-resolved wheel-data pointer comes from.
        // 0 = unresolved / failed. 1 = vehicle+0x168 persistentState
        // pointer. 2 = handle returned by GetVehiclePS(). Written once,
        // read hot-path.
        std::atomic<int> g_psSource{0};

        // Read the 4 per-wheel material CName hashes for this vehicle.
        // Returns true on success; `out` indices 0-3 hold the hashes
        // (0 = no material / read failed for that wheel).
        bool ReadWheelMaterials(void* self, uint64_t out[4])
        {
            out[0] = out[1] = out[2] = out[3] = 0;
            if (g_surfaceLookupFailed.load(std::memory_order_acquire)) return false;

            // One-shot probe across both paths. Prefer the one that
            // actually finds wheelRuntimeData on its object's class chain.
            uint32_t off = g_psWheelDataOffset.load(std::memory_order_acquire);
            int source   = g_psSource.load(std::memory_order_acquire);

            if (off == 0)
            {
                // Path A: vehicle.persistentState at +0x168.
                // RTTI says `handle:gamePersistentState`. In RED4ext a
                // Handle<T> is {T* instance, RefCnt*}; for WeakHandle /
                // Ref<T> the layout differs. Dump raw bytes so we can
                // decode whichever storage variant CDPR used.
                {
                    auto* bytes = reinterpret_cast<const uint64_t*>(
                        reinterpret_cast<const char*>(self)
                        + kVehicleBaseObject_persistentState);
                    log::InfoF("[gwheel:surface] raw vehicle+0x168 words: %016llX %016llX %016llX %016llX",
                               (unsigned long long)bytes[0],
                               (unsigned long long)bytes[1],
                               (unsigned long long)bytes[2],
                               (unsigned long long)bytes[3]);
                }

                // Try each candidate pointer read of the 4 words above.
                // Whichever interpretation yields a valid RTTI-typed
                // object wins.
                for (int variant = 0; variant < 2 && off == 0; ++variant)
                {
                    const auto byteOff = kVehicleBaseObject_persistentState
                                       + variant * static_cast<std::ptrdiff_t>(sizeof(void*));
                    auto* candidate = *reinterpret_cast<RED4ext::ISerializable* const*>(
                        reinterpret_cast<const char*>(self) + byteOff);
                    if (!candidate) continue;
                    // Basic sanity: plausible heap pointer.
                    if (reinterpret_cast<uintptr_t>(candidate) < 0x10000) continue;
                    char label[32];
                    std::snprintf(label, sizeof(label), "vehicle+0x%03llX",
                                  (unsigned long long)byteOff);
                    LogPSDiagnostic(candidate);
                    const uint32_t a = TryResolveWheelDataOnPointer(candidate, label);
                    if (a > 0)
                    {
                        off    = a;
                        source = 1;
                    }
                }

                // Path B fallback: GetVehiclePS() (known to return
                // VehicleComponentPS on this build — sibling class,
                // no wheelRuntimeData).
                if (off == 0)
                {
                    if (auto* fn = GetVehiclePSFn())
                    {
                        RED4ext::Handle<RED4ext::ISerializable> psHandle;
                        RED4ext::ExecuteFunction(self, fn, &psHandle);
                        if (psHandle.instance)
                        {
                            const uint32_t b = TryResolveWheelDataOnPointer(
                                psHandle.instance, "GetVehiclePS()");
                            if (b > 0)
                            {
                                off    = b;
                                source = 2;
                            }
                        }
                    }
                }

                if (off == 0)
                {
                    g_surfaceLookupFailed.store(true, std::memory_order_release);
                    log::Warn("[gwheel:surface] wheelRuntimeData not reachable via either probe — surface reads disabled");
                    return false;
                }
                g_psWheelDataOffset.store(off, std::memory_order_release);
                g_psSource.store(source, std::memory_order_release);
                log::InfoF("[gwheel:surface] RESOLVED — source=%d offset=0x%X", source, off);
            }

            // Re-fetch the PS pointer from the chosen source each tick
            // (cheap: pointer read for path A, ExecuteFunction for B).
            RED4ext::ISerializable* ps = nullptr;
            if (source == 1)
            {
                ps = ReadPersistentStateHandle(self);
            }
            else if (source == 2)
            {
                if (auto* fn = GetVehiclePSFn())
                {
                    RED4ext::Handle<RED4ext::ISerializable> psHandle;
                    RED4ext::ExecuteFunction(self, fn, &psHandle);
                    ps = psHandle.instance;
                }
            }
            if (!ps) return false;

            auto* psBytes = reinterpret_cast<const char*>(ps);
            for (int i = 0; i < 4; ++i)
            {
                out[i] = *reinterpret_cast<const uint64_t*>(psBytes + off + i * kPS_wheelEntryStride);
            }
            return true;
        }

        // Resolve vehicleBaseObject::GetWorldRight once, returning the
        // per-tick world-space right unit vector of the vehicle. Used
        // by the slip-angle computation.
        bool ReadWorldRight(void* self, RED4ext::Vector4& out)
        {
            auto* fn = g_getWorldRightFn.load(std::memory_order_acquire);
            if (!fn)
            {
                if (g_getWorldRightLookupTried.exchange(true, std::memory_order_acq_rel))
                    return false;
                auto* rtti = RED4ext::CRTTISystem::Get();
                if (!rtti) return false;
                auto* cls = rtti->GetClass(RED4ext::CName("vehicleBaseObject"));
                if (!cls) return false;
                auto* resolved = cls->GetFunction(RED4ext::CName("GetWorldRight"));
                if (!resolved)
                {
                    log::Warn("[gwheel:hook] RTTI method 'GetWorldRight' not found — slip-angle disabled");
                    return false;
                }
                g_getWorldRightFn.store(resolved, std::memory_order_release);
                fn = resolved;
                log::InfoF("[gwheel:hook] RTTI resolved: vehicleBaseObject::GetWorldRight -> %p", resolved);
            }
            RED4ext::ExecuteFunction(self, fn, &out);
            return true;
        }

        float ReadVehicleSpeed(void* self)
        {
            auto* fn = g_getCurrentSpeedFn.load(std::memory_order_acquire);
            if (!fn)
            {
                if (g_getCurrentSpeedLookupTried.exchange(true, std::memory_order_acq_rel))
                    return 0.f; // lookup already attempted and failed
                auto* rtti = RED4ext::CRTTISystem::Get();
                if (!rtti)
                {
                    log::Warn("[gwheel:hook] RTTI unavailable — GetCurrentSpeed lookup skipped");
                    return 0.f;
                }
                auto* cls = rtti->GetClass(RED4ext::CName("vehicleBaseObject"));
                if (!cls)
                {
                    log::Warn("[gwheel:hook] RTTI class 'vehicleBaseObject' not found");
                    return 0.f;
                }
                auto* resolved = cls->GetFunction(RED4ext::CName("GetCurrentSpeed"));
                if (!resolved)
                {
                    log::Warn("[gwheel:hook] RTTI method 'vehicleBaseObject::GetCurrentSpeed' not found — centering spring will stay disabled");
                    return 0.f;
                }
                g_getCurrentSpeedFn.store(resolved, std::memory_order_release);
                fn = resolved;
                log::InfoF("[gwheel:hook] RTTI resolved: vehicleBaseObject::GetCurrentSpeed -> %p", resolved);
            }

            float speed = 0.f;
            RED4ext::ExecuteFunction(self, fn, &speed);
            return speed;
        }

        inline float* FloatFieldAt(void* base, std::ptrdiff_t off)
        {
            return reinterpret_cast<float*>(static_cast<char*>(base) + off);
        }

        inline float Clamp(float v, float lo, float hi)
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        void DetourUpdateVehicleCameraInput(void* self)
        {
            if (g_original) g_original(self);

            const auto n = g_fireCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1)
                log::InfoF("[gwheel:hook] UpdateVehicleCameraInput fired for the first time (self=%p)", self);
            if (!self) return;

            // Gate: only write into the player's currently-mounted vehicle.
            // Redscript mount/unmount event wrappers cache the pointer via
            // GWheel_SetPlayerVehicle. If no vehicle is cached, inject
            // nothing — the redscript hook is the authoritative source.
            void* pv = g_playerVehicle.load(std::memory_order_acquire);
            if (pv == nullptr || self != pv) return;

            const auto frame = sources::Current();
            if (!frame.connected) return;
            const auto cfg = config::Current();

            // Steer-direction sign multiplier driven by the user's "Invert
            // FFB direction" toggle. When set, applies to BOTH:
            //   1. the wheel position written into the game's input field
            //      (so turning the wheel right makes the car turn left),
            //   2. the same wheel position fed to the FFB pipeline as
            //      driver.steerNormalized (so signSteer flips, and the
            //      pipeline computes restoring torque relative to the
            //      flipped game-state).
            // The line in wheel.cpp that flips constForce one more time
            // then keeps the *physical* SDK push direction correct (toward
            // center) in the inverted world. Throttle / brake are unsigned
            // (0..1) so inversion doesn't apply to them.
            const float steerSign = cfg.ffb.invertDirection ? -1.f : 1.f;
            const float steerEffective = Clamp(frame.axes.steer * steerSign, -1.0f, 1.0f);

            // Cast once to read labeled fields from the RED4ext SDK struct.
            // acceleration / deceleration are the processed drive commands
            // (what we've historically called throttle/brake in this mod).
            auto* veh = static_cast<RED4ext::vehicle::BaseObject*>(self);

            if (cfg.input.enabled)
            {
                // Compute the wheel's contribution to each axis. G HUB owns
                // the per-profile operating range, so wheel position passes
                // through unmodified.
                const float wheelSteer    = steerEffective;
                const float wheelThrottle = Clamp(frame.axes.throttle,  0.0f, 1.0f);
                const float wheelBrake    = Clamp(frame.axes.brake,     0.0f, 1.0f);

                // Merge with whatever the vanilla input pipeline (keyboard /
                // gamepad) already wrote into the struct. g_original(self)
                // above has already processed WASD / analog stick input into
                // these fields; we take the max-magnitude so wheel and
                // keyboard coexist — whichever source asks for more steer /
                // throttle / brake wins, neither clobbers the other.
                float* pSteer    = FloatFieldAt(self, off::kInputSteer);
                float* pThrottle = &veh->acceleration;
                float* pBrake    = &veh->deceleration;

                if (std::fabs(wheelSteer) > std::fabs(*pSteer))
                    *pSteer = wheelSteer;
                if (wheelThrottle > *pThrottle)
                    *pThrottle = wheelThrottle;
                if (wheelBrake > *pBrake)
                    *pBrake = wheelBrake;

                const float steer    = *pSteer;
                const float throttle = *pThrottle;
                const float brake    = *pBrake;

                const auto m = g_injectCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (m == 1)
                    log::InfoF("[gwheel:hook] first injection: steer=%.3f throttle=%.3f brake=%.3f",
                               steer, throttle, brake);
                else if (m == 5000 || m == 50000 || m == 500000)
                    log::InfoF("[gwheel:hook] inject count = %llu (steer=%.3f throttle=%.3f brake=%.3f)",
                               static_cast<unsigned long long>(m), steer, throttle, brake);
            }

            // Physics-model FFB. Runs independently of cfg.input.enabled so
            // FFB still behaves correctly when the user disables input
            // injection for testing. Gated on (a) the master cfg.ffb.enabled
            // toggle and (b) SteerAxis being bound to a wheel-axis source —
            // there's no point firing motor effects when the user is driving
            // with keyboard / gamepad. The else-branch performs the edge
            // teardown so an unbinding mid-drive doesn't latch effects.
            const bool steerWheelBound = axis_resolver::IsActionWheelBound("SteerAxis");
            if (cfg.ffb.enabled && steerWheelBound)
            {
                // Angular velocity magnitude from the vehicle's PhysicsData,
                // read via the RTTI-resolved offsets. Dominant component is
                // yaw (Z in REDengine), but we take the full vector
                // magnitude so we don't need an axis-convention assumption.
                float angVelMag = 0.f;
                float angVel[3] = {};
                if (vehicle_ext::ReadAngularVelocity(self, angVel))
                {
                    angVelMag = std::sqrt(angVel[0] * angVel[0]
                                        + angVel[1] * angVel[1]
                                        + angVel[2] * angVel[2]);
                }

                // Suspension activity proxy. Per-tick change in roll+pitch
                // angular velocity. Yaw is excluded — that comes from
                // steering input, not suspension. Units are rad per tick
                // (not per second); the downstream road-surface envelope
                // is tuned against this convention. First tick after a
                // vehicle swap will briefly read a big delta (prev=0);
                // the envelope attack is fast enough that the spurious
                // peak dies within a few frames.
                const float prevX = g_prevAngVelX.load(std::memory_order_acquire);
                const float prevY = g_prevAngVelY.load(std::memory_order_acquire);
                const float dX = std::fabs(angVel[0] - prevX);
                const float dY = std::fabs(angVel[1] - prevY);
                g_prevAngVelX.store(angVel[0], std::memory_order_release);
                g_prevAngVelY.store(angVel[1], std::memory_order_release);

                // Vertical linear velocity derivative. Reads world-Z
                // from PhysicsData (same source as the existing velocity
                // read — cheap, no extra RTTI call). Δvz captures the
                // up/down bounce that dirt/gravel produces even when
                // chassis rotation is small. Coefficient scales m/s
                // into roughly the same range as rad/s for blending.
                float linVel[3] = {};
                float dVz = 0.f;
                bool  haveLinVel = vehicle_ext::ReadVelocity(self, linVel);
                if (haveLinVel)
                {
                    const float prevZ = g_prevLinVelZ.load(std::memory_order_acquire);
                    dVz = std::fabs(linVel[2] - prevZ);
                    g_prevLinVelZ.store(linVel[2], std::memory_order_release);
                }
                const float suspensionActivity = dX + dY + 0.8f * dVz;

                // Slip-angle proxy. Dot linear velocity with the car's
                // world-space right vector to get lateral velocity in
                // car-local frame. Positive = sliding rightward,
                // negative = sliding leftward. During a drift this
                // component is non-zero while the gripFactor approaches
                // 0; the FFB layer uses both together to add a
                // countersteer nudge that lightens toward the direction
                // of travel (matches what a real SAT does past peak slip).
                float lateralVelocityMps = 0.f;
                {
                    RED4ext::Vector4 right{};
                    if (haveLinVel && ReadWorldRight(self, right))
                    {
                        lateralVelocityMps = linVel[0] * right.X
                                           + linVel[1] * right.Y
                                           + linVel[2] * right.Z;
                    }
                }

                // Per-wheel road surface material is NOT read from the
                // detour: the only live source in CP2077 is physics-
                // raycast `TraceResult.material`, which we call from
                // redscript via SpatialQueriesSystem (see
                // gwheel_reds/gwheel_surface.reds). The redscript poller
                // forwards each wheel's material CName through the
                // GWheel_OnWheelMaterial native. The prior attempts via
                // vehiclePersistentDataPS.wheelRuntimeData turned out to
                // be save-state infrastructure (written at save/unload
                // boundaries, not per-frame).

                // Per-car physics snapshot drives the FFB pipeline directly
                // through ApplyFfbFromPhysics. The legacy wheelbase-derived
                // cruiseMps / centeringBaseline / yawRef heuristics moved
                // inside that function, sourced from authored TweakDB values
                // instead of hand-fit constants.

                // Run the offset probe once we have a live player vehicle.
                // Self-discovering: scans for back-pointer correlations to
                // find where physicsData / physics live in this build.
                if (!rtti_offsets::StructProbeDone())
                    rtti_offsets::ProbeStructOffsets(self);

                // Inner WheeledPhysics field-probe is defined but no longer
                // called per-mount — we've triangulated the 2.31 offsets
                // and hardcoded them in vehicle_physics_ext.h fallbacks.
                // Re-enable this block if a future patch breaks the reads.
                // (call site: rtti_offsets::ProbeInnerWheeledFields(wp))

                // First-tick per-vehicle dump: once per unique `self`, print
                // the physics values we pulled so users / devs can see what
                // the game reports for each ride and calibrate yaw scaling.
                void* lastLogged = g_lastPhysicsLoggedVehicle.load(std::memory_order_acquire);
                if (lastLogged != self)
                {
                    if (g_lastPhysicsLoggedVehicle.compare_exchange_strong(
                            lastLogged, self, std::memory_order_acq_rel))
                    {
                        // Full PerCarPhysics snapshot — composes TweakDB
                        // authored values + live-state probe. ResolvePerVehicle
                        // is called inside ReadPerCarPhysics, so chain
                        // resolution + log emission still fires once per
                        // unique vehicle. Step 5 will replace the existing
                        // per-tick FFB derivation block with this snapshot.
                        const auto pc = tweakdb_physics::ReadPerCarPhysics(self);

                        const char* kindStr =
                            (pc.kind == tweakdb_physics::ChassisKind::Car)  ? "Car"  :
                            (pc.kind == tweakdb_physics::ChassisKind::Bike) ? "Bike" : "Unknown";

                        log::InfoF("[gwheel:ffb] per-car snapshot vehicle=%p kind=%s wheels=%u chainResolved=%d liveState=%d",
                                   self, kindStr, pc.wheelCount, pc.chainResolved ? 1 : 0, pc.liveStateValid ? 1 : 0);
                        log::InfoF("[gwheel:ffb]   live: vel=(%.2f, %.2f, %.2f) angVel=(%.2f, %.2f, %.2f) speed=%.2fm/s yawMag=%.3frad/s",
                                   pc.vel[0], pc.vel[1], pc.vel[2],
                                   pc.angVel[0], pc.angVel[1], pc.angVel[2],
                                   pc.speedMps, pc.yawMagRadPerSec);
                        log::InfoF("[gwheel:ffb]   slipAngle: curveScale=%.3f minSpeed=%.2f | slipRatio: curveScale=%.3f minSpeed=%.2f",
                                   pc.slipAngleCurveScale, pc.slipAngleMinSpeedThreshold,
                                   pc.slipRatioCurveScale, pc.slipRatioMinSpeedThreshold);
                        log::InfoF("[gwheel:ffb]   weightTransfer: fwd=%.3f side=%.3f | airDrag=%.3f brakeFrict=%.3f handbrakeTq=%.0f",
                                   pc.forwardWeightTransferFactor, pc.sideWeightTransferFactor,
                                   pc.airResistanceFactor, pc.brakingFrictionFactor, pc.handbrakeBrakingTorque);
                        log::InfoF("[gwheel:ffb]   slope: begin=%.2f max=%.2f factor=%.2f | turningRoll=%.3f",
                                   pc.slopeTractionReductionBegin, pc.slopeTractionReductionMax,
                                   pc.slopeTractionReductionFactor, pc.turningRollFactor);
                        log::InfoF("[gwheel:ffb]   turnUpdate: thresh=%.1f/%.1f/%.1f mul=%.3f/%.3f changeMul=%.3f/%.3f",
                                   pc.turnUpdateBaseSpeedThreshold, pc.turnUpdateMidSpeedThreshold, pc.turnUpdateMaxSpeedThreshold,
                                   pc.turnUpdateMidSpeedTurnMul, pc.turnUpdateMaxSpeedTurnMul,
                                   pc.turnUpdateMidSpeedTurnChangeMul, pc.turnUpdateMaxSpeedTurnChangeMul);
                        log::InfoF("[gwheel:ffb]   wheel: maxTurnDeg=%.1f addPerSec=%.1f subPerSec=%.1f perfectSteer=%.3f diffOvershoot=%.3f",
                                   pc.maxWheelTurnDeg, pc.wheelTurnMaxAddPerSecond, pc.wheelTurnMaxSubPerSecond,
                                   pc.perfectSteeringFactor, pc.differentialOvershootFactor);
                        log::InfoF("[gwheel:ffb]   chassis: bankFB=%.3f bankLR=%.3f bodyFrict=%.3f totalMass=%.0f chassisMass=%.0f",
                                   pc.bankBodyFBTanMultiplier, pc.bankBodyLRTanMultiplier,
                                   pc.bodyFriction, pc.totalMass, pc.chassisMass);
                        log::InfoF("[gwheel:ffb]   tires: front r=%.3f w=%.3f | back r=%.3f w=%.3f",
                                   pc.frontTireRadius, pc.frontTireWidth,
                                   pc.backTireRadius, pc.backTireWidth);
                        log::InfoF("[gwheel:ffb]   friction (default surface): lat=%.3f long=%.3f",
                                   pc.frictionLatMultiplier, pc.frictionLongMultiplier);
                        if (pc.kind == tweakdb_physics::ChassisKind::Bike)
                        {
                            log::InfoF("[gwheel:ffb]   bike: maxTilt=%.3f tiltSpeed=%.3f tiltCustom=%.3f tiltReturn=%.3f",
                                       pc.bikeMaxTilt, pc.bikeTiltSpeed,
                                       pc.bikeTiltCustomSpeed, pc.bikeTiltReturnSpeed);
                        }
                        for (uint32_t i = 0; i < pc.wheelCount; ++i)
                        {
                            log::InfoF("[gwheel:ffb]   wheel[%u]: drive=%d brake=%d handbrake=%d", i,
                                       pc.wheels[i].isDrive ? 1 : 0,
                                       pc.wheels[i].isMainBrake ? 1 : 0,
                                       pc.wheels[i].isHandBrake ? 1 : 0);
                        }

                        // Stash the resolved snapshot so the hot path below
                        // doesn't have to call ReadPerCarPhysics every tick.
                        // See comment on g_perCarSnapshot for why per-tick
                        // chain resolution crashes the game.
                        {
                            std::lock_guard<std::mutex> lk(g_perCarMu);
                            g_perCarSelf     = self;
                            g_perCarSnapshot = pc;
                        }
                    }
                }

                // Raw wheel position for active-torque direction. We use
                // the DirectInput-normalised wheelSteer (physical wheel
                // position in its operating range), not the post-merge
                // game input field — which can be tiny if G HUB has a
                // wide operating range (e.g. 30° rotation out of 900°
                // reports ~0.07, which × activeTorque rides below the
                // wheel motor's friction threshold and produces no felt
                // push-back). frame.axes.steer is always normalised -1..+1
                // against the *current* operating range, so a moderate
                // hand position always reads as a moderate value.
                // Uses steerEffective so the user's invert toggle flips
                // signSteer in lockstep with the inverted game input.
                const float torqueSteer = steerEffective;

                // Lift the authored-fields snapshot from the per-mount cache
                // populated by the snapshot-dump block above. Then overwrite
                // the live-state fields from the values we already computed
                // earlier in this tick (linVel / angVel / lateralVelocityMps).
                // No per-tick TweakDB chain walk; no per-tick ExecuteFunction
                // call. See the g_perCarSnapshot declaration for why.
                tweakdb_physics::PerCarPhysics pc{};
                {
                    std::lock_guard<std::mutex> lk(g_perCarMu);
                    if (g_perCarSelf == self)
                        pc = g_perCarSnapshot;
                }
                if (haveLinVel)
                {
                    pc.vel[0] = linVel[0]; pc.vel[1] = linVel[1]; pc.vel[2] = linVel[2];
                    pc.speedMps         = std::sqrt(linVel[0] * linVel[0]
                                                  + linVel[1] * linVel[1]
                                                  + linVel[2] * linVel[2]);
                    pc.verticalSpeedMps = linVel[2];
                    pc.liveStateValid   = true;
                }
                pc.angVel[0] = angVel[0]; pc.angVel[1] = angVel[1]; pc.angVel[2] = angVel[2];
                pc.yawMagRadPerSec = std::abs(angVel[2]);

                // Per-tick read of bike/car-derived live state via hardcoded
                // offsets. ValidateInDomain inside ReadBikeLiveFields / Car
                // catches any drift; in that case `valid` returns false and
                // the FFB pipeline falls back to the lateralVelocity-based
                // proxies. Triangulated 2026-05-02; see fallback_bike /
                // fallback_car namespaces in vehicle_physics_ext.h.
                if (pc.kind == tweakdb_physics::ChassisKind::Bike)
                {
                    auto live = vehicle_ext::ReadBikeLiveFields(self);
                    if (live.valid)
                    {
                        pc.bikeCurrentTilt      = live.currentTilt;
                        pc.bikeTransversalForce = live.transversalForce;
                        pc.bikeLiveValid        = true;
                    }
                }
                else if (pc.kind == tweakdb_physics::ChassisKind::Car)
                {
                    auto live = vehicle_ext::ReadCarLiveFields(self);
                    if (live.valid)
                    {
                        pc.carBankBodyLR = live.bankBodyLR;
                        pc.carLiveValid  = true;
                    }
                }

                // Event-triggered inner-physics probe per chassis kind. Bike
                // and car physics classes are NOT RTTI-visible, so live-state
                // fields (bikeCurrentTilt, bikeTransversalForce, carBankBodyLR,
                // carCurrTurnInput, carBankBodyFB) need offset triangulation
                // out-of-band. Each known-state regime (idle, left-corner,
                // right-corner, hard-brake, hard-accel) latches its own
                // single-shot full-float dump on the first tick its predicate
                // fires; offline diffing across regimes pinpoints the live
                // offsets directly.
                if (pc.chainResolved)
                {
                    if (void* phys = vehicle_ext::GetPhysicsRaw(self))
                    {
                        rtti_offsets::InnerProbeState ps{};
                        ps.isOnGround = veh->isOnGround;
                        ps.speedMps   = pc.speedMps;
                        ps.yawSigned  = angVel[2];
                        ps.steerNorm  = torqueSteer;
                        ps.throttle   = veh->acceleration;
                        ps.brake      = veh->deceleration;

                        if (pc.kind == tweakdb_physics::ChassisKind::Bike)
                            rtti_offsets::TriggerBikeProbeSlot(phys, ps);
                        else if (pc.kind == tweakdb_physics::ChassisKind::Car)
                            rtti_offsets::TriggerCarProbeSlot(phys, ps);
                    }
                }

                wheel::FfbDriverState driver{};
                driver.steerNormalized    = torqueSteer;
                driver.throttle           = veh->acceleration;
                driver.brake              = veh->deceleration;
                driver.isReversing        = veh->isReversing;
                driver.isOnGround         = veh->isOnGround;
                driver.suspensionActivity = suspensionActivity;
                driver.lateralVelocityMps = lateralVelocityMps;

                wheel::FfbUserScalars scalars{};
                scalars.ffbEnabled         = true;
                scalars.ffbInvertDirection = cfg.ffb.invertDirection;
                scalars.ffbOutputSignFlip  = cfg.ffb.outputSignFlip;
                scalars.ffbUseSpring       = cfg.ffb.useSpring;
                scalars.torquePct          = cfg.ffb.torquePct;
                scalars.yawFeedbackPct     = cfg.ffb.yawFeedbackPct;
                scalars.activeTorquePct    = cfg.ffb.activeTorqueStrengthPct;
                scalars.stationaryMps      = cfg.ffb.stationaryThresholdMps;
                scalars.debugLogging       = cfg.ffb.debugLogging;

                wheel::ApplyFfbFromPhysics(pc, driver, scalars);
            }
            else
            {
                // FFB master toggle off (or SteerAxis not wheel-bound).
                // Call with ffbEnabled=false to perform the edge teardown
                // (spring, active, damper, road surface, airborne).
                tweakdb_physics::PerCarPhysics emptyCar{};
                wheel::FfbDriverState        emptyDriver{};
                wheel::FfbUserScalars        emptyScalars{};
                emptyScalars.ffbEnabled    = false;
                emptyScalars.debugLogging  = cfg.ffb.debugLogging;
                wheel::ApplyFfbFromPhysics(emptyCar, emptyDriver, emptyScalars);
            }
        }
    }

    bool Init()
    {
        auto& ctx = Ctx();
        if (!ctx.sdk || !ctx.sdk->hooking || !ctx.sdk->hooking->Attach)
        {
            log::Error("[gwheel:hook] RED4ext hooking API unavailable");
            return false;
        }

        // Resolving the hash terminates the game with a RED4ext MessageBox
        // if the address db doesn't know it for the current build. That's
        // the ecosystem convention - users get a clear "which mod is
        // broken" dialog rather than a silent crash later.
        const auto addr = RED4ext::UniversalRelocBase::Resolve(kUpdateVehicleCameraInputHash);
        g_target = reinterpret_cast<void*>(addr);
        log::InfoF("[gwheel:hook] UpdateVehicleCameraInput resolved: hash=%u addr=%p",
                   kUpdateVehicleCameraInputHash, g_target);

        const bool ok = ctx.sdk->hooking->Attach(
            ctx.handle,
            g_target,
            reinterpret_cast<void*>(&DetourUpdateVehicleCameraInput),
            reinterpret_cast<void**>(&g_original));

        if (!ok)
        {
            log::Error("[gwheel:hook] Attach returned false for UpdateVehicleCameraInput");
            g_target = nullptr;
            return false;
        }

        g_attached.store(true, std::memory_order_release);
        log::Info("[gwheel:hook] UpdateVehicleCameraInput detour installed "
                  "(player vehicle input override: steer/throttle/brake)");
        return true;
    }

    void Shutdown()
    {
        auto& ctx = Ctx();
        if (!g_attached.exchange(false)) return;
        if (ctx.sdk && ctx.sdk->hooking && ctx.sdk->hooking->Detach && g_target)
        {
            ctx.sdk->hooking->Detach(ctx.handle, g_target);
            log::Info("[gwheel:hook] UpdateVehicleCameraInput detour detached");
        }
        g_target = nullptr;
        g_original = nullptr;
    }

    bool IsInstalled() { return g_attached.load(std::memory_order_acquire); }

    uint64_t FireCount() { return g_fireCount.load(std::memory_order_relaxed); }

    void SetPlayerVehicle(void* p)
    {
        void* prev = g_playerVehicle.exchange(p, std::memory_order_acq_rel);
        if (prev != p)
        {
            log::InfoF("[gwheel:hook] player vehicle changed: %p -> %p", prev, p);
            // Reset the per-vehicle physics-logging latch so the next
            // vehicle's turnRate / wheelbase / etc. get dumped on first
            // tick. Also clear the per-car yaw reference and angular-
            // velocity history so stale values from the previous ride
            // don't bleed across the transition.
            g_lastPhysicsLoggedVehicle.store(nullptr, std::memory_order_release);
            g_perCarYawRef.store(0.f, std::memory_order_release);
            g_prevAngVelX.store(0.f, std::memory_order_release);
            g_prevAngVelY.store(0.f, std::memory_order_release);
            g_prevLinVelZ.store(0.f, std::memory_order_release);
            for (int i = 0; i < 4; ++i)
                g_lastWheelMat[i].store(0, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(g_perCarMu);
                g_perCarSelf     = nullptr;
                g_perCarSnapshot = tweakdb_physics::PerCarPhysics{};
            }
        }
        // In-vehicle context flag tracks presence/absence of a mounted
        // vehicle pointer. input_bindings uses it to suppress vehicle-
        // centric dispatches on-foot.
        sources::SetInVehicle(p != nullptr);
    }

    bool IsPlayerVehicle(void* p)
    {
        return p != nullptr && g_playerVehicle.load(std::memory_order_acquire) == p;
    }

    void* GetPlayerVehicle()
    {
        return g_playerVehicle.load(std::memory_order_acquire);
    }

    void OnPersistentStateSample(void* psPointer)
    {
        if (!psPointer) return;

        auto* ser = reinterpret_cast<RED4ext::ISerializable*>(psPointer);
        LogPSDiagnostic(ser);

        // Read the 4 per-wheel CName hashes at offset 0x78, entry
        // stride 0x18 (matches vehiclePersistentDataPS layout confirmed
        // by RTTI on this build). Log transitions only so idle cars
        // stay silent.
        const auto* bytes = reinterpret_cast<const char*>(psPointer);
        for (int i = 0; i < 4; ++i)
        {
            const uint64_t hash = *reinterpret_cast<const uint64_t*>(
                bytes + 0x78 + i * kPS_wheelEntryStride);
            const uint64_t prev = g_lastWheelMat[i].load(std::memory_order_acquire);
            if (hash != prev)
            {
                g_lastWheelMat[i].store(hash, std::memory_order_release);
                RED4ext::CName cname; cname.hash = hash;
                const char* name = RED4ext::CNamePool::Get(cname);
                log::InfoF("[gwheel:surface-ps] wheel[%d] material: %s (hash=0x%016llX)",
                           i,
                           name ? name : "(null)",
                           static_cast<unsigned long long>(hash));
            }
        }
    }
}
