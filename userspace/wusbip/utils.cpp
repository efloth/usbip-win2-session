/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "utils.h"
#include "resource.h"

#include <libusbip/remote.h>
#include <libusbip/win_socket.h>
#include <libusbip/format_message.h>
#include <libusbip/src/usb_ids.h>
#include <libusbip/src/file_ver.h>

#include <wx/log.h>
#include <wx/translation.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <sddl.h>
#include <Windows.h>
#include <guiddef.h>

#pragma comment(lib, "Cfgmgr32.lib")

 // Define the GUID locally for the GUI
DEFINE_GUID(USBIP_GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
        0xB4030C06, 0xDC5F, 0x4FCC, 0x87, 0xEB, 0xE5, 0x51, 0x5A, 0x09, 0x35, 0xC0);

// Make this static so it stays private to this file
static void ApplySessionIsolationRecursive(DEVINST devInst, ULONG sessionId)
{
        // 1. Stamp the current device node
        CM_Set_DevNode_PropertyW(devInst, &DEVPKEY_Device_SessionId, DEVPROP_TYPE_UINT32, (const PBYTE)&sessionId, sizeof(sessionId), 0);

        // 2. Recursively stamp all children
        DEVINST childInst;
        if (CM_Get_Child(&childInst, devInst, 0) == CR_SUCCESS)
        {
                ApplySessionIsolationRecursive(childInst, sessionId);

                DEVINST siblingInst = childInst;
                while (CM_Get_Sibling(&siblingInst, siblingInst, 0) == CR_SUCCESS)
                {
                        ApplySessionIsolationRecursive(siblingInst, sessionId);
                }
        }
}

// FIX: Added the "usbip::" namespace prefix!
void usbip::IsolateUsbDevicesToCurrentSession()
{
        ULONG sessionId;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) return;

        ULONG listSize = 0;
        if (CM_Get_Device_Interface_List_SizeW(&listSize, (LPGUID)&USBIP_GUID_DEVINTERFACE_USB_HOST_CONTROLLER, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
                return;
        }

        std::wstring buffer(listSize, L'\0');
        if (CM_Get_Device_Interface_ListW((LPGUID)&USBIP_GUID_DEVINTERFACE_USB_HOST_CONTROLLER, NULL, buffer.data(), listSize, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
                return;
        }

        // Parse the double-null-terminated string to check all Virtual Host Controllers
        for (const wchar_t* p = buffer.data(); *p; p += wcslen(p) + 1) {
                DEVPROPTYPE propType;
                wchar_t instId[MAX_DEVICE_ID_LEN];
                ULONG instIdLen = sizeof(instId);

                if (CM_Get_Device_Interface_PropertyW(p, &DEVPKEY_Device_InstanceId, &propType, (PBYTE)instId, &instIdLen, 0) == CR_SUCCESS) {
                        DEVINST devInst;
                        if (CM_Locate_DevNodeW(&devInst, instId, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {

                                ULONG ctrlSessionId = 0;
                                ULONG len = sizeof(ctrlSessionId);

                                // Check if this Host Controller belongs to the user's current session
                                if (CM_Get_DevNode_PropertyW(devInst, &DEVPKEY_Device_SessionId, &propType, (PBYTE)&ctrlSessionId, &len, 0) == CR_SUCCESS) {
                                        if (ctrlSessionId == sessionId) {
                                                // It's ours! Force the Session ID down to the Root Hub and USB devices.
                                                ApplySessionIsolationRecursive(devInst, sessionId);
                                        }
                                }
                        }
                }
        }
}
static_assert(UsbLowSpeed == 0);
static_assert(UsbFullSpeed == 1);
static_assert(UsbHighSpeed == 2);
static_assert(UsbSuperSpeed == 3);

const wchar_t *g_usb_speed_str[] { L"Low", L"Full", L"High", L"Super" }; // indexed by enum USB_DEVICE_SPEED

auto &msgtable_dll = L"resources"; // resource-only DLL that contains RT_MESSAGETABLE

auto& get_resource_module() noexcept
{
        static usbip::HModule mod(LoadLibraryEx(msgtable_dll, nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR));
        return mod;
}

auto get_ids_data()
{
        win::Resource r(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDR_USB_IDS), RT_RCDATA);
        wxASSERT(r);
        return r.str();
}



auto win::get_file_version() -> const FileVersion&
{
        static FileVersion v;
        wxASSERT(v);
        return v;
}

/*
 * Use for libusbip.dll errors only. 
 * @see wxSysErrorMsg
 */
wxString usbip::GetLastErrorMsg(_In_ DWORD msg_id)
{
        auto &mod = get_resource_module();
        return wformat_message(mod.get(), msg_id);
}

auto usbip::get_vhci() -> Handle&
{
        static auto handle = vhci::open();
        return handle;
}

auto usbip::get_ids() -> const UsbIds&
{
        static UsbIds ids(get_ids_data());
        wxASSERT(ids);
        return ids;
}

auto usbip::get_event() -> NullableHandle& 
{
        static NullableHandle evt(CreateEvent(nullptr, true, false, nullptr));
        return evt;
}

bool usbip::init(_Inout_ wxString &err)
{
        wxASSERT(err.empty());

        if (!get_resource_module()) {
                auto ec = GetLastError();
                err = wxString::Format(_("Cannot load '%s.dll'\nError %lu\n%s"), msgtable_dll, err, wformat_message(ec));
                return false;
        }

        static InitWinSock2 ws2;
        if (!ws2) {
                auto ec = GetLastError();
                err = wxString::Format(_("WSAStartup error %lu\n%s"), ec, wxSysErrorMsg(ec));
                return false;
        }

        if (!get_event()) {
                auto ec = GetLastError();
                err = wxString::Format(_("Cannot create event\nError %lu\n%s"), ec, wxSysErrorMsg(ec));
                return false;
        }

        return static_cast<bool>(get_vhci());
}

const wchar_t* usbip::get_speed_str(_In_ USB_DEVICE_SPEED speed) noexcept
{
        return speed >= 0 && speed < std::size(g_usb_speed_str) ? g_usb_speed_str[speed] : L"?";
}

bool usbip::get_speed_val(_Out_ USB_DEVICE_SPEED &val, _In_ const wxString &speed) noexcept
{
        for (int i = 0; i < std::size(g_usb_speed_str); ++i) {
                if (speed.IsSameAs(g_usb_speed_str[i], false)) {
                        val = static_cast<USB_DEVICE_SPEED>(i);
                        return true;
                }
        }

        return false;
}

auto usbip::make_imported_device(
        _In_ std::string hostname, _In_ std::string service, _In_ const usb_device &dev) -> imported_device
{
        return imported_device {
                .location = {
                        .hostname = std::move(hostname), 
                        .service = std::move(service), 
                        .busid = dev.busid 
                },

                .devid = make_devid(dev.busnum, dev.devnum),
                .speed = dev.speed,

                .vendor = dev.idVendor,
                .product = dev.idProduct 
        };
}

wxString usbip::make_device_url(_In_ const device_location &loc)
{
        auto url = loc.hostname + ':' + loc.service + '/' + loc.busid;
        return wxString::FromUTF8(url);
}

wxString usbip::make_server_url(_In_ const device_location &loc)
{
        auto url = loc.hostname + ':' + loc.service;
        return wxString::FromUTF8(url);
}

wxString usbip::make_server_url(_In_ const wxString &hostname, _In_ const wxString &service)
{
        return hostname + L':' + service;
}

bool usbip::split_server_url(_In_ const wxString &url, _Inout_ wxString &hostname, _Inout_ wxString &service)
{
        auto pos = url.find_last_of(L':');
        auto found = pos != url.npos;
        
        if (found) {
                hostname = url.substr(0, pos);
                service = url.substr(++pos);
        }

        return found;
}

/*
 * 25FC black medium square
 * 26AA medium white circle, 26AB medium black circle
 */
wxString usbip::to_string(_In_ state state)
{
        wxString s;

        switch (state) {
        case state::unplugged:
                s = L'\u25FB'; // white medium square
                break;
        case state::plugged:
                s = L'\u25A3'; // white square containing small black square
                break;
        default:
                s = wxString::FromAscii(vhci::get_state_str(state));
        }

        return s;
}
