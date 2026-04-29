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

        // Walk one pipe-delimited token at a time. The supplied callback
        // receives [start, end) for each token. Empty tokens (consecutive
        // pipes, leading/trailing) are skipped.
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

        // Returns the axis name pointer + length when `tok` is a
        // `wheel-axis:NAME` token; nullptr otherwise. The returned pointer
        // is into the source string, NOT a copy.
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

    std::string GetBinding(const char* actionName)
    {
        if (!actionName) return {};
        auto& st = S();
        std::lock_guard lk(st.mtx);
        auto it = st.bindings.find(actionName);
        if (it == st.bindings.end()) return {};
        return it->second;
    }

    float Resolve(const char* actionName, const wheel::Snapshot& snap, Combine mode)
    {
        if (!snap.connected) return 0.f;
        const std::string binding = GetBinding(actionName);
        if (binding.empty()) return 0.f;

        float result = 0.f;
        bool  haveSigned = false;
        std::string axisName;

        ForEachToken(binding, [&](const char* tok, size_t len) {
            if (!ParseWheelAxis(tok, len, axisName)) return;
            const wheel::Axis a = wheel::AxisFromName(axisName.c_str());
            if (a == wheel::Axis_kCount) return;
            const float v = snap.axes[a];
            if (mode == Combine::SignedFirst)
            {
                if (!haveSigned)
                {
                    result = std::clamp(v, -1.f, 1.f);
                    haveSigned = true;
                }
            }
            else
            {
                // UnsignedMax. Negative readings (rare but possible if a
                // wheel publishes a signed pedal axis) are clamped to 0
                // before max() so a phantom negative source can't subtract
                // from a real positive press.
                const float u = std::clamp(v, 0.f, 1.f);
                if (u > result) result = u;
            }
        });

        return result;
    }
}
