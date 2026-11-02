#include "vehicle_hook.h"
#include "plugin.h"
#include "logging.h"
#include "sources.h"
#include "config.h"
#include "wheel.h"
#include "vehicle_physics_ext.h"
#include "rtti_offsets.h"
#include "debug_steer.h"

std::atomic<float> g_debugWheelSteer{0.0f};
std::atomic<float> g_debugRawSteer{0.0f};

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

namespace direct_wheel::vehicle_hook
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
            // 2.5 (ChoomLink), measured from a keyboard test Nov 1 21:08: W sets 0x264=1, 0x270=+1, 0x280=+1;
            // S sets 0x268=1, 0x270=-1, 0x280=-1. The game's input reader (2.5 exe 0x1401bc1f3) writes 0x264 accel,
            // 0x268 decel, 0x26C |handbrake|, 0x270 accel-decel, 0x278 steer; it never writes 0x2B4.
            constexpr std::ptrdiff_t kInputThrottle = 0x264;
            constexpr std::ptrdiff_t kInputBrake    = 0x268;
            constexpr std::ptrdiff_t kInputDrive    = 0x270; // throttle - brake
            // 0x280: W=+1, S=-1 from the keyboard (21:08). The burnout's output scales by it (disassembly of exe
            // 0x1406ddc6c, static, not run); the wheel never wrote it, so the keyboard/G HUB value stayed.
            constexpr std::ptrdiff_t kInputDrive2   = 0x280;
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
        // DirectWheel_Set/ClearPlayerVehicle natives. nullptr = no injection.
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
                log::Warn("[direct_wheel:hook] RTTI unavailable — GetVehiclePS lookup skipped");
                return nullptr;
            }
            auto* cls = rtti->GetClass(RED4ext::CName("vehicleBaseObject"));
            if (!cls)
            {
                log::Warn("[direct_wheel:hook] RTTI class 'vehicleBaseObject' not found (GetVehiclePS)");
                return nullptr;
            }
            auto* resolved = cls->GetFunction(RED4ext::CName("GetVehiclePS"));
            if (!resolved)
            {
                log::Warn("[direct_wheel:hook] RTTI method 'vehicleBaseObject::GetVehiclePS' not found — wheel-surface detection disabled");
                return nullptr;
            }
            g_getVehiclePSFn.store(resolved, std::memory_order_release);
            log::InfoF("[direct_wheel:hook] RTTI resolved: vehicleBaseObject::GetVehiclePS -> %p", resolved);
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
            log::InfoF("[direct_wheel:surface] PS handle type=%s at %p", typeName ? typeName : "?", ps);

            // Dump the first 0x200 bytes as 64-bit words. Rows that look
            // like small hash-distribution numbers (not pointer-range
            // 0x00007FF...) are CName-candidate offsets.
            const auto* bytes = reinterpret_cast<const uint64_t*>(ps);
            for (int off = 0; off < 0x200; off += 0x40)
            {
                log::InfoF("[direct_wheel:surface]   PS+0x%03X: %016llX %016llX %016llX %016llX %016llX %016llX %016llX %016llX",
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
            log::InfoF("[direct_wheel:surface] probe via %s: ptr=%p type=%s wheelRuntimeData@=0x%X",
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
                    log::InfoF("[direct_wheel:surface] raw vehicle+0x168 words: %016llX %016llX %016llX %016llX",
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
                    log::Warn("[direct_wheel:surface] wheelRuntimeData not reachable via either probe — surface reads disabled");
                    return false;
                }
                g_psWheelDataOffset.store(off, std::memory_order_release);
                g_psSource.store(source, std::memory_order_release);
                log::InfoF("[direct_wheel:surface] RESOLVED — source=%d offset=0x%X", source, off);
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
                    log::Warn("[direct_wheel:hook] RTTI method 'GetWorldRight' not found — slip-angle disabled");
                    return false;
                }
                g_getWorldRightFn.store(resolved, std::memory_order_release);
                fn = resolved;
                log::InfoF("[direct_wheel:hook] RTTI resolved: vehicleBaseObject::GetWorldRight -> %p", resolved);
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
                    log::Warn("[direct_wheel:hook] RTTI unavailable — GetCurrentSpeed lookup skipped");
                    return 0.f;
                }
                auto* cls = rtti->GetClass(RED4ext::CName("vehicleBaseObject"));
                if (!cls)
                {
                    log::Warn("[direct_wheel:hook] RTTI class 'vehicleBaseObject' not found");
                    return 0.f;
                }
                auto* resolved = cls->GetFunction(RED4ext::CName("GetCurrentSpeed"));
                if (!resolved)
                {
                    log::Warn("[direct_wheel:hook] RTTI method 'vehicleBaseObject::GetCurrentSpeed' not found — centering spring will stay disabled");
                    return 0.f;
                }
                g_getCurrentSpeedFn.store(resolved, std::memory_order_release);
                fn = resolved;
                log::InfoF("[direct_wheel:hook] RTTI resolved: vehicleBaseObject::GetCurrentSpeed -> %p", resolved);
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
                log::InfoF("[direct_wheel:hook] UpdateVehicleCameraInput fired for the first time (self=%p)", self);
            if (!self) return;

            // Gate: only write into the player's currently-mounted vehicle.
            // Redscript mount/unmount event wrappers cache the pointer via
            // DirectWheel_SetPlayerVehicle. If no vehicle is cached, inject
            // nothing — the redscript hook is the authoritative source.
            void* pv = g_playerVehicle.load(std::memory_order_acquire);
            if (pv == nullptr || self != pv) return;

            const auto frame = sources::Current();
            if (!frame.connected) return;
            const auto cfg = config::Current();

            // Cast once to read labeled fields from the RED4ext SDK struct.
            // acceleration / deceleration are the processed drive commands
            // (what we've historically called throttle/brake in this mod).
            auto* veh = static_cast<RED4ext::vehicle::BaseObject*>(self);

            // Read speed early — needed for both speed-steering-boost and FFB.
            const float vehicleSpeed = std::fabs(ReadVehicleSpeed(self));

            if (cfg.input.enabled)
            {
                // Compute the wheel's contribution to each axis. G HUB owns
                // the per-profile operating range, so wheel position passes
                // through unmodified.
                float wheelSteer    = Clamp(frame.axes.steer,    -1.0f, 1.0f);
                const float wheelThrottle = Clamp(frame.axes.throttle,  0.0f, 1.0f);
                const float wheelBrake    = Clamp(frame.axes.brake,     0.0f, 1.0f);

                // Speed Steering Boost: the game internally reduces steering
                // effectiveness at high speed. We apply a multiplier to the
                // equalizer curve so that at higher speeds, the same physical
                // wheel rotation produces a stronger initial steering response.
                // Formula: multiplier = 1 + speedRatio * (pct/100)
                // At pct=100, 100 mph (44.704 m/s): 2.0x boost (max).
                // At pct=50,  100 mph: 1.5x boost.
                float boost = 1.0f;
                if (cfg.input.speedSensitiveSteeringPct > 0 && vehicleSpeed > 0.5f) {
                    const float refSpeedMps = 44.704f; // 100 mph
                    const float speedRatio = vehicleSpeed / refSpeedMps;
                    boost = 1.0f + speedRatio * (cfg.input.speedSensitiveSteeringPct / 100.0f);
                }

                // Steering Equalizer (sqrt curve)
                //
                // The game applies a quadratic steering curve internally:
                //   actual_steer ≈ input² / max_steer
                // This makes small wheel angles feel dead (at 10° only 3.6%
                // of the linear response reaches the wheels). The inverse of
                // x² is √x, so we apply sqrt to perfectly linearise the
                // response across the full range:
                //   sqrt(0.0278) = 0.167 → game squares it → 0.0278 ✓
                //   sqrt(0.25)   = 0.50  → game squares it → 0.25   ✓
                //   sqrt(0.50)   = 0.707 → game squares it → 0.50   ✓
                //
                // Speed boost is applied as a post-sqrt multiplier so that
                // at high speed the linearised signal is amplified further
                // to compensate for the game's speed-dependent attenuation.
                float absSteer = std::fabs(wheelSteer);
                float mappedSteer = Clamp(std::sqrt(absSteer) * boost, 0.0f, 1.0f);
                
                wheelSteer = (wheelSteer >= 0.f) ? mappedSteer : -mappedSteer;

                g_debugRawSteer.store(frame.axes.steer, std::memory_order_relaxed);
                g_debugWheelSteer.store(wheelSteer, std::memory_order_relaxed);

                // Merge with whatever the vanilla input pipeline (keyboard /
                // gamepad) already wrote into the struct. g_original(self)
                // above has already processed WASD / analog stick input into
                // these fields; we take the max-magnitude so wheel and
                // keyboard coexist — whichever source asks for more steer /
                // throttle / brake wins, neither clobbers the other.
                float* pSteer    = FloatFieldAt(self, off::kInputSteer);
                float* pThrottle = FloatFieldAt(self, off::kInputThrottle);
                float* pBrake    = FloatFieldAt(self, off::kInputBrake);
                { // ChoomLink: the game's own input fields once a second (before ours), to prove the 2.5 offsets
                    static ULONGLONG sLast = 0;
                    static int sLines = 0;
                    const ULONGLONG now = GetTickCount64();
                    if (now - sLast >= 1000 && sLines < 600) { // ~10 min per game, not forever
                        sLast = now; ++sLines;
                        const float* f = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(self) + 0x258);
                        char buf[600]; int n = 0;
                        for (int i = 0; i < 28 && n < 480; ++i)
                            n += snprintf(buf + n, sizeof(buf) - n, " %X=%.2f", 0x258 + i * 4, f[i]);
                        log::InfoF("[direct_wheel:input] vanilla%s", buf);
                    }
                }

                if (std::fabs(wheelSteer) > std::fabs(*pSteer))
                    *pSteer = wheelSteer;
                // ChoomLink: the wheel owns gas/brake while connected, so keyboard/gamepad input can't add to them.
                // Added for a tester's burnout with G HUB on while these pedals read 0 (Nov 1 20:46); it did not stop it.
                *pThrottle = wheelThrottle;
                *pBrake = wheelBrake;
                *FloatFieldAt(self, off::kInputDrive) = wheelThrottle - wheelBrake;
                *FloatFieldAt(self, off::kInputDrive2) = wheelThrottle - wheelBrake;

                const float steer    = *pSteer;
                const float throttle = *pThrottle;
                const float brake    = *pBrake;

                const auto m = g_injectCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (m == 1)
                    log::InfoF("[direct_wheel:hook] first injection: steer=%.3f throttle=%.3f brake=%.3f",
                               steer, throttle, brake);
                else if (m == 5000 || m == 50000 || m == 500000)
                    log::InfoF("[direct_wheel:hook] inject count = %llu (steer=%.3f throttle=%.3f brake=%.3f)",
                               static_cast<unsigned long long>(m), steer, throttle, brake);
            }

            // Physics-model FFB. Runs independently of cfg.input.enabled so
            // FFB still behaves correctly when the user disables input
            // injection for testing. cfg.ffb.enabled is the single master
            // toggle; there is no separate "centering only" gate.
            if (cfg.ffb.enabled)
            {
                const float speed = vehicleSpeed;

                // === Collision detection via sudden speed drop ===
                // VehicleBumpEvent is broken in CP2077 2.31 — detect
                // collisions by monitoring speed deltas instead.
                {
                    static std::atomic<float> s_prevSpeed{0.f};
                    static std::atomic<uint32_t> s_cooldown{0};
                    const float prev = s_prevSpeed.load(std::memory_order_relaxed);
                    const float delta = prev - speed; // positive = deceleration
                    s_prevSpeed.store(speed, std::memory_order_relaxed);

                    uint32_t cd = s_cooldown.load(std::memory_order_relaxed);
                    if (cd > 0) {
                        s_cooldown.store(cd - 1, std::memory_order_relaxed);
                    }
                    // Threshold: 5 m/s drop in one tick (~250Hz) = hard hit
                    // Cooldown: 60 ticks (~240ms) between jolts
                    else if (delta > 5.0f && prev > 3.0f) {
                        float intensity = std::clamp(delta / 15.0f, 0.3f, 1.0f);
                        // Random-ish direction based on steer position
                        float dir = (frame.axes.steer > 0.f) ? -1.0f : 1.0f;
                        float kick = dir * intensity;
                        log::InfoF("[direct_wheel:jolt] collision detected! dv=%.1f m/s prev=%.1f kick=%.2f",
                                   delta, prev, kick);
                        wheel::TriggerJolt(kick, 300);
                        s_cooldown.store(60, std::memory_order_relaxed);
                    }
                }
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

                // === Obstacle bump jolt from suspension spikes ===
                // When suspensionActivity exceeds a threshold, it means
                // we hit a curb/pothole/object. Trigger a short jolt.
                {
                    static std::atomic<uint32_t> s_bumpCooldown{0};
                    uint32_t bcd = s_bumpCooldown.load(std::memory_order_relaxed);
                    if (bcd > 0) {
                        s_bumpCooldown.store(bcd - 1, std::memory_order_relaxed);
                    }
                    // Threshold 0.8 = significant bump; cooldown 30 ticks (~120ms)
                    else if (suspensionActivity > 0.8f && speed > 2.0f) {
                        float bumpIntensity = std::clamp(suspensionActivity / 3.0f, 0.3f, 1.0f);
                        // Direction from vertical velocity sign (positive dVz = upward bounce)
                        float bumpDir = (dVz > dX + dY) ? 1.0f : -1.0f;
                        float kick = bumpDir * bumpIntensity;
                        log::InfoF("[direct_wheel:jolt] bump! susp=%.3f dX=%.3f dY=%.3f dVz=%.3f kick=%.2f",
                                   suspensionActivity, dX, dY, dVz, kick);
                        wheel::TriggerJolt(kick, 80); // short 80ms pulse
                        s_bumpCooldown.store(30, std::memory_order_relaxed);
                    }
                }
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
                // direct_wheel_reds/direct_wheel_surface.reds). The redscript poller
                // forwards each wheel's material CName through the
                // DirectWheel_OnWheelMaterial native. The prior attempts via
                // vehiclePersistentDataPS.wheelRuntimeData turned out to
                // be save-state infrastructure (written at save/unload
                // boundaries, not per-frame).

                // Per-car physics snapshot drives yaw reference, cruise
                // speed, and centering-baseline derivation. Hardcoded
                // fallbacks kick in for vehicles whose physics we can't
                // read (tanks, air, first-tick race before the read lands).
                constexpr float kFallbackYawRef      = 1.5f;  // rad/s — generic sports car
                constexpr float kFallbackCruiseMps   = 18.f;  // ~65 km/h
                constexpr float kFallbackCenteringB  = 0.85f; // mid-weight, matches new formula
                constexpr float kWheelbaseMinM       = 0.8f;
                constexpr float kWheelbaseMaxM       = 6.0f;

                const auto wp = vehicle_ext::ReadWheeledPhysics(self);
                float perCarYawRef = 0.f;
                if (wp.valid && wp.turnRate > 0.f)
                    perCarYawRef = std::clamp(wp.turnRate, 0.3f, 10.f);
                g_perCarYawRef.store(perCarYawRef, std::memory_order_relaxed);
                const float yawRef = (perCarYawRef > 0.f) ? perCarYawRef : kFallbackYawRef;

                // Cruise speed derived from wheelbase: longer chassis cruise
                // at higher speeds. cruise = 5 + 5×wheelbase, clamped. Maps
                // a 2.5m wheelbase (sports car) to ~17.5 m/s (63 km/h), a
                // 3.5m truck to 22.5 m/s, a 1.4m bike to 12 m/s.
                //
                // Centering baseline: stiffer car = more SAT per m/s. Use
                // wheelbase as a proxy for chassis stability — heavier
                // longer cars load the wheel harder at cruise. Formula
                // tuned so a 2.5m sports car hits 0.875 at cruise (heavy
                // but not locked), a 3.5m truck hits 1.0 (locked), a 1.4m
                // bike hits 0.71 (loose but present).
                float cruiseMps        = kFallbackCruiseMps;
                float centeringBaseline = kFallbackCenteringB;
                if (wp.valid
                    && wp.frontBackWheelDistance >= kWheelbaseMinM
                    && wp.frontBackWheelDistance <= kWheelbaseMaxM)
                {
                    const float wb = wp.frontBackWheelDistance;
                    cruiseMps        = std::clamp(5.f + 5.f * wb, 10.f, 40.f);
                    centeringBaseline = std::clamp(0.5f + 0.15f * wb, 0.5f, 1.0f);
                }

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
                        log::InfoF("[direct_wheel:ffb] per-car physics for vehicle=%p:", self);
                        if (wp.valid)
                        {
                            log::InfoF("[direct_wheel:ffb]   turnRate=%.3f maxWheelTurnDeg=%.2f slipAngleCurveScale=%.3f "
                                       "numDriveWheels=%u wheelbase=%.3f",
                                       wp.turnRate, wp.maxWheelTurnDeg, wp.slipAngleCurveScale,
                                       wp.numDriveWheels, wp.frontBackWheelDistance);
                        }
                        else
                        {
                            log::Info("[direct_wheel:ffb]   WheeledPhysics unavailable (tank / air / mount race?)");
                        }
                        float vel[3] = {};
                        if (vehicle_ext::ReadVelocity(self, vel))
                        {
                            log::InfoF("[direct_wheel:ffb]   velocity=(%.2f, %.2f, %.2f) angVel=(%.2f, %.2f, %.2f)",
                                       vel[0], vel[1], vel[2],
                                       angVel[0], angVel[1], angVel[2]);
                        }
                        log::InfoF("[direct_wheel:ffb]   yawRef in use: %.3f rad/s (%s)",
                                   yawRef, perCarYawRef > 0.f ? "from turnRate" : "hardcoded fallback");
                        log::InfoF("[direct_wheel:ffb]   derived cruise=%.2f m/s, centeringBaseline=%.3f (%s)",
                                   cruiseMps, centeringBaseline,
                                   (wp.valid
                                    && wp.frontBackWheelDistance >= kWheelbaseMinM
                                    && wp.frontBackWheelDistance <= kWheelbaseMaxM)
                                       ? "from wheelbase" : "hardcoded fallback");
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
                const float torqueSteer = Clamp(frame.axes.steer, -1.0f, 1.0f);

                wheel::UpdateCenteringSpring(
                    speed,
                    angVelMag,
                    suspensionActivity,
                    lateralVelocityMps,
                    torqueSteer,
                    *FloatFieldAt(self, off::kInputThrottle),  // post-merge throttle (0..1)
                    *FloatFieldAt(self, off::kInputBrake),  // post-merge brake (0..1)
                    veh->isReversing,
                    veh->isOnGround,
                    /*enabled*/       true,
                    cfg.ffb.stationaryThresholdMps,
                    cruiseMps,
                    centeringBaseline,
                    cfg.ffb.yawFeedbackPct,
                    yawRef,
                    cfg.ffb.activeTorqueStrengthPct,
                    cfg.ffb.debugLogging);
            }
            else
            {
                // FFB master toggle off — release every effect. Calling
                // Update with enabled=false performs the edge teardown
                // (spring, active, damper, road surface, airborne).
                wheel::UpdateCenteringSpring(0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, false, true, /*enabled*/ false,
                                             0.f, 1.f, 1.f, 0, 1.f, 0, cfg.ffb.debugLogging);
            }
        }
    }

    bool Init()
    {
        auto& ctx = Ctx();
        if (!ctx.sdk || !ctx.sdk->hooking || !ctx.sdk->hooking->Attach)
        {
            log::Error("[direct_wheel:hook] RED4ext hooking API unavailable");
            return false;
        }

        // Resolving the hash terminates the game with a RED4ext MessageBox
        // if the address db doesn't know it for the current build. That's
        // the ecosystem convention - users get a clear "which mod is
        // broken" dialog rather than a silent crash later.
        const auto addr = RED4ext::UniversalRelocBase::Resolve(kUpdateVehicleCameraInputHash);
        g_target = reinterpret_cast<void*>(addr);
        log::InfoF("[direct_wheel:hook] UpdateVehicleCameraInput resolved: hash=%u addr=%p",
                   kUpdateVehicleCameraInputHash, g_target);

        const bool ok = ctx.sdk->hooking->Attach(
            ctx.handle,
            g_target,
            reinterpret_cast<void*>(&DetourUpdateVehicleCameraInput),
            reinterpret_cast<void**>(&g_original));

        if (!ok)
        {
            log::Error("[direct_wheel:hook] Attach returned false for UpdateVehicleCameraInput");
            g_target = nullptr;
            return false;
        }

        g_attached.store(true, std::memory_order_release);
        log::Info("[direct_wheel:hook] UpdateVehicleCameraInput detour installed "
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
            log::Info("[direct_wheel:hook] UpdateVehicleCameraInput detour detached");
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
            log::InfoF("[direct_wheel:hook] player vehicle changed: %p -> %p", prev, p);
            if (p) // ChoomLink (2.5 port): the pointer-sized slots where 2.31 kept physics/physicsData, to find 2.5's
            {
                for (uint32_t off = 0x2A0; off < 0x320; off += 8)
                {
                    void* v = nullptr;
                    direct_wheel::vehicle_ext::SafeCopy(&v, static_cast<char*>(p) + off, sizeof(v));
                    log::InfoF("[direct_wheel:hook] vehicle +0x%03X = %p%s", off, v,
                               direct_wheel::vehicle_ext::LooksLikePtr(v) ? " (pointer)" : "");
                }
            }
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
                log::InfoF("[direct_wheel:surface-ps] wheel[%d] material: %s (hash=0x%016llX)",
                           i,
                           name ? name : "(null)",
                           static_cast<unsigned long long>(hash));
            }
        }
    }
}
