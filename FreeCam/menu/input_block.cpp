// input_block.cpp - keep the game's own controls away from the player while the free
// camera is flying. See input_block.h for why these three places are the right ones.
#define WIN32_LEAN_AND_MEAN
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <windows.h>
#include <dinput.h>

#include <cstdio>
#include <cstring>

#include "input_block.h"
#include "log.h"

namespace tw {
namespace game {
namespace {

volatile LONG g_blocked = 0;
char          g_status[256] = "not installed";
volatile LONG g_installing = 0;
int           g_iatCount = 0;

// The real (system) DirectInput8Create, exported by dinput8_proxy.cpp so this file can
// make a throwaway keyboard device of its own without going through the menu's startup.
extern "C" HRESULT WINAPI TW_RealDirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID*,
                                                    LPUNKNOWN);

// --- what the camera still needs to see --------------------------------------
//
// DirectInput reports the keyboard as 256 scan codes (the DIK_ list), so the keyboard
// filter is written in those. The virtual-key list is for the polling functions.

bool DikAllowed(unsigned code) {
    switch (code) {
        case DIK_ESCAPE:
        case DIK_TAB:
        case DIK_RETURN:
        case DIK_BACK:
        case DIK_INSERT:
        case DIK_F10:
        case DIK_UP:
        case DIK_DOWN:
        case DIK_LEFT:
        case DIK_RIGHT:
        case DIK_P:
        case DIK_O:
        case DIK_L:
        case DIK_K:
        case DIK_M:
            return true;
        default:
            return false;
    }
}

bool VkAllowed(int vk) {
    if (vk < 0 || vk > 255) return true;      // not a key we know: leave it alone
    switch (vk) {
        case VK_ESCAPE:
        case VK_TAB:
        case VK_RETURN:
        case VK_BACK:
        case VK_INSERT:
        case VK_F10:
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case 'P':
        case 'O':
        case 'L':
        case 'K':
        case 'M':
            return true;
        default:
            return false;
    }
}

// --- the DirectInput keyboard ------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* GetDeviceStateFn)(void*, DWORD, void*);
typedef HRESULT(STDMETHODCALLTYPE* GetDeviceDataFn)(void*, DWORD, void*, DWORD*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* GetCapabilitiesFn)(void*, void*);

struct VtablePatch {
    void**           vtable;
    GetDeviceStateFn originalState;
    GetDeviceDataFn  originalData;
};
VtablePatch g_patches[4] = {};
int         g_patchCount = 0;

// Which devices are keyboards, so buffered events can be dropped for those and only those.
struct DeviceKind {
    void* device;
    int   keyboard;
};
DeviceKind g_devices[8] = {};
int        g_deviceCount = 0;

// A device's type, asked of DirectInput itself (slot 3, which is left unpatched).
bool DeviceIsKeyboard(void* self, void** vt) {
    for (int i = 0; i < g_deviceCount; ++i) {
        if (g_devices[i].device == self) return g_devices[i].keyboard != 0;
    }
    if (!vt || !vt[3]) return false;

    DIDEVCAPS caps;
    std::memset(&caps, 0, sizeof caps);
    caps.dwSize = sizeof(caps);

    HRESULT hr = reinterpret_cast<GetCapabilitiesFn>(vt[3])(self, &caps);
    int keyboard = (SUCCEEDED(hr) && (caps.dwDevType & 0xFF00u) == DI8DEVTYPE_KEYBOARD) ? 1 : 0;

    if (g_deviceCount < 8) {
        g_devices[g_deviceCount].device = self;
        g_devices[g_deviceCount].keyboard = keyboard;
        ++g_deviceCount;
    }
    return keyboard != 0;
}

HRESULT STDMETHODCALLTYPE HookGetDeviceState(void* self, DWORD cb, void* data) {
    GetDeviceStateFn real = nullptr;
    void** vt = *(void***)self;

    for (int i = 0; i < g_patchCount; ++i) {
        if (g_patches[i].vtable == vt) { real = g_patches[i].originalState; break; }
    }
    if (!real) return DIERR_NOTINITIALIZED;

    HRESULT hr = real(self, cb, data);
    if (SUCCEEDED(hr) && g_blocked && data && cb == 256) {
        // A keyboard state: every key the camera does not need is reported as up. The
        // character controller polls this, so nothing it reads can move or swing.
        unsigned char* keys = static_cast<unsigned char*>(data);
        for (unsigned i = 0; i < 256; ++i) {
            if (!DikAllowed(i)) keys[i] = 0;
        }
    }
    return hr;
}

// The buffered way of reading the same device. An engine that asks for events instead of
// polling would walk right around the state filter, so a keyboard is answered with
// nothing at all while the camera is flying.
HRESULT STDMETHODCALLTYPE HookGetDeviceData(void* self, DWORD cbObjectData, void* rgdod,
                                            DWORD* pdwInOut, DWORD flags) {
    GetDeviceDataFn real = nullptr;
    void** vt = *(void***)self;

    for (int i = 0; i < g_patchCount; ++i) {
        if (g_patches[i].vtable == vt) { real = g_patches[i].originalData; break; }
    }
    if (!real) return DIERR_NOTINITIALIZED;

    HRESULT hr = real(self, cbObjectData, rgdod, pdwInOut, flags);
    if (SUCCEEDED(hr) && g_blocked && pdwInOut && *pdwInOut && DeviceIsKeyboard(self, vt)) {
        *pdwInOut = 0;
    }
    return hr;
}

// Slot 9 of an IDirectInputDevice8 vtable: QueryInterface, AddRef, Release,
// GetCapabilities, EnumObjects, GetProperty, SetProperty, Acquire, Unacquire, then
// GetDeviceState. The order is part of DirectInput's ABI, and both the A and the W
// interface lay it out the same way.
constexpr int kGetDeviceState = 9;
constexpr int kGetDeviceData = 10;

// The vtable of the device class a keyboard is made of. Patching it in place covers every
// keyboard device in this process, including the one the game created before us.
bool PatchOneVtable(void** vt, IDirectInputDevice8A* probe) {
    if (!vt || !vt[kGetDeviceState] || !vt[kGetDeviceData]) return false;

    for (int i = 0; i < g_patchCount; ++i) {
        if (g_patches[i].vtable == vt) return true;
    }

    // Only touch a function that really lives in dinput8.dll: this checks the slot index
    // as well, because a wrong index would be some other method of the same object.
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(vt[kGetDeviceState]), &owner) ||
        !owner) {
        return false;
    }
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(owner, path, MAX_PATH);
    if (!wcsstr(path, L"dinput8.dll")) return false;

    // And the slot has to behave like GetDeviceState: asked for the 256 bytes of keyboard
    // state it answers with them, or says the device is not acquired (which is still the
    // right kind of answer, and is what an engine that only polls when it wants to sees).
    unsigned char state[256] = {0};
    HRESULT asked = reinterpret_cast<GetDeviceStateFn>(vt[kGetDeviceState])(probe, 256, state);
    if (FAILED(asked) && asked != DIERR_NOTACQUIRED && asked != DIERR_INPUTLOST &&
        asked != DIERR_NOTINITIALIZED) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vt[kGetDeviceState], 2 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return false;
    }
    GetDeviceStateFn originalState = reinterpret_cast<GetDeviceStateFn>(vt[kGetDeviceState]);
    GetDeviceDataFn originalData = reinterpret_cast<GetDeviceDataFn>(vt[kGetDeviceData]);
    vt[kGetDeviceState] = reinterpret_cast<void*>(&HookGetDeviceState);
    vt[kGetDeviceData] = reinterpret_cast<void*>(&HookGetDeviceData);
    VirtualProtect(&vt[kGetDeviceState], 2 * sizeof(void*), oldProtect, &oldProtect);

    g_patches[g_patchCount].vtable = vt;
    g_patches[g_patchCount].originalState = originalState;
    g_patches[g_patchCount].originalData = originalData;
    ++g_patchCount;
    return true;
}

// A hidden window and a private keyboard device, only so the device class can be found.
bool ProbeKeyboard(bool wide, HINSTANCE hinst) {
    HWND wnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                               nullptr, nullptr);
    if (!wnd) return false;

    LPVOID pdi = nullptr;
    HRESULT hr = TW_RealDirectInput8Create(
        hinst, DIRECTINPUT_VERSION, wide ? IID_IDirectInput8W : IID_IDirectInput8A, &pdi, nullptr);
    if (FAILED(hr) || !pdi) {
        DestroyWindow(wnd);
        return false;
    }

    bool ok = false;
    if (wide) {
        auto* di = static_cast<IDirectInput8W*>(pdi);
        IDirectInputDevice8W* dev = nullptr;
        if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) {
            // Everything up to Acquire is a best effort: the vtable is valid as soon as
            // the device exists, and a game holding the keyboard exclusively must not
            // stop the patch (the state check then just gets DIERR_NOTACQUIRED).
            dev->SetDataFormat(&c_dfDIKeyboard);
            dev->SetCooperativeLevel(wnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
            dev->Acquire();
            ok = PatchOneVtable(*reinterpret_cast<void***>(dev),
                                reinterpret_cast<IDirectInputDevice8A*>(dev));
            dev->Unacquire();
            dev->Release();
        }
        di->Release();
    } else {
        auto* di = static_cast<IDirectInput8A*>(pdi);
        IDirectInputDevice8A* dev = nullptr;
        if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) {
            dev->SetDataFormat(&c_dfDIKeyboard);
            dev->SetCooperativeLevel(wnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
            dev->Acquire();
            ok = PatchOneVtable(*reinterpret_cast<void***>(dev), dev);
            dev->Unacquire();
            dev->Release();
        }
        di->Release();
    }

    DestroyWindow(wnd);
    return ok;
}

// --- the polling functions ---------------------------------------------------

typedef SHORT(WINAPI* GetAsyncKeyStateFn)(int);
typedef BOOL(WINAPI* GetKeyboardStateFn)(PBYTE);

GetAsyncKeyStateFn g_realAsync = nullptr;
GetKeyboardStateFn g_realKeyboardState = nullptr;

SHORT WINAPI HookGetAsyncKeyState(int vk) {
    SHORT state = g_realAsync ? g_realAsync(vk) : 0;
    if (g_blocked && !VkAllowed(vk)) return 0;
    return state;
}

BOOL WINAPI HookGetKeyboardState(PBYTE keys) {
    BOOL ok = g_realKeyboardState ? g_realKeyboardState(keys) : FALSE;
    if (ok && g_blocked && keys) {
        for (int vk = 0; vk < 256; ++vk) {
            if (!VkAllowed(vk)) keys[vk] = 0;
        }
    }
    return ok;
}

// Redirect one import of the game's own module. Only the game's table is touched, so this
// DLL, the menu and the camera keep reading the real keyboard.
bool PatchGameImport(const char* dllName, const char* fnName, void* replacement, void** saved) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;

    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         imp->Name; ++imp) {
        const char* dll = reinterpret_cast<const char*>(base + imp->Name);
        if (_stricmp(dll, dllName) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        if (!names) continue;
        for (; names->u1.Function; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), fnName) != 0) continue;

            DWORD oldProtect = 0;
            auto** slot = reinterpret_cast<void**>(&slots->u1.Function);
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
            if (saved) *saved = *slot;
            *slot = replacement;
            VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
            return true;
        }
    }
    return false;
}

}  // namespace

void InputBlockSetActive(bool on) {
    InterlockedExchange(&g_blocked, on ? 1 : 0);
}

bool InputBlockActive() { return g_blocked != 0; }

bool InputBlockSwallowMessage(UINT msg) {
    if (!g_blocked) return false;
    switch (msg) {
        // Raw input: the mouse. Without this the player can still look around, aim and
        // shoot, because the game reads the mouse straight from the device.
        case WM_INPUT:
        // Anything the game might read as a key or a click.
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
        // WM_SYSKEYDOWN/UP and WM_SYSCHAR are left alone on purpose: they are how
        // Alt+F4 and the rest of the system keys reach the window, and no game drives a
        // character from them.
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            return true;
        default:
            return false;
    }
}

void InputBlockInstall(HINSTANCE hinst) {
    if (InterlockedExchange(&g_installing, 1) != 0) return;

    int iat = 0;
    if (PatchGameImport("USER32.dll", "GetAsyncKeyState",
                        reinterpret_cast<void*>(&HookGetAsyncKeyState),
                        reinterpret_cast<void**>(&g_realAsync))) {
        ++iat;
    }
    if (PatchGameImport("USER32.dll", "GetKeyboardState",
                        reinterpret_cast<void*>(&HookGetKeyboardState),
                        reinterpret_cast<void**>(&g_realKeyboardState))) {
        ++iat;
    }

    int vts = 0;
    g_blocked = 0;      // the probe must see a real keyboard
    if (ProbeKeyboard(false, hinst)) ++vts;
    if (ProbeKeyboard(true, hinst)) ++vts;

    g_iatCount = iat;
    std::snprintf(g_status, sizeof(g_status),
                  "%d import(s) redirected, %d keyboard device class(es) filtered", iat, vts);
    g_installing = 0;      // a later attempt may add a device class the game had not made yet
    TW_Log("FreeCam: input blocking ready - %s", g_status);
}

const char* InputBlockStatus() { return g_status; }

bool InputBlockReady() { return g_iatCount > 0 || g_patchCount > 0; }

}  // namespace game
}  // namespace tw
