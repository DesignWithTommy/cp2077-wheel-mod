// libtrueforce probe.
//
// Bypasses trueforce_sdk_x64.dll and lghub_agent.exe entirely. Talks to
// the wheel's USB interface 2 directly via Windows HID (CreateFile +
// WriteFile on the HID device path).
//
// Stages, runnable independently from the command line:
//   list        Enumerate every Logitech wheel HID interface + iface number.
//   usb-tree    Walk USB devices via GUID_DEVCLASS_USB and GUID_DEVCLASS_USBDEVICE
//               regardless of class driver. Confirms whether iface 2 is
//               actually missing vs. just claimed by a non-HID driver.
//   usb-descr   Pull the raw USB configuration descriptor from the parent
//               hub for any matching wheel. Lists every interface the
//               firmware advertises by bInterfaceNumber.
//   init        Open interface 2, send the 68-packet init sequence twice
//               with 2 ms inter-packet delay, close. Watch the wheel for
//               any LED change or motor twitch.
//   sine        Run init, then stream a 100 Hz sine at 10% amplitude for
//               1 second. Listen / feel for a faint hum.
//   dc          Run init, then stream a 5% constant DC offset for 1 second.
//               If the wheel responds, it should pull steadily to one side.
//
// Stages are ordered by escalating risk: list is safe, init is benign
// (the wheel either ignores it or LEDs flash / sequence-error response),
// sine pushes a low-amplitude vibration, dc commands a sustained torque.
//
// First-time test order: list -> init -> sine -> dc. Bail at the first
// stage where the wheel reacts unexpectedly (does not center after, runs
// hot, etc.) and unplug to reset.

#include "tf_hid.h"
#include "tf_init_packets.h"
#include "tf_protocol.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace gwheel::lib_tf;
namespace ch = std::chrono;

namespace
{
    void PrintWheel(const HidWheel& w, int idx)
    {
        char path[1024] = {};
        WideCharToMultiByte(CP_UTF8, 0, w.devicePath.c_str(), -1,
                            path, sizeof(path), nullptr, nullptr);
        std::printf("  [%d] vid=%04x pid=%04x mi=%d col=%d  %s\n",
                    idx, w.vid, w.pid,
                    w.interfaceNumber, w.collection,
                    PidLabel(w.pid));
        std::printf("       path: %s\n", path);
    }

    HANDLE OpenIface2OrFail()
    {
        const auto all = EnumerateAllSupportedWheels();
        if (all.empty()) {
            std::fprintf(stderr, "No supported Logitech wheels found.\n");
            return INVALID_HANDLE_VALUE;
        }
        const HidWheel* iface2 = PickInterface2(all);
        if (!iface2) {
            std::fprintf(stderr,
                "No interface-2 HID node for any attached wheel. "
                "Either the firmware does not expose TrueForce on this PC, or "
                "another process owns it exclusively.\n");
            std::fprintf(stderr, "Detected %zu non-iface2 entries:\n", all.size());
            int i = 0;
            for (const auto& w : all) PrintWheel(w, i++);
            return INVALID_HANDLE_VALUE;
        }
        std::printf("Opening %s pid=%04x interface=%d\n",
                    PidLabel(iface2->pid), iface2->pid, iface2->interfaceNumber);
        HANDLE h = OpenDevice(iface2->devicePath);
        if (h == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "CreateFileW failed, GetLastError=%lu\n", GetLastError());
            return INVALID_HANDLE_VALUE;
        }
        return h;
    }

    bool RunInit(HANDLE h)
    {
        std::printf("Sending 68-packet init sequence (pass 1 of 2)...\n");
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < kTfInitPacketCount; ++i) {
                uint8_t pkt[kTfInitPacketLen];
                std::memcpy(pkt, kTfInitPackets[i], kTfInitPacketLen);
                pkt[kTfInitSeqOffset] = static_cast<uint8_t>((i + 1) & 0xff);

                DWORD wrote = 0;
                if (!WriteReport(h, pkt, kTfInitPacketLen, &wrote)) {
                    std::fprintf(stderr,
                        "WriteFile failed at pass=%d pkt=#%d (type=0x%02x), "
                        "GetLastError=%lu, wrote=%lu\n",
                        pass + 1, i + 1, kTfInitPackets[i][kOffCmdType],
                        GetLastError(), wrote);
                    return false;
                }
                std::this_thread::sleep_for(ch::microseconds(2000));
            }
            std::printf("  pass %d done\n", pass + 1);
        }
        std::printf("Init complete.\n");
        return true;
    }

    void StreamForOneSecond(HANDLE h, const char* label,
                            float (*sample_fn)(float t_secs))
    {
        std::printf("Streaming %s for 1.0 s at %d Hz packet rate...\n", label, kPktHz);

        uint16_t window[kWindowSlots];
        for (int i = 0; i < kWindowSlots; ++i) window[i] = kSampleCenter;

        uint8_t seq = static_cast<uint8_t>(kTfInitPacketCount + 1);
        const auto start = ch::steady_clock::now();
        const auto deadline = start + ch::seconds(1);
        const auto period = ch::nanoseconds(1'000'000'000LL / kPktHz);
        auto next = start + period;

        // ~1 kHz effective sample rate -> 4 new samples per packet.
        // Time per sample = 1.0 / (kPktHz * kNewSamplesPerPkt).
        const float sample_dt = 1.0f / static_cast<float>(kPktHz * kNewSamplesPerPkt);
        float t = 0.0f;

        unsigned long packets = 0;
        unsigned long write_failures = 0;

        while (ch::steady_clock::now() < deadline) {
            // Shift window left by 4 and append 4 new samples.
            for (int i = 0; i < kWindowSlots - kNewSamplesPerPkt; ++i)
                window[i] = window[i + kNewSamplesPerPkt];
            for (int j = 0; j < kNewSamplesPerPkt; ++j) {
                const float v = sample_fn(t);
                window[kWindowSlots - kNewSamplesPerPkt + j] = FloatToWire(v);
                t += sample_dt;
            }

            uint8_t pkt[kReportLen];
            BuildSamplePacket(pkt, seq, window);
            DWORD wrote = 0;
            if (!WriteReport(h, pkt, kReportLen, &wrote)) ++write_failures;
            ++seq;
            ++packets;

            std::this_thread::sleep_until(next);
            next += period;
        }

        // Send one type-0x04 (stop) so the wheel knows the stream ended,
        // then leave the device alone.
        uint8_t stop[kReportLen] = {};
        stop[kOffReportId] = kReportId;
        stop[kOffCmdType]  = kCmdStop;
        stop[kOffSeq]      = seq;
        WriteReport(h, stop, kReportLen);

        std::printf("Stream done. packets=%lu write_failures=%lu\n", packets, write_failures);
    }

    float SampleSine100Hz10pct(float t)
    {
        constexpr float freq = 100.0f;
        constexpr float amp  = 0.10f;
        return amp * std::sin(2.0f * 3.14159265358979323846f * freq * t);
    }

    float SampleDcRight5pct(float)
    {
        return 0.05f;
    }
}

namespace
{
    void PrintUsbEntry(const UsbInterfaceEntry& e, int idx)
    {
        char id[1024] = {};
        char svc[256] = {};
        char fn[512]  = {};
        WideCharToMultiByte(CP_UTF8, 0, e.instanceId.c_str(), -1, id, sizeof(id), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, e.service.c_str(),    -1, svc, sizeof(svc), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, e.friendlyName.c_str(),-1, fn,  sizeof(fn),  nullptr, nullptr);
        std::printf("  [%d] vid=%04x pid=%04x mi=%d  service=%s\n",
                    idx, e.vid, e.pid, e.interfaceNumber, svc);
        if (fn[0]) std::printf("       name: %s\n", fn);
        std::printf("       inst: %s\n", id);
    }

    void DumpDescriptor(const std::vector<unsigned char>& d)
    {
        if (d.size() < 9) {
            std::printf("  (descriptor too short or unreadable)\n");
            return;
        }
        const unsigned total = static_cast<unsigned>(d.size());
        std::printf("  configuration descriptor: %u bytes total\n", total);
        unsigned off = 0;
        int ifaceCount = 0;
        while (off + 2 <= total) {
            const unsigned bLength       = d[off];
            const unsigned bDescriptorType = d[off + 1];
            if (bLength == 0 || off + bLength > total) break;
            switch (bDescriptorType) {
                case 0x02: { // CONFIGURATION
                    const unsigned wTotal = d[off+2] | (d[off+3] << 8);
                    const unsigned numIf  = d[off+4];
                    std::printf("  CONFIG  bNumInterfaces=%u wTotalLength=%u\n", numIf, wTotal);
                    break;
                }
                case 0x04: { // INTERFACE
                    const unsigned bInterfaceNumber = d[off+2];
                    const unsigned bAlt             = d[off+3];
                    const unsigned bNumEP           = d[off+4];
                    const unsigned bClass           = d[off+5];
                    const unsigned bSubClass        = d[off+6];
                    const unsigned bProto           = d[off+7];
                    std::printf("  IFACE   bInterfaceNumber=%u alt=%u endpoints=%u class=%02x sub=%02x proto=%02x\n",
                                bInterfaceNumber, bAlt, bNumEP, bClass, bSubClass, bProto);
                    ++ifaceCount;
                    break;
                }
                case 0x05: { // ENDPOINT
                    const unsigned bEndpointAddress = d[off+2];
                    const unsigned bmAttributes     = d[off+3];
                    const unsigned wMaxPacketSize   = d[off+4] | (d[off+5] << 8);
                    const unsigned bInterval        = d[off+6];
                    const char*    dir              = (bEndpointAddress & 0x80) ? "IN " : "OUT";
                    const unsigned epnum            = bEndpointAddress & 0x0f;
                    const char*    type;
                    switch (bmAttributes & 0x03) {
                        case 0: type = "control";     break;
                        case 1: type = "isochronous"; break;
                        case 2: type = "bulk";        break;
                        default: type = "interrupt";  break;
                    }
                    std::printf("  EP        ep%u %s  %s  wMaxPacketSize=%u  bInterval=%u\n",
                                epnum, dir, type, wMaxPacketSize, bInterval);
                    break;
                }
                default:
                    std::printf("  desc    bLength=%u bDescriptorType=0x%02x\n", bLength, bDescriptorType);
                    break;
            }
            off += bLength;
        }
        std::printf("  parsed %d INTERFACE descriptors\n", ifaceCount);
    }
}

int main(int argc, char** argv)
{
    std::printf("libtrueforce_probe (clean-room G923 experiment)\n");

    const std::string stage = (argc >= 2) ? argv[1] : "list";

    if (stage == "list") {
        const auto all = EnumerateAllSupportedWheels();
        if (all.empty()) {
            std::printf("No supported Logitech wheels found on this PC.\n");
            return 0;
        }
        std::printf("Detected %zu HID interfaces:\n", all.size());
        int i = 0;
        for (const auto& w : all) PrintWheel(w, i++);
        return 0;
    }

    if (stage == "usb-tree") {
        std::printf("USB interface enumeration (GUID_DEVCLASS_USB):\n");
        const auto ifaces = EnumerateUsbInterfaces(kLogitechVid);
        if (ifaces.empty()) {
            std::printf("  (no Logitech USB interfaces found)\n");
        } else {
            int i = 0;
            for (const auto& e : ifaces) PrintUsbEntry(e, i++);
        }
        std::printf("\nUSB device enumeration (GUID_DEVCLASS_USBDEVICE):\n");
        const auto devs = EnumerateUsbDevices(kLogitechVid);
        if (devs.empty()) {
            std::printf("  (no Logitech USB devices found)\n");
        } else {
            int i = 0;
            for (const auto& e : devs) PrintUsbEntry(e, i++);
        }
        return 0;
    }

    if (stage == "usb-descr") {
        const auto devs = EnumerateUsbDevices(kLogitechVid);
        if (devs.empty()) {
            std::printf("No Logitech USB devices found.\n");
            return 1;
        }
        for (const auto& e : devs) {
            if (!IsSupportedPid(e.pid)) continue;
            std::printf("Device pid=%04x  %s\n", e.pid, PidLabel(e.pid));
            const auto d = ReadConfigDescriptor(e.instanceId);
            if (d.empty()) {
                std::printf("  (could not read configuration descriptor)\n");
                continue;
            }
            DumpDescriptor(d);
        }
        return 0;
    }

    HANDLE h = OpenIface2OrFail();
    if (h == INVALID_HANDLE_VALUE) return 1;

    int rc = 0;
    if (stage == "init") {
        rc = RunInit(h) ? 0 : 1;
    }
    else if (stage == "sine") {
        if (!RunInit(h)) rc = 1;
        else StreamForOneSecond(h, "100 Hz sine @ 10%", &SampleSine100Hz10pct);
    }
    else if (stage == "dc") {
        if (!RunInit(h)) rc = 1;
        else StreamForOneSecond(h, "DC offset +5% (right)", &SampleDcRight5pct);
    }
    else {
        std::fprintf(stderr,
            "Unknown stage '%s'. Valid: list, init, sine, dc.\n",
            stage.c_str());
        rc = 2;
    }

    CloseHandle(h);
    return rc;
}
