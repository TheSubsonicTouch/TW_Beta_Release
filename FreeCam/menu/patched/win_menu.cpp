// win_menu.cpp - D3D11 overlay (Dear ImGui), keyboard/gamepad input, the
// job queue that defers game calls to the game thread, and the vectored
// exception handler that keeps game crashes from taking the process down.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <xinput.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "../game_hooks.h"
#include "../menu_core.h"
#include "freecam_game.h"
#include "freecam_spawn.h"
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

enum JobKind {
    JobSpawnVehicle, JobSpawnNpc, JobSpawnWeapon, JobWeather, JobTeleport,
    JobInvincible, JobHudHidden, JobTimeOfDay, JobLevel, JobShader,
};

struct Job {
    JobKind kind = JobSpawnVehicle;
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
    case JobSpawnVehicle: g_menu.setStatus(tw::game::SpawnAsset("Spawn", j.text)); break;
    case JobSpawnNpc:     g_menu.setStatus(tw::game::SpawnAsset("Spawn", j.text)); break;
    case JobSpawnWeapon:  g_menu.setStatus(tw::game::SpawnAsset("Spawn", j.text)); break;
    case JobWeather:      g_menu.setStatus(tw::game::SetWeather(j.text)); break;
    case JobTeleport:     g_menu.setStatus(tw::game::TeleportToWaypoint()); break;
    case JobInvincible:   // TODO: no verified RVA yet
        g_menu.setStatus(std::string("Invincible ") + (j.flag ? "on" : "off") +
                         " (not wired yet)");
        break;
    case JobHudHidden:
        g_menu.setStatus(std::string("HUD ") + (j.flag ? "hidden" : "shown") +
                         " (not wired yet)");
        break;
    case JobTimeOfDay:
        g_menu.setStatus("Time set (not wired yet)");
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
    if (edge(keyDown(VK_UP), kUp)) g_menu.handleKey(tw::Key::Up);
    if (edge(keyDown(VK_DOWN), kDown)) g_menu.handleKey(tw::Key::Down);
    if (edge(keyDown(VK_LEFT), kLeft)) g_menu.handleKey(tw::Key::Left);
    if (edge(keyDown(VK_RIGHT), kRight)) g_menu.handleKey(tw::Key::Right);
    if (edge(keyDown(VK_RETURN), kEnter)) g_menu.handleKey(tw::Key::Select);
    if (edge(keyDown(VK_BACK), kBack)) g_menu.handleKey(tw::Key::Back);

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
    if (n <= 0) return;

    float freshest = 0.0f;
    for (int i = 0; i < n; ++i) if (lines[i].alpha > freshest) freshest = lines[i].alpha;

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

void renderFrame() {
    const bool menuOn = g_menu.visible();
    tw::game::FreeCamHudLine probe[1];
    const bool hudOn = tw::game::FreeCamHudLines(probe, 1) > 0;
    if (!menuOn && !hudOn) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (menuOn) {
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("Triad Wars Debug Menu", nullptr, ImGuiWindowFlags_NoCollapse);
        for (const auto& line : g_menu.renderLines()) {
            if (line.rfind("> ", 0) == 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s", line.c_str());
            } else if (line.rfind("Status: ", 0) == 0) {
                ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", line.c_str());
            } else {
                ImGui::TextUnformatted(line.c_str());
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Keyboard: Insert / arrows / Enter / Backspace");
        ImGui::TextDisabled("Gamepad: Start / D-pad / A / B");
        ImGui::End();
    }

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
    hooks.vehicleNames = tw::game::VehicleNames();
    // The spawn module owns the list, because it is also what FreeCam.npc.txt feeds.
    hooks.npcNames = tw::game::SpawnModels();
    hooks.weaponNames = tw::game::WeaponNames();
    hooks.weatherPresets = tw::game::WeatherPresets();
    hooks.levelTargets = tw::game::LevelTargets();
    hooks.shaderPresets = tw::game::ShaderPresets();

    hooks.spawnVehicle = [](const std::string& n) {
        pushJob(Job{JobSpawnVehicle, n, 0.0f, false});
        return std::string("Spawn: queued");
    };
    hooks.spawnNpc = [](const std::string& n) {
        pushJob(Job{JobSpawnNpc, n, 0.0f, false});
        return std::string("Spawn: queued");
    };
    hooks.spawnWeapon = [](const std::string& n) {
        pushJob(Job{JobSpawnWeapon, n, 0.0f, false});
        return std::string("Spawn: queued");
    };
    hooks.setWeather = [](const std::string& n) {
        pushJob(Job{JobWeather, n, 0.0f, false});
        return std::string("Weather: queued");
    };
    hooks.teleportToWaypoint = [] {
        pushJob(Job{JobTeleport, std::string(), 0.0f, false});
        return std::string("Teleport: queued");
    };
    hooks.setFreeCam = [](bool on) { return tw::game::FreeCamSetActive(on); };
    // The spawn runs on the camera thread, which is the thread that has the game's
    // per-thread allocator; the menu thread only queues it. The result goes to the HUD
    // and to TW_MenuLog.txt, so the line here only says it was taken.
    hooks.spawnNpcInFront = [](const std::string& model, bool hostile) {
        // Take the menu's choice over as the hotkey one, so B repeats what you just did.
        tw::game::SpawnSelectByName(model);
        tw::game::SpawnSetHostile(hostile);
        tw::game::SpawnRequest(model, hostile);
        return std::string("Spawn: queued (") + (hostile ? "hostile" : "friendly") + ")";
    };
    hooks.setInvincible = [](bool on) { pushJob(Job{JobInvincible, std::string(), 0.0f, on}); };
    hooks.setHudHidden = [](bool on) { pushJob(Job{JobHudHidden, std::string(), 0.0f, on}); };
    hooks.setTimeOfDay = [](float v) { pushJob(Job{JobTimeOfDay, std::string(), v, false}); };
    hooks.setLevel = [](const std::string& t, int l) {
        pushJob(Job{JobLevel, t, static_cast<float>(l), false});
        return std::string("Level: queued");
    };
    hooks.setShader = [](const std::string& n) {
        pushJob(Job{JobShader, n, 0.0f, false});
        return std::string("Shader: queued");
    };
    hooks.unlockAllContent = []() {
        return tw::game::UnlockAllContent();
    };

    tw::buildDebugMenu(g_menu, hooks);
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
