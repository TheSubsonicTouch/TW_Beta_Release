// dinput8_proxy.cpp - Loads the menu when the game loads dinput8.dll.
// Place the built dinput8.dll next to TriadWars.exe. The real system
// dinput8.dll is forwarded to, so the game's input keeps working.
//
// This is the TestingSkybox file with one addition: TW_RealDirectInput8Create, so the
// free camera's input blocker can reach the system DirectInput without going through the
// menu's startup again.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>

#include "win_menu.h"
#include "log.h"

namespace {

HMODULE g_realDInput = nullptr;

using PFN_DirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using PFN_DllCanUnloadNow = HRESULT(WINAPI*)();
using PFN_DllGetClassObject = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

PFN_DirectInput8Create realDirectInput8Create = nullptr;
PFN_DllCanUnloadNow realDllCanUnloadNow = nullptr;
PFN_DllGetClassObject realDllGetClassObject = nullptr;

bool loadRealDInput() {
    if (g_realDInput) return true;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + 16 >= MAX_PATH) return false;
    lstrcatW(path, L"\\dinput8.dll");
    g_realDInput = LoadLibraryW(path);
    if (!g_realDInput) return false;
    realDirectInput8Create = reinterpret_cast<PFN_DirectInput8Create>(
        GetProcAddress(g_realDInput, "DirectInput8Create"));
    realDllCanUnloadNow = reinterpret_cast<PFN_DllCanUnloadNow>(
        GetProcAddress(g_realDInput, "DllCanUnloadNow"));
    realDllGetClassObject = reinterpret_cast<PFN_DllGetClassObject>(
        GetProcAddress(g_realDInput, "DllGetClassObject"));
    return realDirectInput8Create != nullptr;
}

}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    if (!loadRealDInput()) { TW_Log("ERROR: could not load system dinput8.dll"); return E_FAIL; }
    // The game calls this once during startup. Start the menu then.
    TW_Log("DirectInput8Create called by game. Starting menu.");
    TW_MenuStart();
    return realDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() {
    if (!loadRealDInput() || !realDllCanUnloadNow) return S_FALSE;
    return realDllCanUnloadNow();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(
    REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!loadRealDInput() || !realDllGetClassObject) return CLASS_E_CLASSNOTAVAILABLE;
    return realDllGetClassObject(rclsid, riid, ppv);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Install the crash handler at the earliest possible moment so the
        // startup load crash is covered even before DirectInput8Create.
        TW_VehInstall();
        DisableThreadLibraryCalls(hinst);
    } else if (reason == DLL_PROCESS_DETACH) {
        TW_MenuStop();
    }
    return TRUE;
}

// The system DirectInput8Create, for input_block.cpp's own throwaway keyboard device.
// (It needs a device only to find the class's vtable, which it patches so the keyboard
// the game reads can be emptied while the camera is flying.)
extern "C" __declspec(dllexport) HRESULT WINAPI TW_RealDirectInput8Create(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    if (!loadRealDInput()) return E_FAIL;
    return realDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}
