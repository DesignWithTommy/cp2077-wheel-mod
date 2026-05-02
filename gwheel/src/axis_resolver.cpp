#include "axis_resolver.h"

#include <algorithm>
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

        return result;
    }
}
