// win_menu.cpp - D3D11 overlay (Dear ImGui), keyboard/gamepad input, the
// job queue that defers game calls to the game thread, and the vectored
// exception handler that keeps game crashes from taking the process down.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <xinput.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "../game_hooks.h"
#include "../menu_core.h"
#include "freecam_game.h"
#include "freecam_hologram.h"
#include "freecam_noclip.h"
#include "freecam_spawn.h"
#include "freecam_unlock.h"
#include "freecam_weather.h"
#include "freecam_sky.h"
#include "freecam_wardrobe.h"
#include "input_block.h"
#include "log.h"
#include "spawn_guard.h"
#include "win_menu.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ---------------------------------------------------------------------------
// Vectored exception handler: startup load-crash fixups + spawn backstop.
// ---------------------------------------------------------------------------

// Fake list-head object used to satisfy the game's null list walks at
// game+0x548A (r8) and game+0x54DD (rax): the code compares [reg+0x58]
// against reg+0x50 and [reg+0x68] against reg+0x60; making them point at
// each other takes the "empty list" branch.
unsigned char g_fakeObj[0x80];

void initFakeObj() {
    *reinterpret_cast<void**>(g_fakeObj + 0x50) = g_fakeObj + 0x50;
    *reinterpret_cast<void**>(g_fakeObj + 0x58) = g_fakeObj + 0x50;
    *reinterpret_cast<void**>(g_fakeObj + 0x60) = g_fakeObj + 0x60;
    *reinterpret_cast<void**>(g_fakeObj + 0x68) = g_fakeObj + 0x60;
}

volatile LONG g_vehBusy = 0;
volatile LONG g_vehLogCount = 0;   // hard cap on VEH log lines
volatile LONG g_loadFixCount = 0;  // hard cap on load-region fixups
volatile LONG g_loadFixLogCount = 0;  // cap on load-fix log lines

bool inRange(uintptr_t v, uintptr_t lo, uintptr_t hi) { return v >= lo && v < hi; }

LONG CALLBACK vehHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&g_vehBusy, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;  // never re-enter

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    CONTEXT* ctx = ep->ContextRecord;
    const uintptr_t rip = ctx->Rip;
    const uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];
    const uintptr_t off = rip - base;
    const char* stage = "none";
    bool acted = false;

    if (fault < 0x10000) {
        // --- Startup load crash: null list walk in function game+0x5400 ---
        if (inRange(off, 0x5400, 0x5C72)) {
            if (off == 0x548A && ctx->R8 < 0x10000) {
                ctx->R8 = reinterpret_cast<DWORD64>(g_fakeObj);
                stage = "load-fix-r8";
                acted = true;
                if (InterlockedIncrement(&g_loadFixLogCount) > 5) stage = "load-fix-r8-quiet";
            } else if (off == 0x54DD && ctx->Rax < 0x10000) {
                ctx->Rax = reinterpret_cast<DWORD64>(g_fakeObj);
                stage = "load-fix-rax";
                acted = true;
                if (InterlockedIncrement(&g_loadFixLogCount) > 5) stage = "load-fix-rax-quiet";
            } else if (InterlockedCompareExchange(&g_loadFixCount, 1, 0) == 0) {
                // Unknown null-deref site in the same function: try the
                // unique register that plausibly produces this fault address.
                DWORD64* regs[] = {&ctx->Rax, &ctx->Rcx, &ctx->Rdx, &ctx->Rbp, &ctx->Rsi,
                                   &ctx->Rdi, &ctx->R8,  &ctx->R9,  &ctx->R10, &ctx->R11,
                                   &ctx->R12, &ctx->R13, &ctx->R14, &ctx->R15};
                DWORD64* hit = nullptr;
                int hits = 0;
                for (DWORD64* r : regs) {
                    if (*r < 0x10000 && fault >= *r && (fault - *r) < 0x2000) {
                        hit = r;
                        ++hits;
                    }
                }
                if (hits == 1 && hit) {
                    *hit = reinterpret_cast<DWORD64>(g_fakeObj);
                    stage = "load-fix-reg";
                    acted = true;
                } else {
                    stage = "load-unknown";  // log and let the game decide
                }
            } else {
                stage = "load-fix-budget";  // already fixed too many times
            }
        }
        // --- Allocator null-deref: game+0xBE5C5 ---
        else if (off == 0xBE5C5 && ctx->Rax < 0x10000) {
            ctx->Rax = reinterpret_cast<DWORD64>(g_fakeObj);
            stage = "alloc-fix";
            acted = true;
        }
        // --- Spawn backstop: our guarded trampoline is on the call stack ---
        else if (tw_in_spawn && tw::game::IsSpawnRip(off)) {
            // Never retry the allocator from inside the VEH (that can itself
            // fault if the TLS outer block is missing). Hard-fail to the guard
            // landing pad so the caller gets an error message instead.
            ctx->Rip = reinterpret_cast<uintptr_t>(&tw_spawn_fail);
            stage = "spawn-fail";
            acted = true;
        }
    }

    if (acted || (fault < 0x10000 && off < 0x3000000)) {
        if (InterlockedIncrement(&g_vehLogCount) <= 64) {
            char buf[512];
            std::snprintf(buf, sizeof buf,
                          "VEH code=%08lX addr=%p game+0x%llX fault=%p stage=%s acted=%d tid=%lu "
                          "rax=%llX rcx=%llX rdx=%llX rbx=%llX rsi=%llX rdi=%llX "
                          "r8=%llX r9=%llX r12=%llX r13=%llX r14=%llX r15=%llX",
                          ep->ExceptionRecord->ExceptionCode, reinterpret_cast<void*>(rip),
                          static_cast<unsigned long long>(off), reinterpret_cast<void*>(fault),
                          stage, acted ? 1 : 0, GetCurrentThreadId(),
                          static_cast<unsigned long long>(ctx->Rax),
                          static_cast<unsigned long long>(ctx->Rcx),
                          static_cast<unsigned long long>(ctx->Rdx),
                          static_cast<unsigned long long>(ctx->Rbx),
                          static_cast<unsigned long long>(ctx->Rsi),
                          static_cast<unsigned long long>(ctx->Rdi),
                          static_cast<unsigned long long>(ctx->R8),
                          static_cast<unsigned long long>(ctx->R9),
                          static_cast<unsigned long long>(ctx->R12),
                          static_cast<unsigned long long>(ctx->R13),
                          static_cast<unsigned long long>(ctx->R14),
                          static_cast<unsigned long long>(ctx->R15));
            TW_VehLog(buf);
        }
    }

    InterlockedExchange(&g_vehBusy, 0);
    return acted ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Job queue: menu actions are queued by the input path and executed on the
// game thread (WndProc/Present), never inside the ImGui/input path itself.
// ---------------------------------------------------------------------------

tw::Menu g_menu;
std::function<void()> g_syncState;   // see DebugHooks::syncState

// How the menu looks. One object, written by the APPEARANCE rows and read by renderMenu on
// the next frame - and saved next to the game so the choice survives a restart.
tw::MenuStyle g_style;
bool g_styleDirty = false;
ULONGLONG g_styleSavedAt = 0;


// Only the work that has to happen on the game thread: the three actions that call into
// the game or write a save file. Everything the camera does is hotkeyed and runs on its
// own thread (freecam_game.cpp), so it does not pass through here.
enum JobKind {
    JobWeather,
    JobLevel,
    JobShader,
    // The weather sliders and the skybox. Every one of them is a field write or a
    // call into the game, so like the weather preset above they run here, on the
    // game thread, rather than from the input path.
    JobWeatherAmount,
    JobWeatherChance,
    JobWeatherWetness,
    JobWeatherInterval,
    JobWeatherLock,
};

struct Job {
    JobKind kind = JobWeather;
    std::string text;
    float value = 0.0f;
    bool flag = false;
};

constexpr int kQueueCap = 32;
Job g_queue[kQueueCap];
volatile LONG g_qHead = 0;
volatile LONG g_qTail = 0;

void pushJob(Job j) {
    const LONG head = g_qHead;
    const LONG next = (head + 1) % kQueueCap;
    if (next == g_qTail) return;  // full: drop (status line still shows queued)
    g_queue[head] = std::move(j);
    g_qHead = next;
}

void runJob(const Job& j) {
    switch (j.kind) {
    case JobWeather:
        g_menu.setStatus(tw::game::SetWeather(j.text));
        break;
    case JobLevel:
        g_menu.setStatus(tw::game::SetLevel(j.text, static_cast<int>(j.value)));
        break;
    case JobShader:
        g_menu.setStatus(tw::game::SetShader(j.text));
        break;
    case JobWeatherAmount:
        g_menu.setStatus(tw::game::WeatherApplyAmount(j.value));
        break;
    case JobWeatherChance:
        g_menu.setStatus(tw::game::WeatherSetChance(j.value));
        break;
    case JobWeatherWetness:
        g_menu.setStatus(tw::game::WeatherSetWetness(j.value));
        break;
    case JobWeatherInterval:
        g_menu.setStatus(tw::game::WeatherSetInterval(static_cast<int>(j.value + 0.5f)));
        break;
    case JobWeatherLock:
        g_menu.setStatus(tw::game::WeatherSetLocked(j.flag));
        break;
    }
}

void drainJobs() {
    while (g_qTail != g_qHead) {
        const LONG tail = g_qTail;
        Job j = g_queue[tail];  // copy: slot may be reused after advance
        g_qTail = (tail + 1) % kQueueCap;
        runJob(j);
    }
}

// ---------------------------------------------------------------------------
// Overlay (Dear ImGui) + input
// ---------------------------------------------------------------------------

// IDXGISwapChain vtable slots (stable across DXGI versions).
constexpr int kVtblPresent = 8;
constexpr int kVtblResizeBuffers = 13;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn oPresent = nullptr;
ResizeFn oResize = nullptr;

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;
WNDPROC g_oldWndProc = nullptr;
bool g_imguiReady = false;
bool g_started = false;

bool keyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Returns true only on the frame the input goes from up to down.
bool edge(bool now, bool& last) {
    bool pressed = now && !last;
    last = now;
    return pressed;
}

void pollInput() {
    // Only react while the game window is focused, so other apps don't trigger it.
    if (GetForegroundWindow() != g_hwnd) return;

    static bool kInsert = false, kUp = false, kDown = false, kLeft = false, kRight = false,
                kEnter = false, kBack = false, kPageUp = false, kPageDown = false;
    static bool gStart = false, gUp = false, gDown = false, gLeft = false, gRight = false,
                gA = false, gB = false;

    if (edge(keyDown(VK_INSERT), kInsert)) { g_menu.handleKey(tw::Key::Toggle); TW_Log("Key: Insert (menu visible=%d)", g_menu.visible()); }

    // While a text field has the keyboard - the Spawn Ped name - every key belongs to it:
    // otherwise a Backspace would close the menu and an arrow key would change the weather
    // while you are editing a name. Insert still works, so the menu can be closed.
    //
    // The edges are still read (and the latch updated) while typing, they are only not
    // acted on: otherwise the Enter that ends a name would be seen as a fresh press on the
    // next frame, and the ped would be spawned twice.
    const bool typing = ImGui::GetIO().WantTextInput;
    const bool up = edge(keyDown(VK_UP), kUp);
    const bool down = edge(keyDown(VK_DOWN), kDown);
    const bool left = edge(keyDown(VK_LEFT), kLeft);
    const bool right = edge(keyDown(VK_RIGHT), kRight);
    const bool enter = edge(keyDown(VK_RETURN), kEnter);
    const bool back = edge(keyDown(VK_BACK), kBack);
    const bool pageUp = edge(keyDown(VK_PRIOR), kPageUp);
    const bool pageDown = edge(keyDown(VK_NEXT), kPageDown);
    if (!typing) {
        if (up) g_menu.handleKey(tw::Key::Up);
        if (down) g_menu.handleKey(tw::Key::Down);
        if (left) g_menu.handleKey(tw::Key::Left);
        if (right) g_menu.handleKey(tw::Key::Right);
        if (enter) g_menu.handleKey(tw::Key::Select);
        if (back) g_menu.handleKey(tw::Key::Back);
        // The sections are the menu's table of contents: one page key per section, so
        // SPAWN is one press away however long the list grows.
        if (pageUp) g_menu.jumpSection(-1);
        if (pageDown) g_menu.jumpSection(1);
    }

    XINPUT_STATE xs{};
    if (XInputGetState(0, &xs) == ERROR_SUCCESS) {
        WORD b = xs.Gamepad.wButtons;
        if (edge(b & XINPUT_GAMEPAD_START, gStart)) g_menu.handleKey(tw::Key::Toggle);
        if (edge(b & XINPUT_GAMEPAD_DPAD_UP, gUp)) g_menu.handleKey(tw::Key::Up);
        if (edge(b & XINPUT_GAMEPAD_DPAD_DOWN, gDown)) g_menu.handleKey(tw::Key::Down);
        if (edge(b & XINPUT_GAMEPAD_DPAD_LEFT, gLeft)) g_menu.handleKey(tw::Key::Left);
        if (edge(b & XINPUT_GAMEPAD_DPAD_RIGHT, gRight)) g_menu.handleKey(tw::Key::Right);
        if (edge(b & XINPUT_GAMEPAD_A, gA)) g_menu.handleKey(tw::Key::Select);
        if (edge(b & XINPUT_GAMEPAD_B, gB)) g_menu.handleKey(tw::Key::Back);
    }
}

LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    drainJobs();  // game-thread execution point for queued menu actions
    if (g_imguiReady) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
    // While the menu is open, keep keyboard keys from reaching the game.
    if (g_menu.visible() && (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR)) {
        return 0;
    }
    // While the free camera is flying the character must not act: the raw mouse (WM_INPUT)
    // and every key and click stop here. ImGui has already had the message above, and O or
    // P switch this off so the player can move, fight and look around again.
    if (tw::game::InputBlockSwallowMessage(msg)) {
        return 0;
    }
    return CallWindowProcW(g_oldWndProc, hwnd, msg, wp, lp);
}

// ---------- rendering ----------

void createRenderTarget(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void releaseRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}


// ---------------------------------------------------------------------------
// Background: the red dragon the menu is drawn on.
// ---------------------------------------------------------------------------

// The artwork is a PNG in the DLL's resources (freecam_menu.rc, RCDATA 2). Windows
// Imaging Component decodes it once, at startup, into a D3D11 texture; ImGui then draws
// it behind the menu rows with a fixed alpha, so the pattern is always the backdrop and
// never competes with the text.
constexpr int kBgResourceId = 2;       // the small pattern, the backdrop the menu shipped with
constexpr int kDragonResourceId = 3;   // the full-size dragon, for the tiled look
ID3D11ShaderResourceView* g_bgSrv = nullptr;
int g_bgW = 0;
int g_bgH = 0;
ID3D11ShaderResourceView* g_dragonSrv = nullptr;
int g_dragonW = 0;
int g_dragonH = 0;

// Every COM object is released on every exit path, including the failures: this runs
// inside the game's process, and a leak here is the game's leak.
// Decodes into whichever pair of globals is passed in, so the two pictures share one
// implementation rather than two copies that could disagree about the format.
bool decodePng(ID3D11Device* dev, const void* data, DWORD size,
               ID3D11ShaderResourceView** outSrv, int* outW, int* outH) {
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11Texture2D* texture = nullptr;
    bool ok = false;

    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) break;
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<void*>(data)),
                                                size))) break;
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand,
                                                    &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;

        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom))) break;

        std::vector<BYTE> pixels(static_cast<size_t>(w) * h * 4);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()),
                                         pixels.data()))) break;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = pixels.data();
        init.SysMemPitch = w * 4;
        if (FAILED(dev->CreateTexture2D(&desc, &init, &texture))) break;

        if (FAILED(dev->CreateShaderResourceView(texture, nullptr, outSrv))) break;
        *outW = static_cast<int>(w);
        *outH = static_cast<int>(h);
        ok = true;
    } while (false);

    if (texture) texture->Release();
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return ok;
}

// One of the two pictures, decoded on first use and kept. Both are in the DLL's resources,
// so nothing is read from disk and nothing can go missing from the folder.
bool loadArtwork(ID3D11Device* dev, int resourceId, ID3D11ShaderResourceView** srv,
                 int* w, int* h) {
    if (*srv) return true;
    if (!dev) return false;
    HINSTANCE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(resourceId),
                                    MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res) {
        TW_Log("Menu background: artwork %d is not in this DLL.", resourceId);
        return false;
    }
    HGLOBAL block = LoadResource(self, res);
    const void* bytes = block ? LockResource(block) : nullptr;
    const DWORD size = SizeofResource(self, res);
    if (!bytes || size == 0) {
        TW_Log("Menu background: artwork %d could not be read.", resourceId);
        return false;
    }
    if (!decodePng(dev, bytes, size, srv, w, h)) {
        TW_Log("Menu background: artwork %d could not be decoded; using the plain panel.",
               resourceId);
        return false;
    }
    TW_Log("Menu background: artwork %d loaded, %dx%d.", resourceId, *w, *h);
    return true;
}

// The appearance settings live next to the game, in FreeCam.menu.txt: one line, the format
// MenuStyle::save writes. Written at most once a second while something is being dragged,
// so a slider does not turn into a file write per frame. A missing or unreadable file is
// not an error - it means the shipped look.
std::string stylePath() {
    wchar_t exe[MAX_PATH] = { 0 };
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n);
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.resize(slash + 1);
    path += L"FreeCam.menu.txt";
    // Narrow it: the file only ever holds ASCII, and everything else here is narrow.
    std::string narrow;
    for (wchar_t c : path) narrow += (c < 128) ? static_cast<char>(c) : '?';
    return narrow;
}

void loadStyle() {
    const std::string path = stylePath();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    char buf[256] = { 0 };
    const size_t got = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    const std::string text(buf, got);
    if (g_style.load(text)) {
        TW_Log("Menu style: loaded from %s: %s", path.c_str(), g_style.save().c_str());
    } else {
        TW_Log("Menu style: %s is not a style this build understands; using the shipped look",
               path.c_str());
    }
}

void saveStyleIfDirty() {
    if (!g_styleDirty) return;
    const ULONGLONG now = GetTickCount64();
    if (g_styleSavedAt && now - g_styleSavedAt < 1000) return;   // debounce a dragged slider
    g_styleSavedAt = now;
    g_styleDirty = false;
    const std::string path = stylePath();
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const std::string text = g_style.save() + "\n";
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

void loadBackground(ID3D11Device* dev) {
    loadArtwork(dev, kBgResourceId, &g_bgSrv, &g_bgW, &g_bgH);
    loadArtwork(dev, kDragonResourceId, &g_dragonSrv, &g_dragonW, &g_dragonH);
}

void initImGui(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev)))) return;
    g_dev->GetImmediateContext(&g_ctx);

    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    createRenderTarget(sc);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    // Chinese dragon theme: deep crimson + gold on dark.
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.95f, 0.85f, 0.50f, 1.00f);          // gold
    colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.50f, 0.30f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.03f, 0.04f, 0.95f);      // dark crimson
    colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.04f, 0.05f, 0.95f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.05f, 0.06f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.60f, 0.40f, 0.10f, 0.80f);        // dark gold
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.08f, 0.09f, 0.80f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.12f, 0.13f, 0.80f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.40f, 0.15f, 0.16f, 0.90f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.25f, 0.06f, 0.07f, 1.00f);       // deep crimson
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.45f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.20f, 0.05f, 0.06f, 0.80f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.20f, 0.06f, 0.07f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.08f, 0.04f, 0.05f, 0.80f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.35f, 0.15f, 0.16f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.45f, 0.20f, 0.21f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.55f, 0.25f, 0.26f, 0.90f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.90f, 0.70f, 0.20f, 1.00f);     // gold
    colors[ImGuiCol_SliderGrab] = ImVec4(0.80f, 0.60f, 0.20f, 1.00f);   // gold
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.80f, 0.30f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.30f, 0.08f, 0.10f, 0.80f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.12f, 0.14f, 0.90f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.55f, 0.18f, 0.20f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.30f, 0.08f, 0.10f, 0.80f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.40f, 0.12f, 0.14f, 0.90f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.50f, 0.18f, 0.20f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.60f, 0.40f, 0.10f, 0.80f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.80f, 0.60f, 0.20f, 0.90f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(1.00f, 0.80f, 0.30f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.30f, 0.10f, 0.12f, 0.50f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.45f, 0.15f, 0.17f, 0.80f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.55f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.25f, 0.08f, 0.10f, 0.80f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.40f, 0.15f, 0.17f, 0.90f);
    colors[ImGuiCol_TabActive] = ImVec4(0.35f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.15f, 0.05f, 0.06f, 0.80f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.25f, 0.08f, 0.10f, 0.90f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.90f, 0.70f, 0.20f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.80f, 0.30f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.80f, 0.60f, 0.20f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.80f, 0.30f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.25f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.60f, 0.40f, 0.10f, 0.80f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.30f, 0.15f, 0.10f, 0.60f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.10f, 0.05f, 0.06f, 0.40f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.50f, 0.20f, 0.22f, 0.60f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 0.80f, 0.30f, 1.00f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.90f, 0.70f, 0.20f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 0.80f, 0.30f, 0.80f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);

    // WIC needs COM on this thread; the game already has it up in most builds, and
    // RPC_E_CHANGED_MODE just means it is in the other apartment model.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    loadStyle();          // before the first frame, so the menu opens in the saved look
    loadBackground(g_dev);

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    g_oldWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));
    g_imguiReady = true;
    TW_Log("OK: ImGui initialized (hwnd=%p).", g_hwnd);
}

// The little hotkey display in the top-right corner. It only exists for a couple of
// seconds after a key was pressed and then fades away, so it never sits on the screen
// while the player is just flying around.
void renderHotkeyHud() {
    tw::game::FreeCamHudLine lines[8];
    const int n = tw::game::FreeCamHudLines(lines, 8);
    // While noclip is on the corner says so all the time, not just for a second after the
    // key: a mode that moves the character is worth being able to see at a glance.
    const bool noclip = tw::game::NoclipActive();
    if (n <= 0 && !noclip) return;

    float freshest = 0.0f;
    for (int i = 0; i < n; ++i) if (lines[i].alpha > freshest) freshest = lines[i].alpha;
    if (noclip) freshest = 1.0f;

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 14.0f, 14.0f), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.35f * freshest);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 2.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.06f, 1.0f));
    ImGui::Begin("##freecam_hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoFocusOnAppearing);
    if (noclip) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.38f, 1.0f));
        ImGui::TextUnformatted("NOCLIP");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.94f, 0.97f, 1.0f));
        ImGui::Text("%s   WASD / Space / C   [ ] speed",
                    tw::game::NoclipSpeedLabels()[tw::game::NoclipSpeedIndex()]);
        ImGui::PopStyleColor();
        if (n > 0) ImGui::Separator();
    }
    for (int i = 0; i < n; ++i) {
        const float a = lines[i].alpha;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.87f, 0.35f, a));
        ImGui::TextUnformatted(lines[i].key);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.94f, 0.97f, 0.9f * a));
        ImGui::TextUnformatted(lines[i].text);
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}


// One row of the menu. The label sits on the left, the current state on the right, and
// the whole row is clickable: the first click puts the cursor on it, a second one acts,
// which is how the keyboard already behaves (arrows move, Enter acts). The right mouse
// button is the arrows' Left/Right, for the two kinds that have a value.
const char* rowValue(const tw::MenuItem& it) {
    static char buf[64];
    switch (it.kind) {
    case tw::ItemKind::Toggle:
        return it.toggled ? "ON" : "OFF";
    case tw::ItemKind::Slider:
        std::snprintf(buf, sizeof buf, "%d", static_cast<int>(it.value));
        return buf;
    case tw::ItemKind::Selector:
        if (it.options.empty()) return "-";
        return it.options[it.optionIndex < it.options.size() ? it.optionIndex : 0].c_str();
    case tw::ItemKind::Info:
        return it.text.c_str();
    default:
        return "";
    }
}

// One colour of a theme as ImGui wants it.
ImVec4 colour(const float c[4]) { return ImVec4(c[0], c[1], c[2], c[3]); }

void renderMenu() {
    const tw::MenuColours& theme = g_style.colours();
    const ImVec4 gold = colour(theme.accent);
    const ImVec4 bone = colour(theme.text);
    const ImVec4 red = colour(theme.header);
    const ImVec4 cyan = colour(theme.status);
    // A dimmed accent for the second line of a title; no separate setting for it, because
    // there is nothing a player would want to say with it that the accent does not say.
    const ImVec4 goldDim(theme.accent[0] * 0.78f, theme.accent[1] * 0.78f,
                         theme.accent[2] * 0.78f, theme.accent[3]);
    // Text size applies to every row: a window over a 4K game needs to be readable and a
    // window over a laptop does not need to be huge.
    ImGui::SetWindowFontScale(g_style.scale());

    // The menu is taller than it used to be, and a window sized to its contents simply
    // grows past the bottom of the screen - where the last sections cannot be read and
    // cannot be scrolled to either, because there is nothing to scroll. So the window is
    // given a height that fits whatever the game is running at, and everything else
    // scrolls.
    const ImGuiIO& io = ImGui::GetIO();
    const float screenH = io.DisplaySize.y > 200.0f ? io.DisplaySize.y : 720.0f;
    const float maxH = screenH - 56.0f;                 // title bar and the window border
    const float wantH = maxH < 760.0f ? maxH : 760.0f;  // never taller than a short screen
    ImGui::SetNextWindowPos(ImVec2(26, 26), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, wantH), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 220), ImVec2(1000, maxH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    ImGui::Begin("TriadMenu", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    // The artwork, behind everything the window draws. Two pictures and four ways of
    // showing them (see MenuBackground): the dragon stretched to cover the window, the same
    // picture tiled at its own size, the small pattern tiled, or nothing but the panel. The
    // panel colour is always painted first, so a picture that failed to decode - or was
    // turned off - still leaves something to read against.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    // The content region, not the whole window: the window's own draw list is painted
    // after its title bar, so an image over the window rect would sit on top of the name.
    const ImVec2 cmin = ImGui::GetWindowContentRegionMin();
    const ImVec2 cmax = ImGui::GetWindowContentRegionMax();
    const ImVec2 ws(cmax.x - cmin.x, cmax.y - cmin.y);
    const ImVec2 origin(wp.x + cmin.x, wp.y + cmin.y);
    if (ws.x > 1.0f && ws.y > 1.0f) {
        // The panel first: it is what the text is really readable against, and the picture
        // is a tint on top of it.
        dl->AddRectFilled(origin, ImVec2(origin.x + ws.x, origin.y + ws.y),
                          ImGui::GetColorU32(colour(theme.panel)), 4.0f);

        const int alpha = static_cast<int>(g_style.backgroundAlpha() * 255.0f + 0.5f);
        // The indices are the model's, in the order MenuBackgroundNames() lists them.
        const int mode = g_style.background();
        const bool dragon = mode == static_cast<int>(tw::MenuBackground::Dragon) ||
                            mode == static_cast<int>(tw::MenuBackground::DragonTiled);
        const bool tiled = mode == static_cast<int>(tw::MenuBackground::DragonTiled) ||
                           mode == static_cast<int>(tw::MenuBackground::Pattern);
        ImTextureID tex = static_cast<ImTextureID>(0);
        int texW = 0, texH = 0;
        if (dragon) {
            tex = reinterpret_cast<ImTextureID>(g_dragonSrv);
            texW = g_dragonW;
            texH = g_dragonH;
        } else if (tiled) {
            tex = reinterpret_cast<ImTextureID>(g_bgSrv);
            texW = g_bgW;
            texH = g_bgH;
        }

        if (tex && texW > 0 && texH > 0 && alpha > 0) {
            const ImU32 tint = IM_COL32(255, 255, 255, alpha);
            if (!tiled) {
                // Cover-fit: the whole picture, cropped to the window's shape so it is
                // never stretched out of proportion.
                ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
                const float imgAspect = static_cast<float>(texW) / static_cast<float>(texH);
                const float winAspect = ws.x / ws.y;
                if (winAspect > imgAspect) {
                    const float keep = imgAspect / winAspect;
                    uv0.y = (1.0f - keep) * 0.5f;
                    uv1.y = 1.0f - uv0.y;
                } else {
                    const float keep = winAspect / imgAspect;
                    uv0.x = (1.0f - keep) * 0.5f;
                    uv1.x = 1.0f - uv0.x;
                }
                dl->AddImageRounded(tex, origin, ImVec2(origin.x + ws.x, origin.y + ws.y),
                                    uv0, uv1, tint, 4.0f);
            } else {
                // Tiled at a size worked out from the picture and the text scale, so the
                // pattern keeps its proportions at every window size. Clipped by hand: the
                // draw list has no scissor here, and a tile that ran past the window would
                // be painted over the game.
                const float tile = (mode == static_cast<int>(tw::MenuBackground::Pattern)
                                        ? 240.0f : 380.0f) *
                                   g_style.scale();
                const float stepX = tile;
                const float stepY = tile * static_cast<float>(texH) / static_cast<float>(texW);
                const ImVec2 clipMax(origin.x + ws.x, origin.y + ws.y);
                for (float y = origin.y; y < clipMax.y; y += stepY) {
                    for (float x = origin.x; x < clipMax.x; x += stepX) {
                        dl->AddImage(tex, ImVec2(x, y),
                                     ImVec2(std::min(x + stepX, clipMax.x),
                                            std::min(y + stepY, clipMax.y)),
                                     ImVec2(0.0f, 0.0f),
                                     ImVec2(std::min(1.0f, (clipMax.x - x) / stepX),
                                            std::min(1.0f, (clipMax.y - y) / stepY)),
                                     tint);
                    }
                }
            }

            // The veil goes on top of the artwork, deliberately. At any useful contrast the
            // dragon is loud enough to swallow the labels, and which part of it lands behind
            // a row is not something the player can control - it depends on the window size.
            // Knocking the whole picture back by the same amount gives the text one surface
            // to sit on, whichever part of the dragon is underneath. It rises with the
            // contrast, so the picture is allowed to be loudest exactly when it is asked to
            // be; the row colours above it never change.
            const float veil = 0.24f + 0.40f * g_style.backgroundAlpha();
            dl->AddRectFilled(origin, ImVec2(origin.x + ws.x, origin.y + ws.y),
                              ImGui::GetColorU32(ImVec4(theme.panel[0], theme.panel[1],
                                                        theme.panel[2], veil)), 4.0f);
        }
    }

    // Title block: the name, a rule, and where the keys are.
    ImGui::PushStyleColor(ImGuiCol_Text, gold);
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted("TriadMenu");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, goldDim);
    ImGui::TextUnformatted("Triad Wars debug menu");
    ImGui::PopStyleColor();
    ImGui::Separator();

    // Rows sit flush against each other, so the selection reads as one bar rather than a
    // row with gaps either side of it.
    // The row has to grow with the text: a fixed height clips a larger font, and the slider
    // exists precisely so the text can be made larger.
    const float rowHeight = 26.0f * g_style.scale();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 1.0f));
    const auto& items = g_menu.items();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const tw::MenuItem& it = items[i];

        if (it.header) {
            ImGui::Dummy(ImVec2(1.0f, 4.0f));
            // Mixed toward the accent: a theme whose heading colour is close to the artwork
            // (Triad's crimson over a crimson dragon) would otherwise disappear into it.
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(theme.header[0] * 0.55f + theme.accent[0] * 0.45f,
                                         theme.header[1] * 0.55f + theme.accent[1] * 0.45f,
                                         theme.header[2] * 0.55f + theme.accent[2] * 0.45f, 1.0f));
            ImGui::TextUnformatted(it.label.c_str());
            ImGui::PopStyleColor();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(p.x, p.y + 1.0f), ImVec2(p.x + rowWidth, p.y + 1.0f),
                        ImGui::GetColorU32(ImVec4(theme.header[0], theme.header[1],
                                                  theme.header[2], 0.55f)), 1.0f);
            ImGui::Dummy(ImVec2(1.0f, 2.0f));
            continue;
        }

        // A value the game owns and the menu only shows. No selection bar, no hover, no
        // click: the cursor steps over it, so it cannot be "used" by accident.
        if (it.kind == tw::ItemKind::Info) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(rowWidth, rowHeight));
            const float textY = p.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
            dl->AddText(ImVec2(p.x + 12.0f, textY),
                        ImGui::GetColorU32(ImVec4(theme.text[0] * 0.74f, theme.text[1] * 0.74f,
                                                  theme.text[2] * 0.74f, 1.0f)), it.label.c_str());
            if (!it.text.empty()) {
                const ImVec2 size = ImGui::CalcTextSize(it.text.c_str());
                dl->AddText(ImVec2(p.x + rowWidth - size.x - 12.0f, textY),
                            ImGui::GetColorU32(cyan), it.text.c_str());
            }
            continue;
        }

        const bool selected = (i == g_menu.selectedIndex());
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None,
                          ImVec2(rowWidth, rowHeight));
        const bool hovered = ImGui::IsItemHovered();
        const bool leftClick = ImGui::IsItemClicked(0);
        const bool rightClick = ImGui::IsItemClicked(1);
        ImGui::PopID();

        // Selection bar.
        if (selected) {
            dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + rowWidth, p.y + rowHeight),
                              ImGui::GetColorU32(colour(theme.selected)), 4.0f);
            dl->AddRectFilled(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x + 2.5f, p.y + rowHeight - 2.0f),
                              ImGui::GetColorU32(gold), 1.0f);
            // The cursor is walked with the arrow keys, so the row it lands on has to be
            // brought into view - otherwise Down past the bottom edge looks like the menu
            // ended. Only when the selection changes, so the wheel is still the wheel.
            static std::size_t shown = static_cast<std::size_t>(-1);
            if (shown != i) {
                shown = i;
                ImGui::SetScrollHereY(0.42f);
            }
        } else if (hovered) {
            dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + rowWidth, p.y + rowHeight),
                              ImGui::GetColorU32(ImVec4(0.45f, 0.10f, 0.10f, 0.28f)), 4.0f);
        }

        const float textY = p.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(p.x + 12.0f, textY), ImGui::GetColorU32(selected ? bone : gold),
                    it.label.c_str());

        // A text entry draws its own field instead of a value on the right: what is typed
        // there is the name that will be spawned.
        if (it.kind == tw::ItemKind::Input) {
            const float fieldX = p.x + 12.0f + ImGui::CalcTextSize(it.label.c_str()).x + 14.0f;
            const float fieldW = p.x + rowWidth - 10.0f - fieldX;
            if (fieldW > 60.0f) {
                char buf[128];
                std::snprintf(buf, sizeof buf, "%s", it.text.c_str());
                ImGui::SetCursorScreenPos(ImVec2(fieldX, p.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f));
                ImGui::SetNextItemWidth(fieldW);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 3.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.04f, 0.05f, 0.90f));
                ImGui::PushID(static_cast<int>(i) + 1000);
                const bool entered =
                    ImGui::InputText("##text", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue);
                const bool changed = ImGui::IsItemEdited();
                const bool active = ImGui::IsItemActive();
                ImGui::PopID();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                if (active) g_menu.select(i);          // typing here means working on this entry
                if (changed) g_menu.setInputText(it.label, buf);
                if (entered) {
                    // The field gave the keyboard back and the name is committed; the
                    // Enter is the "do it" that every other entry uses.
                    g_menu.setInputText(it.label, buf);
                    g_menu.select(i);
                    g_menu.handleKey(tw::Key::Select);
                }
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowHeight));
            }
            if (leftClick) g_menu.select(i);
            continue;
        }

        const char* value = rowValue(it);
        if (value && *value) {
            const ImVec2 size = ImGui::CalcTextSize(value);
            // ON is the theme's status colour and OFF its dimmed text: the colour that says
            // "running" is the same one that says "this is what the camera is doing".
            const ImU32 col = ImGui::GetColorU32(
                it.kind == tw::ItemKind::Toggle
                    ? (it.toggled ? colour(theme.status)
                                  : ImVec4(theme.text[0] * 0.60f, theme.text[1] * 0.60f,
                                           theme.text[2] * 0.60f, 1.0f))
                    : gold);
            dl->AddText(ImVec2(p.x + rowWidth - size.x - 12.0f, textY), col, value);
        }

        if (leftClick) {
            if (selected) g_menu.handleKey(tw::Key::Select);
            else g_menu.select(i);
        }
        if (rightClick && selected) g_menu.handleKey(tw::Key::Right);
    }
    ImGui::PopStyleVar();

    // Status line: what the last action did.
    ImGui::Dummy(ImVec2(1.0f, 2.0f));
    ImGui::Separator();
    if (!g_menu.status().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cyan);
        ImGui::TextWrapped("%s", g_menu.status().c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, goldDim);
        ImGui::TextUnformatted("Pick an entry with the arrow keys and Enter.");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(1.0f, 2.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(theme.text[0] * 0.70f, theme.text[1] * 0.70f,
                                                theme.text[2] * 0.70f, 1.0f));
    ImGui::TextUnformatted("Up/Down move   Left/Right change   Enter apply");
    ImGui::TextUnformatted("PageUp/PageDown jump between sections   mouse wheel scrolls");
    ImGui::TextUnformatted("Right click = right arrow   Insert / Backspace close");
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleVar();
}

// Text centred on x, with a shadow one pixel down: the label hangs over whatever the game
// is drawing (a dark alley, a bright sky) and has to read on both.
void centredText(ImDrawList* dl, float x, float y, ImU32 col, const char* text, float scale) {
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * scale;
    const ImVec2 ts = font->CalcTextSizeA(size, 100000.0f, 0.0f, text);
    const ImVec2 p(x - ts.x * 0.5f, y);
    dl->AddText(font, size, ImVec2(p.x + 1.0f, p.y + 1.0f), IM_COL32(0, 0, 0, 190), text);
    dl->AddText(font, size, p, col, text);
}

// What the next spawn would be: the name in the menu's field, because that is what the
// Spawn Ped entry acts on; if the field has been emptied, the model the B key would use.
std::string spawnPreviewModel() {
    const tw::MenuItem* it = g_menu.item("Spawn Ped");
    if (it && !it->text.empty()) return it->text;
    const std::vector<std::string>& models = tw::game::SpawnModels();
    const int index = tw::game::SpawnModelIndex();
    if (index >= 0 && index < static_cast<int>(models.size())) return models[index];
    return std::string();
}

// The sky paint: the colour the SKY section chose, laid over the top of the frame and
// faded out before the street. It goes into the background draw list with the hologram, so
// it sits over the game and under the menu, and it is drawn in bands because a draw list
// has no gradient of its own - sixty-four of them across the top half is smooth to the eye
// and costs nothing worth measuring.
//
// The shape is the shape of a sky: strongest at the very top of the frame, gone by 62% of
// the way down, so what the paint touches is the sky and the tops of the towers, not the
// pavement or the HUD along the bottom.
void renderSkyPaint() {
    float r = 1.0f, g = 1.0f, b = 1.0f, alpha = 0.0f;
    if (!tw::game::SkyPaint(&r, &g, &b, &alpha)) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x;
    const float h = io.DisplaySize.y;
    if (w < 8.0f || h < 8.0f) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const int bands = 64;
    const float bottom = h * 0.62f;
    for (int i = 0; i < bands; ++i) {
        const float t0 = static_cast<float>(i) / bands;
        const float t1 = static_cast<float>(i + 1) / bands;
        // Squared falloff: a sky is mostly at the top of the picture, and a straight ramp
        // reads as a coloured band across the middle of the screen.
        const float f0 = (1.0f - t0) * (1.0f - t0);
        const float f1 = (1.0f - t1) * (1.0f - t1);
        const ImU32 c0 = ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, alpha * f0));
        const ImU32 c1 = ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, alpha * f1));
        dl->AddRectFilledMultiColor(ImVec2(0.0f, bottom * t0), ImVec2(w, bottom * t1), c0, c0,
                                    c1, c1);
    }
}

// The spawn hologram: a wireframe NPC standing where the next spawn would put one, with
// its model name over its head. It goes into the background draw list, so it sits over the
// game and under the menu - the menu can be open on top of it and the figure is still
// where it will be.
//
// Built fresh every frame from the camera's own matrices and the spawner's own transform,
// which is why it tracks the world when you move rather than sliding around with the
// screen: those are the two things it is meant to show.
void renderSpawnHologram() {
    if (!tw::game::HologramPreviewActive()) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x;
    const float h = io.DisplaySize.y;
    if (w < 8.0f || h < 8.0f) return;

    tw::game::HoloFrame frame;
    if (tw::game::HologramPropMode()) {
        // The prop figure: a crate on the same spot, labelled with the prop's own name, so
        // the two previews answer the same question for the two spawn keys.
        const std::vector<std::string> props = tw::game::PropModels();
        const int index = tw::game::PropModelIndex();
        const std::string name = (index >= 0 && index < static_cast<int>(props.size()))
                                     ? props[index]
                                     : std::string();
        tw::game::HologramBuildPropFrame(name, w, h, &frame);
    } else {
        tw::game::HologramBuildFrame(spawnPreviewModel(), w, h, &frame);
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!frame.visible) {
        // No figure yet: say why, in the same words the module would have put in the menu's
        // status line, rather than showing nothing and leaving it a mystery.
        const char* why = frame.note[0] ? frame.note : "spawn preview: nothing to draw";
        centredText(dl, w * 0.5f, h * 0.70f, IM_COL32(255, 208, 120, 215), why, 1.0f);
        return;
    }

    // Two passes - a wide soft glow under a thin bright line - so the figure reads against
    // both a dark alley and a bright sky without being so loud it hides the game.
    for (int pass = 0; pass < 2; ++pass) {
        const float thickness = pass == 0 ? 4.0f : 1.5f;
        const ImU32 colour = pass == 0 ? IM_COL32(60, 220, 255, 55) : IM_COL32(150, 245, 255, 220);
        for (int i = 0; i < frame.segCount; ++i) {
            const tw::game::HoloSegment& s = frame.segs[i];
            dl->AddLine(ImVec2(s.ax, s.ay), ImVec2(s.bx, s.by), colour, thickness);
        }
    }

    // The name goes over the head and the distance under the feet: two answers, both stuck
    // to the figure, neither in a corner where it could belong to another NPC.
    const float font = ImGui::GetFontSize();
    float nameY = frame.headY - font * 1.7f;
    if (nameY < 4.0f) nameY = 4.0f;
    centredText(dl, frame.headX, nameY, IM_COL32(215, 250, 255, 245), frame.model, 1.12f);

    char below[96];
    std::snprintf(below, sizeof below, "%.0f m ahead - %s",
                  static_cast<double>(tw::game::HologramDistance()),
                  tw::game::SpawnHostile() ? "hostile" : "friendly");
    centredText(dl, frame.feetX, frame.feetY + font * 0.45f, IM_COL32(190, 235, 245, 220),
                below, 0.95f);

    // The feet, so the exact spot is visible once the ring is only a few pixels across.
    dl->AddCircle(ImVec2(frame.feetX, frame.feetY), 3.0f, IM_COL32(160, 245, 255, 200), 12, 1.5f);
}

void renderFrame() {
    const bool menuOn = g_menu.visible();
    // The APPEARANCE rows write g_style directly, so the only way to notice a change is to
    // compare. Saving is debounced inside saveStyleIfDirty.
    {
        static std::string last = g_style.save();
        const std::string now = g_style.save();
        if (now != last) {
            last = now;
            g_styleDirty = true;
            saveStyleIfDirty();
        }
    }
    tw::game::FreeCamHudLine probe[1];
    const bool hudOn = tw::game::FreeCamHudLines(probe, 1) > 0;
    const bool previewOn = tw::game::HologramPreviewActive();
    const bool paintOn = tw::game::SkyPaintActive();
    if (!menuOn && !hudOn && !tw::game::NoclipActive() && !previewOn && !paintOn) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // First, so everything else draws on top of it: the sky goes under the hologram and
    // under the menu, over the game, which is where the sky is.
    renderSkyPaint();
    renderSpawnHologram();

    if (menuOn) renderMenu();

    renderHotkeyHud();

    ImGui::Render();
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_imguiReady) {
        initImGui(sc);
    }
    if (g_imguiReady) {
        pollInput();
        // The model list starts as the executable's handful. A scan of the installed data
        // is what turns it into the game's own list, so it is started once, a little after
        // the game is up - early enough to be ready before the menu is first opened, late
        // enough not to compete with the loading screens for the disk.
        {
            static ULONGLONG firstFrame = 0;
            static bool scanStarted = false;
            const ULONGLONG now = GetTickCount64();
            if (!firstFrame) firstFrame = now;
            if (!scanStarted && now - firstFrame > 8000) {
                scanStarted = tw::game::SpawnScanAsync() || scanStarted;
            }
        }
        if (g_syncState) g_syncState();   // hotkeys can change what the menu shows
        // The camera thread reads the keyboard directly, so it has to be told when the
        // menu is the one using it - otherwise typing a name in Spawn Ped would also fly
        // the camera, spawn the hotkey ped and toggle noclip.
        tw::game::FreeCamSetMenuOpen(g_menu.visible());
        drainJobs();   // run queued actions on the game thread, then show results
        // The sky override is a per-frame job rather than a queued one: the game picks its
        // own sky whenever it likes, so the chosen variant is asked for again every frame
        // it moves. The colour needs no tick - it is drawn with the menu's own frame.
        tw::game::SkyTick();
        // Camera actions are carried out by the camera thread, so their answer arrives on
        // its own schedule; whenever one lands it takes the status line.
        {
            std::string why;
            if (tw::game::FreeCamTakeActionStatus(&why) && !why.empty()) g_menu.setStatus(why);
        }
        renderFrame();
    }
    return oPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                          DXGI_FORMAT fmt, UINT flags) {
    releaseRenderTarget();
    HRESULT hr = oResize(sc, count, w, h, fmt, flags);
    if (SUCCEEDED(hr) && g_dev) createRenderTarget(sc);
    return hr;
}

// ---------- hook installation ----------

// Creates a throwaway D3D11 device and swap chain to read the swap chain vtable.
// All DXGI swap chains share this vtable, so patching it covers the game's chain.
bool readSwapChainVtable(void*** outVtbl) {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "TWDummyWindow";
    RegisterClassExA(&wc);
    HWND dummy = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPED, 0, 0, 100, 100,
                                 nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               &level, 1, D3D11_SDK_VERSION, &sd, &sc, &dev,
                                               nullptr, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
                                           D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    }
    bool ok = SUCCEEDED(hr) && sc;
    if (ok) *outVtbl = *reinterpret_cast<void***>(sc);

    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(dummy);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return ok;
}

void patchSlot(void** slot, void* newFn, void** oldFn) {
    DWORD oldProt;
    VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
    *oldFn = *slot;
    *slot = newFn;
    VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
}

DWORD WINAPI installThread(LPVOID) {
    void** vtbl = nullptr;
    if (!readSwapChainVtable(&vtbl)) {
        TW_Log("ERROR: could not create a D3D11 device to find the swap chain. Game may use another graphics API.");
        return 1;
    }
    patchSlot(&vtbl[kVtblPresent], reinterpret_cast<void*>(&hkPresent),
              reinterpret_cast<void**>(&oPresent));
    patchSlot(&vtbl[kVtblResizeBuffers], reinterpret_cast<void*>(&hkResizeBuffers),
              reinterpret_cast<void**>(&oResize));
    TW_Log("OK: Present and ResizeBuffers hooks installed.");
    return 0;
}

// ---------- menu wiring ----------

// One line describing the model list, for the "Models found" row. Free rather than a
// method so the per-frame sync and the hook that fills the row agree by construction.
std::string pedModelSummaryText() {
    char buf[96];
    const int total = tw::game::SpawnModelCount();
    const int scanned = tw::game::SpawnScannedModelCount();
    if (tw::game::SpawnScanRunning())
        std::snprintf(buf, sizeof buf, "scanning... %d known", total);
    else if (scanned > 0)
        std::snprintf(buf, sizeof buf, "%d known, %d from the game files", total, scanned);
    else
        std::snprintf(buf, sizeof buf, "%d known - Rescan reads the game files", total);
    return std::string(buf);
}

// The same line for the prop list: how many there are, and how many came out of the game
// files rather than out of the executable's own handful.
std::string propModelSummaryText() {
    char buf[96];
    const int total = tw::game::PropModelCount();
    const int scanned = tw::game::PropScannedPropCount();
    if (tw::game::PropScanRunning())
        std::snprintf(buf, sizeof buf, "scanning... %d known", total);
    else if (scanned > 0)
        std::snprintf(buf, sizeof buf, "%d known, %d from the game files", total, scanned);
    else
        std::snprintf(buf, sizeof buf, "%d known - Rescan reads the game files", total);
    return std::string(buf);
}

void buildMenu() {
    tw::DebugHooks hooks;
    hooks.style = &g_style;         // the APPEARANCE rows write what renderMenu reads
    // Only what the menu still offers. The camera, the spawner and noclip keep their
    // hotkeys (see freecam_game.cpp); the menu is for the things that are easier to read
    // than to remember, and for the actions that write a save file.
    // The weather is a continuous 0..2 amount in the game, so the presets are shortcuts
    // and the sliders are the real control (see freecam_weather.h).
    hooks.weatherPresets = tw::game::WeatherModePresets();
    hooks.levelTargets = tw::game::LevelTargets();
    hooks.shaderPresets = tw::game::ShaderPresets();

    hooks.setWeather = [](const std::string& n) {
        pushJob(Job{JobWeather, n, 0.0f, false});
        return std::string("Weather: queued");
    };
    hooks.setWeatherAmount = [](float v) {
        pushJob(Job{JobWeatherAmount, std::string(), v, false});
        return std::string("Rain amount: queued");
    };
    hooks.setWeatherChance = [](float v) {
        pushJob(Job{JobWeatherChance, std::string(), v, false});
        return std::string("Rain chance: queued");
    };
    hooks.setWeatherWetness = [](float v) {
        pushJob(Job{JobWeatherWetness, std::string(), v, false});
        return std::string("Wetness: queued");
    };
    hooks.setWeatherInterval = [](int seconds) {
        pushJob(Job{JobWeatherInterval, std::string(), static_cast<float>(seconds), false});
        return std::string("Weather interval: queued");
    };
    hooks.setWeatherLock = [](bool on) {
        pushJob(Job{JobWeatherLock, std::string(), 0.0f, on});
        return std::string("Weather lock: queued");
    };
    // The values the sliders start at, so the menu opens showing what the game is
    // actually doing rather than zero.
    hooks.weatherAmount = []() { return tw::game::WeatherAmount(); };
    hooks.weatherChance = []() { return tw::game::WeatherChance(); };
    hooks.weatherWetness = []() { return tw::game::WeatherWetness(); };
    hooks.weatherLocked = []() { return tw::game::WeatherLocked(); };
    // The sky. The settings object is shared with the K and Y hotkeys and with the tick
    // in freecam_sky.cpp, so a switch made in the menu and one made on the keyboard are
    // the same switch. The variant list is the game's own skydome list, empty until the
    // world loads, which syncState fills in.
    hooks.sky = &tw::game::Sky();
    hooks.skyVariants = tw::game::SkyVariantNames();
    hooks.skyState = []() { return tw::game::SkyStateLine(); };
    hooks.setFreeCam = [](bool on) { return tw::game::FreeCamSetActive(on); };
    hooks.setLevel = [](const std::string& t, int l) {
        pushJob(Job{JobLevel, t, static_cast<float>(l), false});
        return std::string("Level: queued");
    };
    hooks.setShader = [](const std::string& n) {
        pushJob(Job{JobShader, n, 0.0f, false});
        return std::string("ReShade preset: queued");
    };
    // Unlock all is two halves: the content (vehicles, weapons, upgrades, levels) and the
    // opportunities (rivals off cooldown, every job on the board, the event running).
    // Neither is much use alone, so the one entry does both and reports both. The
    // content half is the older unlock, which needs the game's extracted gamedb.json;
    // the event half grants the same content from this server's own catalogues, so a
    // failure there is not worth showing - the bags are filled either way.
    hooks.unlockAllContent = []() {
        std::string content = tw::game::UnlockAllContent();
        std::string all = tw::game::UnlockEventsAndActivities(true);
        if (content.rfind("UnlockAll failed: gamedb.json", 0) == 0) content.clear();
        return content.empty() ? all : all + "   |   " + content;
    };
    hooks.unlockRaids = []() { return tw::game::UnlockRaidsAndEvents(); };
    // The wardrobe. Every row names a piece from the game's own clothing table (compiled
    // into this build from the installed data), and wearing one rewrites the profile's
    // piece list in the slot that was picked - which is the only way to put an outfit's
    // own trousers under another outfit's top. The client reads the list when it loads
    // its bags, so the menu says so instead of pretending the next frame changes.
    hooks.wardrobeTops = tw::game::WardrobeTopNames();
    hooks.wardrobePants = tw::game::WardrobePantsNames();
    hooks.wardrobeLooks = tw::game::WardrobeLookNames();
    hooks.wearTop = [](const std::string& name) { return tw::game::WardrobeWearTop(name); };
    hooks.wearPants = [](const std::string& name) { return tw::game::WardrobeWearPants(name); };
    hooks.wearCombo = [](const std::string& top, const std::string& pants) {
        return tw::game::WardrobeWearCombo(top, pants);
    };
    hooks.wearLook = [](const std::string& name) { return tw::game::WardrobeWearLook(name); };
    hooks.wardrobeUndo = []() { return tw::game::WardrobeUndo(); };
    hooks.wardrobeStatus = []() { return tw::game::WardrobeStatus(); };
    // Spawn Ped: a character named in the menu, put three metres in front of the player.
    // The names offered in the list are whatever the spawner knows - the built-ins, plus
    // anything in FreeCam.npc.txt - and the field accepts any other property set the game
    // knows, which is how a ped that is in nobody's list can still be spawned.
    {
        const std::vector<std::string>& peds = tw::game::SpawnModels();
        hooks.pedNames = peds;
        if (!peds.empty()) hooks.pedDefault = peds.front();
    }
    hooks.spawnPed = [](const std::string& model) {
        // Spawning sets the hotkey model as well, so B spawns what the menu just did.
        tw::game::SpawnSelectByName(model);
        // The distance the preview is standing at, so moving that slider moves the figure
        // and the NPC together - a preview that stayed at 3 m while the spawn went further
        // would be worse than no preview.
        const float metres = tw::game::HologramDistance();
        tw::game::SpawnRequestAt(model, tw::game::SpawnHostile(), metres);
        char line[200];
        // A spawn asked for before the player exists waits for up to eight seconds and then
        // gives up; saying so now is the difference between "queued" and "queued and
        // actually going to happen".
        std::snprintf(line, sizeof line,
                      "Spawn Ped: %s (%s, %.0f m ahead) - %s",
                      model.c_str(), tw::game::SpawnHostile() ? "hostile" : "friendly",
                      static_cast<double>(metres),
                      tw::game::SpawnPlayerReady()
                          ? "queued, see the corner display"
                          : "queued - waiting for you to be in the world");
        return std::string(line);
    };
    // The side is the same flag the H key flips, so the toggle and the hotkey are one
    // setting rather than two that can drift.
    hooks.setSpawnHostile = [](bool on) {
        tw::game::SpawnSetHostile(on);
        return std::string(on ? "Spawn side: hostile - the NPC is from another faction"
                              : "Spawn side: friendly - the NPC is on yours");
    };
    hooks.spawnHostile = []() { return tw::game::SpawnHostile(); };

    // The hologram. Turning it on starts the camera thread if it is not up yet: that
    // thread is what reads the renderer's camera, and the preview is read-only, so it is
    // the same thread doing the same job with nothing being flown.
    hooks.setHologramPreview = [](bool on) {
        tw::game::HologramSetPreview(on);
        if (on && !tw::game::FreeCamEnsureThread())
            return std::string("Spawn preview: on, but the camera thread would not start");
        return std::string(on ? "Spawn preview: on - the figure stands where the NPC will"
                              : "Spawn preview: off");
    };
    hooks.hologramPreviewActive = []() { return tw::game::HologramPreviewActive(); };
    hooks.setHologramDistance = [](float metres) {
        tw::game::HologramSetDistance(metres);
        char buf[80];
        std::snprintf(buf, sizeof buf, "Spawn preview: %.0f m ahead", static_cast<double>(metres));
        return std::string(buf);
    };
    hooks.hologramDistance = []() { return tw::game::HologramDistance(); };

    // The spawner's own state, so a spawner that has switched itself off after a fault is
    // visible in the menu instead of only in the log.
    hooks.spawnerStatus = []() {
        if (tw::game::SpawnBlocked()) return std::string("blocked - R resets");
        if (tw::game::SpawnPending()) return std::string("working...");
        const std::string s = tw::game::SpawnStatusCopy();
        return s.empty() ? std::string("ready") : s;
    };
    hooks.resetSpawner = []() {
        tw::game::SpawnClearFault();
        return tw::game::SpawnBlocked() ? std::string("Spawner: still blocked")
                                        : std::string("Spawner: reset - one more spawn allowed");
    };
    // The model list: one row starts a scan of the installed game data (in the background,
    // because it reads a lot), and one shows what the list holds now.
    hooks.rescanPedModels = []() {
        if (tw::game::SpawnScanAsync())
            return std::string("NPC models: scanning the game folder - the list fills in "
                               "when it is done");
        return std::string("NPC models: a scan is already running");
    };
    hooks.pedModelSummary = []() { return pedModelSummaryText(); };
    // Picking a rival crew out of the list switches the side to hostile, because that is
    // what the name means and a friendly triad is a bystander in a nice jacket. One
    // direction only: picking a civilian leaves a hostile side alone, so setting it on
    // purpose is not undone by walking the list.
    hooks.pedPicked = [](const std::string& model) {
        if (!tw::game::SpawnModelIsEnemy(model)) return "Spawn Ped: " + model;
        tw::game::SpawnSetHostile(true);
        return "Rival crew: " + tw::game::SpawnShortName(model) +
               " - hostile, Enter on Spawn Ped puts it in the street";
    };
    hooks.pedEnemySummary = []() {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%d rival crews, hostile by default",
                      tw::game::SpawnEnemyModelCount());
        return std::string(buf);
    };

    // Props: the same spawner, aimed at the game's objects. The list is whatever the
    // prop side of the scan knows - the vehicles and weapons the executable names, plus
    // everything FreeCam.props.txt or the game files add.
    {
        const std::vector<std::string> props = tw::game::PropModels();
        hooks.propNames = props;
    }
    hooks.selectProp = [](const std::string& model) {
        tw::game::PropSelectByName(model);
        return "Prop: " + tw::game::PropShortName(model);
    };
    hooks.spawnProp = []() {
        const std::vector<std::string> props = tw::game::PropModels();
        const int index = tw::game::PropModelIndex();
        if (props.empty() || index < 0 || index >= static_cast<int>(props.size()))
            return std::string("Prop: no prop selected - Rescan props finds the game's objects");
        const std::string& name = props[index];
        tw::game::PropRequestAt(name, tw::game::HologramDistance());
        char line[200];
        std::snprintf(line, sizeof line, "Prop: %s queued, %.0f m ahead - %s",
                      tw::game::PropShortName(name).c_str(),
                      static_cast<double>(tw::game::HologramDistance()),
                      tw::game::PropPending() ? "waiting for the world" : "on its way");
        return std::string(line);
    };
    hooks.rescanProps = []() {
        if (tw::game::PropScanAsync())
            return std::string("Props: scanning the game folder - the list fills in when it is done");
        return std::string("Props: a scan is already running");
    };
    hooks.propSummary = []() { return propModelSummaryText(); };
    hooks.setPropPreview = [](bool on) {
        tw::game::HologramSetPropMode(on);
        if (on && !tw::game::HologramPreviewActive()) tw::game::HologramSetPreview(true);
        return on ? std::string("Preview: a crate on the spawn spot")
                  : std::string("Preview: the NPC figure on the spawn spot");
    };
    hooks.propPreviewActive = []() { return tw::game::HologramPropMode(); };

    // The player's own controls: U, or the menu entry. The camera has to be on for it
    // to mean anything - with it off the player has the controls already - and the
    // row is kept honest by syncState below, which reads what the hotkey did.
    hooks.setPlayerFree = [](bool on) {
        if (!tw::game::FreeCamActive()) {
            tw::game::FreeCamSetPlayerFree(false);
            return std::string("Player free: switch the free camera on first");
        }
        tw::game::FreeCamSetPlayerFree(on);
        return std::string(on ? "Player free: move, fight, interact (U puts them back)"
                              : "Player frozen: the keys fly the camera again (U frees them)");
    };

    // Noclip: the toggle, the speed picker and two way of keeping them honest.
    static const char* const kNoclipSpeeds[] = { "1x", "2x", "3x", "4x", "6x", "8x" };
    for (int i = 0; i < tw::game::NoclipSpeedCount(); ++i)
        hooks.noclipSpeeds.push_back(kNoclipSpeeds[i]);
    hooks.setNoclip = [](bool on) {
        tw::game::NoclipSet(on);
        if (!on) return std::string("Noclip: off - the character drops to the ground");
        // Two things have to be true for it to move anything: the thread that writes the
        // transform has to be running, and there has to be a player to write. Both are
        // reported, because "on" that does nothing is the worst possible answer.
        const bool thread = tw::game::FreeCamEnsureThread();
        if (!thread) return std::string("Noclip: on, but the camera thread would not start");
        if (!tw::game::NoclipPlayerReady())
            return std::string("Noclip: on - waiting for you to be in the world");
        return std::string("Noclip: on - WASD, Space up, C down; V or this row turns it off");
    };
    hooks.setNoclipSpeed = [](const std::string& label) {
        for (int i = 0; i < tw::game::NoclipSpeedCount(); ++i) {
            if (label == kNoclipSpeeds[i]) {
                tw::game::NoclipSetSpeedIndex(i);
                break;
            }
        }
        return std::string("Noclip speed: ") + label;
    };

    // The camera's other modes. Every one of them is state the camera thread owns, so the
    // menu queues the same action its hotkey runs; the answer comes back through
    // FreeCamTakeActionStatus and lands in the status line a frame later.
    hooks.setFreeze = [](bool on) {
        tw::game::FreeCamRequestAction(on ? tw::game::kFreeCamFreezeOn
                                          : tw::game::kFreeCamFreezeOff);
        return std::string(on ? "Freeze: queued" : "Unfreeze: queued");
    };
    hooks.frozen = []() { return tw::game::FreeCamFrozen(); };
    hooks.setFollow = [](bool on) {
        tw::game::FreeCamRequestAction(on ? tw::game::kFreeCamFollowOn
                                          : tw::game::kFreeCamFollowOff);
        return std::string(on ? "Follow: queued" : "Stop following: queued");
    };
    hooks.following = []() { return tw::game::FreeCamFollowing(); };
    hooks.setLookAtPlayer = [](bool on) {
        tw::game::FreeCamRequestAction(on ? tw::game::kFreeCamLookOn : tw::game::kFreeCamLookOff);
        return std::string(on ? "Look at player: queued" : "Hold still: queued");
    };
    hooks.lookingAtPlayer = []() { return tw::game::FreeCamLookingAt(); };
    hooks.setFlySpeed = [](float metres) {
        tw::game::FreeCamRequestAction(tw::game::kFreeCamSetFlySpeed,
                                       static_cast<int>(metres + 0.5f));
        char buf[64];
        std::snprintf(buf, sizeof buf, "Fly speed: %.0f m/s queued", static_cast<double>(metres));
        return std::string(buf);
    };
    hooks.flySpeed = []() { return tw::game::FreeCamSpeed(); };
    hooks.setFov = [](float degrees) {
        if (tw::game::FreeCamPlaying()) return std::string("FOV: stop playback before editing");
        tw::game::FreeCamRequestAction(tw::game::kFreeCamSetFov, static_cast<int>(degrees + 0.5f));
        return std::string("Free camera FOV: queued");
    };
    hooks.fov = []() { return tw::game::FreeCamFov(); };

    // The path: record, play, stop and forget, plus the speed ladder the arrows walk. The
    // labels are built from the core's own ladder, so the menu cannot offer a speed the
    // playback code does not have.
    hooks.recordKeyframe = []() {
        tw::game::FreeCamRequestAction(tw::game::kFreeCamKeyframe);
        return std::string("Keyframe: queued");
    };
    hooks.setPathPlaying = [](bool on) {
        tw::game::FreeCamRequestAction(on ? tw::game::kFreeCamPlay : tw::game::kFreeCamStop);
        return std::string(on ? "Playback: queued" : "Stop: queued");
    };
    hooks.pathPlaying = []() { return tw::game::FreeCamPlaying(); };
    hooks.clearPath = []() {
        tw::game::FreeCamRequestAction(tw::game::kFreeCamClearPath);
        return std::string("Clear path: queued");
    };
    for (int i = 0; i < tw::game::FreeCamPlaySpeedCount(); ++i) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "%gx", static_cast<double>(tw::game::FreeCamPlaySpeedAt(i)));
        hooks.playbackSpeeds.push_back(buf);
    }
    hooks.setPlaybackSpeed = [](int index) {
        tw::game::FreeCamRequestAction(tw::game::kFreeCamSetPlaySpeed, index);
        return std::string("Playback speed: queued");
    };
    hooks.keyframeCount = []() { return tw::game::FreeCamKeyframeCount(); };

    // Keys pressed in game and the menu have to agree: U frees and freezes the player,
    // V turns noclip on, [ and ] move the speed picker, and the camera can be switched
    // off from its own hotkeys (or by F10). One pass per frame keeps what is drawn
    // equal to what is true.
    hooks.syncState = []() {
        g_menu.setToggled("Free Camera", tw::game::FreeCamActive());
        g_menu.setToggled("Player free (U)", tw::game::FreeCamActive() &&
                                              tw::game::FreeCamPlayerFree());
        g_menu.setToggled("Noclip (V)", tw::game::NoclipActive());
        // The camera's modes and the path: all of them can be changed by a hotkey while
        // the menu is open (or before it was opened), so every row reads the truth.
        g_menu.setToggled("Freeze camera (O)", tw::game::FreeCamFrozen());
        g_menu.setToggled("Follow player (P)", tw::game::FreeCamFollowing());
        g_menu.setToggled("Look at player (P)", tw::game::FreeCamLookingAt());
        g_menu.setInfo("FOV projection", tw::game::FreeCamFovStatus());
        const bool playingFov = tw::game::FreeCamPlaying();
        static bool wasPlayingFov = false;
        if (playingFov || wasPlayingFov) {
            if (auto* row = g_menu.item("Free camera FOV (degrees)")) row->value = tw::game::FreeCamFov();
        }
        wasPlayingFov = playingFov;
        g_menu.setToggled("Play path (J)", playingFov);
        g_menu.setOptionIndex("Playback speed",
                              static_cast<std::size_t>(tw::game::FreeCamPlaySpeedIndex()));
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%d recorded", tw::game::FreeCamKeyframeCount());
            g_menu.setInfo("Keyframes", buf);
            std::string spawner;
            if (tw::game::SpawnBlocked()) spawner = "blocked - R resets";
            else if (tw::game::SpawnPending()) spawner = "working...";
            else {
                spawner = tw::game::SpawnStatusCopy();
                if (spawner.empty()) spawner = "ready";
            }
            g_menu.setInfo("Spawner", spawner);
        }
        g_menu.setOptionIndex("Noclip speed", static_cast<std::size_t>(tw::game::NoclipSpeedIndex()));
        // The weather has its own controls outside the menu - the preset selector and
        // the sliders write the same fields the game reads - so keep the lock toggle
        // honest. The skydome list only exists once the world has loaded, and
        // setOptions is a no-op once it matches, so this costs nothing per frame.
        g_menu.setToggled("Lock weather", tw::game::WeatherLocked());
        // The sky: the keys switch the override and cycle the variants, and the list only
        // exists once the world has loaded. setOptions and setToggled are no-ops when the
        // value already matches, so this costs nothing on a quiet frame.
        {
            tw::SkySettings& sky = tw::game::Sky();
            g_menu.setToggled("Sky override", sky.enabled());
            g_menu.setOptions("Sky variant", tw::game::SkyVariantNames());
            g_menu.setOptionIndex("Sky variant", static_cast<std::size_t>(sky.variant()));
            g_menu.setOptionIndex("Sky colour", static_cast<std::size_t>(sky.colour()));
            // The sliders are moved by the presets and by the settings themselves, so
            // their positions follow the truth rather than only the last drag.
            if (tw::MenuItem* strength = g_menu.item("Sky colour strength"))
                strength->value = sky.strength();
            static const char* const kChannels[3] = { "Tint red", "Tint green", "Tint blue" };
            for (int c = 0; c < 3; ++c) {
                if (tw::MenuItem* row = g_menu.item(kChannels[c])) row->value = sky.channel(c);
            }
            g_menu.setInfo("Sky", tw::game::SkyStateLine());
        }
        // H flips the side in game and the preview toggle can be switched by the same
        // hand that read it last frame; both entries follow the truth.
        g_menu.setToggled("Hostile NPC", tw::game::SpawnHostile());
        g_menu.setToggled("Spawn preview", tw::game::HologramPreviewActive());
        // The spawner's model list is built from the game folder, which may not have been
        // readable when the menu was built. Refill the choices the moment there are any:
        // the row is there from the first frame, so it should never stay empty.
        {
            static int seenRev = -1;
            const int rev = tw::game::SpawnModelsRevision();
            const tw::MenuItem* row = g_menu.item("Ped model (list)");
            const bool empty = !row || row->options.empty();
            if (rev != seenRev || (empty && !tw::game::SpawnModels().empty())) {
                seenRev = rev;
                g_menu.setOptions("Ped model (list)", tw::game::SpawnModels());
            }
        }
        g_menu.setInfo("Models found", pedModelSummaryText());
        if (g_menu.item("Prop model (list)")) {
            static int propRevision = -1;
            const int rev = tw::game::PropModelsRevision();
            if (rev != propRevision) {
                propRevision = rev;
                g_menu.setOptions("Prop model (list)", tw::game::PropModels());
            }
            const int chosen = tw::game::PropModelIndex();
            const int count = tw::game::PropModelCount();
            if (chosen >= 0 && chosen < count)
                g_menu.setOptionIndex("Prop model (list)", static_cast<std::size_t>(chosen));
            g_menu.setInfo("Props found", propModelSummaryText());
        }
    };

    tw::buildDebugMenu(g_menu, hooks);
    g_syncState = hooks.syncState;
}

}  // namespace

extern "C" void TW_VehInstall() {
    static volatile LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    initFakeObj();
    AddVectoredExceptionHandler(1 /* first */, vehHandler);
    TW_Log("VEH installed: load-crash fixups + spawn backstop.");
}

extern "C" void TW_MenuStart() {
    if (g_started) return;
    g_started = true;
    TW_Log("TW_MenuStart: menu starting.");

    buildMenu();
    g_menu.setStatus("Ready.");

    // The camera thread is also the only place the hotkeys are read, and two of
    // them need no camera at all: V turns noclip on (the character flies) and F10
    // is the panic key. Starting it with the menu is what makes V work from the
    // first minute of play instead of only after the free camera has been on.
    tw::game::FreeCamEnsureThread();

    CreateThread(nullptr, 0, installThread, nullptr, 0, nullptr);
}

extern "C" void TW_MenuStop() { g_started = false; }
