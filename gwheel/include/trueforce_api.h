#pragma once

// Clean-room interop declarations for the Logitech TrueForce runtime.
//
// This header is our own work. It is NOT a copy of, or derived from,
// any Logitech-licensed materials; once we receive the official TrueForce
// SDK from Logitech's Partner Developer Lab, this file is replaced with
// the licensed headers and this comment block goes away.
//
// We never link statically against trueforce_sdk_x64.dll. The runtime
// is loaded dynamically from the user's existing G HUB installation
// (see trueforce_loader.cpp for the discovery path), and we never
// redistribute Logitech bytes in our release archives.
//
// ---------------------------------------------------------------------------
// API shape (v1.3.11.0 of trueforce_sdk_x64.dll)
//
// Names below are taken from the export table of the installed DLL on
// a stock G HUB setup. The DLL is deployed by trueforce_manager.exe to
//
//     %ProgramFiles%\Logi\Trueforce\<version>\trueforce_sdk_x64.dll
//
// Two parallel force channels live in the same DLL:
//
//   * Kinesthetic Force (KF): low-frequency, classic spring/damper/torque
//     pipeline. Roughly equivalent to the LogiPlayConstantForce /
//     LogiPlaySpringForce surface from the older Steering Wheel SDK.
//
//   * TrueForce (TF): high-frequency, audio-derived sample stream. This
//     is what G HUB's "Audio Effects" slider scales. Fed via either
//     SetStreamTF (continuous buffer) or SetTorqueTF{type} (per-sample).
//
// Wheel handles are obtained via OpenByDirectInput, which takes our
// existing IDirectInputDevice8 pointer (we already enumerate Logitech
// wheels via DInput in mod_settings_seed and wheel.cpp).
//
// ---------------------------------------------------------------------------
// Signature accuracy
//
// Function NAMES are confirmed against the export dump.
// Function SIGNATURES are inferred from naming conventions and the legacy
// Logitech Gaming SDK (LogitechSteeringWheelLib.h) surface. They will
// almost certainly need correction once we have either:
//   - Official Logitech-supplied headers, or
//   - A confirmed reverse of the DLL's call shape.
//
// Until that lands, the loader resolves these symbols by name and the
// per-signal call sites use them at our best-guess shape. Wrong-shape
// calls will crash; we keep the loader's IsActive() gate strict so
// nothing fires until we explicitly enable it.

#include <windows.h>
#include <cstdint>

struct IDirectInputDevice8A;
struct IDirectInputDevice8W;

namespace gwheel::trueforce::api
{
    // ---- Lifecycle ---------------------------------------------------------

    using PFN_dllOpen   = bool (__cdecl*)();
    using PFN_dllClose  = bool (__cdecl*)();

    // Returns a wheel handle (>= 0) or a negative error code.
    using PFN_logiWheelOpenByDirectInputW =
        int (__cdecl*)(IDirectInputDevice8W* device);
    using PFN_logiWheelClose = bool (__cdecl*)(int wheelHandle);

    using PFN_logiWheelSdkHasControl = bool (__cdecl*)(int wheelHandle);
    using PFN_logiWheelGetVersion              = int (__cdecl*)();
    using PFN_logiWheelGetCoreLibraryVersion   = int (__cdecl*)();

    // ---- Capability probes -------------------------------------------------

    using PFN_logiTrueForceAvailable = bool (__cdecl*)();
    using PFN_logiTrueForceSupported = bool (__cdecl*)(int wheelHandle);
    using PFN_logiTrueForceSupportedByDirectInputW =
        bool (__cdecl*)(IDirectInputDevice8W* device);
    using PFN_logiWheelSupportedByDirectInputW =
        bool (__cdecl*)(IDirectInputDevice8W* device);

    // ---- Force mode (overall enable / clamp) -------------------------------
    // Values for forceMode are TBD; legacy SDK had on/off semantics.

    using PFN_logiWheelGetForceMode = int  (__cdecl*)(int wheelHandle);
    using PFN_logiWheelSetForceMode = bool (__cdecl*)(int wheelHandle, int forceMode);

    // ---- Operating range ---------------------------------------------------

    using PFN_logiWheelSetOperatingRangeDegrees =
        bool (__cdecl*)(int wheelHandle, double degrees);
    using PFN_logiWheelGetOperatingRangeDegrees =
        bool (__cdecl*)(int wheelHandle, double* outDegrees);

    // ---- Wheel state -------------------------------------------------------

    using PFN_logiTrueForceGetAngleDegrees =
        bool (__cdecl*)(int wheelHandle, double* outDegrees);
    using PFN_logiTrueForceGetAngularVelocityDegrees =
        bool (__cdecl*)(int wheelHandle, double* outDegPerSec);

    // ---- Pause / sync ------------------------------------------------------

    using PFN_logiTrueForcePause     = bool (__cdecl*)(int wheelHandle);
    using PFN_logiTrueForceResume    = bool (__cdecl*)(int wheelHandle);
    using PFN_logiTrueForceIsPaused  = bool (__cdecl*)(int wheelHandle);
    using PFN_logiTrueForceSync      = bool (__cdecl*)(int wheelHandle);
    using PFN_logiTrueForceGetHapticRate =
        bool (__cdecl*)(int wheelHandle, int* outHz);

    // ---- Kinesthetic (KF) channel ------------------------------------------

    using PFN_logiTrueForceSetTorqueKF =
        bool (__cdecl*)(int wheelHandle, double torqueNm);
    using PFN_logiTrueForceClearKF =
        bool (__cdecl*)(int wheelHandle);
    using PFN_logiTrueForceSetGainKF =
        bool (__cdecl*)(int wheelHandle, double gain01);
    using PFN_logiTrueForceGetMaxPeakTorqueKF =
        bool (__cdecl*)(int wheelHandle, double* outNm);
    using PFN_logiTrueForceGetMaxContinuousTorqueKF =
        bool (__cdecl*)(int wheelHandle, double* outNm);

    // KFPiecewise lets you push a multi-segment shaping curve in one go;
    // exact argument shape is TBD.
    using PFN_logiTrueForceSetTorqueKFPiecewise = bool (__cdecl*)(
        int wheelHandle,
        const double* segments,
        int segmentCount);

    // ---- TrueForce (TF) / vibrotactile channel -----------------------------
    //
    // TWO ways to feed TF:
    //   * SetStreamTF: hand the runtime a buffer it consumes at the wheel's
    //     haptic rate (queryable via logiTrueForceGetHapticRate).
    //   * SetTorqueTF{type}: write a single sample at the current rate.
    //
    // The {type} suffix variants exist so callers can hand the SDK their
    // native sample format without conversion (float is the canonical one
    // for our use; int16 is useful if we end up tapping audio_monitor's
    // existing PCM capture).

    using PFN_logiTrueForceSetTorqueTFfloat =
        bool (__cdecl*)(int wheelHandle, float sample);
    using PFN_logiTrueForceSetTorqueTFdouble =
        bool (__cdecl*)(int wheelHandle, double sample);
    using PFN_logiTrueForceSetTorqueTFint16 =
        bool (__cdecl*)(int wheelHandle, int16_t sample);

    // The exact argument list for SetStreamTF is TBD. Best-guess shape
    // mirrors common audio-stream APIs: pointer + frame count + sample
    // rate, with format implied by the {type} variant. We may need a
    // separate SetStreamTF variant per sample type; the export table
    // only shows one, so it likely takes a format enum.
    using PFN_logiTrueForceSetStreamTF = bool (__cdecl*)(
        int          wheelHandle,
        const void*  samples,
        int          frameCount,
        int          sampleRateHz,
        int          sampleFormat /* TBD enum */);

    using PFN_logiTrueForceClearTF = bool (__cdecl*)(int wheelHandle);
    using PFN_logiTrueForceSetGainTF =
        bool (__cdecl*)(int wheelHandle, double gain01);
    using PFN_logiTrueForceGetTorqueTFRateBounds =
        bool (__cdecl*)(int wheelHandle, int* outMinHz, int* outMaxHz);

    // ---- Damping -----------------------------------------------------------

    using PFN_logiTrueForceSetDamping =
        bool (__cdecl*)(int wheelHandle, double coeff01);
    using PFN_logiTrueForceGetDamping =
        bool (__cdecl*)(int wheelHandle, double* outCoeff01);
    using PFN_logiTrueForceGetDampingMax =
        bool (__cdecl*)(int wheelHandle, double* outMax);

    // ---- LEDs --------------------------------------------------------------

    using PFN_logiWheelPlayLeds = bool (__cdecl*)(
        int wheelHandle,
        float currentRpm,
        float rpmFirstLedTurnsOn,
        float rpmRedLine);
    using PFN_logiWheelSetRpmLeds = bool (__cdecl*)(int wheelHandle, int ledMask);

    // ---- Bound vtable ------------------------------------------------------

    struct Vtable
    {
        // Lifecycle
        PFN_dllOpen                                dllOpen                                = nullptr;
        PFN_dllClose                               dllClose                               = nullptr;
        PFN_logiWheelOpenByDirectInputW            logiWheelOpenByDirectInputW            = nullptr;
        PFN_logiWheelClose                         logiWheelClose                         = nullptr;
        PFN_logiWheelSdkHasControl                 logiWheelSdkHasControl                 = nullptr;
        PFN_logiWheelGetVersion                    logiWheelGetVersion                    = nullptr;
        PFN_logiWheelGetCoreLibraryVersion         logiWheelGetCoreLibraryVersion         = nullptr;

        // Capability
        PFN_logiTrueForceAvailable                 logiTrueForceAvailable                 = nullptr;
        PFN_logiTrueForceSupported                 logiTrueForceSupported                 = nullptr;
        PFN_logiTrueForceSupportedByDirectInputW   logiTrueForceSupportedByDirectInputW   = nullptr;
        PFN_logiWheelSupportedByDirectInputW       logiWheelSupportedByDirectInputW       = nullptr;

        // Force mode
        PFN_logiWheelGetForceMode                  logiWheelGetForceMode                  = nullptr;
        PFN_logiWheelSetForceMode                  logiWheelSetForceMode                  = nullptr;

        // Range
        PFN_logiWheelSetOperatingRangeDegrees      logiWheelSetOperatingRangeDegrees      = nullptr;
        PFN_logiWheelGetOperatingRangeDegrees      logiWheelGetOperatingRangeDegrees      = nullptr;

        // Wheel state
        PFN_logiTrueForceGetAngleDegrees           logiTrueForceGetAngleDegrees           = nullptr;
        PFN_logiTrueForceGetAngularVelocityDegrees logiTrueForceGetAngularVelocityDegrees = nullptr;

        // Pause/sync
        PFN_logiTrueForcePause                     logiTrueForcePause                     = nullptr;
        PFN_logiTrueForceResume                    logiTrueForceResume                    = nullptr;
        PFN_logiTrueForceIsPaused                  logiTrueForceIsPaused                  = nullptr;
        PFN_logiTrueForceSync                      logiTrueForceSync                      = nullptr;
        PFN_logiTrueForceGetHapticRate             logiTrueForceGetHapticRate             = nullptr;

        // KF
        PFN_logiTrueForceSetTorqueKF               logiTrueForceSetTorqueKF               = nullptr;
        PFN_logiTrueForceClearKF                   logiTrueForceClearKF                   = nullptr;
        PFN_logiTrueForceSetGainKF                 logiTrueForceSetGainKF                 = nullptr;
        PFN_logiTrueForceGetMaxPeakTorqueKF        logiTrueForceGetMaxPeakTorqueKF        = nullptr;
        PFN_logiTrueForceGetMaxContinuousTorqueKF  logiTrueForceGetMaxContinuousTorqueKF  = nullptr;
        PFN_logiTrueForceSetTorqueKFPiecewise      logiTrueForceSetTorqueKFPiecewise      = nullptr;

        // TF
        PFN_logiTrueForceSetTorqueTFfloat          logiTrueForceSetTorqueTFfloat          = nullptr;
        PFN_logiTrueForceSetTorqueTFdouble         logiTrueForceSetTorqueTFdouble         = nullptr;
        PFN_logiTrueForceSetTorqueTFint16          logiTrueForceSetTorqueTFint16          = nullptr;
        PFN_logiTrueForceSetStreamTF               logiTrueForceSetStreamTF               = nullptr;
        PFN_logiTrueForceClearTF                   logiTrueForceClearTF                   = nullptr;
        PFN_logiTrueForceSetGainTF                 logiTrueForceSetGainTF                 = nullptr;
        PFN_logiTrueForceGetTorqueTFRateBounds     logiTrueForceGetTorqueTFRateBounds     = nullptr;

        // Damping
        PFN_logiTrueForceSetDamping                logiTrueForceSetDamping                = nullptr;
        PFN_logiTrueForceGetDamping                logiTrueForceGetDamping                = nullptr;
        PFN_logiTrueForceGetDampingMax             logiTrueForceGetDampingMax             = nullptr;

        // LEDs
        PFN_logiWheelPlayLeds                      logiWheelPlayLeds                      = nullptr;
        PFN_logiWheelSetRpmLeds                    logiWheelSetRpmLeds                    = nullptr;
    };
}
