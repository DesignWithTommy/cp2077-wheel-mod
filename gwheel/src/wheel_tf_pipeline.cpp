#include "wheel_tf_pipeline.h"
#include "config.h"
#include "logging.h"
#include "wheel_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

// We need a pointer to the active backend to feed samples. wheel.cpp owns
// the unique_ptr; we expose it through a tiny accessor inside the wheel::
// namespace's anonymous-namespace block via a friend-style external getter.
// To avoid coupling, this file declares its own access function and wheel.cpp
// defines it — the indirection keeps wheel_tf_pipeline.cpp from including
// wheel.cpp's internals.
namespace gwheel::wheel
{
    // Defined in wheel.cpp. Returns the active backend pointer, or nullptr
    // when no backend is bound. Allows wheel_tf_pipeline to feed samples
    // without holding its own backend handle.
    wheel_backend::IWheelBackend* GetActiveBackend();
}

namespace gwheel::wheel_tf
{
    namespace
    {
        // Engine oscillator phase accumulator. Driven by RPM-derived cycle
        // frequency. Wraps in [0, 2π).
        std::atomic<float>    g_enginePhase{0.f};

        // Wall-clock timestamp of the last Feed call, used to compute dt
        // for phase advance. 0 = uninitialised; first call seeds.
        std::atomic<uint64_t> g_lastFeedNs{0};

        // Decaying impact-burst envelope. A spike in surfaceEnvelope arms
        // the burst; subsequent ticks decay it linearly so a curb impact
        // produces a brief hum even after the surfaceEnvelope itself has
        // dropped (the LPF in wheel.cpp is asymmetric: slow attack, fast
        // decay).
        std::atomic<float>    g_impactBurst{0.f};
        std::atomic<float>    g_lastSurface{0.f};

        // Cached config snapshot to avoid re-reading every tick (config::
        // Current() is a snapshot read; cheap but called 60+ times/sec).
        std::atomic<bool>     g_feedEnabled{false};
        std::atomic<uint64_t> g_lastConfigCheckTickMs{0};

        // Refresh feedEnabled flag at most once per ~250 ms.
        bool ShouldFeed()
        {
            const uint64_t now = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            const uint64_t last = g_lastConfigCheckTickMs.load(std::memory_order_relaxed);
            if (now - last > 250)
            {
                g_feedEnabled.store(config::Current().trueForce.feedSignals,
                                    std::memory_order_relaxed);
                g_lastConfigCheckTickMs.store(now, std::memory_order_relaxed);
            }
            return g_feedEnabled.load(std::memory_order_relaxed);
        }

        // Pseudo-random in [-1, +1]. Fast LCG; not cryptographic.
        float NoiseSample()
        {
            static thread_local uint32_t s = 0xC0FFEEu;
            s = s * 1664525u + 1013904223u;
            return (static_cast<float>(s >> 8) / 8388608.f) - 1.f;
        }
    }

    void Reset()
    {
        g_enginePhase.store(0.f, std::memory_order_relaxed);
        g_lastFeedNs.store(0, std::memory_order_relaxed);
        g_impactBurst.store(0.f, std::memory_order_relaxed);
        g_lastSurface.store(0.f, std::memory_order_relaxed);
    }

    void Feed(const Inputs& in)
    {
        if (!ShouldFeed()) return;

        auto* backend = gwheel::wheel::GetActiveBackend();
        if (!backend) return;
        const auto& caps = backend->GetCapabilities();
        if (!caps.vibrotactile) return;

        // ---- dt -----------------------------------------------------------
        const uint64_t nowNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        const uint64_t lastNs = g_lastFeedNs.exchange(nowNs, std::memory_order_acq_rel);
        const float dt = (lastNs == 0)
                       ? 0.f
                       : std::clamp(static_cast<float>(nowNs - lastNs) * 1e-9f, 0.f, 0.05f);

        // ---- Engine pulse -------------------------------------------------
        // Engine cycle frequency: rpmNorm * redlineHz where redlineHz is a
        // perceptual constant (we don't have authored cylinder counts and
        // the wheel can't reproduce true engine fundamentals at our 60 Hz
        // call rate anyway). 80 Hz at redline gives a felt buzz that scales
        // with RPM. Throttle modulates amplitude so coasting is quieter
        // than wide-open-throttle at the same RPM.
        constexpr float kEngineRedlineHz = 80.f;
        constexpr float kEngineIdleHz    = 12.f;
        const float cycleHz = kEngineIdleHz
                            + (kEngineRedlineHz - kEngineIdleHz) * std::clamp(in.rpmNorm, 0.f, 1.f);

        float phase = g_enginePhase.load(std::memory_order_relaxed);
        phase += 2.f * 3.14159265f * cycleHz * dt;
        // Wrap to keep float precision over long sessions.
        constexpr float k2pi = 6.2831853f;
        while (phase >= k2pi) phase -= k2pi;
        g_enginePhase.store(phase, std::memory_order_relaxed);

        // Amplitude: idle baseline + throttle-driven gain. Stationary
        // and reversing both cut amplitude (engine quiet at idle, in-
        // reverse the wheel feel pipeline already attenuates everything).
        const float throttleGain = 0.4f + 0.6f * std::clamp(in.throttle, 0.f, 1.f);
        const float engineAmp    = (in.isMoving ? 0.18f : 0.05f)
                                 * throttleGain
                                 * (in.isReversing ? 0.5f : 1.f);
        const float engineSample = std::sin(phase) * engineAmp;

        // ---- Suspension impact burst -------------------------------------
        // Detect rising edge in surfaceEnvelope: jump from low to high
        // arms a burst. Burst decays linearly per tick. This sits ON TOP
        // of the steady-state surfaceEnvelope itself, so curbs feel like
        // a hit + a brief tail rather than a slow ramp.
        const float prevSurf = g_lastSurface.exchange(in.surfaceEnvelope, std::memory_order_acq_rel);
        const float surfRise = std::max(0.f, in.surfaceEnvelope - prevSurf);
        constexpr float kImpactArmThreshold = 0.05f;
        if (surfRise > kImpactArmThreshold)
        {
            // Add to existing burst (multiple impacts compound briefly).
            float burst = g_impactBurst.load(std::memory_order_relaxed);
            burst = std::min(0.4f, burst + surfRise * 1.5f);
            g_impactBurst.store(burst, std::memory_order_relaxed);
        }
        // Decay burst per tick (target: ~150 ms half-life at 60 Hz).
        float burst = g_impactBurst.load(std::memory_order_relaxed);
        burst *= 0.93f;
        if (burst < 0.001f) burst = 0.f;
        g_impactBurst.store(burst, std::memory_order_relaxed);

        const float impactSample = burst * NoiseSample();

        // ---- Skid texture -------------------------------------------------
        // Past peak grip we want a buzzy, granular texture in the rim.
        // Pseudo-random noise modulated by skidNorm. Only fires when
        // skidNorm crosses a threshold so light cornering stays clean.
        constexpr float kSkidGate = 0.20f;
        const float skidNorm = std::clamp(in.skidNorm, 0.f, 1.f);
        const float skidGain = (skidNorm > kSkidGate)
                                 ? std::min(0.25f, (skidNorm - kSkidGate) * 0.5f)
                                 : 0.f;
        const float skidSample = NoiseSample() * skidGain;

        // ---- Surface baseline hum ----------------------------------------
        // The LPF'd surface envelope already encodes "we're on a textured
        // surface". Feed a low-amplitude noise scaled by it so dirt /
        // gravel buzzes continuously, asphalt stays silent.
        const float surfaceSample = NoiseSample() * std::clamp(in.surfaceEnvelope * 0.3f, 0.f, 0.2f);

        // ---- Airborne ----------------------------------------------------
        // Wheel is disconnected from the road; output silence. The
        // kinesthetic pipeline already plays the airborne effect on
        // backends that have one; TF-side we just go quiet.
        if (!in.isOnGround)
        {
            backend->TfFeedSample(0.f);
            return;
        }

        // ---- Mix + safety cap -------------------------------------------
        // Conservative cap pending signature validation. Once the SDK's
        // sample-value semantics are confirmed (audio-sample [-1,+1] vs
        // raw torque Nm vs normalised magnitude), this cap can move.
        constexpr float kSafetyCap = 0.5f;
        float sample = engineSample + impactSample + skidSample + surfaceSample;
        sample = std::clamp(sample, -kSafetyCap, kSafetyCap);

        backend->TfFeedSample(sample);
    }
}
