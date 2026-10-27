// Bibliotheque de jeux sous Windows : fenetre sans cadre (la barre de titre est dessinee par l'interface),
// rendu Dear ImGui avec Direct3D 11, boites de dialogue de Windows, lancement des jeux dans une nouvelle fenetre
// (le meme programme, relance avec le chemin du jeu).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "ui/library_ui.hpp"
#include "ui/win32_common.hpp"
#include "platform/input_win32.hpp"
#include "ui/updater.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring exePath() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::wstring(buf, n);
}
std::wstring exeDir() {
    std::wstring p = exePath();
    const size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? L"." : p.substr(0, cut);
}

// ---- Direct3D 11 ----
ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gContext = nullptr;
IDXGISwapChain* gSwap = nullptr;
ID3D11RenderTargetView* gTarget = nullptr;

void createTarget() {
    ID3D11Texture2D* back = nullptr;
    gSwap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { gDevice->CreateRenderTargetView(back, nullptr, &gTarget); back->Release(); }
}
void releaseTarget() { if (gTarget) { gTarget->Release(); gTarget = nullptr; } }

bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
        &sd, &gSwap, &gDevice, &got, &gContext);
    if (hr == DXGI_ERROR_UNSUPPORTED)                               // pas de carte graphique compatible : rendu logiciel
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
            &sd, &gSwap, &gDevice, &got, &gContext);
    if (FAILED(hr)) return false;
    createTarget();
    return true;
}
void destroyDevice() {
    releaseTarget();
    if (gSwap) { gSwap->Release(); gSwap = nullptr; }
    if (gContext) { gContext->Release(); gContext = nullptr; }
    if (gDevice) { gDevice->Release(); gDevice = nullptr; }
}

// ---- l'hote : ce que l'interface demande a Windows ----
class Win32Host : public LibraryHost {
public:
    HWND hwnd = nullptr;
    struct Running { HANDLE process; DWORD pid; };
    std::map<std::string, Running> running;
    std::string pendingError;

    std::string pickFolder() override { return dialog(true); }
    std::string pickRom() override { return dialog(false); }

    bool launch(const std::string& rom, const Library& lib, std::string& error) override {
        const std::wstring exe = exePath(), dir = exeDir();
        std::wstring cmd = L"\"" + exe + L"\" \"" + widen(rom) + L"\" --fenetre";
        if (!lib.sound) cmd += L" --sans-son";
        if (lib.report) cmd += L" --rapport";
        STARTUPINFOW si = {};
        si.cb = sizeof si;
        HANDLE log = INVALID_HANDLE_VALUE;
        if (lib.report) {                                           // le rapport du jeu va dans diag.txt
            SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
            log = CreateFileW((dir + L"\\diag.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log != INVALID_HANDLE_VALUE) {
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdOutput = log;
                si.hStdError = log;
                si.hStdInput = nullptr;
            }
        }
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> line(cmd.begin(), cmd.end());
        line.push_back(0);
        const BOOL ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, log != INVALID_HANDLE_VALUE,
            CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (!ok) { error = "Windows refuse de d\xC3\xA9marrer ndsemu (erreur " + std::to_string(GetLastError()) + ")"; return false; }
        CloseHandle(pi.hThread);
        running[rom] = { pi.hProcess, pi.dwProcessId };
        return true;
    }

    bool isRunning(const std::string& rom) override {
        auto it = running.find(rom);
        return it != running.end() && WaitForSingleObject(it->second.process, 0) == WAIT_TIMEOUT;
    }

    void stop(const std::string& rom) override {
        auto it = running.find(rom);
        if (it == running.end()) return;
        // on ferme la fenetre du jeu comme si l'utilisateur cliquait sur la croix : la sauvegarde est ecrite
        struct Ctx { DWORD pid; };
        Ctx c{ it->second.pid };
        EnumWindows([](HWND w, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == reinterpret_cast<Ctx*>(lp)->pid && IsWindowVisible(w)) PostMessageW(w, WM_CLOSE, 0, 0);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&c));
    }

    std::string takeGameError() override {
        for (auto it = running.begin(); it != running.end();) {
            if (WaitForSingleObject(it->second.process, 0) != WAIT_OBJECT_0) { ++it; continue; }
            DWORD code = 0;
            GetExitCodeProcess(it->second.process, &code);
            CloseHandle(it->second.process);
            if (code == 2) pendingError = "Le jeu s'est arr\xC3\xAAt\xC3\xA9 sur une erreur : rapport dans ndsemu_erreur.txt (dossier de ndsemu)";
            else if (code != 0) pendingError = "Le jeu n'a pas pu d\xC3\xA9marrer (fichier illisible ou ROM invalide)";
            it = running.erase(it);
        }
        std::string e;
        e.swap(pendingError);
        return e;
    }

    void reveal(const std::string& path) override {
        const std::wstring w = widen(path);
        const DWORD attr = GetFileAttributesW(w.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            ShellExecuteW(hwnd, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else {
            const std::wstring args = L"/select,\"" + w + L"\"";
            ShellExecuteW(hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }

    // Les icones (32 x 32) sont agrandies 4 fois sans lissage avant l'envoi : le filtre lineaire d'ImGui les
    // garde alors nettes a l'ecran.
    void* createTexture(const uint32_t* rgba, int w, int h) override {
        const int k = 4, W = w * k, H = h * k;
        std::vector<uint32_t> big(size_t(W) * size_t(H));
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) big[size_t(y) * W + x] = rgba[(y / k) * w + x / k];
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = UINT(W); d.Height = UINT(H); d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub = { big.data(), UINT(W * 4), 0 };
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(gDevice->CreateTexture2D(&d, &sub, &tex))) return nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = d.Format; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        gDevice->CreateShaderResourceView(tex, &sd, &srv);
        tex->Release();
        return srv;
    }
    void destroyTexture(void* t) override { if (t) static_cast<ID3D11ShaderResourceView*>(t)->Release(); }

    void minimize() override { ShowWindow(hwnd, SW_MINIMIZE); }
    void toggleMaximize() override { ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); }
    bool isMaximized() override { return IsZoomed(hwnd) != 0; }
    void close() override { PostMessageW(hwnd, WM_CLOSE, 0, 0); }
    void openInputSettings() override { inputEditor.open(); }
    void drawHostWindows(const UiFonts& f, float s) override { inputEditor.draw(f, s); }
    Updater* updater() override { return upd.get(); }
    bool anyGameRunning() override {
        for (const auto& kv : running) if (WaitForSingleObject(kv.second.process, 0) == WAIT_TIMEOUT) return true;
        return false;
    }
    InputEditor inputEditor;
    std::unique_ptr<Updater> upd;

private:
    std::string dialog(bool folder) {
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return "";
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
        if (!folder) {
            const COMDLG_FILTERSPEC types[] = { { L"Jeux Nintendo DS (*.nds)", L"*.nds" }, { L"Tous les fichiers", L"*.*" } };
            dlg->SetFileTypes(2, types);
            dlg->SetTitle(L"Ouvrir une ROM");
        } else {
            dlg->SetTitle(L"Ajouter un dossier de jeux");
        }
        std::string result;
        if (SUCCEEDED(dlg->Show(hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = narrow(p); CoTaskMemFree(p); }
                item->Release();
            }
        }
        dlg->Release();
        return result;
    }
};

Win32Host* gHost = nullptr;
LibraryUI* gUi = nullptr;
float gScale = 1.0f;
bool gFontsDirty = false;
bool gResize = false;
UINT gResizeW = 0, gResizeH = 0;

UINT dpiOf(HWND w) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fn && w) return fn(w);
    HDC dc = GetDC(nullptr);
    const UINT d = UINT(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

// Icone de la fenetre : carre orange arrondi avec deux ecrans blancs (comme dans la barre de titre)
HICON makeIcon(int size) {
    std::vector<uint32_t> px(size_t(size) * size, 0);
    const float r = float(size) * 0.22f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            const float fx = float(x) + 0.5f, fy = float(y) + 0.5f, S = float(size);
            const float dx = std::max({ r - fx, fx - (S - r), 0.0f }), dy = std::max({ r - fy, fy - (S - r), 0.0f });
            const float d = std::sqrt(dx * dx + dy * dy) - r;
            if (d > 0.5f) continue;
            const float a = d < -0.5f ? 1.0f : 0.5f - d;
            const float t = fy / S;
            uint32_t R = uint32_t(255 - 15 * t), G = uint32_t(154 - 64 * t), B = uint32_t(74 - 34 * t);
            const bool onTop = fx > S * 0.25f && fx < S * 0.75f && fy > S * 0.2f && fy < S * 0.47f;
            const bool onBottom = fx > S * 0.25f && fx < S * 0.75f && fy > S * 0.53f && fy < S * 0.8f;
            if (onTop || onBottom) { R = 255; G = 255; B = 255; }
            const uint32_t A = uint32_t(a * 255.0f);
            px[size_t(y) * size + x] = (A << 24) | (((R * A) / 255) << 16) | (((G * A) / 255) << 8) | ((B * A) / 255);
        }
    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof bh; bh.bV5Width = size; bh.bV5Height = -size; bh.bV5Planes = 1; bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    std::memcpy(bits, px.data(), px.size() * 4);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp == TRUE) {                                           // toute la fenetre est zone client (pas de cadre)
            if (IsZoomed(h)) {                                      // agrandie : Windows deborde du cadre, on le retire
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                const int f = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                const int g = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left += f; p->rgrc[0].right -= f; p->rgrc[0].top += g; p->rgrc[0].bottom -= g;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &pt);
        RECT rc;
        GetClientRect(h, &rc);
        const int b = int(6 * gScale);
        if (!IsZoomed(h)) {                                         // bords : redimensionnement
            const bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, d = pt.y >= rc.bottom - b;
            if (t && l) return HTTOPLEFT;
            if (t && r) return HTTOPRIGHT;
            if (d && l) return HTBOTTOMLEFT;
            if (d && r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT;
            if (r) return HTRIGHT;
            if (t) return HTTOP;
            if (d) return HTBOTTOM;
        }
        if (gUi && pt.y < LONG(gUi->titleBarHeight) && pt.x < LONG(gUi->captionButtonsX)) return HTCAPTION;   // on deplace la fenetre
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* mi = reinterpret_cast<MINMAXINFO*>(lp);
        mi->ptMinTrackSize.x = LONG(780 * gScale);
        mi->ptMinTrackSize.y = LONG(520 * gScale);
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { gResize = true; gResizeW = LOWORD(lp); gResizeH = HIWORD(lp); }
        return 0;
    case WM_DPICHANGED: {
        gScale = float(HIWORD(wp)) / 96.0f;
        gFontsDirty = true;
        const RECT* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        std::vector<std::string> paths;
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring w(len, L'\0');
            DragQueryFileW(drop, i, w.data(), len + 1);
            paths.push_back(narrow(w));
        }
        DragFinish(drop);
        if (gUi) gUi->onFilesDropped(paths);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;                  // pas de menu systeme avec Alt
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

UiFontFiles systemFonts() {
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::string dir = narrow(win) + "\\Fonts\\";
    auto pick = [&](std::initializer_list<const char*> names) -> std::string {
        for (const char* n : names) {
            const std::string p = dir + n;
            if (GetFileAttributesW(widen(p).c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
        return "";
    };
    UiFontFiles f;
    f.regular = pick({ "segoeui.ttf", "arial.ttf" });
    f.semibold = pick({ "seguisb.ttf", "segoeuib.ttf", "arialbd.ttf" });
    f.bold = pick({ "segoeuib.ttf", "arialbd.ttf" });
    f.mono = pick({ "consola.ttf", "cour.ttf" });
    f.monoBold = pick({ "consolab.ttf", "courbd.ttf" });
    return f;
}

}  // namespace

HICON makeAppIcon(int size) { return makeIcon(size); }
UiFontFiles systemUiFonts() { return systemFonts(); }
std::wstring exeDirectory() { return exeDir(); }
UINT windowDpi(HWND hwnd) { return dpiOf(hwnd); }
std::string utf8FromWide(const std::wstring& w) { return narrow(w); }
std::wstring wideFromUtf8(const std::string& s) { return widen(s); }
void enablePerMonitorDpi() {
    using SetCtx = BOOL(WINAPI*)(HANDLE);
    if (auto fn = reinterpret_cast<SetCtx>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))))
        fn(reinterpret_cast<HANDLE>(-4));                           // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}

int runLibrary() {
    // la console n'est pas utile ici : on la cache si elle a ete ouverte pour nous (double-clic sur l'exe)
    {
        DWORD procs[2];
        if (GetConsoleProcessList(procs, 2) <= 1) FreeConsole();
    }
    enablePerMonitorDpi();
    removeOldUpdateFiles();                                         // restes d'une mise a jour precedente
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = makeIcon(48);
    wc.hIconSm = makeIcon(16);
    wc.lpszClassName = L"ndsemu_bibliotheque";
    RegisterClassExW(&wc);

    // taille de depart : 1230 x 840 a 96 ppp, centree sur l'ecran principal
    const UINT sysDpi = dpiOf(nullptr);
    gScale = float(sysDpi) / 96.0f;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = int(1230 * gScale), h = int(840 * gScale);
    w = std::min(w, int(work.right - work.left) - 40);
    h = std::min(h, int(work.bottom - work.top) - 40);
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"ndsemu", WS_OVERLAPPEDWINDOW,
        work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
        nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    gScale = float(dpiOf(hwnd)) / 96.0f;

    // coins arrondis (Windows 11), ombre, barre de titre sombre
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    const int round = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    const MARGINS shadow = { 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &shadow);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    if (!createDevice(hwnd)) {
        MessageBoxW(hwnd, L"Direct3D 11 n'a pas pu être initialisé.", L"ndsemu", MB_ICONERROR);
        destroyDevice();
        DestroyWindow(hwnd);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    DragAcceptFiles(hwnd, TRUE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(gDevice, gContext);

    Library lib;
    lib.configPath = narrow(exeDir() + L"\\ndsemu_bibliotheque.ini");
    lib.load();
    Win32Host host;
    host.hwnd = hwnd;
    host.upd = createWin32Updater(hwnd);
    LibraryUI ui(lib, host);
    gHost = &host;
    gUi = &ui;
    const UiFontFiles fontFiles = systemFonts();
    auto applyScale = [&]() {
        ui.setScale(gScale);
        ui.setFonts(loadUiFonts(fontFiles, gScale));
        ui.applyStyle();
    };
    applyScale();

    const auto t0 = std::chrono::steady_clock::now();
    auto lastInput = t0;
    bool quit = false;
    while (!quit) {
        // au repos (aucune entree depuis 1 s), on attend un message au plus 250 ms : peu de processeur utilise
        // pendant qu'un jeu tourne a cote
        const auto now = std::chrono::steady_clock::now();
        if (now - lastInput > std::chrono::seconds(1) && !host.inputEditor.isOpen()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) quit = true;
            if (msg.message != WM_TIMER && msg.message != WM_PAINT) lastInput = std::chrono::steady_clock::now();
        }
        if (quit) break;
        if (IsIconic(hwnd)) { Sleep(50); continue; }
        if (gResize) {
            gResize = false;
            releaseTarget();
            gSwap->ResizeBuffers(0, gResizeW, gResizeH, DXGI_FORMAT_UNKNOWN, 0);
            createTarget();
        }
        if (gFontsDirty) {
            gFontsDirty = false;
            ImGui_ImplDX11_InvalidateDeviceObjects();
            applyScale();
            ImGui_ImplDX11_CreateDeviceObjects();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ui.draw(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        ImGui::Render();
        const float clear[4] = { 0.102f, 0.110f, 0.122f, 1.0f };
        gContext->OMSetRenderTargets(1, &gTarget, nullptr);
        gContext->ClearRenderTargetView(gTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        gSwap->Present(1, 0);
    }

    lib.save();
    host.upd.reset();                                               // attend la fin d'un telechargement en cours
    ui.releaseTextures();
    gUi = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroyDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    CoUninitialize();
    relaunchAfterUpdate();                                          // "Redemarrer maintenant" apres une mise a jour
    return 0;
}
#endif
