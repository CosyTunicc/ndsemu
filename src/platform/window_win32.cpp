#ifdef _WIN32
#include "platform/window_win32.hpp"
#include <d3d11.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "ui/library_ui.hpp"
#include "ui/win32_common.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
ID3D11DeviceContext* gPointCtx = nullptr;                  // pour le rappel qui choisit le filtrage "pixels nets"
ID3D11SamplerState* gPointSampler = nullptr;
void usePointSampler(const ImDrawList*, const ImDrawCmd*) {
    if (gPointCtx && gPointSampler) gPointCtx->PSSetSamplers(0, 1, &gPointSampler);
}
UiFonts& fontsOf(void* p) { return *static_cast<UiFonts*>(p); }
}

Window::~Window() {
    saveSettings();
    for (Slot& sl : slots_) if (sl.thumb) { sl.thumb->Release(); sl.thumb = nullptr; }
    if (imguiReady_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    releaseGraphics();
    delete static_cast<UiFonts*>(fonts_);
    if (hwnd) DestroyWindow(hwnd);
}

bool Window::create(const char* title) {
    enablePerMonitorDpi();
    settingsPath_ = utf8FromWide(exeDirectory() + L"\\ndsemu_fenetre.ini");
    loadSettings();
    input_.load();
    inputStamp_ = inputConfigStamp();
    title_ = title ? title : "ndsemu";

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // fleche (IDC_ARROW)
    wc.hIcon = makeAppIcon(48);
    wc.hIconSm = makeAppIcon(16);
    wc.lpszClassName = L"ndsemu_jeu";
    if (!RegisterClassExW(&wc)) return false;

    hwnd = CreateWindowExW(0, wc.lpszClassName, wideFromUtf8(title_).c_str(), WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 600, 800, nullptr, nullptr, inst, this);
    if (!hwnd) return false;
    scale_ = float(windowDpi(hwnd)) / 96.0f;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    if (!initGraphics()) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(dev_, ctx_);
    imguiReady_ = true;
    fonts_ = new UiFonts();
    rebuildFonts();

    setZoom(zoom_);                                        // taille de la fenetre selon le reglage garde
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}

bool Window::initGraphics() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &swap_, &dev_, &got, &ctx_);
    if (FAILED(hr))                                        // pas de carte graphique compatible : rendu logiciel
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &swap_, &dev_, &got, &ctx_);
    if (FAILED(hr)) return false;
    ID3D11Texture2D* back = nullptr;
    swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { dev_->CreateRenderTargetView(back, nullptr, &rtv_); back->Release(); }

    // Image de la console : 256 x 384, mise a jour a chaque image
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 256; td.Height = 384; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tex_))) return false;
    dev_->CreateShaderResourceView(tex_, nullptr, &srv_);

    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    dev_->CreateSamplerState(&smp, &point_);
    gPointCtx = ctx_;
    gPointSampler = point_;
    return true;
}

void Window::releaseGraphics() {
    gPointCtx = nullptr;
    gPointSampler = nullptr;
    if (point_) { point_->Release(); point_ = nullptr; }
    if (srv_) { srv_->Release(); srv_ = nullptr; }
    if (tex_) { tex_->Release(); tex_ = nullptr; }
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    if (swap_) { swap_->Release(); swap_ = nullptr; }
    if (ctx_) { ctx_->Release(); ctx_ = nullptr; }
    if (dev_) { dev_->Release(); dev_ = nullptr; }
}

void Window::rebuildFonts() {
    if (imguiReady_) ImGui_ImplDX11_InvalidateDeviceObjects();
    fontsOf(fonts_) = loadUiFonts(systemUiFonts(), scale_);
    applyDarkStyle(scale_);
    if (imguiReady_) ImGui_ImplDX11_CreateDeviceObjects();
    barH_ = std::round(34.0f * scale_);
}

bool Window::pump() {
    pad_.poll();
    if (++inputCheck_ >= 60) {                             // la bibliotheque a peut-etre change les touches
        inputCheck_ = 0;
        const int64_t st = inputConfigStamp();
        if (st != inputStamp_) { inputStamp_ = st; input_ = InputConfig(); input_.load(); }
    }
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) closed = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !closed;
}

void Window::setTitle(const char* utf8Title) {
    title_ = utf8Title;
    if (hwnd) SetWindowTextW(hwnd, wideFromUtf8(title_).c_str());
}

// ---- taille, plein ecran ----------------------------------------------------------------------------------
void Window::setZoom(int z) {
    zoom_ = std::clamp(z, 1, 4);
    saveSettings();
    if (!hwnd || fullscreen_) return;
    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    RECT wr, cr;
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    const int frameW = (wr.right - wr.left) - (cr.right - cr.left), frameH = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    const int w = layoutW() * zoom_ + frameW, h = layoutH() * zoom_ + int(barH_) + frameH;
    SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::setFullscreen(bool on) {
    if (on == fullscreen_ || !hwnd) return;
    if (on) {
        placement_.length = sizeof placement_;
        GetWindowPlacement(hwnd, &placement_);
        MONITORINFO mi = {};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &placement_);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    fullscreen_ = on;
}

// ---- reglages : ndsemu_fenetre.ini ------------------------------------------------------------------------
void Window::loadSettings() {
    std::ifstream in(std::filesystem::u8path(settingsPath_));
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const int v = std::atoi(line.c_str() + eq + 1);
        if (k == "taille") zoom_ = std::clamp(v, 1, 4);
        else if (k == "lissage") smooth_ = v != 0;
        else if (k == "disposition") layout_ = std::clamp(v, 0, 1);
        else if (k == "performances") showPerf_ = v != 0;
        else if (k == "volume") volumePct_ = std::clamp(v, 0, 100);
        else if (k == "muet") muted_ = v != 0;
    }
}

void Window::saveSettings() const {
    if (settingsPath_.empty()) return;
    std::ofstream out(std::filesystem::u8path(settingsPath_), std::ios::trunc);
    out << "# Fenetre de jeu de ndsemu (ecrit automatiquement)\n"
        << "taille=" << zoom_ << "\nlissage=" << (smooth_ ? 1 : 0) << "\ndisposition=" << layout_
        << "\nperformances=" << (showPerf_ ? 1 : 0) << "\nvolume=" << volumePct_ << "\nmuet=" << (muted_ ? 1 : 0) << "\n";
}

// ---- affichage --------------------------------------------------------------------------------------------
void Window::present(const uint32_t* rgb, int width, int height) {
    if (!hwnd || !dev_) return;
    if (IsIconic(hwnd)) return;
    if (resize_) {
        resize_ = false;
        if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
        swap_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* back = nullptr;
        swap_->GetBuffer(0, IID_PPV_ARGS(&back));
        if (back) { dev_->CreateRenderTargetView(back, nullptr, &rtv_); back->Release(); }
    }
    if (fontsDirty_) { fontsDirty_ = false; rebuildFonts(); }

    // image de la console -> texture (l'octet du haut doit valoir 255 : opaque)
    if (rgb && width == 256 && height == 384) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx_->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (int y = 0; y < 384; y++) {
                uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
                const uint32_t* src = rgb + size_t(y) * 256;
                for (int x = 0; x < 256; x++) dst[x] = src[x] | 0xFF000000u;
            }
            ctx_->Unmap(tex_, 0);
        }
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    drawUi();
    ImGui::Render();
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->ClearRenderTargetView(rtv_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swap_->Present(0, 0);                                   // le rythme (59,8 images/s) est donne par l'emulation
}

void Window::drawUi() {
    const UiFonts& f = fontsOf(fonts_);
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size = io.DisplaySize;
    const auto S = [this](float v) { return v * scale_; };
    ImGui::PushFont(f.body);

    // Barre de menus : toujours visible en fenetre ; en plein ecran, seulement quand la souris est en haut
    const bool showBar = !fullscreen_ || menuOpen_ || (io.MousePos.y >= 0 && io.MousePos.y < barH_ * 1.5f && ImGui::IsMousePosValid());
    menuOpen_ = false;
    float top = fullscreen_ ? 0.0f : barH_;

    // Zone des ecrans : la plus grande possible en gardant les proportions
    const float aw = size.x, ah = size.y - top;
    const float lw = float(layoutW()), lh = float(layoutH());
    const float k = std::max(0.01f, std::min(aw / lw, ah / lh));
    const float iw = lw * k, ih = lh * k;
    const float ox = std::floor((aw - iw) * 0.5f), oy = std::floor(top + (ah - ih) * 0.5f);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (!smooth_) bg->AddCallback(usePointSampler, nullptr);
    const ImTextureID tid = ImTextureID(srv_);
    for (int s = 0; s < 2; s++) {
        float x0, y0;
        if (layout_ == 1) { x0 = ox + float(s) * 256.0f * k; y0 = oy; }
        else { x0 = ox; y0 = oy + float(s) * 192.0f * k; }
        const float x1 = x0 + 256.0f * k, y1 = y0 + 192.0f * k;
        bg->AddImage(tid, ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(0, s * 0.5f), ImVec2(1, 0.5f + s * 0.5f));
        screenRect_[s][0] = x0; screenRect_[s][1] = y0; screenRect_[s][2] = x1; screenRect_[s][3] = y1;
    }
    if (!smooth_) bg->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    // Pause : image assombrie et message
    if (paused_) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + iw, oy + ih), IM_COL32(0, 0, 0, 130));
        const char* t = "En pause";
        const char* h = "P pour reprendre";
        const ImVec2 ts = f.title->CalcTextSizeA(f.title->FontSize, FLT_MAX, 0, t);
        const ImVec2 hs = f.body->CalcTextSizeA(f.body->FontSize, FLT_MAX, 0, h);
        const float bw = std::max(ts.x, hs.x) + S(48), bh = ts.y + hs.y + S(30);
        const ImVec2 a(ox + (iw - bw) * 0.5f, oy + (ih - bh) * 0.5f);
        fg->AddRectFilled(a, ImVec2(a.x + bw, a.y + bh), IM_COL32(0x23, 0x25, 0x29, 240), S(12));
        fg->AddRect(a, ImVec2(a.x + bw, a.y + bh), IM_COL32(0x3D, 0xD6, 0x8C, 120), S(12));
        fg->AddText(f.title, f.title->FontSize, ImVec2(a.x + (bw - ts.x) * 0.5f, a.y + S(12)), IM_COL32(0xE8, 0xEA, 0xED, 255), t);
        fg->AddText(f.body, f.body->FontSize, ImVec2(a.x + (bw - hs.x) * 0.5f, a.y + S(16) + ts.y), IM_COL32(0x9A, 0xA0, 0xA6, 255), h);
    }

    // Message (etat sauvegarde / charge...)
    {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!toast_.empty() && now < toastUntil_) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const float fade = float(std::min(1.0, (toastUntil_ - now) * 3.0));
            const ImVec2 ts = f.body->CalcTextSizeA(f.body->FontSize, FLT_MAX, 0, toast_.c_str());
            const float w = ts.x + S(28), h = ts.y + S(16);
            const ImVec2 a(std::floor((size.x - w) * 0.5f), size.y - h - S(16));
            fg->AddRectFilled(a, ImVec2(a.x + w, a.y + h), IM_COL32(0x23, 0x25, 0x29, int(235 * fade)), S(10));
            fg->AddRect(a, ImVec2(a.x + w, a.y + h), toastError_ ? IM_COL32(0xFF, 0x5D, 0x5D, int(160 * fade)) : IM_COL32(0x3D, 0xD6, 0x8C, int(140 * fade)), S(10));
            fg->AddText(f.body, f.body->FontSize, ImVec2(a.x + S(14), a.y + S(8)), IM_COL32(0xE8, 0xEA, 0xED, int(255 * fade)), toast_.c_str());
        }
    }

    // Performances (Affichage > Performances)
    if (showPerf_ && !perf_.empty()) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 ts = f.mono->CalcTextSizeA(f.mono->FontSize, FLT_MAX, 0, perf_.c_str());
        const ImVec2 a(ox + S(8), oy + S(8));
        fg->AddRectFilled(a, ImVec2(a.x + ts.x + S(16), a.y + ts.y + S(12)), IM_COL32(0, 0, 0, 170), S(6));
        fg->AddText(f.mono, f.mono->FontSize, ImVec2(a.x + S(8), a.y + S(6)), IM_COL32(0xE8, 0xEA, 0xED, 255), perf_.c_str());
    }

    if (showBar) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), (barH_ - f.body->FontSize) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(10)));
        ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.102f, 0.110f, 0.122f, 1.0f));
        if (ImGui::BeginMainMenuBar()) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(12), S(10)));
            ImGui::SetNextWindowSizeConstraints(ImVec2(S(240), 0), ImVec2(FLT_MAX, FLT_MAX));
            if (ImGui::BeginMenu("Jeu")) {
                menuOpen_ = true;
                if (ImGui::MenuItem("Pause", "P", paused_)) paused_ = !paused_;
                if (ImGui::MenuItem("R\xC3\xA9initialiser", "Ctrl+R")) resetRequest = true;
                ImGui::Separator();
                ImGui::SetNextWindowSizeConstraints(ImVec2(S(300), 0), ImVec2(FLT_MAX, FLT_MAX));
                if (ImGui::BeginMenu("Sauvegarder l'\xC3\xA9tat")) { drawSlotMenu(true); ImGui::EndMenu(); }
                ImGui::SetNextWindowSizeConstraints(ImVec2(S(300), 0), ImVec2(FLT_MAX, FLT_MAX));
                if (ImGui::BeginMenu("Charger l'\xC3\xA9tat")) { drawSlotMenu(false); ImGui::EndMenu(); }
                ImGui::Separator();
                if (ImGui::MenuItem("Touches et manette\xE2\x80\xA6")) inputEditor_.open();
                if (ImGui::MenuItem("Instantan\xC3\xA9 de diagnostic", "F12")) snapshotRequest = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Quitter", "Alt+F4")) PostMessageW(hwnd, WM_CLOSE, 0, 0);
                ImGui::EndMenu();
            }
            ImGui::SetNextWindowSizeConstraints(ImVec2(S(260), 0), ImVec2(FLT_MAX, FLT_MAX));
            if (ImGui::BeginMenu("Affichage")) {
                menuOpen_ = true;
                for (int z = 1; z <= 4; z++) {
                    char lbl[32], key[16];
                    std::snprintf(lbl, sizeof lbl, "Taille \xC3\x97%d", z);
                    std::snprintf(key, sizeof key, "Ctrl+%d", z);
                    if (ImGui::MenuItem(lbl, key, zoom_ == z && !fullscreen_)) { setFullscreen(false); setZoom(z); }
                }
                if (ImGui::MenuItem("Plein \xC3\xA9" "cran", "F11", fullscreen_)) setFullscreen(!fullscreen_);
                ImGui::Separator();
                if (ImGui::MenuItem("\xC3\x89" "crans l'un sous l'autre", nullptr, layout_ == 0)) { layout_ = 0; setZoom(zoom_); }
                if (ImGui::MenuItem("\xC3\x89" "crans c\xC3\xB4te \xC3\xA0 c\xC3\xB4te", nullptr, layout_ == 1)) { layout_ = 1; setZoom(zoom_); }
                ImGui::Separator();
                if (ImGui::MenuItem("Lissage", nullptr, smooth_)) { smooth_ = !smooth_; saveSettings(); }
                if (ImGui::MenuItem("Performances", nullptr, showPerf_)) { showPerf_ = !showPerf_; saveSettings(); }
                ImGui::EndMenu();
            }
            ImGui::SetNextWindowSizeConstraints(ImVec2(S(240), 0), ImVec2(FLT_MAX, FLT_MAX));
            if (ImGui::BeginMenu("Son")) {
                menuOpen_ = true;
                if (ImGui::MenuItem("Couper le son", "M", muted_)) { muted_ = !muted_; saveSettings(); }
                ImGui::Spacing();
                ImGui::SetNextItemWidth(S(220));
                if (ImGui::SliderInt("##volume", &volumePct_, 0, 100, "Volume : %d %%")) { muted_ = false; }
                if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();
                ImGui::EndMenu();
            }
            ImGui::PopStyleVar();
            // a droite : etat (pause, images par seconde, son)
            std::string st = status_;
            if (paused_) st = "En pause";
            if (muted_ || volumePct_ == 0) st += "   \xC2\xB7   son coup\xC3\xA9";
            const float tw = ImGui::CalcTextSize(st.c_str()).x;
            ImGui::SameLine(ImGui::GetWindowWidth() - tw - S(16));
            ImGui::PushStyleColor(ImGuiCol_Text, paused_ ? ImVec4(0.24f, 0.84f, 0.55f, 1.0f) : ImVec4(0.6f, 0.63f, 0.65f, 1.0f));
            ImGui::TextUnformatted(st.c_str());
            ImGui::PopStyleColor();
            ImGui::EndMainMenuBar();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }
    // Fenetre "Touches et manette" (dans une fenetre ImGui invisible, plein ecran)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##hote", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (inputEditor_.draw(f, scale_)) { input_ = InputConfig(); input_.load(); inputStamp_ = inputConfigStamp(); }
    ImGui::End();
    ImGui::PopFont();
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || inputEditor_.isOpen()) menuOpen_ = true;
}

// ---- messages Windows -------------------------------------------------------------------------------------
LRESULT CALLBACK Window::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    Window* self = reinterpret_cast<Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->handle(h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

bool Window::takeSnapshotRequest() {
    const bool r = snapshotRequest;
    snapshotRequest = false;
    return r;
}

int Window::takeSaveRequest() { const int r = saveRequest_; saveRequest_ = 0; return r; }
int Window::takeLoadRequest() { const int r = loadRequest_; loadRequest_ = 0; return r; }

void Window::notify(const std::string& text, bool error) {
    toast_ = text;
    toastError_ = error;
    toastUntil_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() + (error ? 5.0 : 2.5);
}

// Emplacement : existe-t-il, date, miniature (envoyee a la carte graphique une fois)
void Window::setSlotInfo(int slot, bool exists, int64_t time, const std::vector<uint32_t>& thumb) {
    if (slot < 1 || slot > kSlots) return;
    Slot& sl = slots_[slot - 1];
    sl.exists = exists;
    sl.time = time;
    if (sl.thumb) { sl.thumb->Release(); sl.thumb = nullptr; }
    if (!exists || thumb.size() != 256 * 384 || !dev_) return;
    std::vector<uint32_t> px(thumb.size());
    for (size_t i = 0; i < px.size(); i++) px[i] = thumb[i] | 0xFF000000u;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 256; td.Height = 384; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { px.data(), 256 * 4, 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev_->CreateTexture2D(&td, &sd, &tex))) return;
    dev_->CreateShaderResourceView(tex, nullptr, &sl.thumb);
    tex->Release();
}

// Liste des 4 emplacements dans le menu : miniature, numero, date, raccourci
void Window::drawSlotMenu(bool save) {
    const UiFonts& f = fontsOf(fonts_);
    const auto S = [this](float v) { return v * scale_; };
    const float th = S(72), tw = th * 256.0f / 384.0f, rowH = th + S(8);
    for (int i = 0; i < kSlots; i++) {
        const Slot& sl = slots_[i];
        ImGui::PushID(i + (save ? 100 : 200));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool enabled = save || sl.exists;
        if (ImGui::Selectable("##emplacement", false, enabled ? 0 : ImGuiSelectableFlags_Disabled, ImVec2(S(280), rowH))) {
            if (save) saveRequest_ = i + 1; else loadRequest_ = i + 1;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 ia(p.x + S(4), p.y + S(4));
        if (sl.thumb) dl->AddImage(ImTextureID(sl.thumb), ia, ImVec2(ia.x + tw, ia.y + th));
        else dl->AddRectFilled(ia, ImVec2(ia.x + tw, ia.y + th), IM_COL32(0x15, 0x16, 0x18, 255), S(3));
        dl->AddRect(ia, ImVec2(ia.x + tw, ia.y + th), IM_COL32(0x36, 0x39, 0x3E, 255), S(3));
        char title[32], when[48], key[24];
        std::snprintf(title, sizeof title, "Emplacement %d", i + 1);
        if (sl.exists) {
            std::time_t t = std::time_t(sl.time);
            std::tm tmv = {};
            localtime_s(&tmv, &t);
            std::strftime(when, sizeof when, "%d/%m/%Y  %H:%M", &tmv);
        } else std::snprintf(when, sizeof when, "vide");
        std::snprintf(key, sizeof key, save ? "Maj+F%d" : "F%d", i + 1);
        const ImU32 tc = enabled ? IM_COL32(0xE8, 0xEA, 0xED, 255) : IM_COL32(0x7A, 0x7F, 0x86, 255);
        const float tx = ia.x + tw + S(12);
        dl->AddText(f.medium, f.medium->FontSize, ImVec2(tx, p.y + rowH * 0.5f - f.medium->FontSize - S(1)), tc, title);
        dl->AddText(f.body, f.body->FontSize, ImVec2(tx, p.y + rowH * 0.5f + S(2)), IM_COL32(0x9A, 0xA0, 0xA6, 255), when);
        const ImVec2 ks = f.mono->CalcTextSizeA(f.mono->FontSize, FLT_MAX, 0, key);
        dl->AddText(f.mono, f.mono->FontSize, ImVec2(p.x + S(280) - ks.x - S(6), p.y + (rowH - ks.y) * 0.5f), IM_COL32(0x7A, 0x7F, 0x86, 255), key);
        ImGui::PopID();
    }
}

bool Window::takeResetRequest() {
    const bool r = resetRequest;
    resetRequest = false;
    return r;
}

LRESULT Window::handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (imguiReady_ && ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            const bool first = !(lp & (1 << 30));               // pas la repetition automatique
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (lp & (1 << 29)) != 0;
            if (first) {
                if (wp == VK_F12) snapshotRequest = true;
                else if (wp >= VK_F1 && wp <= VK_F4) {                   // F1..F4 charger, Maj+F1..F4 sauvegarder
                    const int slot = int(wp - VK_F1) + 1;
                    if (GetKeyState(VK_SHIFT) & 0x8000) saveRequest_ = slot; else loadRequest_ = slot;
                }
                else if (wp == VK_F11 || (alt && wp == VK_RETURN)) { setFullscreen(!fullscreen_); return 0; }
                else if (wp == VK_ESCAPE && fullscreen_) setFullscreen(false);
                else if (wp == 'P' && !ctrl) paused_ = !paused_;
                else if (wp == 'M' && !ctrl) { muted_ = !muted_; saveSettings(); }
                else if (wp == 'R' && ctrl) resetRequest = true;
                else if (ctrl && wp >= '1' && wp <= '4') { setFullscreen(false); setZoom(int(wp - '0')); }
            }
            if (wp < 256 && !alt) keys[wp] = true;
            if (msg == WM_SYSKEYDOWN && wp != VK_F4) return 0;   // Alt seul n'ouvre pas le menu systeme
            break;
        }
        case WM_KEYUP: case WM_SYSKEYUP:
            if (wp < 256) keys[wp] = false;
            if (msg == WM_SYSKEYUP) return 0;
            break;
        case WM_SYSCHAR:
            return 0;                                          // pas de bip avec Alt+Entree
        case WM_KILLFOCUS:                                     // plus de touche "coincee" quand la fenetre perd le focus
            for (bool& k : keys) k = false;
            mouseDown = false;
            return 0;
        case WM_LBUTTONDOWN:
            SetCapture(h);
            mouseDown = true;
            mouseX = static_cast<short>(LOWORD(lp));
            mouseY = static_cast<short>(HIWORD(lp));
            return 0;
        case WM_MOUSEMOVE:
            mouseX = static_cast<short>(LOWORD(lp));
            mouseY = static_cast<short>(HIWORD(lp));
            return 0;
        case WM_LBUTTONUP:
            mouseDown = false;
            ReleaseCapture();
            return 0;
        case WM_LBUTTONDBLCLK:
            return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) resize_ = true;
            return 0;
        case WM_DPICHANGED: {
            scale_ = float(HIWORD(wp)) / 96.0f;
            fontsDirty_ = true;
            const RECT* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mi = reinterpret_cast<MINMAXINFO*>(lp);
            mi->ptMinTrackSize.x = 256;
            mi->ptMinTrackSize.y = 200;
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;                                          // pas d'effacement : evite le scintillement
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            hwnd = nullptr;
            closed = true;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Bouton de la console enfonce : sa touche du clavier ou son entree de la manette (et le stick pour la croix)
static bool held(const bool* keys, const InputConfig& c, uint32_t pad, int b) {
    if (c.key[b] > 0 && c.key[b] < 256 && keys[c.key[b]]) return true;
    if (c.pad[b] && (pad & c.pad[b])) return true;
    if (c.stickAsDpad) {
        static const uint32_t stick[4] = { PadStickRight, PadStickLeft, PadStickUp, PadStickDown };
        if (b >= BtnRight && b <= BtnDown && (pad & stick[b - BtnRight])) return true;
    }
    return false;
}

uint16_t Window::keyInput() const {
    uint16_t v = 0x03FF;                                   // toutes les touches relachees
    if (menuOpen_) return v;                               // un menu est ouvert : les touches sont pour lui
    const uint32_t pad = GetForegroundWindow() == hwnd ? pad_.state() : 0;   // manette : seulement pour la fenetre active
    for (int b = 0; b < 10; b++) if (held(keys, input_, pad, b)) v = uint16_t(v & ~(1u << b));
    return v;
}

uint8_t Window::keyXY() const {
    uint8_t v = 0x4B;                                      // etat "rien d'appuye" (voir KEYXY)
    if (menuOpen_) return v;
    const uint32_t pad = GetForegroundWindow() == hwnd ? pad_.state() : 0;
    if (held(keys, input_, pad, BtnX)) v = uint8_t(v & ~1u);
    if (held(keys, input_, pad, BtnY)) v = uint8_t(v & ~2u);
    return v;
}

// La souris sur l'ecran du bas (la ou il est affiche, quelle que soit la disposition) -> stylet
bool Window::touch(int& x, int& y) const {
    if (!mouseDown || !hwnd || menuOpen_ || paused_) return false;
    const float* r = screenRect_[1];
    const float w = r[2] - r[0], h = r[3] - r[1];
    if (w <= 0 || h <= 0) return false;
    const int sx = int((float(mouseX) - r[0]) * 256.0f / w);
    const int sy = int((float(mouseY) - r[1]) * 192.0f / h);
    if (sx < 0 || sx > 255 || sy < 0 || sy > 191) return false;
    x = sx;
    y = sy;
    return true;
}
#endif
