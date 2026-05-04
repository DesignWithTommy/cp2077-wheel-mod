#include "wheel_backend.h"
#include "wheel_backend_logi_legacy.h"
#include "wheel_backend_logi_trueforce.h"
#include "trueforce_loader.h"
#include "config.h"
#include "logging.h"

namespace gwheel::wheel_backend
{
    namespace
    {
        // ---- Applicability predicates -------------------------------------
        //
        // Cheap "should we even try this backend?" checks. Run before the
        // (more expensive) create-and-init path. A predicate returns true
        // means "this backend is permitted to attempt initialization."
        //
        // Adding a new vendor: drop a sibling predicate + creator function
        // into the kCandidates table below. No factory-body churn.

        bool TrueForceApplicable()
        {
            // ----------------------------------------------------------------
            // TrueForce backend is LOCKED OFF as of 2026-05-04 pending either:
            //   1. Official Logitech SDK headers (dev support email outstanding)
            //   2. A confirmed-working sample integration we can disassemble
            //   3. Community reverse-engineering of the IPC protocol
            //
            // Forensic summary of what we proved this round:
            //   - DLL loads cleanly: trueforce_sdk_x64.dll v1.3.11 at
            //     %ProgramFiles%\Logi\Trueforce\1_3_11\.
            //   - All 75 exports resolve via GetProcAddress.
            //   - dllOpen() returns true.
            //   - logiTrueForceSupported(0) returns true (SDK detects G923).
            //   - logiTrueForceAvailable() returns false or AVs (0xC0000005).
            //   - Resume(0) returns true but IsPaused(0) stays true.
            //   - Get* (range, peak torque, angle) return "OK" with zeroed outputs.
            //   - All Set* (torque, gain, damping) return true but never reach the wheel.
            //   - Behavior is IDENTICAL inside CP2077 and in a 100-line standalone
            //     probe (tools/trueforce_probe.exe) — not process-specific.
            //   - Partner-game-running hypothesis falsified: Forza Horizon 5 and
            //     Assetto Corsa both running did NOT change disconnected state.
            //     Original AC uses DInput, not the SDK; Forza had no SDK DLL
            //     visible in its install dir.
            //   - Named pipe \\.\pipe\logi.trueforce.connect exists and is
            //     reachable from arbitrary processes (PowerShell connects fine),
            //     but the SDK-internal handshake never completes.
            //
            // Net: dllOpen runs in a permanent disconnected stub mode. Without
            // protocol knowledge or a known-working comparison process, we cannot
            // bridge the gap. The architecture below stays in place; lighting
            // up TrueForce becomes a single-file change to this predicate plus
            // signature corrections in wheel_backend_logi_trueforce.cpp once we
            // have the protocol info.
            return false;

            // Original logic, kept as a comment for the day this lights up:
            // return config::Current().trueForce.enabled
            //     && trueforce::IsActive();
        }

        bool LegacyApplicable()
        {
            // Catch-all fallback. Always permitted to attempt; if no Logi
            // wheel is attached, the backend's own Init/Open stays in retry
            // mode and we simply have no driver — same as today.
            return true;
        }

        // Future:
        //   bool ThrustmasterApplicable() {
        //       return SomeThrustmasterRuntimeProbe();
        //   }
        //   bool MozaApplicable() {
        //       return SomeMozaRuntimeProbe();
        //   }

        struct Candidate
        {
            const char* name;
            bool      (*applicable)();
            std::unique_ptr<IWheelBackend> (*create)();
        };

        // Priority order is data, not branching logic. First applicable +
        // successful Init wins. Order = highest-priority-first.
        constexpr Candidate kCandidates[] = {
            { "Logitech (TrueForce SDK)",     &TrueForceApplicable, &CreateLogitechTrueForceBackend },
            { "Logitech (legacy SDK)",        &LegacyApplicable,    &CreateLogitechLegacyBackend    },
            // Future:
            // { "Thrustmaster (TMSDK)",       &ThrustmasterApplicable, &CreateThrustmasterBackend },
            // { "Moza (Pit House SDK)",       &MozaApplicable,         &CreateMozaBackend         },
        };
    }

    std::unique_ptr<IWheelBackend> CreateActive()
    {
        for (const auto& c : kCandidates)
        {
            if (!c.applicable())
            {
                log::DebugF("[gwheel] backend '%s': not applicable; skipping", c.name);
                continue;
            }
            auto backend = c.create();
            if (!backend)
            {
                log::WarnF("[gwheel] backend '%s': create returned nullptr; trying next", c.name);
                continue;
            }
            if (!backend->Init())
            {
                log::WarnF("[gwheel] backend '%s': Init failed; trying next", c.name);
                continue;
            }
            log::InfoF("[gwheel] backend selected: %s", c.name);
            return backend;
        }

        log::Warn("[gwheel] no wheel backend could be activated; wheel:: namespace will no-op");
        return nullptr;
    }
}
