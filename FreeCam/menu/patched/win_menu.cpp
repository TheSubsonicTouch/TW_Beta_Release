// win_menu.cpp - D3D11 overlay (Dear ImGui), keyboard/gamepad input, the
// job queue that defers game calls to the game thread, and the vectored
// exception handler that keeps game crashes from taking the process down.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <xinput.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "../game_hooks.h"
#include "../menu_core.h"
#include "freecam_game.h"
#include "freecam_noclip.h"
#include "freecam_spawn.h"
#include "freecam_unlock.h"
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


// Only the work that has to happen on the game thread: the three actions that call into
// the game or write a save file. Everything the camera does is hotkeyed and runs on its
// own thread (freecam_game.cpp), so it does not pass through here.
enum JobKind { JobWeather, JobLevel, JobShader };

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
                kEnter = false, kBack = false;
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
    if (!typing) {
        if (up) g_menu.handleKey(tw::Key::Up);
        if (down) g_menu.handleKey(tw::Key::Down);
        if (left) g_menu.handleKey(tw::Key::Left);
        if (right) g_menu.handleKey(tw::Key::Right);
        if (enter) g_menu.handleKey(tw::Key::Select);
        if (back) g_menu.handleKey(tw::Key::Back);
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
constexpr int kBgResourceId = 2;
ID3D11ShaderResourceView* g_bgSrv = nullptr;
int g_bgW = 0;
int g_bgH = 0;

// Every COM object is released on every exit path, including the failures: this runs
// inside the game's process, and a leak here is the game's leak.
bool decodePng(ID3D11Device* dev, const void* data, DWORD size) {
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

        if (FAILED(dev->CreateShaderResourceView(texture, nullptr, &g_bgSrv))) break;
        g_bgW = static_cast<int>(w);
        g_bgH = static_cast<int>(h);
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

void loadBackground(ID3D11Device* dev) {
    if (g_bgSrv || !dev) return;
    HINSTANCE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(kBgResourceId),
                                       MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res) {
        TW_Log("Menu background: the dragon artwork is not in this DLL.");
        return;
    }
    HGLOBAL block = LoadResource(self, res);
    const void* bytes = block ? LockResource(block) : nullptr;
    const DWORD size = SizeofResource(self, res);
    if (!bytes || size == 0) {
        TW_Log("Menu background: the artwork could not be read.");
        return;
    }
    if (!decodePng(dev, bytes, size)) {
        TW_Log("Menu background: the artwork could not be decoded; using the plain panel.");
        return;
    }
    TW_Log("Menu background: %dx%d dragon pattern loaded.", g_bgW, g_bgH);
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
    default:
        return "";
    }
}

void renderMenu() {
    const ImVec4 gold(0.93f, 0.78f, 0.42f, 1.0f);
    const ImVec4 goldDim(0.72f, 0.55f, 0.28f, 1.0f);
    const ImVec4 bone(0.94f, 0.92f, 0.88f, 1.0f);
    const ImVec4 red(0.86f, 0.24f, 0.22f, 1.0f);
    const ImVec4 cyan(0.55f, 0.85f, 0.92f, 1.0f);

    ImGui::SetNextWindowPos(ImVec2(26, 26), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(400, 0), ImVec2(1000, 4000));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    ImGui::Begin("TriadMenu", nullptr, ImGuiWindowFlags_NoCollapse);

    // The dragon, behind everything the window draws: cover-fit, so the tiles never
    // stretch. It is drawn at a little over half strength and the window background stays
    // dark, so the rows keep their contrast.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    // The content region, not the whole window: the window's own draw list is painted
    // after its title bar, so an image over the window rect would sit on top of the name.
    const ImVec2 cmin = ImGui::GetWindowContentRegionMin();
    const ImVec2 cmax = ImGui::GetWindowContentRegionMax();
    const ImVec2 ws(cmax.x - cmin.x, cmax.y - cmin.y);
    const ImVec2 origin(wp.x + cmin.x, wp.y + cmin.y);
    if (g_bgSrv && ws.x > 1.0f && ws.y > 1.0f) {
        ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
        const float imgAspect = static_cast<float>(g_bgW) / static_cast<float>(g_bgH);
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
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(g_bgSrv), origin,
                            ImVec2(origin.x + ws.x, origin.y + ws.y), uv0, uv1,
                            IM_COL32(255, 255, 255, 120), 4.0f);
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
    const float rowHeight = 26.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 1.0f));
    const auto& items = g_menu.items();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const tw::MenuItem& it = items[i];

        if (it.header) {
            ImGui::Dummy(ImVec2(1.0f, 4.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, red);
            ImGui::TextUnformatted(it.label.c_str());
            ImGui::PopStyleColor();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(p.x, p.y + 1.0f), ImVec2(p.x + rowWidth, p.y + 1.0f),
                        ImGui::GetColorU32(ImVec4(0.55f, 0.32f, 0.14f, 0.55f)), 1.0f);
            ImGui::Dummy(ImVec2(1.0f, 2.0f));
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
                              ImGui::GetColorU32(ImVec4(0.55f, 0.10f, 0.10f, 0.55f)), 4.0f);
            dl->AddRectFilled(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x + 2.5f, p.y + rowHeight - 2.0f),
                              ImGui::GetColorU32(gold), 1.0f);
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
                ImGui::SetCursorScreenPos(ImVec2(fieldX, p.y + 3.0f));
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
            const ImU32 col = ImGui::GetColorU32(
                it.kind == tw::ItemKind::Toggle ? (it.toggled ? ImVec4(0.45f, 0.90f, 0.45f, 1.0f) : ImVec4(0.62f, 0.58f, 0.52f, 1.0f))
                                                : ImVec4(0.86f, 0.76f, 0.52f, 1.0f));
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
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.52f, 0.48f, 1.0f));
    ImGui::TextUnformatted("Up/Down move   Left/Right change   Enter apply");
    ImGui::TextUnformatted("Right click = right arrow   Insert / Backspace close");
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleVar();
}

void renderFrame() {
    const bool menuOn = g_menu.visible();
    tw::game::FreeCamHudLine probe[1];
    const bool hudOn = tw::game::FreeCamHudLines(probe, 1) > 0;
    if (!menuOn && !hudOn && !tw::game::NoclipActive()) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

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
        if (g_syncState) g_syncState();   // hotkeys can change what the menu shows
        // The camera thread reads the keyboard directly, so it has to be told when the
        // menu is the one using it - otherwise typing a name in Spawn Ped would also fly
        // the camera, spawn the hotkey ped and toggle noclip.
        tw::game::FreeCamSetMenuOpen(g_menu.visible());
        drainJobs();   // run queued actions on the game thread, then show results
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

void buildMenu() {
    tw::DebugHooks hooks;
    // Only what the menu still offers. The camera, the spawner and noclip keep their
    // hotkeys (see freecam_game.cpp); the menu is for the things that are easier to read
    // than to remember, and for the actions that write a save file.
    hooks.weatherPresets = tw::game::WeatherPresets();
    hooks.levelTargets = tw::game::LevelTargets();
    hooks.shaderPresets = tw::game::ShaderPresets();

    hooks.setWeather = [](const std::string& n) {
        pushJob(Job{JobWeather, n, 0.0f, false});
        return std::string("Weather: queued");
    };
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
    // opportunities (rivals off cooldown, job ladder open). Neither is much use alone, so
    // the one entry does both and reports both.
    hooks.unlockAllContent = []() {
        std::string content = tw::game::UnlockAllContent();
        std::string raids = tw::game::UnlockRaidsAndEvents();
        return content + "   |   " + raids;
    };
    hooks.unlockRaids = []() { return tw::game::UnlockRaidsAndEvents(); };
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
        tw::game::SpawnSetHostile(false);
        tw::game::SpawnRequest(model, false);
        return "Spawn Ped: " + model + " - queued, see the corner display";
    };

    // Noclip: the toggle, the speed picker and two way of keeping them honest.
    static const char* const kNoclipSpeeds[] = { "1x", "2x", "3x", "4x", "6x", "8x" };
    for (int i = 0; i < tw::game::NoclipSpeedCount(); ++i)
        hooks.noclipSpeeds.push_back(kNoclipSpeeds[i]);
    hooks.setNoclip = [](bool on) {
        tw::game::NoclipSet(on);
        return std::string(on ? "Noclip: on - WASD, Space up, C down; [ and ] change the speed"
                              : "Noclip: off - the character drops to the ground");
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

    // U turns noclip on and the menu has to agree with it, and the speed keys move the
    // picker; the camera can also be switched off from the free camera's own hotkeys. One
    // pass per frame keeps what is drawn equal to what is true.
    hooks.syncState = []() {
        g_menu.setToggled("Free Camera", tw::game::FreeCamActive());
        g_menu.setToggled("Noclip (fly the player)", tw::game::NoclipActive());
        g_menu.setOptionIndex("Noclip speed", static_cast<std::size_t>(tw::game::NoclipSpeedIndex()));
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

    CreateThread(nullptr, 0, installThread, nullptr, 0, nullptr);
}

extern "C" void TW_MenuStop() { g_started = false; }
