/* FreeCam - the Windows half.
 *
 * Everything with a rule in it lives in freecam.c and is unit tested; this file only
 * does the parts that need Windows: the clock, the keyboard, the mouse, the process
 * memory and the DLL exports.
 *
 * The DLL is loaded into the game process (see FreeCam-Inject.exe), so the matrix is
 * read and written directly - no ReadProcessMemory, no handle. Every access is
 * guarded with VirtualQuery, so a wrong address is logged and skipped instead of
 * taking the game down.
 *
 * Build (cross compiler on Linux, or MinGW on Windows):
 *   zig cc -target x86_64-windows-gnu -shared -O2 -o FreeCam.dll \
 *          src/freecam.c src/win32.c -lkernel32 -luser32
 */
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freecam.h"

#define FC_LOG "FreeCam.log"
#define FC_INI "FreeCam.ini"

/* What the keys do. Hold the right mouse button to look around. */
#define VK_LOOK VK_RBUTTON
static const int key_map[FC_KEY_COUNT] = {
    'W',              /* forward */
    'S',              /* back    */
    'A',              /* left    */
    'D',              /* right   */
    VK_SPACE,         /* up      */
    'C',              /* down    */
    VK_SHIFT,         /* sprint  */
    VK_CONTROL        /* slow    */
};

typedef struct {
    fc_config cfg;
    fc_state  st;
    void     *target;
    int       have_target;
    FILETIME  ini_time;
    ULONGLONG last_ini_check;
    POINT     look_origin;
    int       looking;
    int       reported_bad;
} fc_win;

static HMODULE g_self;
static HANDLE  g_thread;
static volatile LONG g_quit;
static volatile LONG g_active;    /* mirrors st->active, for the exported getter */
static volatile LONG g_request;   /* 1 = asked to switch on, 2 = asked to switch off */
static char   g_dir[MAX_PATH];
static CRITICAL_SECTION g_log_cs;
static fc_win g_win;

/* ---- small helpers --------------------------------------------------------- */

static void log_line(const char *fmt, ...)
{
    char path[MAX_PATH];
    char text[1024];
    va_list ap;
    FILE *f;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    text[sizeof(text) - 1] = 0;

    EnterCriticalSection(&g_log_cs);
    snprintf(path, sizeof(path), "%s\\%s", g_dir, FC_LOG);
    f = fopen(path, "a");
    if (f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d.%03d  %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_cs);
}

static char *find_last_slash(char *s)
{
    char *last = NULL;
    for (; *s; s++) if (*s == '\\') last = s;
    return last;
}

static void warn_cb(void *ctx, int line, const char *msg)
{
    (void)ctx;
    log_line("config line %d: %s", line, msg);
}

static char *read_file(const char *path, long *size_out)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    if (size && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = 0;
    fclose(f);
    if (size_out) *size_out = size;
    return buf;
}

/* Is [addr, addr+len) readable in this process? */
static int region_ok(const void *addr, size_t len, int want_write)
{
    unsigned char *p = (unsigned char *)addr;
    unsigned char *end = p + len;
    MEMORY_BASIC_INFORMATION mbi;
    DWORD good;

    good = want_write ? (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
                      : (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    while (p < end) {
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        if (!((mbi.Protect & 0xff) & good)) return 0;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
        p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

static int readable(const void *addr, size_t len) { return region_ok(addr, len, 0); }
static int writable(const void *addr, size_t len) { return region_ok(addr, len, 1); }

/* ---- finding the matrix ---------------------------------------------------- */

static void *game_module_base(const fc_config *cfg)
{
    if (cfg->module[0]) return (void *)GetModuleHandleA(cfg->module);
    return (void *)GetModuleHandleA(NULL);          /* the exe itself */
}

/* The chain walk itself is in freecam.c; this only supplies the guarded read. */
static int guarded_read(void *ctx, const void *addr, void *out, unsigned size)
{
    (void)ctx;
    if (!readable(addr, size)) return 0;
    memcpy(out, addr, size);
    return 1;
}

static void *walk_chain(void *base, const fc_config *cfg)
{
    if (!base || (uintptr_t)base < 0x10000u) return NULL;
    return fc_chain_resolve(base, cfg, guarded_read, NULL);
}

static void *resolve_target(const fc_config *cfg)
{
    if (cfg->address) return (void *)(uintptr_t)cfg->address;
    if (cfg->chain_len > 0) return walk_chain(game_module_base(cfg), cfg);
    return NULL;
}

/* ---- state ----------------------------------------------------------------- */

static int key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* Caps Lock is a latch: bit 0 is the locked state, which is what we want. */
static int caps_locked(void) { return (GetAsyncKeyState(VK_CAPITAL) & 1) != 0; }

static void load_config(fc_win *w, int keep_pose)
{
    char path[MAX_PATH];
    char *text;
    int applied;
    fc_config cfg;

    snprintf(path, sizeof(path), "%s\\%s", g_dir, FC_INI);
    fc_config_defaults(&cfg);
    text = read_file(path, NULL);
    applied = 0;
    if (!text) {
        log_line("no %s next to the DLL (%s); using the built in defaults", FC_INI, path);
    } else {
        applied = fc_config_parse(&cfg, text, warn_cb, NULL);
        free(text);
        if (applied < 0) log_line("config has a bad value; the defaults are kept for it");
    }
    w->cfg = cfg;
    if (!keep_pose) {
        fc_init(&w->st, &w->cfg);
    } else {
        w->st.cfg = w->cfg;          /* keep flying, pick up the new speeds */
    }
    log_line("config: %d setting(s), mode=%s toggle=%s chain=%d address=0x%llx view=%d",
             applied < 0 ? 0 : applied,
             w->cfg.mode == FC_MODE_PLAYER ? "player" : "camera",
             fc_key_name(w->cfg.toggle_vk), w->cfg.chain_len,
             (unsigned long long)w->cfg.address, w->cfg.view);
}

static void watch_config(fc_win *w)
{
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULONGLONG now = GetTickCount64();

    if (now - w->last_ini_check < 2000) return;
    w->last_ini_check = now;
    snprintf(path, sizeof(path), "%s\\%s", g_dir, FC_INI);
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return;
    if (CompareFileTime(&fad.ftLastWriteTime, &w->ini_time) == 0) return;
    w->ini_time = fad.ftLastWriteTime;
    log_line("%s changed, reloading", FC_INI);
    load_config(w, w->st.active);
    w->have_target = 0;              /* the address may have moved */
}

static int read_matrix(fc_win *w, fc_mat *m)
{
    if (!w->have_target) {
        w->target = resolve_target(&w->cfg);
        if (w->target && readable(w->target, sizeof(fc_mat))) {
            w->have_target = 1;
            w->reported_bad = 0;
            log_line("matrix at 0x%llx (module: %s)",
                     (unsigned long long)(uintptr_t)w->target,
                     w->cfg.module[0] ? w->cfg.module : "the game exe");
        } else {
            if (!w->reported_bad) {
                log_line("no matrix at the configured address/chain yet; still looking");
                w->reported_bad = 1;
            }
            w->target = NULL;
            return 0;
        }
    }
    if (!readable(w->target, sizeof(fc_mat))) {
        w->target = NULL;
        w->have_target = 0;
        return 0;
    }
    memcpy(m, w->target, sizeof(fc_mat));
    return 1;
}

static void write_matrix(fc_win *w, const fc_mat *m)
{
    if (!w->target || !writable(w->target, sizeof(fc_mat))) return;
    memcpy(w->target, m, sizeof(fc_mat));
}

static void set_active(fc_win *w, int on, const fc_mat *target)
{
    fc_mat back;

    if (on) {
        if (w->st.active) return;
        if (!target) { log_line("free cam refused: no readable matrix"); return; }
        if (!fc_mat_valid(target)) { log_line("free cam refused: that is not a transform"); return; }
        if (!fc_set_active(&w->st, 1, target)) { log_line("free cam refused"); return; }
        log_line("free cam ON (%s mode) at %.1f %.1f %.1f",
                 w->cfg.mode == FC_MODE_PLAYER ? "player" : "camera",
                 w->st.pos[0], w->st.pos[1], w->st.pos[2]);
        InterlockedExchange(&g_active, 1);
    } else {
        if (!w->st.active) return;
        if (fc_restore(&w->st, &back)) {
            write_matrix(w, &back);
            log_line("free cam OFF, put back where it started (%.0f m flown)", w->st.moved);
        } else {
            log_line("free cam OFF (%.0f m flown)", w->st.moved);
        }
        fc_set_active(&w->st, 0, NULL);
        InterlockedExchange(&g_active, 0);
    }
}

/* ---- the loop -------------------------------------------------------------- */

static DWORD WINAPI loop_thread(LPVOID param)
{
    fc_win *w = (fc_win *)param;
    LARGE_INTEGER freq, prev, now;
    fc_mat target, out;
    int last_caps;
    int log_ticks = 0;
    int i;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    log_line("FreeCam watching %s", g_dir);

    load_config(w, 0);
    last_caps = caps_locked();

    for (;;) {
        float dt;
        POINT cur;
        float mouse_dx = 0.0f, mouse_dy = 0.0f;
        LONG request;

        if (g_quit) break;
        Sleep(1);

        QueryPerformanceCounter(&now);
        dt = (float)((double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart);
        prev = now;
        if (dt <= 0.0f) dt = 0.001f;

        watch_config(w);
        if (!read_matrix(w, &target)) {
            if (w->st.active) {
                log_line("lost the matrix while flying; switching free cam off");
                set_active(w, 0, NULL);
            }
            continue;
        }

        /* Caps Lock toggles: edge detect on the locked state, not on the key being down */
        {
            int caps = caps_locked();
            if (caps != last_caps) {
                last_caps = caps;
                set_active(w, !w->st.active, &target);
            }
        }
        /* or the panic key, held for a moment */
        if (key_down(w->cfg.panic_vk) && w->st.active) {
            log_line("panic key held; putting the camera back");
            set_active(w, 0, NULL);
        }
        /* or an outside caller, e.g. a menu item later on */
        request = InterlockedExchange(&g_request, 0);
        if (request == 1) set_active(w, 1, &target);
        else if (request == 2) set_active(w, 0, NULL);

        if (!w->st.active) { last_caps = caps_locked(); continue; }

        for (i = 0; i < FC_LOOK; i++) w->st.keys[i] = key_down(key_map[i]) ? 1 : 0;

        if (key_down(VK_LOOK)) {
            if (!w->looking) {
                GetCursorPos(&w->look_origin);
                w->looking = 1;
            }
            GetCursorPos(&cur);
            mouse_dx = (float)(cur.x - w->look_origin.x);
            mouse_dy = (float)(cur.y - w->look_origin.y);
            if (mouse_dx != 0.0f || mouse_dy != 0.0f)
                SetCursorPos(w->look_origin.x, w->look_origin.y);
            w->st.keys[FC_LOOK] = 1;
        } else {
            w->looking = 0;
        }

        if (fc_step(&w->st, dt, mouse_dx, mouse_dy, &out)) write_matrix(w, &out);

        if ((++log_ticks % 2000) == 0) {
            log_line("flying: %.1f %.1f %.1f (yaw %.0f, pitch %.0f)", w->st.pos[0], w->st.pos[1],
                     w->st.pos[2], w->st.yaw * 57.2957795f, w->st.pitch * 57.2957795f);
        }
    }

    log_line("FreeCam thread stopping");
    return 0;
}

/* ---- exports --------------------------------------------------------------- */

/* 0x00010000 = 1.0. */
__declspec(dllexport) unsigned FreeCamVersion(void) { return 0x00010000u; }

__declspec(dllexport) int FreeCamIsActive(void) { return g_active != 0; }

/* 1 = switched on, 0 = switched off. The loop picks the request up on its next tick. */
__declspec(dllexport) int FreeCamSetActive(int on)
{
    if (on && !g_win.have_target) return 0;      /* nothing to fly yet */
    InterlockedExchange(&g_request, on ? 1 : 2);
    return 1;
}

__declspec(dllexport) const char *FreeCamIniPath(void)
{
    static char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", g_dir, FC_INI);
    return path;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    char *slash;
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, sizeof(g_dir));
        slash = find_last_slash(g_dir);
        if (slash) *slash = 0;
        InitializeCriticalSection(&g_log_cs);
        g_thread = CreateThread(NULL, 0, loop_thread, &g_win, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_quit = 1;
        if (g_thread) {
            WaitForSingleObject(g_thread, 200);
            CloseHandle(g_thread);
            g_thread = NULL;
        }
        /* leaving the game with the camera still outside it would be rude */
        if (g_win.st.active) set_active(&g_win, 0, NULL);
        DeleteCriticalSection(&g_log_cs);
    }
    return TRUE;
}
