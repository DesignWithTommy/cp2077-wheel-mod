#pragma once

// TrueForce wire-protocol constants and packet helpers.
//
// Protocol facts documented at
//   github.com/mescon/logitech-rs50-linux-driver/blob/master/docs/TRUEFORCE_PROTOCOL.md
// rederived independently here. Behavioural summary:
//   - 64-byte HID reports on USB interface 2, report ID 0x01, endpoint 0x03 OUT.
//   - Common header: byte[0]=0x01, [1..3]=0, [4]=command type, [5]=u8 sequence
//     counter (rolling, shared across all command types), [6..]=type-specific
//     payload.
//   - The TF audio stream is type 0x01: a 13-slot rolling u16 LE window of
//     offset-binary samples (centre 0x8000, full-left 0x0000, full-right
//     0xFFFF), each slot duplicated for the wheel's "stereo" payload, with
//     the most-recent sample also duplicated at offset 6..9 as a preamble.
//     New samples per packet = 4; packet rate ~250 Hz; effective audio
//     rate ~1000 Hz.
//   - Bytes 10 (0x04 = LOGITF_TF_NEW) and 11 (0x0d, an unknown but constant
//     flag) are observed-constant in every G HUB capture.

#include <cstdint>
#include <cstring>

namespace gwheel::lib_tf
{
    // ---- USB identification --------------------------------------------------

    inline constexpr uint16_t kLogitechVid = 0x046D;

    // Wheels with verified TrueForce streaming on interface 2.
    //   c266 / c26e : G923, NOT verified upstream — the experiment.
    //   c276        : RS50 Direct Drive Base.
    //   c268 / c272 : G PRO Racing Wheel (PS / Xbox).
    inline constexpr uint16_t kPidG923Ps    = 0xC266;
    inline constexpr uint16_t kPidG923Xbox  = 0xC26E;
    inline constexpr uint16_t kPidRs50      = 0xC276;
    inline constexpr uint16_t kPidGProPs    = 0xC268;
    inline constexpr uint16_t kPidGProXbox  = 0xC272;

    // The probe walks every recognised PID; production code (a future
    // backend) will pick exactly one based on whichever wheel attaches.
    inline constexpr uint16_t kSupportedPids[] = {
        kPidG923Ps, kPidG923Xbox,
        kPidRs50,
        kPidGProPs, kPidGProXbox,
    };

    inline const char* PidLabel(uint16_t pid)
    {
        switch (pid) {
            case kPidG923Ps:   return "G923 (PS)";
            case kPidG923Xbox: return "G923 (Xbox)";
            case kPidRs50:     return "RS50";
            case kPidGProPs:   return "G PRO (PS)";
            case kPidGProXbox: return "G PRO (Xbox)";
            default:           return "Unknown";
        }
    }

    inline bool IsSupportedPid(uint16_t pid)
    {
        for (uint16_t p : kSupportedPids) if (p == pid) return true;
        return false;
    }

    // ---- HID transport constants --------------------------------------------

    inline constexpr int      kInterfaceTf       = 2;
    inline constexpr unsigned kReportLen         = 64;
    inline constexpr uint8_t  kReportId          = 0x01;
    inline constexpr int      kPktHz             = 250;
    inline constexpr int      kNewSamplesPerPkt  = 4;
    inline constexpr int      kWindowSlots       = 13;
    inline constexpr uint16_t kSampleCenter      = 0x8000;

    // Offsets inside the 64-byte report.
    inline constexpr int kOffReportId      = 0;   // 0x01
    inline constexpr int kOffCmdType       = 4;   // 0x01 = sample stream, etc.
    inline constexpr int kOffSeq           = 5;
    inline constexpr int kOffPreambleLo    = 6;   // bytes 6..9: newest sample, duplicated
    inline constexpr int kOffNewCount      = 10;  // 0x04
    inline constexpr int kOffStreamFlag    = 11;  // 0x0d
    inline constexpr int kOffWindowStart   = 12;  // 13 slots * 4 bytes = 52 bytes -> ends at 63

    static_assert(kOffWindowStart + kWindowSlots * 4 == 64);

    // Type bytes for each command observed in the protocol document.
    inline constexpr uint8_t kCmdSample      = 0x01;
    inline constexpr uint8_t kCmdStart       = 0x03;
    inline constexpr uint8_t kCmdStop        = 0x04;
    inline constexpr uint8_t kCmdParam       = 0x05;
    inline constexpr uint8_t kCmdSlot        = 0x06;
    inline constexpr uint8_t kCmdHandshake   = 0x07;
    inline constexpr uint8_t kCmdRuntime     = 0x09;
    inline constexpr uint8_t kCmdFrequency   = 0x0E;

    // ---- Sample format conversion -------------------------------------------

    inline uint16_t S16ToWire(int16_t s)
    {
        return static_cast<uint16_t>(static_cast<int32_t>(s) + kSampleCenter);
    }

    inline uint16_t FloatToWire(float v)
    {
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        return static_cast<uint16_t>(static_cast<int>(v * 32767.0f) + kSampleCenter);
    }

    // ---- Sample packet builder ----------------------------------------------
    //
    // `out` must be 64 bytes. `window` is 13 wire-format samples ordered
    // oldest at [0], newest at [kWindowSlots-1]. Sequence is the raw byte
    // value to write to offset 5; caller manages the rolling counter.

    inline void BuildSamplePacket(uint8_t out[kReportLen],
                                  uint8_t seq,
                                  const uint16_t window[kWindowSlots])
    {
        std::memset(out, 0, kReportLen);
        out[kOffReportId]   = kReportId;
        out[kOffCmdType]    = kCmdSample;
        out[kOffSeq]        = seq;

        const uint16_t newest = window[kWindowSlots - 1];
        out[kOffPreambleLo + 0] = static_cast<uint8_t>(newest & 0xff);
        out[kOffPreambleLo + 1] = static_cast<uint8_t>(newest >> 8);
        out[kOffPreambleLo + 2] = static_cast<uint8_t>(newest & 0xff);
        out[kOffPreambleLo + 3] = static_cast<uint8_t>(newest >> 8);

        out[kOffNewCount]   = static_cast<uint8_t>(kNewSamplesPerPkt);
        out[kOffStreamFlag] = 0x0d;

        for (int i = 0; i < kWindowSlots; ++i) {
            uint8_t* p = out + kOffWindowStart + i * 4;
            const uint16_t v = window[i];
            p[0] = static_cast<uint8_t>(v & 0xff);
            p[1] = static_cast<uint8_t>(v >> 8);
            p[2] = static_cast<uint8_t>(v & 0xff);
            p[3] = static_cast<uint8_t>(v >> 8);
        }
    }
}
