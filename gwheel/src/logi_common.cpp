#include "logi_common.h"

#include <algorithm>

namespace gwheel::logi_common
{
    float NormalizeSteer(LONG v)
    {
        constexpr float kMax = 32767.f;
        return std::clamp(static_cast<float>(v) / kMax, -1.f, 1.f);
    }

    float NormalizePedal(LONG v)
    {
        constexpr float kMax = 32767.f;
        constexpr float kRange = 65535.f; // kMax - kMin = 32767 - (-32768)
        const float t = (kMax - static_cast<float>(v)) / kRange;
        return std::clamp(t, 0.f, 1.f);
    }

    HWND FindOwnGameWindow()
    {
        const DWORD myPid = GetCurrentProcessId();
        auto ownedByMe = [&](HWND h) -> bool {
            if (!h || !IsWindow(h)) return false;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            return pid == myPid;
        };
        HWND h = FindWindowW(L"RED4Engine", nullptr);
        if (ownedByMe(h)) return h;
        h = FindWindowW(L"Cyberpunk 2077", nullptr);
        if (ownedByMe(h)) return h;

        struct Ctx { DWORD pid; HWND hit; } ctx{ myPid, nullptr };
        EnumWindows([](HWND w, LPARAM p) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid != c->pid) return TRUE;
            if (!IsWindowVisible(w)) return TRUE;
            RECT r{};
            if (!GetClientRect(w, &r) || (r.right - r.left) < 400 || (r.bottom - r.top) < 300)
                return TRUE;
            c->hit = w;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.hit;
    }

    void PopulateSnapshotFromDInput(const DIJOYSTATE2& raw, wheel::Snapshot& out)
    {
        using namespace wheel;

        // Steer axis (lX) keeps -1..+1 sign; every other axis is normalised
        // as a pedal (0..1, idle->full press). The user's action bindings
        // select which axis drives steer / throttle / brake at runtime.
        out.axes[Axis_lX]      = NormalizeSteer(raw.lX);
        out.axes[Axis_lY]      = NormalizePedal(raw.lY);
        out.axes[Axis_lZ]      = NormalizePedal(raw.lZ);
        out.axes[Axis_lRx]     = NormalizePedal(raw.lRx);
        out.axes[Axis_lRy]     = NormalizePedal(raw.lRy);
        out.axes[Axis_lRz]     = NormalizePedal(raw.lRz);
        out.axes[Axis_slider0] = NormalizePedal(raw.rglSlider[0]);
        out.axes[Axis_slider1] = NormalizePedal(raw.rglSlider[1]);

        out.pov = static_cast<uint16_t>(raw.rgdwPOV[0] & 0xFFFF);

        uint32_t bits = 0;
        for (int i = 0; i < 32; ++i)
            if (raw.rgbButtons[i] & 0x80) bits |= (1u << i);
        out.buttons = bits;
    }
}
