#ifdef _WIN32
#include "platform/input_win32.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#include <setupapi.h>
extern "C" {                        // les en-tetes HID ne le declarent pas toujours eux-memes
#include <hidsdi.h>
}
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "imgui.h"
#include "ui/library_ui.hpp"
#include "ui/win32_common.hpp"

namespace {

// XInput est charge a l'execution : rien a lier, et le programme demarre meme sans la DLL
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
GetStateFn xinputGetState() {
    static bool tried = false;
    static GetStateFn fn = nullptr;
    if (!tried) {
        tried = true;
        for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" }) {
            if (HMODULE m = LoadLibraryW(dll)) {
                fn = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
                if (fn) break;
            }
        }
    }
    return fn;
}

uint32_t readPad(int index, bool& ok) {
    ok = false;
    GetStateFn fn = xinputGetState();
    if (!fn) return 0;
    XINPUT_STATE st = {};
    if (fn(DWORD(index), &st) != ERROR_SUCCESS) return 0;
    ok = true;
    const XINPUT_GAMEPAD& g = st.Gamepad;
    uint32_t v = g.wButtons;
    if (g.bLeftTrigger > 100) v |= PadLT;
    if (g.bRightTrigger > 100) v |= PadRT;
    const int dz = 16000;                                  // zone morte du stick (sur 32767)
    if (g.sThumbLX < -dz) v |= PadStickLeft;
    if (g.sThumbLX > dz) v |= PadStickRight;
    if (g.sThumbLY > dz) v |= PadStickUp;
    if (g.sThumbLY < -dz) v |= PadStickDown;
    return v;
}

// Touches qui ont deja un role dans la fenetre de jeu
bool reservedKey(int vk) {
    return vk == 'P' || vk == 'M' || vk == VK_F11 || vk == VK_F12 || vk == VK_ESCAPE || vk == VK_CONTROL ||
           vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || (vk >= VK_F1 && vk <= VK_F4);
}

}  // namespace

// ---- configuration ----------------------------------------------------------------------------------------
InputConfig::InputConfig() {
    const int keys[BtnCount] = { 'Z', 'X', VK_BACK, VK_RETURN, VK_RIGHT, VK_LEFT, VK_UP, VK_DOWN, 'W', 'Q', 'S', 'A' };
    // Disposition Nintendo : le A de la DS est a droite, comme le B d'une manette Xbox
    const uint32_t pads[BtnCount] = { XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_X };
    for (int i = 0; i < BtnCount; i++) { key[i] = keys[i]; pad[i] = pads[i]; }
}

std::string inputConfigPath() { return utf8FromWide(exeDirectory() + L"\\ndsemu_touches.ini"); }

int64_t inputConfigStamp() {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(std::filesystem::u8path(inputConfigPath()), ec);
    return ec ? 0 : int64_t(t.time_since_epoch().count());
}

bool InputConfig::load() {
    std::ifstream in(std::filesystem::u8path(inputConfigPath()));
    if (!in) return false;
    static const char* names[BtnCount] = { "a", "b", "select", "start", "droite", "gauche", "haut", "bas", "r", "l", "x", "y" };
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        const std::string k = line.substr(0, eq);
        const char* v = line.c_str() + eq + 1;
        if (k == "stick_croix") { stickAsDpad = std::atoi(v) != 0; continue; }
        for (int i = 0; i < BtnCount; i++) {
            if (k == std::string("clavier_") + names[i]) key[i] = std::atoi(v) & 0xFF;
            else if (k == std::string("manette_") + names[i]) pad[i] = uint32_t(std::strtoul(v, nullptr, 10));
        }
    }
    return true;
}

void InputConfig::save() const {
    std::ofstream out(std::filesystem::u8path(inputConfigPath()), std::ios::trunc);
    if (!out) return;
    static const char* names[BtnCount] = { "a", "b", "select", "start", "droite", "gauche", "haut", "bas", "r", "l", "x", "y" };
    out << "# Touches de ndsemu (ecrit automatiquement ; clavier = code de touche Windows, manette = entree XInput)\n";
    for (int i = 0; i < BtnCount; i++) out << "clavier_" << names[i] << "=" << key[i] << "\n";
    for (int i = 0; i < BtnCount; i++) out << "manette_" << names[i] << "=" << pad[i] << "\n";
    out << "stick_croix=" << (stickAsDpad ? 1 : 0) << "\n";
}

const char* dsButtonName(int b) {
    static const char* n[BtnCount] = { "A", "B", "Select", "Start", "Droite", "Gauche", "Haut", "Bas", "R", "L", "X", "Y" };
    return (b >= 0 && b < BtnCount) ? n[b] : "?";
}

std::string keyName(int vk) {
    switch (vk) {
        case 0: return "\xE2\x80\x94";
        case VK_LEFT: return "Fl\xC3\xA8" "che gauche";
        case VK_RIGHT: return "Fl\xC3\xA8" "che droite";
        case VK_UP: return "Fl\xC3\xA8" "che haut";
        case VK_DOWN: return "Fl\xC3\xA8" "che bas";
        case VK_RETURN: return "Entr\xC3\xA9" "e";
        case VK_BACK: return "Retour arri\xC3\xA8re";
        case VK_SPACE: return "Espace";
        case VK_TAB: return "Tab";
        case VK_SHIFT: return "Maj";
        case VK_CAPITAL: return "Verr. maj";
        case VK_INSERT: return "Inser";
        case VK_DELETE: return "Suppr";
        case VK_HOME: return "D\xC3\xA9" "but";
        case VK_END: return "Fin";
        case VK_PRIOR: return "Page pr\xC3\xA9" "c.";
        case VK_NEXT: return "Page suiv.";
        default: break;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Pav\xC3\xA9 " + std::to_string(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    wchar_t buf[64] = {};
    const UINT sc = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextW(LONG(sc << 16), buf, 64) > 0) return utf8FromWide(buf);
    char tmp[16];
    std::snprintf(tmp, sizeof tmp, "Touche %d", vk);
    return tmp;
}

std::string padInputName(uint32_t bit, bool ps) {
    if (ps) {
        switch (bit) {
            case XINPUT_GAMEPAD_A: return "Croix";
            case XINPUT_GAMEPAD_B: return "Rond";
            case XINPUT_GAMEPAD_X: return "Carr\xC3\xA9";
            case XINPUT_GAMEPAD_Y: return "Triangle";
            case XINPUT_GAMEPAD_LEFT_SHOULDER: return "L1";
            case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "R1";
            case PadLT: return "L2";
            case PadRT: return "R2";
            case XINPUT_GAMEPAD_BACK: return "Create / Share";
            case XINPUT_GAMEPAD_START: return "Options";
            case XINPUT_GAMEPAD_LEFT_THUMB: return "L3";
            case XINPUT_GAMEPAD_RIGHT_THUMB: return "R3";
            default: break;
        }
    }
    switch (bit) {
        case PadHome: return ps ? "PS" : "Guide";
        case PadTouchpad: return "Pav\xC3\xA9 tactile";
        case 0: return "\xE2\x80\x94";
        case XINPUT_GAMEPAD_A: return "A";
        case XINPUT_GAMEPAD_B: return "B";
        case XINPUT_GAMEPAD_X: return "X";
        case XINPUT_GAMEPAD_Y: return "Y";
        case XINPUT_GAMEPAD_LEFT_SHOULDER: return "LB";
        case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "RB";
        case XINPUT_GAMEPAD_BACK: return "View / Back";
        case XINPUT_GAMEPAD_START: return "Menu / Start";
        case XINPUT_GAMEPAD_LEFT_THUMB: return "Clic stick gauche";
        case XINPUT_GAMEPAD_RIGHT_THUMB: return "Clic stick droit";
        case XINPUT_GAMEPAD_DPAD_UP: return "Croix haut";
        case XINPUT_GAMEPAD_DPAD_DOWN: return "Croix bas";
        case XINPUT_GAMEPAD_DPAD_LEFT: return "Croix gauche";
        case XINPUT_GAMEPAD_DPAD_RIGHT: return "Croix droite";
        case PadLT: return "LT";
        case PadRT: return "RT";
        case PadStickLeft: return "Stick gauche \xE2\x86\x90";
        case PadStickRight: return "Stick gauche \xE2\x86\x92";
        case PadStickUp: return "Stick gauche \xE2\x86\x91";
        case PadStickDown: return "Stick gauche \xE2\x86\x93";
        default: return "?";
    }
}

// ---- manettes PlayStation (HID) ---------------------------------------------------------------------------
namespace {

// Croix directionnelle codee comme une boussole : 0 = haut, 1 = haut-droite... 7 = haut-gauche, 8 = relachee
uint32_t hatBits(int hat) {
    static const uint32_t m[8] = {
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP };
    return (hat >= 0 && hat < 8) ? m[hat] : 0u;
}

// Octets communs a toutes ces manettes : sticks (0 a 255, 128 au centre), puis 3 octets de boutons
//   b0 : croix directionnelle (4 bits), Carre, Croix, Rond, Triangle
//   b1 : L1, R1, L2, R2, Create/Share, Options, L3, R3        b2 : PS, pave tactile
uint32_t psButtons(uint8_t lx, uint8_t ly, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t l2, uint8_t r2) {
    uint32_t v = hatBits(b0 & 15);
    if (b0 & 0x10) v |= XINPUT_GAMEPAD_X;          // Carre
    if (b0 & 0x20) v |= XINPUT_GAMEPAD_A;          // Croix
    if (b0 & 0x40) v |= XINPUT_GAMEPAD_B;          // Rond
    if (b0 & 0x80) v |= XINPUT_GAMEPAD_Y;          // Triangle
    if (b1 & 0x01) v |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (b1 & 0x02) v |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if ((b1 & 0x04) || l2 > 100) v |= PadLT;
    if ((b1 & 0x08) || r2 > 100) v |= PadRT;
    if (b1 & 0x10) v |= XINPUT_GAMEPAD_BACK;
    if (b1 & 0x20) v |= XINPUT_GAMEPAD_START;
    if (b1 & 0x40) v |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (b1 & 0x80) v |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (b2 & 0x01) v |= PadHome;
    if (b2 & 0x02) v |= PadTouchpad;
    if (lx < 64) v |= PadStickLeft;
    if (lx > 192) v |= PadStickRight;
    if (ly < 64) v |= PadStickUp;                  // vers le haut = valeur petite
    if (ly > 192) v |= PadStickDown;
    return v;
}

// Rapport d'entree -> boutons. model : 1 DualSense, 2 DualShock 4 ; reportLen = taille des rapports du peripherique.
bool parsePsReport(const uint8_t* r, size_t n, int model, size_t reportLen, uint32_t& out) {
    if (n < 10) return false;
    if (model == 1) {
        if (r[0] == 0x01 && reportLen >= 64 && reportLen < 78 && n >= 11) {          // USB : rapport complet
            out = psButtons(r[1], r[2], r[8], r[9], r[10], r[5], r[6]);
            return true;
        }
        if (r[0] == 0x31 && n >= 12) {                                               // Bluetooth, rapport complet
            out = psButtons(r[2], r[3], r[9], r[10], r[11], r[6], r[7]);
            return true;
        }
        if (r[0] == 0x01) {                                                          // Bluetooth, rapport simple
            out = psButtons(r[1], r[2], r[5], r[6], r[7], r[8], r[9]);
            return true;
        }
        return false;
    }
    if (r[0] == 0x01) { out = psButtons(r[1], r[2], r[5], r[6], r[7], r[8], r[9]); return true; }   // DS4 en USB
    if (r[0] == 0x11 && n >= 12) { out = psButtons(r[3], r[4], r[7], r[8], r[9], r[10], r[11]); return true; }   // DS4 en Bluetooth
    return false;
}

// Cherche une manette PlayStation parmi les peripheriques HID ; renvoie un acces en lecture (asynchrone).
HANDLE openPlayStationPad(int& model, size_t& reportLen) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd = {};
    ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<uint8_t> buf(need);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE)                     // certains pilotes refusent l'ecriture : lecture seule
            h = CreateFileW(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at = {};
        at.Size = sizeof at;
        int m = 0;
        if (HidD_GetAttributes(h, &at) && at.VendorID == 0x054C) {                   // Sony
            if (at.ProductID == 0x0CE6 || at.ProductID == 0x0DF2) m = 1;            // DualSense, DualSense Edge
            else if (at.ProductID == 0x05C4 || at.ProductID == 0x09CC || at.ProductID == 0x0BA0) m = 2;   // DualShock 4
        }
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps = {};
        if (m && HidD_GetPreparsedData(h, &pp)) {
            HidP_GetCaps(pp, &caps);
            HidD_FreePreparsedData(pp);
            // on ne garde que l'interface "manette" (page 1, usage 5)
            if (caps.UsagePage == 1 && caps.Usage == 5 && caps.InputReportByteLength >= 10) {
                model = m;
                reportLen = caps.InputReportByteLength;
                found = h;
                // En Bluetooth, lire le rapport de calibration fait passer la manette aux rapports complets
                uint8_t feat[80] = {};
                feat[0] = m == 1 ? 0x05 : 0x02;
                HidD_GetFeature(h, feat, m == 1 ? 41 : 37);
                continue;
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

}  // namespace

PlayStationPad::PlayStationPad() { thread_ = std::thread([this] { run(); }); }
PlayStationPad::~PlayStationPad() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}
const char* PlayStationPad::model() const {
    switch (model_.load()) {
        case 1: return "DualSense (PS5)";
        case 2: return "DualShock 4 (PS4)";
        default: return "";
    }
}

void PlayStationPad::run() {
    while (!stop_) {
        int model = 0;
        size_t len = 0;
        HANDLE h = openPlayStationPad(model, len);
        if (h == INVALID_HANDLE_VALUE) {                    // rien : on recommence dans 2 secondes
            for (int i = 0; i < 20 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        model_ = model;
        connected_ = true;
        std::vector<uint8_t> buf(len);
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool alive = true;
        while (!stop_ && alive) {
            ResetEvent(ov.hEvent);
            DWORD got = 0;
            if (!ReadFile(h, buf.data(), DWORD(len), &got, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) { alive = false; break; }
                // attente par tranches de 100 ms pour pouvoir s'arreter
                DWORD w;
                while ((w = WaitForSingleObject(ov.hEvent, 100)) == WAIT_TIMEOUT && !stop_) {}
                if (stop_) { CancelIo(h); WaitForSingleObject(ov.hEvent, 200); break; }
                if (!GetOverlappedResult(h, &ov, &got, FALSE)) { alive = false; break; }
            }
            uint32_t v = 0;
            if (parsePsReport(buf.data(), got, model, len, v)) state_ = v;
        }
        CloseHandle(ov.hEvent);
        CloseHandle(h);
        state_ = 0;
        connected_ = false;
        model_ = 0;
    }
}

// ---- manette ----------------------------------------------------------------------------------------------
void Gamepad::poll() {
    bool ok = false;
    if (index_ >= 0) {
        state_ = readPad(index_, ok);
        if (!ok) { index_ = -1; state_ = 0; }
        return;
    }
    // aucune manette : on en cherche une toutes les ~2 secondes (interroger une manette absente est lent)
    if (probeWait_-- > 0) return;
    probeWait_ = 120;
    for (int i = 0; i < 4; i++) {
        const uint32_t v = readPad(i, ok);
        if (ok) { index_ = i; state_ = v; return; }
    }
}

// ---- fenetre de reglage -----------------------------------------------------------------------------------
bool InputEditor::draw(const UiFonts& f, float s) {
    auto S = [s](float v) { return v * s; };
    if (request_) {
        request_ = false;
        cfg_ = InputConfig();
        cfg_.load();
        captureRow_ = -1;
        message_.clear();
        ImGui::OpenPopup("Touches et manette");
    }
    pad_.poll();
    bool saved = false;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(8)));
    visible_ = false;
    if (ImGui::BeginPopupModal("Touches et manette", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        visible_ = true;
        ImGui::PushFont(f.title);
        ImGui::TextUnformatted("Touches et manette");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.63f, 0.65f, 1.0f));
        ImGui::TextUnformatted("Clique sur une case, puis appuie sur la touche ou le bouton voulu.");
        ImGui::PopStyleColor();
        // etat de la manette
        {
            const ImVec4 c = pad_.connected() ? ImVec4(0.24f, 0.84f, 0.55f, 1.0f) : ImVec4(0.5f, 0.52f, 0.55f, 1.0f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddCircleFilled(ImVec2(p.x + S(5), p.y + f.body->FontSize * 0.5f + S(1)), S(4), ImGui::ColorConvertFloat4ToU32(c), 12);
            ImGui::Dummy(ImVec2(S(12), 0));
            ImGui::SameLine();
            if (pad_.connected()) ImGui::Text("Manette branch\xC3\xA9" "e : %s", pad_.model());
            else ImGui::TextUnformatted("Aucune manette d\xC3\xA9tect\xC3\xA9" "e (Xbox, PS5 DualSense ou PS4 DualShock 4)");
        }
        ImGui::Spacing();

        // Capture en cours : premiere touche / premier bouton nouvellement appuye
        if (captureRow_ >= 0) {
            if (captureCol_ == 0) {
                for (int vk = 8; vk < 256; vk++) {
                    if (vk >= 0xA0 && vk <= 0xA5) continue;            // Maj / Ctrl / Alt gauche-droite : on garde la touche generique
                    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    if (down && !keyDownAtStart_[vk]) {
                        if (vk == VK_ESCAPE) { captureRow_ = -1; message_.clear(); }
                        else if (reservedKey(vk)) message_ = keyName(vk) + " est r\xC3\xA9serv\xC3\xA9" "e aux raccourcis de la fen\xC3\xAAtre de jeu";
                        else {
                            for (int i = 0; i < BtnCount; i++) if (i != captureRow_ && cfg_.key[i] == vk) cfg_.key[i] = 0;   // pas de doublon
                            cfg_.key[captureRow_] = vk;
                            captureRow_ = -1;
                            message_.clear();
                        }
                        break;
                    }
                    if (!down) keyDownAtStart_[vk] = false;
                }
            } else {
                const uint32_t now = pad_.state();
                const uint32_t fresh = now & ~padAtStart_;
                padAtStart_ &= now;
                if (fresh) {
                    uint32_t bit = fresh & (~fresh + 1);               // un seul bit
                    for (int i = 0; i < BtnCount; i++) if (i != captureRow_ && cfg_.pad[i] == bit) cfg_.pad[i] = 0;
                    cfg_.pad[captureRow_] = bit;
                    captureRow_ = -1;
                    message_.clear();
                } else if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                    captureRow_ = -1;
                }
            }
        }

        if (ImGui::BeginTable("##touches", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Console", ImGuiTableColumnFlags_WidthFixed, S(110));
            ImGui::TableSetupColumn("Clavier", ImGuiTableColumnFlags_WidthFixed, S(190));
            ImGui::TableSetupColumn("Manette", ImGuiTableColumnFlags_WidthFixed, S(190));
            ImGui::PushFont(f.caps);
            ImGui::TableHeadersRow();
            ImGui::PopFont();
            for (int i = 0; i < BtnCount; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::PushFont(f.medium);
                ImGui::TextUnformatted(dsButtonName(i));
                ImGui::PopFont();
                for (int col = 0; col < 2; col++) {
                    ImGui::TableNextColumn();
                    const bool waiting = captureRow_ == i && captureCol_ == col;
                    std::string label = waiting ? (col == 0 ? std::string("Appuie sur une touche\xE2\x80\xA6") : std::string("Appuie sur un bouton\xE2\x80\xA6"))
                                                : (col == 0 ? keyName(cfg_.key[i]) : padInputName(cfg_.pad[i], pad_.playstation()));
                    label += "##" + std::to_string(i * 2 + col);
                    if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.84f, 0.55f, 0.25f));
                    if (ImGui::Button(label.c_str(), ImVec2(S(180), 0))) {
                        captureRow_ = i;
                        captureCol_ = col;
                        message_.clear();
                        for (int vk = 0; vk < 256; vk++) keyDownAtStart_[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
                        padAtStart_ = pad_.state();
                    }
                    if (waiting) ImGui::PopStyleColor();
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {   // clic droit : effacer
                        if (col == 0) cfg_.key[i] = 0; else cfg_.pad[i] = 0;
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::Checkbox("Le stick gauche sert aussi de croix directionnelle", &cfg_.stickAsDpad);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.63f, 0.65f, 1.0f));
        ImGui::TextUnformatted("Clic droit sur une case pour l'effacer. \xC3\x89" "chap annule l'attente.");
        ImGui::PopStyleColor();
        if (!message_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
            ImGui::TextUnformatted(message_.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        if (ImGui::Button("Valeurs par d\xC3\xA9" "faut", ImVec2(S(170), 0))) { cfg_ = InputConfig(); captureRow_ = -1; }
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(S(100), 0));
        ImGui::SameLine();
        if (ImGui::Button("Annuler", ImVec2(S(100), 0))) { captureRow_ = -1; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.84f, 0.55f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.33f, 0.88f, 0.62f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.75f, 0.5f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.04f, 0.14f, 0.09f, 1.0f));
        if (ImGui::Button("Enregistrer", ImVec2(S(120), 0))) {
            cfg_.save();
            saved = true;
            captureRow_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(4);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    return saved;
}
#endif
