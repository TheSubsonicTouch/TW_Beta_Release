/* FreeCam-Inject - starts TriadWars.exe and loads FreeCam.dll into it.
 *
 * A second dinput8.dll is not an option (the debug menu already owns that name, and
 * ReShade wants dxgi.dll), so the camera goes in as a plain DLL load:
 *
 *   FreeCam-Inject.exe                     start TriadWars.exe next to this file
 *   FreeCam-Inject.exe --attach            load into a TriadWars.exe already running
 *   FreeCam-Inject.exe --attach other.exe  ... or into another process by name
 *   FreeCam-Inject.exe --dll X.dll         use a different DLL
 *   FreeCam-Inject.exe --delay 8000        wait longer before loading
 *   FreeCam-Inject.exe --no-launch         only attach, never start the game
 *
 * Nothing here patches the game, and nothing stays running: the injector loads the
 * DLL and exits, the DLL lives in the game until the game closes.
 *
 * Build:
 *   zig cc -target x86_64-windows-gnu -O2 -o FreeCam-Inject.exe src/inject.c -lkernel32
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_EXE "TriadWars.exe"
#define GAME_DLL "FreeCam.dll"

static char g_dir[MAX_PATH];

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static char *find_last_slash(char *s)
{
    char *last = NULL;
    for (; *s; s++) if (*s == '\\' || *s == '/') last = s;
    return last;
}

static int file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static DWORD find_process(const char *name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    DWORD pid = 0;

    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (!_stricmp(pe.szExeFile, name)) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static int has_window(DWORD pid)
{
    HWND w = NULL;
    DWORD owner = 0;
    int found = 0;

    while ((w = FindWindowExA(NULL, w, NULL, NULL)) != NULL) {
        GetWindowThreadProcessId(w, &owner);
        if (owner == pid && IsWindowVisible(w)) { found = 1; break; }
    }
    return found;
}

/* LoadLibraryA(szDll) in the target, the ordinary way. */
static int inject(DWORD pid, const char *dll)
{
    HANDLE proc;
    void *remote;
    size_t len = strlen(dll) + 1;
    HANDLE thread;
    DWORD waited;

    proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                           PROCESS_VM_WRITE | PROCESS_VM_READ,
                       0, pid);
    if (!proc) {
        say("could not open the game process (error %lu); run as administrator?", GetLastError());
        return 0;
    }
    remote = VirtualAllocEx(proc, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        say("could not allocate in the game process (error %lu)", GetLastError());
        CloseHandle(proc);
        return 0;
    }
    if (!WriteProcessMemory(proc, remote, dll, len, NULL)) {
        say("could not write the DLL name (error %lu)", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 0;
    }
    thread = CreateRemoteThread(proc, NULL, 0,
                                (LPTHREAD_START_ROUTINE)(void *)GetProcAddress(
                                    GetModuleHandleA("kernel32.dll"), "LoadLibraryA"),
                                remote, 0, NULL);
    if (!thread) {
        say("could not start the loader thread (error %lu)", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 0;
    }
    waited = WaitForSingleObject(thread, 15000);
    if (waited == WAIT_TIMEOUT) say("the game did not finish loading the DLL in 15 seconds");
    else {
        DWORD code = 0;
        GetExitCodeThread(thread, &code);
        if (!code) say("the game refused to load %s (LoadLibrary returned 0)", dll);
    }
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return 1;
}

int main(int argc, char **argv)
{
    const char *exe = GAME_EXE;
    const char *dll = GAME_DLL;
    const char *attach = NULL;
    int delay = 4000;
    int launch = 1;
    int i;
    char exe_path[MAX_PATH], dll_path[MAX_PATH];
    char *slash;
    DWORD pid = 0;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    GetModuleFileNameA(NULL, g_dir, sizeof(g_dir));
    slash = find_last_slash(g_dir);
    if (slash) *slash = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--attach")) {
            attach = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : exe;
        } else if (!strcmp(argv[i], "--dll") && i + 1 < argc) {
            dll = argv[++i];
        } else if (!strcmp(argv[i], "--delay") && i + 1 < argc) {
            delay = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--no-launch")) {
            launch = 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            say("usage: FreeCam-Inject [--attach [name]] [--dll file] [--delay ms] [--no-launch]");
            return 0;
        } else if (argv[i][0] != '-') {
            exe = argv[i];
        }
    }

    snprintf(exe_path, sizeof(exe_path), "%s\\%s", g_dir, exe);
    snprintf(dll_path, sizeof(dll_path), "%s\\%s", g_dir, dll);
    if (!file_exists(dll_path)) {
        say("%s is not next to the injector (%s)", dll, dll_path);
        return 2;
    }

    if (attach) {
        say("looking for %s ...", attach);
        for (i = 0; i < 40 && !pid; i++) {
            pid = find_process(attach);
            if (!pid) Sleep(250);
        }
        if (!pid) {
            say("%s is not running", attach);
            return 3;
        }
    } else {
        if (!file_exists(exe_path)) {
            say("%s is not next to the injector (%s)", exe, exe_path);
            say("put FreeCam-Inject.exe and FreeCam.dll in the game folder, or use --attach");
            return 2;
        }
        if (!launch) {
            say("--no-launch was given but no --attach name was; nothing to do");
            return 2;
        }
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));
        say("starting %s ...", exe_path);
        if (!CreateProcessA(exe_path, NULL, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) {
            say("could not start the game (error %lu)", GetLastError());
            return 4;
        }
        pid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    /* give the game time to build its world before the camera looks for it */
    say("waiting for the game window (%d ms at most) ...", delay * 3);
    for (i = 0; i < delay * 3 / 250 && !has_window(pid); i++) Sleep(250);
    if (delay > 0) Sleep((DWORD)delay);

    say("loading %s into pid %lu ...", dll, pid);
    if (!inject(pid, dll_path)) return 5;
    say("done. CapsLock flies, F10 puts the camera back. See FreeCam.log next to the DLL.");
    return 0;
}
