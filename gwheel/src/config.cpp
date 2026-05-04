#include "config.h"
#include "axis_resolver.h"
#include "input_bindings.h"
#include "logging.h"
#include "wheel.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace gwheel::config
{
    namespace
    {
        struct Store
        {
            std::mutex            writerMutex;
            std::atomic<int>      publishedIdx{0};
            Config                slots[2]{};
            std::filesystem::path path; // resolved lazily
        };

        Store& S()
        {
            static Store s;
            return s;
        }

        std::filesystem::path ResolvePath()
        {
            // gwheel.dll lives at <CP2077>/red4ext/plugins/gwheel/gwheel.dll
            // — config.json sits next to the DLL.
            HMODULE mod = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ResolvePath),
                &mod);
            wchar_t buf[MAX_PATH] = {};
            GetModuleFileNameW(mod, buf, MAX_PATH);
            std::filesystem::path p(buf);
            p.replace_filename(L"config.json");
            return p;
        }

        // --- very small JSON helpers, sufficient for our flat schema ------------

        void EscapeJsonTo(std::string& dst, std::string_view s)
        {
            dst.push_back('"');
            for (char c : s)
            {
                switch (c)
                {
                case '"':  dst += "\\\""; break;
                case '\\': dst += "\\\\"; break;
                case '\b': dst += "\\b";  break;
                case '\f': dst += "\\f";  break;
                case '\n': dst += "\\n";  break;
                case '\r': dst += "\\r";  break;
                case '\t': dst += "\\t";  break;
                default:   dst.push_back(c); break;
                }
            }
            dst.push_back('"');
        }

        std::string Emit(const Config& c)
        {
            std::ostringstream out;
            out << "{\n";
            out << "  \"version\": " << c.version << ",\n";

            out << "  \"input\": {\n";
            out << "    \"enabled\": "          << (c.input.enabled ? "true" : "false") << ",\n";
            out << "    \"steerDeadzonePct\": " << c.input.steerDeadzonePct             << ",\n";
            {
                std::string esc;
                EscapeJsonTo(esc, c.input.responseCurve);
                out << "    \"responseCurve\": " << esc << "\n";
            }
            out << "  },\n";

            out << "  \"ffb\": {\n";
            out << "    \"enabled\": "                 << (c.ffb.enabled ? "true" : "false")           << ",\n";
            out << "    \"debugLogging\": "            << (c.ffb.debugLogging ? "true" : "false")      << ",\n";
            out << "    \"torquePct\": "               << c.ffb.torquePct                              << ",\n";
            out << "    \"stationaryThresholdMps\": "  << c.ffb.stationaryThresholdMps                 << ",\n";
            out << "    \"yawFeedbackPct\": "          << c.ffb.yawFeedbackPct                         << ",\n";
            out << "    \"activeTorqueStrengthPct\": " << c.ffb.activeTorqueStrengthPct                << ",\n";
            out << "    \"invertDirection\": "         << (c.ffb.invertDirection ? "true" : "false")   << ",\n";
            out << "    \"outputSignFlip\": "          << (c.ffb.outputSignFlip ? "true" : "false")    << ",\n";
            out << "    \"useSpring\": "               << (c.ffb.useSpring ? "true" : "false")         << "\n";
            out << "  },\n";

            out << "  \"handshake\": {\n";
            out << "    \"playOnStart\": " << (c.handshake.playOnStart ? "true" : "false") << "\n";
            out << "  },\n";

            out << "  \"led\": {\n";
            out << "    \"enabled\": "              << (c.led.enabled ? "true" : "false")              << ",\n";
            out << "    \"visualizerWhileMusic\": " << (c.led.visualizerWhileMusic ? "true" : "false") << "\n";
            out << "  },\n";

            out << "  \"music\": {\n";
            {
                std::string esc;
                EscapeJsonTo(esc, c.music.processName);
                out << "    \"processName\": " << esc << "\n";
            }
            out << "  },\n";

            out << "  \"trueForce\": {\n";
            out << "    \"enabled\": "     << (c.trueForce.enabled ? "true" : "false")     << ",\n";
            out << "    \"feedSignals\": " << (c.trueForce.feedSignals ? "true" : "false") << "\n";
            out << "  },\n";

            auto emitVeh = [&](const char* name, const PerVehicle& pv, bool last) {
                out << "    \"" << name << "\": { "
                    << "\"steeringMultiplier\": " << pv.steeringMultiplier << ", "
                    << "\"responseDelayMs\": " << pv.responseDelayMs << " }"
                    << (last ? "\n" : ",\n");
            };
            out << "  \"perVehicle\": {\n";
            emitVeh("car", c.car, false);
            emitVeh("motorcycle", c.motorcycle, false);
            emitVeh("truck", c.truck, false);
            emitVeh("van", c.van, true);
            out << "  },\n";

            // Sort by key so emitted JSON is deterministic across runs;
            // an unordered_map's iteration order is implementation-defined
            // and would otherwise produce noisy diffs.
            std::vector<std::pair<std::string, std::string>> entries(
                c.actionBindings.begin(), c.actionBindings.end());
            std::sort(entries.begin(), entries.end());

            out << "  \"actionBindings\": {";
            for (size_t i = 0; i < entries.size(); ++i)
            {
                out << (i == 0 ? "\n    " : ",\n    ");
                std::string esc;
                EscapeJsonTo(esc, entries[i].first);
                out << esc << ": ";
                esc.clear();
                EscapeJsonTo(esc, entries[i].second);
                out << esc;
            }
            if (!entries.empty()) out << "\n  ";
            out << "}\n";

            out << "}\n";
            return out.str();
        }

        // Parse the actionBindings object. Tagged tokens never contain
        // quote characters, so a naive scan that finds key/value string
        // pairs between braces is sufficient — no escape-aware parsing
        // needed. Missing object = empty map (tabula rasa).
        void ParseActionBindings(const std::string& text, Config& c)
        {
            c.actionBindings.clear();
            size_t pos = text.find("\"actionBindings\"");
            if (pos == std::string::npos) return;
            size_t lbrace = text.find('{', pos);
            if (lbrace == std::string::npos) return;
            size_t rbrace = text.find('}', lbrace);
            if (rbrace == std::string::npos) return;

            size_t i = lbrace + 1;
            while (i < rbrace)
            {
                while (i < rbrace && text[i] != '"') ++i;
                if (i >= rbrace) break;
                size_t kStart = i + 1;
                size_t kEnd = text.find('"', kStart);
                if (kEnd == std::string::npos || kEnd >= rbrace) break;
                std::string key(text, kStart, kEnd - kStart);

                size_t colon = text.find(':', kEnd);
                if (colon == std::string::npos || colon >= rbrace) break;
                i = colon + 1;
                while (i < rbrace && text[i] != '"') ++i;
                if (i >= rbrace) break;
                size_t vStart = i + 1;
                size_t vEnd = text.find('"', vStart);
                if (vEnd == std::string::npos || vEnd >= rbrace) break;
                std::string val(text, vStart, vEnd - vStart);

                c.actionBindings[std::move(key)] = std::move(val);
                i = vEnd + 1;
            }
        }

        // Case-sensitive "find `"key"` after the last occurrence of `section`".
        // Returns the offset of the value's first char, or std::string::npos.
        size_t FindValueOffset(const std::string& text, std::string_view section, std::string_view key)
        {
            size_t sec = section.empty() ? 0 : text.find(std::string("\"") + std::string(section) + "\"");
            if (sec == std::string::npos) return std::string::npos;
            size_t k = text.find(std::string("\"") + std::string(key) + "\"", sec);
            if (k == std::string::npos) return std::string::npos;
            size_t colon = text.find(':', k);
            if (colon == std::string::npos) return std::string::npos;
            size_t v = colon + 1;
            while (v < text.size() && (text[v] == ' ' || text[v] == '\t')) ++v;
            return v;
        }

        bool ExtractBool(const std::string& text, std::string_view section, std::string_view key, bool& out)
        {
            size_t v = FindValueOffset(text, section, key);
            if (v == std::string::npos) return false;
            if (text.compare(v, 4, "true") == 0)  { out = true; return true; }
            if (text.compare(v, 5, "false") == 0) { out = false; return true; }
            return false;
        }

        bool ExtractInt(const std::string& text, std::string_view section, std::string_view key, int32_t& out)
        {
            size_t v = FindValueOffset(text, section, key);
            if (v == std::string::npos) return false;
            char* endp = nullptr;
            long val = std::strtol(text.c_str() + v, &endp, 10);
            if (endp == text.c_str() + v) return false;
            out = static_cast<int32_t>(val);
            return true;
        }

        bool ExtractFloat(const std::string& text, std::string_view section, std::string_view key, float& out)
        {
            size_t v = FindValueOffset(text, section, key);
            if (v == std::string::npos) return false;
            char* endp = nullptr;
            double val = std::strtod(text.c_str() + v, &endp);
            if (endp == text.c_str() + v) return false;
            out = static_cast<float>(val);
            return true;
        }

        bool ExtractString(const std::string& text, std::string_view section, std::string_view key, std::string& out)
        {
            size_t v = FindValueOffset(text, section, key);
            if (v == std::string::npos || v >= text.size() || text[v] != '"') return false;
            size_t end = text.find('"', v + 1);
            if (end == std::string::npos) return false;
            out.assign(text, v + 1, end - v - 1);
            return true;
        }

        void Parse(const std::string& text, Config& c)
        {
            ExtractInt   (text, {},         "version",                c.version);

            ExtractBool  (text, "input",    "enabled",                c.input.enabled);
            ExtractInt   (text, "input",    "steerDeadzonePct",       c.input.steerDeadzonePct);
            ExtractString(text, "input",    "responseCurve",          c.input.responseCurve);

            ExtractBool  (text, "ffb",      "enabled",                c.ffb.enabled);
            ExtractBool  (text, "ffb",      "debugLogging",           c.ffb.debugLogging);
            ExtractInt   (text, "ffb",      "torquePct",              c.ffb.torquePct);
            ExtractFloat (text, "ffb",      "stationaryThresholdMps", c.ffb.stationaryThresholdMps);
            ExtractInt   (text, "ffb",      "yawFeedbackPct",         c.ffb.yawFeedbackPct);
            ExtractInt   (text, "ffb",      "activeTorqueStrengthPct", c.ffb.activeTorqueStrengthPct);
            ExtractBool  (text, "ffb",      "invertDirection",        c.ffb.invertDirection);
            ExtractBool  (text, "ffb",      "outputSignFlip",         c.ffb.outputSignFlip);
            ExtractBool  (text, "ffb",      "useSpring",              c.ffb.useSpring);

            // "handshake" is the current key. Older config.json files from
            // pre-rename installs used "hello" for the same field; read
            // that first so existing settings migrate, then overwrite with
            // the new key if present.
            ExtractBool  (text, "hello",     "playOnStart",            c.handshake.playOnStart);
            ExtractBool  (text, "handshake", "playOnStart",            c.handshake.playOnStart);

            ExtractBool  (text, "led",      "enabled",                c.led.enabled);
            ExtractBool  (text, "led",      "visualizerWhileMusic",   c.led.visualizerWhileMusic);

            ExtractString(text, "music",    "processName",            c.music.processName);

            ExtractBool  (text, "trueForce", "enabled",               c.trueForce.enabled);
            ExtractBool  (text, "trueForce", "feedSignals",           c.trueForce.feedSignals);

            auto vehExtract = [&](const char* section, PerVehicle& pv) {
                ExtractFloat(text, section, "steeringMultiplier", pv.steeringMultiplier);
                ExtractInt  (text, section, "responseDelayMs",    pv.responseDelayMs);
            };
            vehExtract("car", c.car);
            vehExtract("motorcycle", c.motorcycle);
            vehExtract("truck", c.truck);
            vehExtract("van", c.van);

            ParseActionBindings(text, c);
        }

        void SaveLocked(const Config& c)
        {
            auto& st = S();
            std::error_code ec;
            std::filesystem::create_directories(st.path.parent_path(), ec);
            if (ec)
            {
                log::WarnF("[gwheel] could not create config directory: %s", ec.message().c_str());
            }
            std::ofstream out(st.path, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                char utf8Path[MAX_PATH * 4];
                WideCharToMultiByte(CP_UTF8, 0, st.path.c_str(), -1, utf8Path, sizeof(utf8Path), nullptr, nullptr);
                log::WarnF("[gwheel] failed to open %s for write — settings will not persist", utf8Path);
                return;
            }
            out << Emit(c);
            log::Debug("[gwheel] config saved");
        }

        void ApplyDerived(const Config& c)
        {
            // Anything that must take effect immediately when config changes.
            //
            // Edge-triggered info log on the debug-logging toggle so the
            // user can confirm in the log file that the Mod Settings
            // checkbox actually propagated. Without this, a user
            // diagnosing a "debug logs aren't working" issue has no way
            // to tell whether the flag flipped or not — the absence of
            // debug lines could mean either "toggle didn't apply" or
            // "no debug paths fired".
            static std::atomic<int> s_lastDebugFlag{-1}; // -1 = first call
            const int newFlag = c.ffb.debugLogging ? 1 : 0;
            const int prev = s_lastDebugFlag.exchange(newFlag, std::memory_order_acq_rel);
            log::SetDebugEnabled(c.ffb.debugLogging);
            if (prev != newFlag)
            {
                log::InfoF("[gwheel] debug logging %s",
                           c.ffb.debugLogging ? "ENABLED" : "disabled");
            }

            wheel::SetGlobalStrength(std::clamp(c.ffb.torquePct, 0, 100) / 100.f);

            // Mirror the persisted action bindings into both runtime
            // dispatchers. Clear-then-repopulate handles deletions cleanly:
            // a key removed from c.actionBindings disappears from both
            // resolvers on the next Mutate.
            //
            // Each entry is fanned out to every dispatcher; each dispatcher
            // ignores entries it doesn't own (axis_resolver ignores button
            // tokens, input_bindings ignores axis tokens, both ignore
            // empty values).
            axis_resolver::Clear();
            input_bindings::Clear();
            for (const auto& kv : c.actionBindings)
            {
                axis_resolver::SetBinding(kv.first.c_str(), kv.second.c_str());
                input_bindings::SetActionBinding(kv.first, kv.second);
            }
        }

        void Publish(const Config& next)
        {
            auto& st = S();
            const int writeIdx = 1 - st.publishedIdx.load(std::memory_order_relaxed);
            st.slots[writeIdx] = next;
            st.publishedIdx.store(writeIdx, std::memory_order_release);
            ApplyDerived(next);
            SaveLocked(next);
        }

        template <typename F>
        void Mutate(F&& f)
        {
            auto& st = S();
            std::lock_guard lock(st.writerMutex);
            Config next = Current();
            f(next);
            Publish(next);
        }
    }

    Config Current()
    {
        auto& st = S();
        const int idx = st.publishedIdx.load(std::memory_order_acquire);
        return st.slots[idx];
    }

    void Load()
    {
        auto& st = S();
        std::lock_guard lock(st.writerMutex);
        st.path = ResolvePath();

        char utf8Path[MAX_PATH * 4];
        WideCharToMultiByte(CP_UTF8, 0, st.path.c_str(), -1, utf8Path, sizeof(utf8Path), nullptr, nullptr);
        log::InfoF("[gwheel] config path: %s", utf8Path);

        Config c;
        if (std::filesystem::exists(st.path))
        {
            std::ifstream in(st.path, std::ios::binary);
            std::stringstream buf;
            buf << in.rdbuf();
            try
            {
                Parse(buf.str(), c);
                log::InfoF("[gwheel] config loaded (version=%d, input.enabled=%s, ffb.enabled=%s, ffb.debugLogging=%s)",
                           c.version,
                           c.input.enabled ? "true" : "false",
                           c.ffb.enabled ? "true" : "false",
                           c.ffb.debugLogging ? "true" : "false");
            }
            catch (...)
            {
                log::Warn("[gwheel] config parse failed — using defaults. "
                          "If config.json has been hand-edited, check for invalid JSON or out-of-range values.");
                c = {};
            }
        }
        else
        {
            log::Info("[gwheel] config.json missing — using defaults (file will be written on first change)");
        }

        Publish(c);
    }

    std::string ReadAsJson()
    {
        return Emit(Current());
    }

    void SetInputEnabled(bool v)            { Mutate([&](Config& c){ c.input.enabled = v; }); }

    void SetSteerDeadzonePct(int32_t v)     { Mutate([&](Config& c){ c.input.steerDeadzonePct = std::clamp(v, 0, 15); }); }

    void SetResponseCurve(std::string_view v)
    {
        std::string s(v);
        if (s != "default" && s != "subdued" && s != "sharp") return;
        Mutate([&](Config& c){ c.input.responseCurve = s; });
    }

    void SetFfbEnabled(bool v)              { Mutate([&](Config& c){ c.ffb.enabled = v; }); }
    void SetFfbDebugLogging(bool v)         { Mutate([&](Config& c){ c.ffb.debugLogging = v; }); }
    void SetFfbTorquePct(int32_t v)         { Mutate([&](Config& c){ c.ffb.torquePct = std::clamp(v, 0, 100); }); }

    void SetStationaryThresholdMps(float v) { Mutate([&](Config& c){ c.ffb.stationaryThresholdMps = std::clamp(v, 0.f, 10.f); }); }
    void SetYawFeedbackPct(int32_t v)       { Mutate([&](Config& c){ c.ffb.yawFeedbackPct = std::clamp(v, 0, 100); }); }
    void SetActiveTorqueStrengthPct(int32_t v) { Mutate([&](Config& c){ c.ffb.activeTorqueStrengthPct = std::clamp(v, 0, 100); }); }
    void SetFfbInvertDirection(bool v)         { Mutate([&](Config& c){ c.ffb.invertDirection = v; }); }
    void SetFfbOutputSignFlip(bool v)          { Mutate([&](Config& c){ c.ffb.outputSignFlip = v; }); }
    void SetFfbUseSpring(bool v)               { Mutate([&](Config& c){ c.ffb.useSpring = v; }); }

    void SetHandshakePlayOnStart(bool v)    { Mutate([&](Config& c){ c.handshake.playOnStart = v; }); }

    void SetLedEnabled(bool v)              { Mutate([&](Config& c){ c.led.enabled = v; }); }
    void SetLedVisualizerWhileMusic(bool v) { Mutate([&](Config& c){ c.led.visualizerWhileMusic = v; }); }

    void SetMusicProcessName(std::string_view v)
    {
        std::string s(v);
        Mutate([&](Config& c){ c.music.processName = s; });
    }

    void SetTrueForceEnabled(bool v)        { Mutate([&](Config& c){ c.trueForce.enabled = v; }); }
    void SetTrueForceFeedSignals(bool v)    { Mutate([&](Config& c){ c.trueForce.feedSignals = v; }); }

    void SetActionBinding(std::string_view name, std::string_view value)
    {
        if (name.empty()) return;
        std::string k(name);
        std::string v(value);
        Mutate([&](Config& c) {
            if (v.empty())
                c.actionBindings.erase(k);
            else
                c.actionBindings[k] = v;
        });
    }
}
