#include "axis_resolver.h"
#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace gwheel::axis_resolver
{
    namespace
    {
        struct State
        {
            std::mutex mtx;
            std::unordered_map<std::string, std::string> bindings;
        };

        State& S()
        {
            static State s;
            return s;
        }

        // Walk one pipe-delimited token at a time. Empty tokens (consecutive
        // pipes, leading/trailing) are skipped. Multi-binding is now a max-2-
        // slot system but the pipe walker handles arbitrary count for safety.
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

        // Returns true if `tok` is a `wheel-axis:NAME` token, copying NAME
        // into out. False otherwise.
        bool ParseWheelAxis(const char* tok, size_t len, std::string& out)
        {
            constexpr const char prefix[] = "wheel-axis:";
            constexpr size_t plen = sizeof(prefix) - 1;
            if (len <= plen) return false;
            if (std::memcmp(tok, prefix, plen) != 0) return false;
            out.assign(tok + plen, len - plen);
            return true;
        }

        // Apply center deadzone + response curve to a signed steering input
        // in [-1, +1]. Pedals (UnsignedMax) skip this — they already clamp
        // 0..1 and don't suffer from center bias.
        //
        // Deadzone: |v| < dz returns 0; otherwise rescale linearly so that
        // values just outside the deadzone start at 0 and full deflection
        // still reaches +/-1. Without rescaling, the wheel would lose 2-15%
        // of its range to the deadzone (a quiet sensitivity nerf).
        //
        // Response curve: power-law on |v|, sign preserved. Subdued > 1
        // softens center; Sharp < 1 sharpens center. Default is linear.
        float ShapeSignedSteer(float v, int32_t deadzonePct, const std::string& curve)
        {
            const float dz = std::clamp(deadzonePct, 0, 99) * 0.01f;
            const float a  = std::fabs(v);
            if (a <= dz) return 0.f;

            // Rescale so a == dz -> 0, a == 1 -> 1.
            const float rescaled = (a - dz) / (1.f - dz);
            const float clamped  = std::clamp(rescaled, 0.f, 1.f);

            float shaped = clamped;
            if (curve == "subdued")
            {
                // |v|^1.7 — convex; halves response near center, recovers
                // toward full at the rim.
                shaped = std::pow(clamped, 1.7f);
            }
            else if (curve == "sharp")
            {
                // |v|^0.6 — concave; small movements off-center already
                // produce ~half deflection.
                shaped = std::pow(clamped, 0.6f);
            }
            // "default" / unknown: linear (already in `shaped`).

            return std::copysign(shaped, v);
        }
    }

    void SetBinding(const char* actionName, const char* value)
    {
        if (!actionName) return;
        auto& st = S();
        std::lock_guard lk(st.mtx);
        if (!value || *value == '\0')
            st.bindings.erase(actionName);
        else
            st.bindings[actionName] = value;
    }

    void Clear()
    {
        auto& st = S();
        std::lock_guard lk(st.mtx);
        st.bindings.clear();
    }

    std::string GetBinding(const char* actionName)
    {
        if (!actionName) return {};
        auto& st = S();
        std::lock_guard lk(st.mtx);
        auto it = st.bindings.find(actionName);
        if (it == st.bindings.end()) return {};
        return it->second;
    }

    bool IsActionWheelBound(const char* actionName)
    {
        const std::string binding = GetBinding(actionName);
        if (binding.empty()) return false;
        bool found = false;
        std::string axisName;
        ForEachToken(binding, [&](const char* tok, size_t len) {
            if (found) return;
            if (ParseWheelAxis(tok, len, axisName)) found = true;
        });
        return found;
    }

    float Resolve(const char* actionName, const wheel::Snapshot& snap, Combine mode)
    {
        if (!snap.connected) return 0.f;
        const std::string binding = GetBinding(actionName);
        if (binding.empty()) return 0.f;

        float result      = 0.f;
        bool  haveSigned  = false;
        std::string axisName;

        ForEachToken(binding, [&](const char* tok, size_t len) {
            if (!ParseWheelAxis(tok, len, axisName)) return;
            const wheel::Axis a = wheel::AxisFromName(axisName.c_str());
            if (a == wheel::Axis_kCount) return;
            const float v = snap.axes[a];
            if (mode == Combine::SignedFirst)
            {
                // First signed source wins (steer should not max() across
                // sources — left + right paddles bound to steer would pick
                // the larger absolute deflection regardless of sign).
                if (!haveSigned)
                {
                    result = std::clamp(v, -1.f, 1.f);
                    haveSigned = true;
                }
            }
            else
            {
                // UnsignedMax: pedal-class. Negative reads on a signed pedal
                // axis are clamped to 0 so a phantom negative source can't
                // subtract from a real positive press.
                const float u = std::clamp(v, 0.f, 1.f);
                if (u > result) result = u;
            }
        });

        if (mode == Combine::SignedFirst && haveSigned)
        {
            // Steer-only filtering. Pedals skip this — they're 0..1 and
            // don't suffer from a center offset.
            //
            // We deliberately do NOT scale the steer axis by the G HUB
            // operating range. The DInput lX axis already maps full
            // physical rotation to ±1 across whatever range G HUB has
            // configured: at G HUB=180 the wheel hits axis=±1 at ±90°
            // physical, at G HUB=900 it hits ±1 at ±450° physical. The
            // game's in-game wheel is then driven from that ±1 axis,
            // so wider G HUB ranges naturally translate into more rim
            // travel for the same in-game lock — which is what the
            // user wants. FFB feel changes with range (the rangeScale
            // factor in ApplyFfbFromPhysics handles this) but
            // steering ratio stays "G HUB range = full lock".
            const auto cfg = config::Current();
            result = ShapeSignedSteer(result, cfg.input.steerDeadzonePct, cfg.input.responseCurve);
        }

        return result;
    }
}
