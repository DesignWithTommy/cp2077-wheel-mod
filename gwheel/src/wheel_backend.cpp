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
            // User must opt in via FOMOD or hand-edited config.json. Default
            // is OFF — even on a TrueForce-capable wheel we ship the legacy
            // path until the user (or the FOMOD installer) flips the flag.
            // Once trueforce::Initialize has run, IsActive reflects whether
            // the DLL was found, loaded, and exports were resolved.
            return config::Current().trueForce.enabled
                && trueforce::IsActive();
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
