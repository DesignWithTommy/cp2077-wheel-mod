#pragma once

// Windows HID-direct transport for TrueForce. Walks SetupDi to find HID
// devices matching a Logitech wheel VID/PID on USB interface 2, opens
// the device path, and exposes a 64-byte WriteFile primitive.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gwheel::lib_tf
{
    struct HidWheel
    {
        uint16_t     vid             = 0;
        uint16_t     pid             = 0;
        int          interfaceNumber = -1;  // parsed from "&mi_NN"
        int          collection      = -1;  // parsed from "&col0N"; 0 if absent
        std::wstring devicePath;            // \\?\HID#... ready for CreateFileW
    };

    // Enumerate every HID device whose VID matches kLogitechVid and whose
    // PID is in tf_protocol.h's supported list. Multiple entries per
    // physical wheel are expected (separate top-level collections appear
    // as separate HID device interfaces). The caller chooses which one
    // to open.
    std::vector<HidWheel> EnumerateAllSupportedWheels();

    // Pick the entry that targets USB interface 2 (the TrueForce stream
    // endpoint). Returns nullptr if no candidate matches; for diagnostics,
    // EnumerateAllSupportedWheels() returns every entry seen.
    const HidWheel* PickInterface2(const std::vector<HidWheel>& all);

    // Open the device at `path` for read/write, no sharing. Returns
    // INVALID_HANDLE_VALUE on failure; call GetLastError() for diagnostics.
    HANDLE OpenDevice(const std::wstring& path);

    // Write a single 64-byte report. Returns true on success.
    // out_bytes_written is set to the WriteFile result on both paths.
    bool WriteReport(HANDLE h, const uint8_t* data, unsigned len, DWORD* out_bytes_written);

    // Convenience for callers that don't care about the byte count.
    inline bool WriteReport(HANDLE h, const uint8_t* data, unsigned len)
    {
        DWORD ignored = 0;
        return WriteReport(h, data, len, &ignored);
    }

    // ---- Driver-agnostic USB enumeration -----------------------------------
    //
    // GUID_DEVINTERFACE_HID only sees interfaces claimed by hidclass.sys. If
    // a vendor driver (e.g. lghub) claims an interface with WinUSB or its
    // own class driver, that interface is invisible to the HID walk. The
    // calls below iterate the broader USB device set so we can confirm
    // whether interface 2 exists at all on the G923.

    struct UsbInterfaceEntry
    {
        uint16_t     vid             = 0;
        uint16_t     pid             = 0;
        int          interfaceNumber = -1;  // -1 if unknown
        std::wstring instanceId;            // raw setupapi instance ID, e.g. USB\VID_046D&PID_C26E&MI_02\7&...
        std::wstring service;               // class driver name (HIDClass, WinUSB, lghub_*, etc.)
        std::wstring friendlyName;
    };

    // Walk every USB interface visible to setupapi (regardless of driver).
    // Filters to vid==kLogitechVid by default; pass 0 to include all.
    std::vector<UsbInterfaceEntry> EnumerateUsbInterfaces(uint16_t vid_filter);

    // Walk every USB device (top-level, not just interfaces). Useful if a
    // wheel is enumerated as a single non-composite device. Same filter
    // semantics.
    std::vector<UsbInterfaceEntry> EnumerateUsbDevices(uint16_t vid_filter);

    // Pull the USB configuration descriptor for a given device path by
    // walking up to its parent hub and issuing IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION.
    // Returns the raw descriptor bytes (config descriptor + all interface +
    // endpoint descriptors). Empty vector on failure.
    std::vector<unsigned char> ReadConfigDescriptor(const std::wstring& deviceInstanceId);
}
