#include "sources.h"

#include <array>
#include <atomic>
#include <mutex>

namespace gwheel::sources
{
    namespace
    {
        struct State
        {
            std::mutex         mtx;
            Frame              frame{};
            std::atomic<bool>  inVehicle{false};
            std::atomic<float> engineRpmRaw{0.f};
            std::atomic<bool>  radioActive{false};

            // Engine profile, populated once per mount. Reads from the LED
            // tick are lock-free; writes only happen at attach / detach.
            std::atomic<float>   idleRpm{0.f};
            std::atomic<float>   redlineRpm{0.f};
            std::atomic<int32_t> gearCount{0};
            std::array<std::atomic<float>, kMaxGears> perGearShiftRpm{};
            std::atomic<int32_t> currentGear{0};
        };

        State& S() { static State s; return s; }
    }

    void Publish(const Frame& f)
    {
        auto& st = S();
        std::lock_guard lk(st.mtx);
        st.frame = f;
    }

    Frame Current()
    {
        auto& st = S();
        std::lock_guard lk(st.mtx);
        return st.frame;
    }

    void SetInVehicle(bool v) { S().inVehicle.store(v, std::memory_order_release); }
    bool InVehicle()          { return S().inVehicle.load(std::memory_order_acquire); }

    void SetEngineRpmRaw(float v)
    {
        const float clamped = (v < 0.f) ? 0.f : v;
        S().engineRpmRaw.store(clamped, std::memory_order_release);
    }
    float EngineRpmRaw()
    {
        return S().engineRpmRaw.load(std::memory_order_acquire);
    }

    void SetEngineProfile(float idleRpm, float redlineRpm,
                          const float* perGearShiftRpms, int gearCount)
    {
        auto& st = S();
        st.idleRpm.store(idleRpm > 0.f ? idleRpm : 0.f, std::memory_order_release);
        st.redlineRpm.store(redlineRpm > 0.f ? redlineRpm : 0.f, std::memory_order_release);
        const int n = (gearCount < 0) ? 0
                    : (gearCount > kMaxGears) ? kMaxGears : gearCount;
        for (int i = 0; i < n; ++i)
        {
            const float v = perGearShiftRpms ? perGearShiftRpms[i] : 0.f;
            st.perGearShiftRpm[i].store(v > 0.f ? v : 0.f, std::memory_order_release);
        }
        for (int i = n; i < kMaxGears; ++i)
        {
            st.perGearShiftRpm[i].store(0.f, std::memory_order_release);
        }
        st.gearCount.store(n, std::memory_order_release);
    }

    void ClearEngineProfile()
    {
        auto& st = S();
        st.idleRpm.store(0.f, std::memory_order_release);
        st.redlineRpm.store(0.f, std::memory_order_release);
        st.gearCount.store(0, std::memory_order_release);
        for (int i = 0; i < kMaxGears; ++i)
            st.perGearShiftRpm[i].store(0.f, std::memory_order_release);
        st.currentGear.store(0, std::memory_order_release);
    }

    float IdleRpm()    { return S().idleRpm.load(std::memory_order_acquire); }
    float RedlineRpm() { return S().redlineRpm.load(std::memory_order_acquire); }

    float ShiftRpmForGear(int gear)
    {
        auto& st = S();
        const int n = st.gearCount.load(std::memory_order_acquire);
        if (gear < 1 || gear > n) return st.redlineRpm.load(std::memory_order_acquire);
        const float v = st.perGearShiftRpm[gear - 1].load(std::memory_order_acquire);
        return v > 0.f ? v : st.redlineRpm.load(std::memory_order_acquire);
    }

    void SetCurrentGear(int gear) { S().currentGear.store(gear, std::memory_order_release); }
    int  CurrentGear()            { return S().currentGear.load(std::memory_order_acquire); }

    void SetRadioActive(bool v) { S().radioActive.store(v, std::memory_order_release); }
    bool RadioActive()          { return S().radioActive.load(std::memory_order_acquire); }
}
