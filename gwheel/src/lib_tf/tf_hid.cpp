#include "tf_hid.h"
#include "tf_protocol.h"

#include <windows.h>
#include <winioctl.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <usbiodef.h>
#include <usbioctl.h>

#include <algorithm>
#include <cwctype>
#include <cstring>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace gwheel::lib_tf
{
    namespace
    {
        // Pull "&mi_02" or "&col01" out of a HID device path. Windows HID
        // exposes USB interface number through the bus-relations device
        // instance ID, which Setupapi stitches into the symbolic-link path.
        // Not every device has both — composite devices have mi_xx, single-
        // interface devices skip it, and col0N appears on multi-collection
        // top-level descriptors.
        int ParseHexFieldAfter(const std::wstring& path, const wchar_t* tag)
        {
            const std::wstring lc = [&]{
                std::wstring s = path;
                std::transform(s.begin(), s.end(), s.begin(),
                               [](wchar_t c){ return static_cast<wchar_t>(std::towlower(c)); });
                return s;
            }();
            const auto pos = lc.find(tag);
            if (pos == std::wstring::npos) return -1;
            const wchar_t* p = lc.c_str() + pos + std::char_traits<wchar_t>::length(tag);
            int v = 0;
            int digits = 0;
            while (*p && digits < 4) {
                wchar_t c = *p;
                int d;
                if (c >= L'0' && c <= L'9')      d = c - L'0';
                else if (c >= L'a' && c <= L'f') d = 10 + (c - L'a');
                else break;
                v = (v << 4) | d;
                ++p; ++digits;
            }
            return digits == 0 ? -1 : v;
        }
    }

    std::vector<HidWheel> EnumerateAllSupportedWheels()
    {
        std::vector<HidWheel> result;

        GUID hidGuid{};
        HidD_GetHidGuid(&hidGuid);

        HDEVINFO devs = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                              DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devs == INVALID_HANDLE_VALUE) return result;

        SP_DEVICE_INTERFACE_DATA ifd{};
        ifd.cbSize = sizeof(ifd);

        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, nullptr, &hidGuid, i, &ifd); ++i) {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(devs, &ifd, nullptr, 0, &needed, nullptr);
            if (needed == 0) continue;

            std::vector<unsigned char> buf(needed);
            auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifd, detail, needed, nullptr, nullptr))
                continue;

            HANDLE h = CreateFileW(detail->DevicePath,
                                   0, // query attrs only
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;

            HIDD_ATTRIBUTES attrs{};
            attrs.Size = sizeof(attrs);
            const bool got = HidD_GetAttributes(h, &attrs) != FALSE;
            CloseHandle(h);
            if (!got) continue;

            if (attrs.VendorID != kLogitechVid) continue;
            if (!IsSupportedPid(attrs.ProductID)) continue;

            HidWheel w;
            w.vid             = attrs.VendorID;
            w.pid             = attrs.ProductID;
            w.devicePath      = detail->DevicePath;
            w.interfaceNumber = ParseHexFieldAfter(w.devicePath, L"&mi_");
            w.collection      = ParseHexFieldAfter(w.devicePath, L"&col");
            result.push_back(std::move(w));
        }

        SetupDiDestroyDeviceInfoList(devs);
        return result;
    }

    const HidWheel* PickInterface2(const std::vector<HidWheel>& all)
    {
        for (const auto& w : all) {
            if (w.interfaceNumber == kInterfaceTf) return &w;
        }
        return nullptr;
    }

    HANDLE OpenDevice(const std::wstring& path)
    {
        return CreateFileW(path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0,
                           nullptr,
                           OPEN_EXISTING,
                           0,
                           nullptr);
    }

    bool WriteReport(HANDLE h, const uint8_t* data, unsigned len, DWORD* out_bytes_written)
    {
        if (h == INVALID_HANDLE_VALUE || !data || len == 0) return false;
        DWORD wrote = 0;
        const BOOL ok = WriteFile(h, data, len, &wrote, nullptr);
        if (out_bytes_written) *out_bytes_written = wrote;
        return ok && wrote == len;
    }

    // ---- Driver-agnostic USB enumeration ------------------------------------

    namespace
    {
        std::wstring GetDeviceRegProp(HDEVINFO devs, SP_DEVINFO_DATA& info, DWORD prop)
        {
            DWORD type = 0, sz = 0;
            SetupDiGetDeviceRegistryPropertyW(devs, &info, prop, &type, nullptr, 0, &sz);
            if (sz == 0) return {};
            std::wstring out(sz / sizeof(wchar_t), L'\0');
            if (!SetupDiGetDeviceRegistryPropertyW(devs, &info, prop, &type,
                    reinterpret_cast<PBYTE>(out.data()), sz, nullptr)) return {};
            while (!out.empty() && out.back() == L'\0') out.pop_back();
            return out;
        }

        std::wstring GetInstanceId(HDEVINFO, SP_DEVINFO_DATA& info)
        {
            ULONG sz = 0;
            CM_Get_Device_ID_Size(&sz, info.DevInst, 0);
            if (sz == 0) return {};
            std::wstring out(sz + 1, L'\0');
            if (CM_Get_Device_IDW(info.DevInst, out.data(), sz + 1, 0) != CR_SUCCESS) return {};
            while (!out.empty() && out.back() == L'\0') out.pop_back();
            return out;
        }

        // Parse VID and PID out of a USB instance ID like
        // "USB\VID_046D&PID_C26E&MI_02\7&abc&0&0002".
        bool ParseVidPidMi(const std::wstring& id, uint16_t& vid, uint16_t& pid, int& mi)
        {
            mi = -1;
            std::wstring lc(id);
            std::transform(lc.begin(), lc.end(), lc.begin(),
                           [](wchar_t c){ return static_cast<wchar_t>(std::towlower(c)); });
            const auto vpos = lc.find(L"vid_");
            const auto ppos = lc.find(L"pid_");
            if (vpos == std::wstring::npos || ppos == std::wstring::npos) return false;
            vid = static_cast<uint16_t>(wcstoul(lc.c_str() + vpos + 4, nullptr, 16));
            pid = static_cast<uint16_t>(wcstoul(lc.c_str() + ppos + 4, nullptr, 16));
            const auto mipos = lc.find(L"mi_");
            if (mipos != std::wstring::npos)
                mi = static_cast<int>(wcstoul(lc.c_str() + mipos + 3, nullptr, 16));
            return true;
        }

        // Walk every device on the system and filter to USB descendants whose
        // instance ID matches the prefix. `wantInterfaces`=true keeps only
        // composite-interface entries (USB\VID_..&PID_..&MI_..); false keeps
        // only top-level USB devices (no &MI_ in the instance ID).
        std::vector<UsbInterfaceEntry> EnumerateUsbBy(uint16_t vid_filter, bool wantInterfaces)
        {
            std::vector<UsbInterfaceEntry> out;
            HDEVINFO devs = SetupDiGetClassDevsW(nullptr, L"USB",
                                                 nullptr,
                                                 DIGCF_PRESENT | DIGCF_ALLCLASSES);
            if (devs == INVALID_HANDLE_VALUE) return out;

            SP_DEVINFO_DATA info{};
            info.cbSize = sizeof(info);
            for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &info); ++i) {
                std::wstring id = GetInstanceId(devs, info);
                if (id.empty()) continue;
                if (id.size() < 4) continue;
                if (id[0] != L'U' && id[0] != L'u') continue;
                if (_wcsnicmp(id.c_str(), L"USB\\", 4) != 0) continue;

                uint16_t vid = 0, pid = 0;
                int mi = -1;
                if (!ParseVidPidMi(id, vid, pid, mi)) continue;
                if (vid_filter != 0 && vid != vid_filter) continue;

                const bool isInterface = (mi >= 0);
                if (wantInterfaces != isInterface) continue;

                UsbInterfaceEntry e;
                e.vid             = vid;
                e.pid             = pid;
                e.interfaceNumber = mi;
                e.instanceId      = id;
                e.service         = GetDeviceRegProp(devs, info, SPDRP_SERVICE);
                e.friendlyName    = GetDeviceRegProp(devs, info, SPDRP_FRIENDLYNAME);
                if (e.friendlyName.empty())
                    e.friendlyName = GetDeviceRegProp(devs, info, SPDRP_DEVICEDESC);
                out.push_back(std::move(e));
            }
            SetupDiDestroyDeviceInfoList(devs);
            return out;
        }
    }

    std::vector<UsbInterfaceEntry> EnumerateUsbInterfaces(uint16_t vid_filter)
    {
        return EnumerateUsbBy(vid_filter, /*wantInterfaces=*/true);
    }

    std::vector<UsbInterfaceEntry> EnumerateUsbDevices(uint16_t vid_filter)
    {
        return EnumerateUsbBy(vid_filter, /*wantInterfaces=*/false);
    }

    // ---- Configuration descriptor read via parent hub -----------------------

    namespace
    {
        std::wstring GetParentInstanceId(DEVINST devInst)
        {
            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, devInst, 0) != CR_SUCCESS) return {};
            ULONG sz = 0;
            CM_Get_Device_ID_Size(&sz, parent, 0);
            if (sz == 0) return {};
            std::wstring out(sz + 1, L'\0');
            if (CM_Get_Device_IDW(parent, out.data(), sz + 1, 0) != CR_SUCCESS) return {};
            while (!out.empty() && out.back() == L'\0') out.pop_back();
            return out;
        }

        // Fetch the device's address (port number) on its parent hub.
        ULONG GetDeviceAddressOnParent(DEVINST devInst)
        {
            ULONG addr = 0;
            ULONG sz = sizeof(addr);
            ULONG type = 0;
            if (CM_Get_DevNode_Registry_PropertyW(devInst, CM_DRP_ADDRESS, &type,
                    &addr, &sz, 0) != CR_SUCCESS) return 0;
            return addr;
        }

        // Open the parent hub's device interface so we can issue
        // USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION ioctls.
        HANDLE OpenParentHubInterface(const std::wstring& hubInstanceId)
        {
            HDEVINFO devs = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_HUB,
                                                 nullptr, nullptr,
                                                 DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (devs == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
            HANDLE result = INVALID_HANDLE_VALUE;
            SP_DEVINFO_DATA info{};
            info.cbSize = sizeof(info);
            for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &info); ++i) {
                if (GetInstanceId(devs, info) != hubInstanceId) continue;
                SP_DEVICE_INTERFACE_DATA ifd{};
                ifd.cbSize = sizeof(ifd);
                if (!SetupDiEnumDeviceInterfaces(devs, &info,
                        &GUID_DEVINTERFACE_USB_HUB, 0, &ifd)) break;
                DWORD needed = 0;
                SetupDiGetDeviceInterfaceDetailW(devs, &ifd, nullptr, 0, &needed, nullptr);
                if (needed == 0) break;
                std::vector<unsigned char> buf(needed);
                auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifd, detail, needed, nullptr, nullptr)) break;
                result = CreateFileW(detail->DevicePath,
                                     GENERIC_WRITE, FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
                break;
            }
            SetupDiDestroyDeviceInfoList(devs);
            return result;
        }
    }

    std::vector<unsigned char> ReadConfigDescriptor(const std::wstring& deviceInstanceId)
    {
        std::vector<unsigned char> out;

        // Resolve devInst from instance ID.
        DEVINST devInst = 0;
        if (CM_Locate_DevNodeW(&devInst, const_cast<wchar_t*>(deviceInstanceId.c_str()),
                CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return out;

        const std::wstring hubId = GetParentInstanceId(devInst);
        if (hubId.empty()) return out;
        const ULONG portIndex = GetDeviceAddressOnParent(devInst);
        if (portIndex == 0) return out;

        HANDLE hub = OpenParentHubInterface(hubId);
        if (hub == INVALID_HANDLE_VALUE) return out;

        // First request: 9-byte config descriptor header to learn wTotalLength.
        constexpr int kReqBufSz = sizeof(USB_DESCRIPTOR_REQUEST) + 4096;
        std::vector<unsigned char> req(kReqBufSz, 0);
        auto* hdr = reinterpret_cast<USB_DESCRIPTOR_REQUEST*>(req.data());
        hdr->ConnectionIndex = portIndex;
        hdr->SetupPacket.bmRequest = 0x80;
        hdr->SetupPacket.bRequest  = USB_REQUEST_GET_DESCRIPTOR;
        hdr->SetupPacket.wValue    = (USB_CONFIGURATION_DESCRIPTOR_TYPE << 8) | 0;
        hdr->SetupPacket.wIndex    = 0;
        hdr->SetupPacket.wLength   = sizeof(USB_CONFIGURATION_DESCRIPTOR);

        DWORD bytes = 0;
        if (!DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
                req.data(), sizeof(USB_DESCRIPTOR_REQUEST) + sizeof(USB_CONFIGURATION_DESCRIPTOR),
                req.data(), sizeof(USB_DESCRIPTOR_REQUEST) + sizeof(USB_CONFIGURATION_DESCRIPTOR),
                &bytes, nullptr)) {
            CloseHandle(hub);
            return out;
        }
        const auto* cfg = reinterpret_cast<USB_CONFIGURATION_DESCRIPTOR*>(hdr + 1);
        const USHORT total = cfg->wTotalLength;
        if (total == 0 || total > 4096) { CloseHandle(hub); return out; }

        // Second request: full config descriptor (includes interface + endpoint descriptors).
        std::memset(req.data(), 0, kReqBufSz);
        hdr = reinterpret_cast<USB_DESCRIPTOR_REQUEST*>(req.data());
        hdr->ConnectionIndex = portIndex;
        hdr->SetupPacket.bmRequest = 0x80;
        hdr->SetupPacket.bRequest  = USB_REQUEST_GET_DESCRIPTOR;
        hdr->SetupPacket.wValue    = (USB_CONFIGURATION_DESCRIPTOR_TYPE << 8) | 0;
        hdr->SetupPacket.wIndex    = 0;
        hdr->SetupPacket.wLength   = total;

        if (!DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
                req.data(), sizeof(USB_DESCRIPTOR_REQUEST) + total,
                req.data(), sizeof(USB_DESCRIPTOR_REQUEST) + total,
                &bytes, nullptr)) {
            CloseHandle(hub);
            return out;
        }
        CloseHandle(hub);

        out.assign(req.data() + sizeof(USB_DESCRIPTOR_REQUEST),
                   req.data() + sizeof(USB_DESCRIPTOR_REQUEST) + total);
        return out;
    }
}
