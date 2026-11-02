#include "config.h"
#include "logging.h"
#include "input_bindings.h"
#include "wheel.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace direct_wheel::config
{
    // AxisMap::Read - resolve a string like "lX", "Y", "Rz", "slider0", "S0"
    // to the corresponding member of a DIJOYSTATE2. Case-insensitive.
    // Accepts both DirectInput names (lX, lRz) and short names (X, Rz, S0)
    // matching input_probe.exe output.
    long AxisMap::Read(const DIJOYSTATE2& js, const std::string& axisName)
    {
        // Tiny linear scan - only called 4x per tick, zero allocation.
        struct Entry { const char* name; long DIJOYSTATE2::* member; };
        static const Entry kTable[] = {
            { "lX",  &DIJOYSTATE2::lX },  { "X",  &DIJOYSTATE2::lX },
            { "lY",  &DIJOYSTATE2::lY },  { "Y",  &DIJOYSTATE2::lY },
            { "lZ",  &DIJOYSTATE2::lZ },  { "Z",  &DIJOYSTATE2::lZ },
            { "lRx", &DIJOYSTATE2::lRx }, { "Rx", &DIJOYSTATE2::lRx },
            { "lRy", &DIJOYSTATE2::lRy }, { "Ry", &DIJOYSTATE2::lRy },
            { "lRz", &DIJOYSTATE2::lRz }, { "Rz", &DIJOYSTATE2::lRz },
        };
        for (const auto& e : kTable)
        {
            if (_stricmp(axisName.c_str(), e.name) == 0)
                return js.*e.member;
        }
        if (_stricmp(axisName.c_str(), "slider0") == 0 || _stricmp(axisName.c_str(), "S0") == 0) return js.rglSlider[0];
        if (_stricmp(axisName.c_str(), "slider1") == 0 || _stricmp(axisName.c_str(), "S1") == 0) return js.rglSlider[1];
        return 0; // unknown axis name - treat as zero
    }
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
            // direct_wheel.dll lives at <CP2077>/red4ext/plugins/direct_wheel/direct_wheel.dll
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
            out << "    \"enabled\": "       << (c.input.enabled ? "true" : "false")       << ",\n";
            out << "    \"clutchAsBrake\": " << (c.input.clutchAsBrake ? "true" : "false") << ",\n";
            out << "    \"invertSteering\": " << (c.input.invertSteering ? "true" : "false") << ",\n";
            out << "    \"invertThrottle\": "<< (c.input.invertThrottle ? "true" : "false") << ",\n";
            out << "    \"invertBrake\": "   << (c.input.invertBrake ? "true" : "false")   << ",\n";
            out << "    \"invertClutch\": "  << (c.input.invertClutch ? "true" : "false")  << ",\n";
            {
                std::string esc;
                EscapeJsonTo(esc, c.input.responseCurve);
                out << "    \"responseCurve\": " << esc << ",\n";
            }
            out << "    \"speedSensitiveSteeringPct\": " << c.input.speedSensitiveSteeringPct << ",\n";
            out << "    \"steeringCurve25\": "          << c.input.steeringCurve25          << ",\n";
            out << "    \"steeringCurve50\": "          << c.input.steeringCurve50          << ",\n";
            out << "    \"steeringCurve75\": "          << c.input.steeringCurve75          << "\n";
            out << "  },\n";

            out << "  \"ffb\": {\n";
            out << "    \"enabled\": "                 << (c.ffb.enabled ? "true" : "false")           << ",\n";
            out << "    \"debugLogging\": "            << (c.ffb.debugLogging ? "true" : "false")      << ",\n";
            out << "    \"torquePct\": "               << c.ffb.torquePct                              << ",\n";
            out << "    \"stationaryThresholdMps\": "  << c.ffb.stationaryThresholdMps                 << ",\n";
            out << "    \"yawFeedbackPct\": "          << c.ffb.yawFeedbackPct                         << ",\n";
            out << "    \"springNonLinearityPct\": "   << c.ffb.springNonLinearityPct                  << ",\n";
            out << "    \"activeTorqueStrengthPct\": " << c.ffb.activeTorqueStrengthPct                << ",\n";
            out << "    \"constantForcePct\": "        << c.ffb.constantForcePct                       << ",\n";
            out << "    \"springForcePct\": "          << c.ffb.springForcePct                         << ",\n";
            out << "    \"damperForcePct\": "          << c.ffb.damperForcePct                         << ",\n";
            out << "    \"frictionForcePct\": "        << c.ffb.frictionForcePct                       << ",\n";
            out << "    \"sineForcePct\": "            << c.ffb.sineForcePct                           << ",\n";
            out << "    \"joltForcePct\": "            << c.ffb.joltForcePct                           << "\n";
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

            out << "  \"axes\": {\n";
            {
                std::string esc;
                EscapeJsonTo(esc, c.axes.steer);
                out << "    \"steer\": " << esc << ",\n";
                esc.clear(); EscapeJsonTo(esc, c.axes.throttle);
                out << "    \"throttle\": " << esc << ",\n";
                esc.clear(); EscapeJsonTo(esc, c.axes.brake);
                out << "    \"brake\": " << esc << ",\n";
                esc.clear(); EscapeJsonTo(esc, c.axes.clutch);
                out << "    \"clutch\": " << esc << ",\n";
                esc.clear(); EscapeJsonTo(esc, c.axes.wheelDeviceName);
                out << "    \"wheelDeviceName\": " << esc << ",\n";
                esc.clear(); EscapeJsonTo(esc, c.axes.pedalDeviceName);
                out << "    \"pedalDeviceName\": " << esc << "\n";
            }
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

            out << "  \"bindings\": [";
            for (size_t i = 0; i < c.bindings.size(); ++i)
            {
                if (i) out << ", ";
                out << c.bindings[i];
            }
            out << "]\n";

            out << "}\n";
            return out.str();
        }

        // Parse the flat "bindings" array — fixed-length, one int per
        // PhysicalInput. Missing array or missing elements default to 0
        // (Action::None) so older config.json files upgrade cleanly.
        void ParseBindings(const std::string& text, Config& c)
        {
            c.bindings.fill(0);
            size_t arr = text.find("\"bindings\"");
            if (arr == std::string::npos) return;
            size_t lbrack = text.find('[', arr);
            if (lbrack == std::string::npos) return;
            size_t rbrack = text.find(']', lbrack);
            if (rbrack == std::string::npos) return;

            size_t i = lbrack + 1;
            size_t slot = 0;
            while (i < rbrack && slot < c.bindings.size())
            {
                // Skip whitespace and commas.
                while (i < rbrack && (text[i] == ' ' || text[i] == '\t' || text[i] == ',' ||
                                      text[i] == '\n' || text[i] == '\r'))
                    ++i;
                if (i >= rbrack) break;

                char* endp = nullptr;
                long val = std::strtol(text.c_str() + i, &endp, 10);
                if (endp == text.c_str() + i) break; // no digit found

                c.bindings[slot++] = static_cast<int32_t>(val);
                i = static_cast<size_t>(endp - text.c_str());
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
            ExtractBool  (text, "input",    "clutchAsBrake",          c.input.clutchAsBrake);
            ExtractBool  (text, "input",    "invertSteering",         c.input.invertSteering);
            ExtractBool  (text, "input",    "invertThrottle",         c.input.invertThrottle);
            ExtractBool  (text, "input",    "invertBrake",            c.input.invertBrake);
            ExtractBool  (text, "input",    "invertClutch",           c.input.invertClutch);
            ExtractString(text, "input",    "responseCurve",          c.input.responseCurve);
            ExtractInt   (text, "input",    "speedSensitiveSteeringPct", c.input.speedSensitiveSteeringPct);
            ExtractInt   (text, "input",    "steeringCurve25",        c.input.steeringCurve25);
            ExtractInt   (text, "input",    "steeringCurve50",        c.input.steeringCurve50);
            ExtractInt   (text, "input",    "steeringCurve75",        c.input.steeringCurve75);


            ExtractBool  (text, "ffb",      "enabled",                c.ffb.enabled);
            ExtractBool  (text, "ffb",      "debugLogging",           c.ffb.debugLogging);
            ExtractInt   (text, "ffb",      "torquePct",              c.ffb.torquePct);
            ExtractFloat (text, "ffb",      "stationaryThresholdMps", c.ffb.stationaryThresholdMps);
            ExtractInt   (text, "ffb",      "yawFeedbackPct",         c.ffb.yawFeedbackPct);
            ExtractInt   (text, "ffb",      "springNonLinearityPct",  c.ffb.springNonLinearityPct);
            ExtractInt   (text, "ffb",      "activeTorqueStrengthPct", c.ffb.activeTorqueStrengthPct);
            ExtractInt   (text, "ffb",      "constantForcePct",       c.ffb.constantForcePct);
            ExtractInt   (text, "ffb",      "springForcePct",         c.ffb.springForcePct);
            ExtractInt   (text, "ffb",      "damperForcePct",         c.ffb.damperForcePct);
            ExtractInt   (text, "ffb",      "frictionForcePct",       c.ffb.frictionForcePct);
            ExtractInt   (text, "ffb",      "sineForcePct",           c.ffb.sineForcePct);
            ExtractInt   (text, "ffb",      "joltForcePct",           c.ffb.joltForcePct);

            // "handshake" is the current key. Older config.json files from
            // pre-rename installs used "hello" for the same field; read
            // that first so existing settings migrate, then overwrite with
            // the new key if present.
            ExtractBool  (text, "hello",     "playOnStart",            c.handshake.playOnStart);
            ExtractBool  (text, "handshake", "playOnStart",            c.handshake.playOnStart);

            ExtractBool  (text, "led",      "enabled",                c.led.enabled);
            ExtractBool  (text, "led",      "visualizerWhileMusic",   c.led.visualizerWhileMusic);

            ExtractString(text, "music",    "processName",            c.music.processName);

            ExtractString(text, "axes",     "steer",                  c.axes.steer);
            ExtractString(text, "axes",     "throttle",               c.axes.throttle);
            ExtractString(text, "axes",     "brake",                  c.axes.brake);
            ExtractString(text, "axes",     "clutch",                 c.axes.clutch);
            ExtractString(text, "axes",     "wheelDeviceName",        c.axes.wheelDeviceName);
            ExtractString(text, "axes",     "pedalDeviceName",        c.axes.pedalDeviceName);

            auto vehExtract = [&](const char* section, PerVehicle& pv) {
                ExtractFloat(text, section, "steeringMultiplier", pv.steeringMultiplier);
                ExtractInt  (text, section, "responseDelayMs",    pv.responseDelayMs);
            };
            vehExtract("car", c.car);
            vehExtract("motorcycle", c.motorcycle);
            vehExtract("truck", c.truck);
            vehExtract("van", c.van);

            ParseBindings(text, c);
        }

        void SaveLocked(const Config& c)
        {
            auto& st = S();
            std::error_code ec;
            std::filesystem::create_directories(st.path.parent_path(), ec);
            if (ec)
            {
                log::WarnF("[direct_wheel] could not create config directory: %s", ec.message().c_str());
            }
            std::ofstream out(st.path, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                char utf8Path[MAX_PATH * 4];
                WideCharToMultiByte(CP_UTF8, 0, st.path.c_str(), -1, utf8Path, sizeof(utf8Path), nullptr, nullptr);
                log::WarnF("[direct_wheel] failed to open %s for write — settings will not persist", utf8Path);
                return;
            }
            out << Emit(c);
            log::Debug("[direct_wheel] config saved");
        }

        void ApplyDerived(const Config& c)
        {
            // Anything that must take effect immediately when config changes.
            log::SetDebugEnabled(c.ffb.debugLogging);
            input_bindings::ReplaceAll(c.bindings);
            wheel::SetGlobalStrength(std::clamp(c.ffb.torquePct, 0, 100) / 100.f);
        }

        void Publish(const Config& next)
        {
            auto& st = S();
            const int writeIdx = 1 - st.publishedIdx.load(std::memory_order_relaxed);
            st.slots[writeIdx] = next;
            st.publishedIdx.store(writeIdx, std::memory_order_release);
            ApplyDerived(next);
            SaveLocked(next);
            { // ChoomLink: one line per real pedal-setting change (Push calls ~60 setters per apply)
                char line[160];
                snprintf(line, sizeof(line), "throttle=%s brake=%s clutch=%s invert thr=%d brk=%d clu=%d",
                         next.axes.throttle.c_str(), next.axes.brake.c_str(), next.axes.clutch.c_str(),
                         next.input.invertThrottle, next.input.invertBrake, next.input.invertClutch);
                static std::string sLastApplied;
                if (sLastApplied != line) { sLastApplied = line; log::InfoF("[direct_wheel] applied: %s", line); }
            }
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
        log::InfoF("[direct_wheel] config path: %s", utf8Path);

        Config c;
        if (std::filesystem::exists(st.path))
        {
            std::ifstream in(st.path, std::ios::binary);
            std::stringstream buf;
            buf << in.rdbuf();
            try
            {
                Parse(buf.str(), c);
                log::InfoF("[direct_wheel] config loaded (version=%d, input.enabled=%s, ffb.enabled=%s, ffb.debugLogging=%s)",
                           c.version,
                           c.input.enabled ? "true" : "false",
                           c.ffb.enabled ? "true" : "false",
                           c.ffb.debugLogging ? "true" : "false");
                log::InfoF("[direct_wheel] axis map: steer=%s throttle=%s brake=%s clutch=%s",
                           c.axes.steer.c_str(), c.axes.throttle.c_str(),
                           c.axes.brake.c_str(), c.axes.clutch.c_str());
            }
            catch (...)
            {
                log::Warn("[direct_wheel] config parse failed — using defaults. "
                          "If config.json has been hand-edited, check for invalid JSON or out-of-range values.");
                c = {};
            }
        }
        else
        {
            log::Info("[direct_wheel] config.json missing — using defaults (file will be written on first change)");
        }

        Publish(c);
    }

    std::string ReadAsJson()
    {
        return Emit(Current());
    }

    void SetInputEnabled(bool v)            { Mutate([&](Config& c){ c.input.enabled = v; }); }
    void SetClutchAsBrake(bool v)           { Mutate([&](Config& c){ c.input.clutchAsBrake = v; }); }
    void SetInvertSteering(bool v)          { Mutate([&](Config& c){ c.input.invertSteering = v; }); }
    void SetInvertThrottle(bool v)          { Mutate([&](Config& c){ c.input.invertThrottle = v; }); }
    void SetInvertBrake(bool v)             { Mutate([&](Config& c){ c.input.invertBrake = v; }); }
    void SetInvertClutch(bool v)            { Mutate([&](Config& c){ c.input.invertClutch = v; }); }
    void SetSteeringDeadzoneDegrees(float v) { Mutate([&](Config& c){ c.input.steeringDeadzoneDegrees = std::clamp(v, 0.0f, 450.0f); }); }

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
    void SetSpringNonLinearityPct(int32_t v){ Mutate([&](Config& c){ c.ffb.springNonLinearityPct = std::clamp(v, 0, 100); }); }
    void SetActiveTorqueStrengthPct(int32_t v) { Mutate([&](Config& c){ c.ffb.activeTorqueStrengthPct = std::clamp(v, 0, 100); }); }
    void SetConstantForcePct(int32_t v) { Mutate([&](Config& c){ c.ffb.constantForcePct = std::clamp(v, 0, 100); }); }
    void SetSpringForcePct(int32_t v)   { Mutate([&](Config& c){ c.ffb.springForcePct = std::clamp(v, 0, 100); }); }
    void SetDamperForcePct(int32_t v)   { Mutate([&](Config& c){ c.ffb.damperForcePct = std::clamp(v, 0, 100); }); }
    void SetFrictionForcePct(int32_t v)  { Mutate([&](Config& c){ c.ffb.frictionForcePct = std::clamp(v, 0, 100); }); }
    void SetSineForcePct(int32_t v)      { Mutate([&](Config& c){ c.ffb.sineForcePct = std::clamp(v, 0, 100); }); }
    void SetJoltForcePct(int32_t v)      { Mutate([&](Config& c){ c.ffb.joltForcePct = std::clamp(v, 0, 100); }); }
    void SetSpeedSensitiveSteeringPct(int32_t v) { Mutate([&](Config& c){ c.input.speedSensitiveSteeringPct = std::clamp(v, 0, 100); }); }
    void SetSteeringCurve25(int32_t v)           { Mutate([&](Config& c){ c.input.steeringCurve25 = std::clamp(v, 0, 100); }); }
    void SetSteeringCurve50(int32_t v)           { Mutate([&](Config& c){ c.input.steeringCurve50 = std::clamp(v, 0, 100); }); }
    void SetSteeringCurve75(int32_t v)           { Mutate([&](Config& c){ c.input.steeringCurve75 = std::clamp(v, 0, 100); }); }


    void SetHandshakePlayOnStart(bool v)    { Mutate([&](Config& c){ c.handshake.playOnStart = v; }); }

    void SetLedEnabled(bool v)              { Mutate([&](Config& c){ c.led.enabled = v; }); }
    void SetLedVisualizerWhileMusic(bool v) { Mutate([&](Config& c){ c.led.visualizerWhileMusic = v; }); }

    void SetMusicProcessName(std::string_view v)
    {
        std::string s(v);
        Mutate([&](Config& c){ c.music.processName = s; });
    }

    void SetInputBinding(int32_t inputId, int32_t action)
    {
        if (inputId < 0 || static_cast<size_t>(inputId) >= Config::kBindingCount)
        {
            log::WarnF("[direct_wheel] SetInputBinding: inputId %d out of range [0..%zu)",
                       inputId, Config::kBindingCount);
            return;
        }
        Mutate([&](Config& c) {
            c.bindings[static_cast<size_t>(inputId)] = action;
        });
    }

    // Axis map setters
    void SetAxisSteer(std::string_view v)    { Mutate([&](Config& c){ c.axes.steer    = std::string(v); }); }
    void SetAxisThrottle(std::string_view v) { Mutate([&](Config& c){ c.axes.throttle = std::string(v); }); }
    void SetAxisBrake(std::string_view v)    { Mutate([&](Config& c){ c.axes.brake    = std::string(v); }); }
    void SetAxisClutch(std::string_view v)   { Mutate([&](Config& c){ c.axes.clutch   = std::string(v); }); }
    void SetWheelDeviceName(std::string_view v)
    {
        std::string s(v);
        Mutate([&](Config& c){ c.axes.wheelDeviceName = s; });
    }
    void SetPedalDeviceName(std::string_view v)
    {
        std::string s(v);
        Mutate([&](Config& c){ c.axes.pedalDeviceName = s; });
    }
}
