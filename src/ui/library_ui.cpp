#include "ui/library_ui.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/updater.hpp"

// Interface de la bibliotheque : tout est dessine a la main (ImDrawList) pour suivre la maquette : barre de titre,
// menus, recherche, colonnes, dossiers en cartes, etat vide avec illustration, pied de page.

namespace {

// ---- couleurs (theme sombre) ----
constexpr ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
const ImU32 kBg        = rgb(0x1A, 0x1C, 0x1F);
const ImU32 kLine      = rgb(0x2A, 0x2D, 0x31);
const ImU32 kCard      = rgb(0x1F, 0x21, 0x25);
const ImU32 kCardLine  = rgb(0x31, 0x34, 0x39);
const ImU32 kHover     = rgb(0x26, 0x29, 0x2D);
const ImU32 kText      = rgb(0xE8, 0xEA, 0xED);
const ImU32 kDim       = rgb(0x9A, 0xA0, 0xA6);
const ImU32 kFaint     = rgb(0x7A, 0x7F, 0x86);
const ImU32 kAccent    = rgb(0x3D, 0xD6, 0x8C);
const ImU32 kAccentDim = rgb(0x3D, 0xD6, 0x8C, 40);
const ImU32 kOnAccent  = rgb(0x0B, 0x24, 0x18);
const ImU32 kField     = rgb(0x20, 0x22, 0x26);
const ImU32 kFieldLine = rgb(0x36, 0x39, 0x3E);
const ImU32 kPill      = rgb(0x2A, 0x2D, 0x32);
const ImU32 kDanger    = rgb(0xFF, 0x5D, 0x5D);
const ImU32 kFolder    = rgb(0xF5, 0xB9, 0x4A);

ImU32 compatColor(Compat c) {
    switch (c) {
        case Compat::Perfect:  return rgb(0x4A, 0xA3, 0xFF);
        case Compat::Playable: return kAccent;
        case Compat::InGame:   return rgb(0xF5, 0xC4, 0x51);
        case Compat::Menus:    return rgb(0xFF, 0x9F, 0x43);
        case Compat::Broken:   return kDanger;
        default:               return rgb(0x80, 0x86, 0x8D);
    }
}

ImU32 withAlpha(ImU32 c, int a) { return (c & 0x00FFFFFFu) | (ImU32(a) << 24); }

ImVec2 textSize(ImFont* f, const char* t) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, t); }

void text(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 c, const char* t, const char* end = nullptr) {
    dl->AddText(f, f->FontSize, p, c, t, end);
}

// Texte en petites capitales espacees (en-tetes de colonnes)
// longueur en octets du caractere UTF-8 qui commence en p
int utf8Len(const char* p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
}
float spacedWidth(ImFont* f, const char* t, float spacing) {
    float w = 0;
    for (const char* p = t; *p; p += utf8Len(p)) w += textSize(f, std::string(p, size_t(utf8Len(p))).c_str()).x + spacing;
    return w > 0 ? w - spacing : 0;
}
void spacedText(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 c, const char* t, float spacing) {
    for (const char* q = t; *q; q += utf8Len(q)) {
        const std::string ch(q, size_t(utf8Len(q)));
        dl->AddText(f, f->FontSize, p, c, ch.c_str());
        p.x += textSize(f, ch.c_str()).x + spacing;
    }
}

// Rectangle arrondi en pointilles (zone ou l'on peut deposer des fichiers)
void dashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float r, float dash, float gap, float th) {
    auto line = [&](ImVec2 p, ImVec2 q) {
        const float len = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
        if (len <= 0) return;
        const ImVec2 d((q.x - p.x) / len, (q.y - p.y) / len);
        for (float t = 0; t < len; t += dash + gap) {
            const float e = std::min(len, t + dash);
            dl->AddLine(ImVec2(p.x + d.x * t, p.y + d.y * t), ImVec2(p.x + d.x * e, p.y + d.y * e), c, th);
        }
    };
    line(ImVec2(a.x + r, a.y), ImVec2(b.x - r, a.y));
    line(ImVec2(b.x, a.y + r), ImVec2(b.x, b.y - r));
    line(ImVec2(b.x - r, b.y), ImVec2(a.x + r, b.y));
    line(ImVec2(a.x, b.y - r), ImVec2(a.x, a.y + r));
    const float pi = 3.14159265f;
    dl->PathArcTo(ImVec2(a.x + r, a.y + r), r, pi, pi * 1.5f, 6); dl->PathStroke(c, 0, th);
    dl->PathArcTo(ImVec2(b.x - r, a.y + r), r, pi * 1.5f, pi * 2.0f, 6); dl->PathStroke(c, 0, th);
    dl->PathArcTo(ImVec2(b.x - r, b.y - r), r, 0, pi * 0.5f, 6); dl->PathStroke(c, 0, th);
    dl->PathArcTo(ImVec2(a.x + r, b.y - r), r, pi * 0.5f, pi, 6); dl->PathStroke(c, 0, th);
}

void plusIcon(ImDrawList* dl, ImVec2 c, float h, ImU32 col, float th) {
    dl->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x + h, c.y), col, th);
    dl->AddLine(ImVec2(c.x, c.y - h), ImVec2(c.x, c.y + h), col, th);
}

std::string lowerStr(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool matches(const GameEntry& g, const std::string& q) {
    if (q.empty()) return true;
    return lowerStr(g.title).find(q) != std::string::npos || lowerStr(g.fileName).find(q) != std::string::npos ||
           lowerStr(g.gameCode).find(q) != std::string::npos || lowerStr(g.subtitle).find(q) != std::string::npos;
}

// "a l'instant", "il y a 5 min", "hier", "il y a 3 jours", "12/10/2026"
std::string agoText(int64_t when) {
    const int64_t now = int64_t(std::time(nullptr));
    const int64_t d = now - when;
    char buf[48];
    if (d < 60) return "\xC3\x80 l'instant";
    if (d < 3600) { std::snprintf(buf, sizeof buf, "Il y a %lld min", (long long)(d / 60)); return buf; }
    if (d < 86400) { std::snprintf(buf, sizeof buf, "Il y a %lld h", (long long)(d / 3600)); return buf; }
    if (d < 2 * 86400) return "Hier";
    if (d < 7 * 86400) { std::snprintf(buf, sizeof buf, "Il y a %lld jours", (long long)(d / 86400)); return buf; }
    std::time_t t = std::time_t(when);
    if (const std::tm* tmv = std::localtime(&t)) { std::strftime(buf, sizeof buf, "%d/%m/%Y", tmv); return buf; }
    return "";
}

// "moins d'une minute", "25 min", "3 h 05"
std::string durationText(int64_t s) {
    char buf[32];
    if (s < 60) return "moins d'une minute";
    if (s < 3600) { std::snprintf(buf, sizeof buf, "%lld min", (long long)(s / 60)); return buf; }
    std::snprintf(buf, sizeof buf, "%lld h %02lld", (long long)(s / 3600), (long long)((s / 60) % 60));
    return buf;
}

}  // namespace

// ---- polices ---------------------------------------------------------------------------------------------
UiFonts loadUiFonts(const UiFontFiles& files, float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0152, 0x0153, 0x2013, 0x2026, 0x2022, 0x2022, 0x2190, 0x2193, 0x2318, 0x2318, 0 };
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    auto load = [&](const std::string& path, float size) -> ImFont* {
        ImFont* f = nullptr;
        if (!path.empty()) f = io.Fonts->AddFontFromFileTTF(path.c_str(), size * scale, &cfg, ranges);
        if (!f) { ImFontConfig d; d.SizePixels = size * scale; f = io.Fonts->AddFontDefault(&d); }
        return f;
    };
    UiFonts f;
    f.body = load(files.regular, 15.0f);                  // la premiere chargee est la police par defaut
    f.medium = load(files.semibold, 15.0f);
    f.title = load(files.semibold, 18.0f);
    f.caps = load(files.semibold, 11.5f);
    f.mono = load(files.mono, 14.0f);
    f.monoBold = load(files.monoBold.empty() ? files.mono : files.monoBold, 14.0f);
    return f;
}

void LibraryUI::applyStyle() { applyDarkStyle(s_); }

void applyDarkStyle(float scale) {
    auto S = [scale](float v) { return v * scale; };
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowPadding = ImVec2(S(6), S(6));
    st.FramePadding = ImVec2(S(10), S(7));
    st.ItemSpacing = ImVec2(S(10), S(9));
    st.PopupRounding = S(8);
    st.FrameRounding = S(8);
    st.WindowRounding = 0;
    st.ChildRounding = 0;
    st.ScrollbarRounding = S(6);
    st.ScrollbarSize = S(10);
    st.GrabRounding = S(6);
    st.PopupBorderSize = 1;
    st.WindowBorderSize = 0;
    st.FrameBorderSize = 1;
    ImVec4* c = st.Colors;
    auto v = [](ImU32 x) { return ImGui::ColorConvertU32ToFloat4(x); };
    c[ImGuiCol_Text] = v(kText);
    c[ImGuiCol_TextDisabled] = v(kFaint);
    c[ImGuiCol_WindowBg] = v(kBg);
    c[ImGuiCol_ChildBg] = v(rgb(0, 0, 0, 0));
    c[ImGuiCol_PopupBg] = v(rgb(0x23, 0x25, 0x29));
    c[ImGuiCol_Border] = v(kFieldLine);
    c[ImGuiCol_FrameBg] = v(kField);
    c[ImGuiCol_FrameBgHovered] = v(rgb(0x24, 0x26, 0x2A));
    c[ImGuiCol_FrameBgActive] = v(rgb(0x24, 0x26, 0x2A));
    c[ImGuiCol_Header] = v(rgb(0x2C, 0x2F, 0x34));
    c[ImGuiCol_HeaderHovered] = v(rgb(0x2E, 0x32, 0x37));
    c[ImGuiCol_HeaderActive] = v(rgb(0x33, 0x37, 0x3C));
    c[ImGuiCol_Separator] = v(kLine);
    c[ImGuiCol_ScrollbarBg] = v(rgb(0, 0, 0, 0));
    c[ImGuiCol_ScrollbarGrab] = v(rgb(0x3A, 0x3D, 0x42));
    c[ImGuiCol_ScrollbarGrabHovered] = v(rgb(0x48, 0x4C, 0x52));
    c[ImGuiCol_ScrollbarGrabActive] = v(rgb(0x55, 0x59, 0x60));
    c[ImGuiCol_CheckMark] = v(kAccent);
    c[ImGuiCol_Button] = v(kField);
    c[ImGuiCol_ButtonHovered] = v(kHover);
    c[ImGuiCol_ButtonActive] = v(kPill);
    c[ImGuiCol_NavHighlight] = v(withAlpha(kAccent, 120));
    c[ImGuiCol_ModalWindowDimBg] = v(rgb(0, 0, 0, 140));
    c[ImGuiCol_TextSelectedBg] = v(withAlpha(kAccent, 70));
}

// ---- utilitaires -----------------------------------------------------------------------------------------
void* LibraryUI::iconTexture(const GameEntry& g) {
    if (!g.hasIcon) return nullptr;
    auto it = textures_.find(g.path);
    if (it != textures_.end() && it->second.first == g.mtime) return it->second.second;
    if (it != textures_.end()) host_.destroyTexture(it->second.second);
    void* t = host_.createTexture(g.icon, 32, 32);
    textures_[g.path] = { g.mtime, t };
    return t;
}

void LibraryUI::releaseTextures() {
    for (auto& kv : textures_) host_.destroyTexture(kv.second.second);
    textures_.clear();
}

void LibraryUI::notify(const std::string& t, bool error) {
    toast_ = t;
    toastError_ = error;
    toastUntil_ = now_ + (error ? 7.0 : 3.5);
}

void LibraryUI::launch(const GameEntry& g) {
    selected_ = g.path;
    if (host_.isRunning(g.path)) { notify(g.title + " est d\xC3\xA9j\xC3\xA0 lanc\xC3\xA9", false); return; }
    std::string err;
    if (host_.launch(g.path, lib_, err)) notify("Lancement de " + g.title + "\xE2\x80\xA6", false);
    else notify("Impossible de lancer le jeu : " + err, true);
}

void LibraryUI::addFolderDialog() {
    const std::string p = host_.pickFolder();
    if (p.empty()) return;
    if (lib_.addFolder(p)) notify("Dossier ajout\xC3\xA9", false);
    else notify("Ce dossier est d\xC3\xA9j\xC3\xA0 dans la biblioth\xC3\xA8que", false);
}

void LibraryUI::openRomDialog() {
    const std::string p = host_.pickRom();
    if (p.empty()) return;
    GameEntry g;
    if (!Library::readRomInfo(p, g)) { notify("Ce fichier n'est pas une ROM Nintendo DS lisible", true); return; }
    lib_.addFile(p);
    launch(g);
}

void LibraryUI::onFilesDropped(const std::vector<std::string>& paths) {
    int folders = 0, files = 0;
    for (const std::string& p : paths) {
        std::error_code ec;
        const std::filesystem::path fp = std::filesystem::u8path(p);
        if (std::filesystem::is_directory(fp, ec)) { if (lib_.addFolder(p)) folders++; }
        else if (lowerStr(fp.extension().u8string()) == ".nds") { lib_.addFile(p); files++; }
    }
    if (folders + files == 0) notify("Rien \xC3\xA0 ajouter : d\xC3\xA9pose des fichiers .nds ou un dossier", true);
    else if (folders) notify(std::to_string(folders) + (folders > 1 ? " dossiers ajout\xC3\xA9s" : " dossier ajout\xC3\xA9"), false);
    else notify(std::to_string(files) + (files > 1 ? " jeux ajout\xC3\xA9s" : " jeu ajout\xC3\xA9"), false);
}

// ---- image principale ------------------------------------------------------------------------------------
void LibraryUI::draw(double now) {
    now_ = now;
    if (lib_.poll(now)) { /* les textures se mettent a jour d'elles-memes (date du fichier) */ }
    const std::string err = host_.takeGameError();
    if (!err.empty()) notify(err, true);
    if (!updateStarted_) {                                            // nouvelle version ? (une fois, en arriere-plan)
        updateStarted_ = true;
        if (Updater* u = host_.updater(); u && lib_.checkUpdates) u->check(false);
    }

    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput || ImGui::IsKeyDown(ImGuiMod_Ctrl)) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSearch_ = true;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) openRomDialog();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) { lib_.rescanAll(); notify("Dossiers analys\xC3\xA9s", false); }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##racine", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PushFont(fonts_.body);

    drawTitleBar();
    drawMenuBar();
    const float x0 = vp->Pos.x + S(16), x1 = vp->Pos.x + vp->Size.x - S(16);
    drawColumnHeaders(x0, x1);
    drawContent();
    drawFooter();
    drawToast();
    drawModals();

    ImGui::PopFont();
    ImGui::End();
}

// Barre de titre : icone, nom, boutons de la fenetre (la fenetre n'a pas de cadre Windows)
void LibraryUI::drawTitleBar() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = S(46), X = vp->Pos.x, Y = vp->Pos.y, W = vp->Size.x;
    titleBarHeight = h;

    // icone : carre orange arrondi avec une DS stylisee
    const ImVec2 ip(X + S(16), Y + (h - S(24)) * 0.5f);
    dl->AddRectFilledMultiColor(ip, ImVec2(ip.x + S(24), ip.y + S(24)), rgb(0xFF, 0x9A, 0x4A), rgb(0xFF, 0x8A, 0x3D), rgb(0xF0, 0x5A, 0x28), rgb(0xF2, 0x6B, 0x30));
    dl->AddRect(ImVec2(ip.x - 1, ip.y - 1), ImVec2(ip.x + S(25), ip.y + S(25)), kBg, S(7), 0, S(3));   // coins arrondis
    dl->AddRectFilled(ImVec2(ip.x + S(6), ip.y + S(5)), ImVec2(ip.x + S(18), ip.y + S(11.5f)), rgb(255, 255, 255, 235), S(2));
    dl->AddRectFilled(ImVec2(ip.x + S(6), ip.y + S(12.5f)), ImVec2(ip.x + S(18), ip.y + S(19)), rgb(255, 255, 255, 235), S(2));
    text(dl, fonts_.medium, ImVec2(ip.x + S(34), Y + (h - fonts_.medium->FontSize) * 0.5f), kText, "ndsemu");

    // boutons : reduire, agrandir / restaurer, fermer
    const float bw = S(46);
    captionButtonsX = X + W - bw * 3 - S(8);
    for (int i = 0; i < 3; i++) {
        const ImVec2 a(captionButtonsX + bw * float(i), Y), b(a.x + bw, Y + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##fen", ImVec2(bw, h));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) dl->AddRectFilled(ImVec2(a.x + S(4), a.y + S(7)), ImVec2(b.x - S(4), b.y - S(7)), i == 2 ? rgb(0xE5, 0x48, 0x4D) : kHover, S(6));
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const float k = S(5);
        if (i == 0) dl->AddLine(ImVec2(c.x - k, c.y), ImVec2(c.x + k, c.y), kDim, S(1.2f));
        if (i == 1) {
            if (host_.isMaximized()) {
                dl->AddRect(ImVec2(c.x - k + S(2), c.y - k), ImVec2(c.x + k, c.y + k - S(2)), kDim, S(1.5f), 0, S(1.2f));
                dl->AddRectFilled(ImVec2(c.x - k, c.y - k + S(2)), ImVec2(c.x + k - S(2), c.y + k), kBg, S(1.5f));
                dl->AddRect(ImVec2(c.x - k, c.y - k + S(2)), ImVec2(c.x + k - S(2), c.y + k), kDim, S(1.5f), 0, S(1.2f));
            } else {
                dl->AddRect(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), kDim, S(2), 0, S(1.2f));
            }
        }
        if (i == 2) {
            const ImU32 xc = hov ? rgb(255, 255, 255) : rgb(0xFF, 0x6B, 0x6B);
            dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), xc, S(1.4f));
            dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), xc, S(1.4f));
        }
        if (clicked) {
            if (i == 0) host_.minimize();
            else if (i == 1) host_.toggleMaximize();
            else host_.close();
        }
    }
    dl->AddLine(ImVec2(X, Y + h - 0.5f), ImVec2(X + W, Y + h - 0.5f), kLine);
}

// Menus a gauche ; recherche, choix liste / grille et reglages a droite
void LibraryUI::drawMenuBar() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = vp->Pos.y + titleBarHeight, h = S(50), X = vp->Pos.x, W = vp->Size.x;

    static const char* kMenus[5] = { "File", "Emulation", "View", "Tools", "Help" };
    float mx = X + S(10);
    bool anyOpen = false;
    for (int i = 0; i < 5; i++) {
        const ImVec2 ts = textSize(fonts_.body, kMenus[i]);
        const ImVec2 a(mx, y + S(9)), sz(ts.x + S(20), h - S(18));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(100 + i);
        const bool clicked = ImGui::InvisibleButton("##menu", sz);
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        char id[16];
        std::snprintf(id, sizeof id, "##menu%d", i);
        const bool open = ImGui::IsPopupOpen(id);
        if (open || hov) dl->AddRectFilled(a, ImVec2(a.x + sz.x, a.y + sz.y), open ? kPill : kHover, S(6));
        const ImVec2 tp(a.x + S(10), a.y + (sz.y - ts.y) * 0.5f);
        text(dl, fonts_.body, tp, kText, kMenus[i]);
        const float uw = textSize(fonts_.body, std::string(1, kMenus[i][0]).c_str()).x;     // initiale soulignee
        dl->AddLine(ImVec2(tp.x, tp.y + ts.y + S(0.5f)), ImVec2(tp.x + uw, tp.y + ts.y + S(0.5f)), kDim, 1.0f);
        if (clicked || (hov && openMenu_ >= 0 && openMenu_ != i)) { ImGui::OpenPopup(id); openMenu_ = i; }
        ImGui::SetNextWindowPos(ImVec2(a.x, a.y + sz.y + S(4)));
        ImGui::SetNextWindowSizeConstraints(ImVec2(S(250), 0), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(12), S(10)));
        if (ImGui::BeginPopup(id)) {
            anyOpen = true;
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
            const GameEntry* sel = nullptr;
            for (const GameFolder& f : lib_.folders) for (const GameEntry& g : f.games) if (g.path == selected_) sel = &g;
            switch (i) {
            case 0:
                if (ImGui::MenuItem("Ouvrir une ROM\xE2\x80\xA6", "Ctrl+O")) openRomDialog();
                if (ImGui::MenuItem("Ajouter un dossier\xE2\x80\xA6")) addFolderDialog();
                ImGui::Separator();
                if (ImGui::MenuItem("Quitter", "Alt+F4")) host_.close();
                break;
            case 1:
                if (ImGui::MenuItem("Lancer le jeu s\xC3\xA9lectionn\xC3\xA9", "Entr\xC3\xA9" "e", false, sel != nullptr)) launch(*sel);
                if (ImGui::MenuItem("Arr\xC3\xAAter le jeu", nullptr, false, sel && host_.isRunning(sel->path))) host_.stop(sel->path);
                ImGui::Separator();
                if (ImGui::MenuItem("Son", nullptr, lib_.sound)) { lib_.sound = !lib_.sound; lib_.save(); }
                if (ImGui::MenuItem("Rapport de diagnostic (diag.txt)", nullptr, lib_.report)) { lib_.report = !lib_.report; lib_.save(); }
                break;
            case 2:
                if (ImGui::MenuItem("Liste", nullptr, !lib_.gridView)) { lib_.gridView = false; lib_.save(); }
                if (ImGui::MenuItem("Grille", nullptr, lib_.gridView)) { lib_.gridView = true; lib_.save(); }
                ImGui::Separator();
                if (ImGui::MenuItem("R\xC3\xA9" "cemment jou\xC3\xA9s", nullptr, lib_.showRecent)) { lib_.showRecent = !lib_.showRecent; lib_.save(); }
                ImGui::Separator();
                if (ImGui::MenuItem("Tout d\xC3\xA9plier")) { for (GameFolder& f : lib_.folders) f.open = true; lib_.save(); }
                if (ImGui::MenuItem("Tout replier")) { for (GameFolder& f : lib_.folders) f.open = false; lib_.save(); }
                break;
            case 3:
                if (ImGui::MenuItem("Analyser les dossiers", "F5")) { lib_.rescanAll(); notify("Dossiers analys\xC3\xA9s", false); }
                if (ImGui::MenuItem("Ouvrir le dossier de ndsemu")) host_.reveal(lib_.configPath);
                break;
            default:
                if (ImGui::MenuItem("Touches et manette\xE2\x80\xA6")) host_.openInputSettings();
                if (Updater* u = host_.updater(); u && ImGui::MenuItem("Rechercher des mises \xC3\xA0 jour\xE2\x80\xA6")) {
                    const Updater::State st = u->state();
                    if (st != Updater::State::Available && st != Updater::State::Downloading &&
                        st != Updater::State::Installing && st != Updater::State::Installed) u->check(true);
                    showUpdate_ = true;
                }
                if (ImGui::MenuItem("\xC3\x80 propos de ndsemu")) showAbout_ = true;
                break;
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        mx += sz.x + S(2);
    }
    if (!anyOpen) openMenu_ = -1;

    // a droite : reglages, liste / grille, recherche
    const float r = X + W - S(16), cy = y + h * 0.5f;
    // reglages (roue dentee)
    {
        const float sz = S(32);
        const ImVec2 a(r - sz, cy - sz * 0.5f);
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##reglages", ImVec2(sz, sz))) ImGui::OpenPopup("##popreglages");
        if (ImGui::IsItemHovered()) dl->AddRectFilled(a, ImVec2(a.x + sz, a.y + sz), kHover, S(8));
        const ImVec2 c(a.x + sz * 0.5f, a.y + sz * 0.5f);
        for (int k = 0; k < 8; k++) {
            const float ang = float(k) * 3.14159265f / 4.0f;
            dl->AddLine(ImVec2(c.x + std::cos(ang) * S(5.5f), c.y + std::sin(ang) * S(5.5f)),
                        ImVec2(c.x + std::cos(ang) * S(8.5f), c.y + std::sin(ang) * S(8.5f)), kDim, S(2.6f));
        }
        dl->AddCircle(c, S(6), kDim, 20, S(1.6f));
        dl->AddCircleFilled(c, S(4.6f), kBg, 16);
        dl->AddCircle(c, S(2.4f), kDim, 12, S(1.4f));
        ImGui::SetNextWindowPos(ImVec2(a.x + sz, a.y + sz + S(6)), 0, ImVec2(1, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
        if (ImGui::BeginPopup("##popreglages")) {
            ImGui::PushFont(fonts_.medium);
            ImGui::TextUnformatted("R\xC3\xA9glages");
            ImGui::PopFont();
            ImGui::Spacing();
            if (ImGui::Checkbox("Son", &lib_.sound)) lib_.save();
            if (ImGui::Checkbox("Rapport de diagnostic (diag.txt)", &lib_.report)) lib_.save();
            if (host_.updater() && ImGui::Checkbox("Rechercher les mises \xC3\xA0 jour au d\xC3\xA9marrage", &lib_.checkUpdates)) lib_.save();
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kDim));
            ImGui::TextUnformatted("Pris en compte au prochain lancement d'un jeu.");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (ImGui::Button("Touches et manette\xE2\x80\xA6", ImVec2(-FLT_MIN, 0))) { ImGui::CloseCurrentPopup(); host_.openInputSettings(); }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    // liste / grille
    const float segW = S(64), segH = S(32);
    const ImVec2 sa(r - S(32) - S(10) - segW, cy - segH * 0.5f);
    dl->AddRectFilled(sa, ImVec2(sa.x + segW, sa.y + segH), kField, S(8));
    dl->AddRect(sa, ImVec2(sa.x + segW, sa.y + segH), kFieldLine, S(8));
    for (int i = 0; i < 2; i++) {
        const ImVec2 a(sa.x + S(3) + float(i) * (segW - S(6)) * 0.5f, sa.y + S(3));
        const ImVec2 b(a.x + (segW - S(6)) * 0.5f, sa.y + segH - S(3));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(200 + i);
        if (ImGui::InvisibleButton("##vue", ImVec2(b.x - a.x, b.y - a.y))) { lib_.gridView = i == 1; lib_.save(); }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool on = lib_.gridView == (i == 1);
        if (on) dl->AddRectFilled(a, b, rgb(0x33, 0x36, 0x3B), S(6));
        else if (hov) dl->AddRectFilled(a, b, kHover, S(6));
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const ImU32 ic = on ? kText : kFaint;
        if (i == 0) for (int k = -1; k <= 1; k++) dl->AddLine(ImVec2(c.x - S(6), c.y + float(k) * S(4.5f)), ImVec2(c.x + S(6), c.y + float(k) * S(4.5f)), ic, S(1.5f));
        else for (int k = 0; k < 4; k++) {
            const ImVec2 q(c.x + ((k & 1) ? S(1) : -S(6)), c.y + ((k & 2) ? S(1) : -S(6)));
            dl->AddRect(q, ImVec2(q.x + S(5), q.y + S(5)), ic, S(1.2f), 0, S(1.3f));
        }
    }
    // recherche
    {
        const float w = S(288), fh = S(34);
        const ImVec2 a(sa.x - S(10) - w, cy - fh * 0.5f);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(34), (fh - fonts_.body->FontSize) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kFieldLine));
        ImGui::SetNextItemWidth(w);
        if (focusSearch_) { ImGui::SetKeyboardFocusHere(); focusSearch_ = false; }
        ImGui::InputTextWithHint("##recherche", "Rechercher un jeu\xE2\x80\xA6", search_, sizeof search_);
        const bool active = ImGui::IsItemActive();
        if (active && ImGui::IsKeyPressed(ImGuiKey_Escape)) { search_[0] = 0; ImGui::ClearActiveID(); }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (active) dl->AddRect(a, ImVec2(a.x + w, a.y + fh), withAlpha(kAccent, 160), S(8), 0, S(1.5f));
        // loupe
        const ImVec2 c(a.x + S(17), a.y + fh * 0.5f - S(1));
        dl->AddCircle(c, S(5), kFaint, 16, S(1.5f));
        dl->AddLine(ImVec2(c.x + S(3.6f), c.y + S(3.6f)), ImVec2(c.x + S(7), c.y + S(7)), kFaint, S(1.6f));
        // raccourci
        if (!active && search_[0] == 0) {
            const char* k = "Ctrl F";
            const ImVec2 ks = textSize(fonts_.mono, k);
            const ImVec2 kb(a.x + w - S(10) - ks.x - S(12), a.y + (fh - S(20)) * 0.5f);
            dl->AddRectFilled(kb, ImVec2(kb.x + ks.x + S(12), kb.y + S(20)), kPill, S(4));
            dl->AddRect(kb, ImVec2(kb.x + ks.x + S(12), kb.y + S(20)), kFieldLine, S(4));
            text(dl, fonts_.mono, ImVec2(kb.x + S(6), kb.y + (S(20) - ks.y) * 0.5f), kDim, k);
        }
    }
    dl->AddLine(ImVec2(X, y + h - 0.5f), ImVec2(X + W, y + h - 0.5f), kLine);
}

namespace {
struct Cols { float compat, addons, type, sizeRight; };
Cols columns(float x1, float s) { return { x1 - 530 * s, x1 - 376 * s, x1 - 253 * s, x1 - 48 * s }; }
const char* kColNames[5] = { "NAME", "COMPATIBILITY", "ADD-ONS", "FILE TYPE", "SIZE" };
}

void LibraryUI::drawColumnHeaders(float x0, float x1) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = vp->Pos.y + titleBarHeight + S(50), h = S(40);
    const Cols c = columns(x1, s_);
    const float sp = S(1.1f);
    const float xs[5] = { x0 + S(8), c.compat, c.addons, c.type, c.sizeRight - spacedWidth(fonts_.caps, "SIZE", sp) };
    for (int i = 0; i < 5; i++) {
        const float w = spacedWidth(fonts_.caps, kColNames[i], sp);
        const ImVec2 p(xs[i], y + (h - fonts_.caps->FontSize) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(p.x - S(4), y + S(6)));
        ImGui::PushID(300 + i);
        if (ImGui::InvisibleButton("##col", ImVec2(w + S(22), h - S(12)))) {
            if (lib_.sortColumn == i) lib_.sortAscending = !lib_.sortAscending;
            else { lib_.sortColumn = i; lib_.sortAscending = true; }
            lib_.save();
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        spacedText(dl, fonts_.caps, p, hov ? kText : kFaint, kColNames[i], sp);
        if (lib_.sortColumn == i) {                                  // fleche de tri
            const float ax = (i == 4) ? p.x - S(10) : p.x + w + S(7), ay = p.y + fonts_.caps->FontSize * 0.5f;
            const float k = S(3.2f);
            if (lib_.sortAscending) {
                dl->AddLine(ImVec2(ax - k, ay + k * 0.6f), ImVec2(ax, ay - k * 0.6f), kDim, S(1.4f));
                dl->AddLine(ImVec2(ax, ay - k * 0.6f), ImVec2(ax + k, ay + k * 0.6f), kDim, S(1.4f));
            } else {
                dl->AddLine(ImVec2(ax - k, ay - k * 0.6f), ImVec2(ax, ay + k * 0.6f), kDim, S(1.4f));
                dl->AddLine(ImVec2(ax, ay + k * 0.6f), ImVec2(ax + k, ay - k * 0.6f), kDim, S(1.4f));
            }
        }
    }
    dl->AddLine(ImVec2(vp->Pos.x, y + h - 0.5f), ImVec2(vp->Pos.x + vp->Size.x, y + h - 0.5f), kLine);
}

// ---- contenu : cartes de dossiers, jeux, etat vide ---------------------------------------------------------
void LibraryUI::drawContent() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float top = vp->Pos.y + titleBarHeight + S(90), bottom = vp->Pos.y + vp->Size.y - S(62);
    ImGui::SetCursorScreenPos(ImVec2(vp->Pos.x, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    ImGui::BeginChild("##contenu", ImVec2(vp->Size.x, bottom - top), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = ImGui::GetCursorScreenPos().x;
    const float x1 = x0 + ImGui::GetContentRegionAvail().x;
    const std::string q = lowerStr(search_);

    // Navigation au clavier dans les jeux visibles
    std::vector<const GameEntry*> visible;
    bool anyShown = false;
    if (q.empty() && lib_.showRecent) drawRecent(x0, x1);

    for (size_t i = 0; i < lib_.folders.size(); i++) {
        GameFolder& f = lib_.folders[i];
        std::vector<const GameEntry*> games;
        for (const GameEntry& g : f.games) if (matches(g, q)) games.push_back(&g);
        if (!q.empty() && games.empty()) continue;
        if (f.path.empty() && f.games.empty()) continue;
        anyShown = true;
        const int col = lib_.sortColumn;
        const bool asc = lib_.sortAscending;
        std::stable_sort(games.begin(), games.end(), [&](const GameEntry* a, const GameEntry* b) {
            int d = 0;
            switch (col) {
                case 1: d = int(lib_.compatOf(*a)) - int(lib_.compatOf(*b)); break;
                case 2: d = (int(a->hasSave) + a->states * 2) - (int(b->hasSave) + b->states * 2); break;
                case 4: d = a->size < b->size ? -1 : (a->size > b->size ? 1 : 0); break;
                default: break;
            }
            if (d == 0) d = lowerStr(a->title).compare(lowerStr(b->title));
            return asc ? d < 0 : d > 0;
        });

        drawFolderHeader(i, f, int(games.size()), x0, x1);
        if (i >= lib_.folders.size()) break;                     // dossier retire depuis son menu
        if (!f.open && q.empty()) { ImGui::Dummy(ImVec2(0, S(8))); continue; }
        const float ind = x0 + S(34);
        if (games.empty()) {
            const float y0 = ImGui::GetCursorScreenPos().y + S(4);
            const float avail = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - y0 - S(15);
            const float hh = (lib_.folders.size() == 1) ? std::max(S(330), avail) : S(330);
            drawEmptyState(ind, x1, y0, y0 + hh, false);
            ImGui::SetCursorScreenPos(ImVec2(x0, y0 + hh));
            ImGui::Dummy(ImVec2(0, lib_.folders.size() == 1 ? 0.0f : S(12)));
            continue;
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        if (lib_.gridView) {
            const float tw = S(150), th = S(168), gap = S(12);
            const int perRow = std::max(1, int((x1 - ind + gap) / (tw + gap)));
            float y = ImGui::GetCursorScreenPos().y;
            for (size_t k = 0; k < games.size(); k++) {
                const int cx = int(k) % perRow;
                if (k && cx == 0) y += th + gap;
                drawGameTile(*games[k], ind + float(cx) * (tw + gap), y, tw, th);
                visible.push_back(games[k]);
            }
            ImGui::SetCursorScreenPos(ImVec2(x0, y + th + S(14)));
            ImGui::Dummy(ImVec2(0, 0));
        } else {
            for (const GameEntry* g : games) { drawGameRow(*g, ind, x1); visible.push_back(g); }
            ImGui::Dummy(ImVec2(0, S(10)));
        }
    }

    if (lib_.folders.empty() || (!anyShown && q.empty())) {
        const float y0 = ImGui::GetCursorScreenPos().y;
        const float y1 = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - S(15);
        drawEmptyState(x0, x1, y0, std::max(y1, y0 + S(330)), true);
        ImGui::SetCursorScreenPos(ImVec2(x0, std::max(y1, y0 + S(330))));
        ImGui::Dummy(ImVec2(0, 0));
    } else if (!anyShown) {
        const std::string msg = std::string("Aucun jeu ne correspond \xC3\xA0 \xC2\xAB ") + search_ + " \xC2\xBB";
        const ImVec2 ts = textSize(fonts_.medium, msg.c_str());
        const ImVec2 p((x0 + x1 - ts.x) * 0.5f, ImGui::GetCursorScreenPos().y + S(60));
        text(dl, fonts_.medium, p, kDim, msg.c_str());
        ImGui::Dummy(ImVec2(0, S(100)));
    }

    // clavier : haut / bas pour choisir, Entree pour lancer
    if (!ImGui::GetIO().WantTextInput && !visible.empty() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        int cur = -1;
        for (size_t k = 0; k < visible.size(); k++) if (visible[k]->path == selected_) cur = int(k);
        const int step = lib_.gridView ? 1 : 1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            selected_ = visible[size_t(std::min(int(visible.size()) - 1, cur + step))]->path;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            selected_ = visible[size_t(std::max(0, cur < 0 ? 0 : cur - step))]->path;
        if (cur >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
            launch(*visible[size_t(cur)]);
    }
    ImGui::EndChild();
}

// En-tete d'un dossier : fleche, icone, chemin (le dernier element en gras), nombre de jeux, surveillance, menu
bool LibraryUI::drawFolderHeader(size_t index, GameFolder& f, int shown, float x0, float x1) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float h = S(54);
    const ImVec2 b(x1, a.y + h);
    ImGui::PushID(int(index) + 1000);
    ImGui::SetCursorScreenPos(a);
    const bool toggled = ImGui::InvisibleButton("##entete", ImVec2(x1 - x0 - S(46), h));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, hov ? rgb(0x22, 0x24, 0x28) : kCard, S(10));
    dl->AddRect(a, b, kCardLine, S(10));
    const float cy = a.y + h * 0.5f;

    // fleche
    const ImVec2 ch(a.x + S(20), cy);
    const float k = S(4);
    if (f.open) { dl->AddLine(ImVec2(ch.x - k, ch.y - k * 0.5f), ImVec2(ch.x, ch.y + k * 0.5f), kDim, S(1.5f)); dl->AddLine(ImVec2(ch.x, ch.y + k * 0.5f), ImVec2(ch.x + k, ch.y - k * 0.5f), kDim, S(1.5f)); }
    else { dl->AddLine(ImVec2(ch.x - k * 0.5f, ch.y - k), ImVec2(ch.x + k * 0.5f, ch.y), kDim, S(1.5f)); dl->AddLine(ImVec2(ch.x + k * 0.5f, ch.y), ImVec2(ch.x - k * 0.5f, ch.y + k), kDim, S(1.5f)); }

    // dossier
    const ImVec2 fo(a.x + S(42), cy - S(7.5f));
    dl->AddRectFilled(fo, ImVec2(fo.x + S(8), fo.y + S(4)), kFolder, S(1.5f));
    dl->AddRectFilled(ImVec2(fo.x, fo.y + S(2.5f)), ImVec2(fo.x + S(18), fo.y + S(15)), kFolder, S(2));

    // chemin : "C:\Jeux\" en gris, "Nintendo DS" en gras
    float tx = a.x + S(72);
    std::string head, tail;
    if (f.path.empty()) tail = "Fichiers ajout\xC3\xA9s";
    else {
        std::string p = f.path;
        while (p.size() > 3 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
        const size_t cut = p.find_last_of("/\\");
        head = cut == std::string::npos ? "" : p.substr(0, cut + 1);
        tail = cut == std::string::npos ? p : p.substr(cut + 1);
    }
    const float maxPathW = (x1 - S(220)) - tx;
    if (!head.empty()) {
        // chemin trop long : on garde la fin
        while (head.size() > 4 && textSize(fonts_.mono, head.c_str()).x + textSize(fonts_.monoBold, tail.c_str()).x > maxPathW)
            head = "\xE2\x80\xA6" + head.substr(std::min(head.size(), size_t(4)));
        text(dl, fonts_.mono, ImVec2(tx, cy - fonts_.mono->FontSize * 0.5f), kDim, head.c_str());
        tx += textSize(fonts_.mono, head.c_str()).x;
    }
    ImFont* tf = f.path.empty() ? fonts_.medium : fonts_.monoBold;
    text(dl, tf, ImVec2(tx, cy - tf->FontSize * 0.5f), kText, tail.c_str());
    tx += textSize(tf, tail.c_str()).x + S(12);

    // nombre de jeux
    char cnt[32];
    std::snprintf(cnt, sizeof cnt, "%d %s", shown, shown > 1 ? "jeux" : "jeu");
    const ImVec2 cs = textSize(fonts_.body, cnt);
    const float ph = S(24);
    dl->AddRectFilled(ImVec2(tx, cy - ph * 0.5f), ImVec2(tx + cs.x + S(20), cy + ph * 0.5f), kBg, ph * 0.5f);
    dl->AddRect(ImVec2(tx, cy - ph * 0.5f), ImVec2(tx + cs.x + S(20), cy + ph * 0.5f), kCardLine, ph * 0.5f);
    ImGui::PushFont(fonts_.body);
    text(dl, fonts_.body, ImVec2(tx + S(10), cy - cs.y * 0.5f), kDim, cnt);
    ImGui::PopFont();

    // a droite : surveillance, menu "..."
    const float mx = x1 - S(44);
    if (!f.path.empty()) {
        const char* st = f.missing ? "Introuvable" : (f.watched ? "Surveill\xC3\xA9" : "Non surveill\xC3\xA9");
        const ImU32 dc = f.missing ? kDanger : (f.watched ? kAccent : kFaint);
        const ImVec2 ss = textSize(fonts_.body, st);
        const float sx = mx - S(14) - ss.x;
        dl->AddCircleFilled(ImVec2(sx - S(12), cy), S(5), withAlpha(dc, 50), 16);
        dl->AddCircleFilled(ImVec2(sx - S(12), cy), S(3.2f), dc, 16);
        text(dl, fonts_.body, ImVec2(sx, cy - ss.y * 0.5f), f.missing ? kDanger : kDim, st);
    }
    ImGui::SetCursorScreenPos(ImVec2(mx, cy - S(15)));
    if (ImGui::InvisibleButton("##plus", ImVec2(S(32), S(30)))) ImGui::OpenPopup("##menudossier");
    const bool mh = ImGui::IsItemHovered();
    if (mh || ImGui::IsPopupOpen("##menudossier")) dl->AddRectFilled(ImVec2(mx, cy - S(15)), ImVec2(mx + S(32), cy + S(15)), kHover, S(6));
    for (int d = -1; d <= 1; d++) dl->AddCircleFilled(ImVec2(mx + S(16) + float(d) * S(5.5f), cy), S(1.6f), kDim, 8);

    bool removed = false;
    ImGui::SetNextWindowPos(ImVec2(mx + S(32), cy + S(18)), 0, ImVec2(1, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(240), 0), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
    if (ImGui::BeginPopup("##menudossier")) {
        if (ImGui::MenuItem("Analyser maintenant")) { lib_.rescan(index); notify("Dossier analys\xC3\xA9", false); }
        if (!f.path.empty()) {
            if (ImGui::MenuItem("Surveillance automatique", nullptr, f.watched)) { f.watched = !f.watched; lib_.save(); }
            if (ImGui::MenuItem("Ouvrir dans l'explorateur")) host_.reveal(f.path);
            if (ImGui::MenuItem("Copier le chemin")) ImGui::SetClipboardText(f.path.c_str());
        }
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kDanger));
        if (ImGui::MenuItem("Retirer de la biblioth\xC3\xA8que")) removed = true;
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();

    if (toggled) { f.open = !f.open; lib_.save(); }
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + S(10)));
    ImGui::Dummy(ImVec2(0, 0));
    if (removed) {
        lib_.removeFolder(index);
        notify("Dossier retir\xC3\xA9 (les fichiers ne sont pas supprim\xC3\xA9s)", false);
    }
    return !removed;
}

// Zone vide : illustration d'une DS, message, boutons
void LibraryUI::drawEmptyState(float x0, float x1, float y0, float y1, bool noFolder) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dashedRect(dl, ImVec2(x0, y0), ImVec2(x1, y1), rgb(0x3A, 0x3D, 0x42), S(12), S(4), S(4), 1.0f);
    const float cx = (x0 + x1) * 0.5f;
    const float blockH = S(300);
    float y = (y0 + y1) * 0.5f - blockH * 0.5f;
    // halo vert discret
    for (int k = 0; k < 16; k++) dl->AddCircleFilled(ImVec2(cx, y + S(80)), S(125 - 6.5f * float(k)), rgb(0x3D, 0xD6, 0x8C, 2), 64);

    // console : ecran du haut
    const ImU32 shell = rgb(0x1E, 0x20, 0x23), edge = rgb(0x45, 0x48, 0x4D), screen = rgb(0x15, 0x16, 0x18);
    const float w = S(122);
    ImVec2 a(cx - w * 0.5f, y);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + S(72)), shell, S(10));
    dl->AddRect(a, ImVec2(a.x + w, a.y + S(72)), edge, S(10), 0, S(1.5f));
    const ImVec2 s1(a.x + S(17), a.y + S(12)), s2(a.x + w - S(17), a.y + S(60));
    dl->AddRectFilled(s1, s2, screen, S(3));
    dl->AddRect(s1, s2, rgb(0x33, 0x36, 0x3A), S(3));
    for (int k = 1; k < 4; k++) {                                   // quadrillage
        const float gx = s1.x + (s2.x - s1.x) * float(k) / 4.0f;
        dl->AddLine(ImVec2(gx, s1.y + 1), ImVec2(gx, s2.y - 1), rgb(0x26, 0x28, 0x2C));
    }
    for (int k = 1; k < 3; k++) {
        const float gy = s1.y + (s2.y - s1.y) * float(k) / 3.0f;
        dl->AddLine(ImVec2(s1.x + 1, gy), ImVec2(s2.x - 1, gy), rgb(0x26, 0x28, 0x2C));
    }
    // ecran du bas
    a = ImVec2(cx - w * 0.5f, y + S(76));
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + S(72)), shell, S(10));
    dl->AddRect(a, ImVec2(a.x + w, a.y + S(72)), edge, S(10), 0, S(1.5f));
    const ImVec2 t1(a.x + S(28), a.y + S(10)), t2(a.x + w - S(28), a.y + S(62));
    dl->AddRectFilled(t1, t2, screen, S(4));
    dl->AddRect(t1, t2, rgb(0x33, 0x36, 0x3A), S(4));
    plusIcon(dl, ImVec2((t1.x + t2.x) * 0.5f, (t1.y + t2.y) * 0.5f), S(9), kAccent, S(2.2f));
    plusIcon(dl, ImVec2(a.x + S(14), a.y + S(26)), S(6), rgb(0x5A, 0x5E, 0x64), S(3));        // croix directionnelle
    const ImVec2 bc(a.x + w - S(13), a.y + S(26));                                              // boutons
    dl->AddCircleFilled(ImVec2(bc.x, bc.y - S(5)), S(2.6f), rgb(0x5A, 0x5E, 0x64), 10);
    dl->AddCircleFilled(ImVec2(bc.x - S(5), bc.y), S(2.6f), rgb(0x5A, 0x5E, 0x64), 10);
    dl->AddCircleFilled(ImVec2(bc.x + S(5), bc.y), S(2.6f), rgb(0x5A, 0x5E, 0x64), 10);
    dl->AddCircleFilled(ImVec2(bc.x, bc.y + S(5)), S(2.6f), kAccent, 10);

    // textes
    y += S(172);
    const char* title = noFolder ? "Ta biblioth\xC3\xA8que est vide" : "Aucun jeu dans ce dossier";
    const ImVec2 ts = textSize(fonts_.title, title);
    text(dl, fonts_.title, ImVec2(cx - ts.x * 0.5f, y), kText, title);
    y += ts.y + S(14);
    // ligne 1 avec ".nds" dans une pastille
    const char* l1a = noFolder ? "Ajoute le dossier de tes fichiers " : "Glisse tes fichiers ";
    const char* l1b = noFolder ? ", ou glisse-les" : "ici, ou ajoute un autre dossier";
    const char* code = ".nds";
    const float wa = textSize(fonts_.body, l1a).x, wb = textSize(fonts_.body, l1b).x;
    const float wc = textSize(fonts_.mono, code).x + S(12);
    float lx = cx - (wa + wc + (noFolder ? 0.0f : S(4)) + wb) * 0.5f;
    text(dl, fonts_.body, ImVec2(lx, y), kDim, l1a);
    lx += wa;
    dl->AddRectFilled(ImVec2(lx, y - S(2)), ImVec2(lx + wc, y + fonts_.body->FontSize + S(3)), kPill, S(4));
    dl->AddRect(ImVec2(lx, y - S(2)), ImVec2(lx + wc, y + fonts_.body->FontSize + S(3)), kFieldLine, S(4));
    text(dl, fonts_.mono, ImVec2(lx + S(6), y + (fonts_.body->FontSize - fonts_.mono->FontSize) * 0.5f), kText, code);
    lx += wc + (noFolder ? 0.0f : S(4));
    text(dl, fonts_.body, ImVec2(lx, y), kDim, l1b);
    y += fonts_.body->FontSize + S(8);
    const char* l2 = noFolder ? "directement dans cette fen\xC3\xAAtre pour construire ta biblioth\xC3\xA8que." : "pour construire ta biblioth\xC3\xA8que.";
    text(dl, fonts_.body, ImVec2(cx - textSize(fonts_.body, l2).x * 0.5f, y), kDim, l2);
    y += fonts_.body->FontSize + S(26);

    // boutons
    const char* b1 = "Ajouter un dossier";
    const char* b2 = "Ouvrir une ROM\xE2\x80\xA6";
    const char* k2 = "Ctrl O";
    const float bh = S(38);
    const float w1 = textSize(fonts_.medium, b1).x + S(56);
    const float w2 = textSize(fonts_.medium, b2).x + textSize(fonts_.mono, k2).x + S(56);
    const float gap = S(12);
    const float bx = cx - (w1 + gap + w2) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(bx, y));
    ImGui::PushID(noFolder ? 1 : 2);
    ImGui::PushID(int(y0));
    if (ImGui::InvisibleButton("##ajout", ImVec2(w1, bh))) addFolderDialog();
    const bool h1 = ImGui::IsItemHovered();
    dl->AddRectFilled(ImVec2(bx, y + S(3)), ImVec2(bx + w1, y + bh + S(5)), rgb(0x3D, 0xD6, 0x8C, 30), S(9));   // ombre verte
    dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + w1, y + bh), h1 ? rgb(0x55, 0xE0, 0x9D) : kAccent, S(8));
    plusIcon(dl, ImVec2(bx + S(22), y + bh * 0.5f), S(5.5f), kOnAccent, S(1.8f));
    text(dl, fonts_.medium, ImVec2(bx + S(38), y + (bh - fonts_.medium->FontSize) * 0.5f), kOnAccent, b1);
    const float b2x = bx + w1 + gap;
    ImGui::SetCursorScreenPos(ImVec2(b2x, y));
    if (ImGui::InvisibleButton("##rom", ImVec2(w2, bh))) openRomDialog();
    const bool h2 = ImGui::IsItemHovered();
    ImGui::PopID();
    ImGui::PopID();
    dl->AddRectFilled(ImVec2(b2x, y), ImVec2(b2x + w2, y + bh), h2 ? kHover : kCard, S(8));
    dl->AddRect(ImVec2(b2x, y), ImVec2(b2x + w2, y + bh), kFieldLine, S(8));
    text(dl, fonts_.medium, ImVec2(b2x + S(18), y + (bh - fonts_.medium->FontSize) * 0.5f), kText, b2);
    const ImVec2 ks = textSize(fonts_.mono, k2);
    const ImVec2 kb(b2x + w2 - S(16) - ks.x - S(10), y + (bh - S(20)) * 0.5f);
    dl->AddRectFilled(kb, ImVec2(kb.x + ks.x + S(10), kb.y + S(20)), kPill, S(4));
    dl->AddRect(kb, ImVec2(kb.x + ks.x + S(10), kb.y + S(20)), kFieldLine, S(4));
    text(dl, fonts_.mono, ImVec2(kb.x + S(5), kb.y + (S(20) - ks.y) * 0.5f), kDim, k2);
}

// Ligne d'un jeu (vue liste)
void LibraryUI::drawGameRow(const GameEntry& g, float x0, float x1) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 p(x0, a.y);
    const float h = S(56);
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(g.path.c_str());
    ImGui::InvisibleButton("##jeu", ImVec2(x1 - x0, h));
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) selected_ = g.path;
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) launch(g);
    if (ImGui::BeginPopupContextItem("##ctx")) { gameContextMenu(g); ImGui::EndPopup(); }
    ImGui::PopID();
    const bool sel = selected_ == g.path;
    const ImVec2 b(x1, p.y + h);
    if (sel) {
        dl->AddRectFilled(p, b, rgb(0x3D, 0xD6, 0x8C, 22), S(8));
        dl->AddRect(p, b, rgb(0x3D, 0xD6, 0x8C, 70), S(8));
    } else if (hov) dl->AddRectFilled(p, b, kHover, S(8));
    const float cy = p.y + h * 0.5f;

    // icone (32 x 32, agrandie sans lissage)
    const float is = S(36);
    const ImVec2 ia(p.x + S(12), cy - is * 0.5f);
    dl->AddRectFilled(ia, ImVec2(ia.x + is, ia.y + is), rgb(0x24, 0x26, 0x2A), S(6));
    if (void* t = iconTexture(g)) dl->AddImage(ImTextureID(t), ImVec2(ia.x + S(2), ia.y + S(2)), ImVec2(ia.x + is - S(2), ia.y + is - S(2)));
    dl->AddRect(ia, ImVec2(ia.x + is, ia.y + is), kCardLine, S(6));

    // titre + sous-titre
    const Cols c = columns(x1, s_);
    const float tx = ia.x + is + S(14);
    dl->PushClipRect(ImVec2(tx, p.y), ImVec2(c.compat - S(16), b.y), true);
    text(dl, fonts_.medium, ImVec2(tx, cy - S(18)), kText, g.title.c_str());
    float badgeX = tx + textSize(fonts_.medium, g.title.c_str()).x + S(10);
    std::string sub = g.subtitle.empty() ? g.fileName : g.subtitle;
    sub += "  \xC2\xB7  " + g.gameCode;
    if (const PlayStat* ps = lib_.playOf(g)) if (ps->seconds >= 60) sub += "  \xC2\xB7  " + durationText(ps->seconds);
    text(dl, fonts_.body, ImVec2(tx, cy + S(1)), kFaint, sub.c_str());
    if (host_.isRunning(g.path)) {
        const char* r = "En cours";
        const ImVec2 rs = textSize(fonts_.caps, r);
        dl->AddRectFilled(ImVec2(badgeX, cy - S(17)), ImVec2(badgeX + rs.x + S(14), cy - S(1)), kAccentDim, S(8));
        text(dl, fonts_.caps, ImVec2(badgeX + S(7), cy - S(9) - rs.y * 0.5f), kAccent, r);
    }
    dl->PopClipRect();

    // compatibilite
    const Compat cp = lib_.compatOf(g);
    const char* cl = compatLabel(cp);
    const ImU32 cc = compatColor(cp);
    const ImVec2 cs = textSize(fonts_.body, cl);
    const float ph = S(26);
    dl->AddRectFilled(ImVec2(c.compat, cy - ph * 0.5f), ImVec2(c.compat + cs.x + S(30), cy + ph * 0.5f), withAlpha(cc, 26), ph * 0.5f);
    dl->AddCircleFilled(ImVec2(c.compat + S(12), cy), S(3.2f), cc, 12);
    text(dl, fonts_.body, ImVec2(c.compat + S(21), cy - cs.y * 0.5f), cc, cl);

    // extras, type, taille
    // extras : sauvegarde du jeu (.sav) et sauvegardes d'etat, sur une ou deux lignes
    std::vector<std::string> extras;
    if (g.hasSave) extras.push_back("Sauvegarde");
    if (g.states) extras.push_back(std::to_string(g.states) + (g.states > 1 ? " \xC3\xA9tats" : " \xC3\xA9tat"));
    dl->PushClipRect(ImVec2(c.addons, p.y), ImVec2(c.type - S(8), b.y), true);
    if (extras.empty()) text(dl, fonts_.body, ImVec2(c.addons, cy - fonts_.body->FontSize * 0.5f), kFaint, "\xE2\x80\x94");
    else if (extras.size() == 1) text(dl, fonts_.body, ImVec2(c.addons, cy - fonts_.body->FontSize * 0.5f), kDim, extras[0].c_str());
    else {
        text(dl, fonts_.body, ImVec2(c.addons, cy - S(18)), kDim, extras[0].c_str());
        text(dl, fonts_.body, ImVec2(c.addons, cy + S(1)), kFaint, extras[1].c_str());
    }
    dl->PopClipRect();
    text(dl, fonts_.mono, ImVec2(c.type, cy - fonts_.mono->FontSize * 0.5f), kDim, "NDS");
    const std::string sz = formatSize(g.size);
    text(dl, fonts_.body, ImVec2(c.sizeRight - textSize(fonts_.body, sz.c_str()).x, cy - fonts_.body->FontSize * 0.5f), kDim, sz.c_str());

    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + S(4)));
    ImGui::Dummy(ImVec2(0, 0));
}

// Vignette d'un jeu (vue grille)
void LibraryUI::drawGameTile(const GameEntry& g, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(x, y), b(x + w, y + h);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(g.path.c_str());
    ImGui::InvisibleButton("##tuile", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) selected_ = g.path;
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) launch(g);
    if (ImGui::BeginPopupContextItem("##ctx")) { gameContextMenu(g); ImGui::EndPopup(); }
    ImGui::PopID();
    const bool sel = selected_ == g.path;
    dl->AddRectFilled(a, b, sel ? rgb(0x22, 0x2B, 0x26) : (hov ? kHover : kCard), S(10));
    dl->AddRect(a, b, sel ? rgb(0x3D, 0xD6, 0x8C, 110) : kCardLine, S(10));

    const float is = S(64);
    const ImVec2 ia(x + (w - is) * 0.5f, y + S(16));
    if (void* t = iconTexture(g)) dl->AddImage(ImTextureID(t), ia, ImVec2(ia.x + is, ia.y + is));
    else dl->AddRectFilled(ia, ImVec2(ia.x + is, ia.y + is), rgb(0x24, 0x26, 0x2A), S(8));
    if (host_.isRunning(g.path)) dl->AddCircleFilled(ImVec2(b.x - S(14), a.y + S(14)), S(4), kAccent, 12);

    // titre sur deux lignes au plus, centre
    ImFont* f = fonts_.medium;
    const char* s = g.title.c_str();
    const char* end = s + g.title.size();
    float ty = ia.y + is + S(12);
    for (int line = 0; line < 2 && s < end; line++) {
        const char* e = f->CalcWordWrapPositionA(1.0f, s, end, w - S(20));
        if (e == s) e = s + 1;
        std::string ln(s, e);
        if (line == 1 && e < end) { while (!ln.empty() && textSize(f, (ln + "\xE2\x80\xA6").c_str()).x > w - S(20)) ln.pop_back(); ln += "\xE2\x80\xA6"; }
        while (!ln.empty() && ln.back() == ' ') ln.pop_back();
        text(dl, f, ImVec2(x + (w - textSize(f, ln.c_str()).x) * 0.5f, ty), kText, ln.c_str());
        ty += f->FontSize + S(2);
        s = e;
        while (s < end && *s == ' ') s++;
    }
    const Compat cp = lib_.compatOf(g);
    const char* cl = compatLabel(cp);
    const float cw = textSize(fonts_.body, cl).x + S(12);
    const float cx = x + (w - cw) * 0.5f, cy = b.y - S(18);
    dl->AddCircleFilled(ImVec2(cx + S(3), cy), S(3), compatColor(cp), 12);
    text(dl, fonts_.body, ImVec2(cx + S(12), cy - fonts_.body->FontSize * 0.5f), kFaint, cl);
}

// "Recemment joues" : les derniers jeux lances, sur une ligne de cartes
void LibraryUI::drawRecent(float x0, float x1) {
    std::vector<std::pair<const GameEntry*, PlayStat>> list;
    for (const GameFolder& f : lib_.folders)
        for (const GameEntry& g : f.games)
            if (const PlayStat* p = lib_.playOf(g)) {
                if (p->last <= 0) continue;
                bool dup = false;                                  // le meme jeu dans deux dossiers : une seule carte
                for (auto& e : list) if (e.first->gameCode == g.gameCode && e.first->headerCrc == g.headerCrc) dup = true;
                if (!dup) list.push_back({ &g, *p });
            }
    if (list.empty()) return;
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.last > b.second.last; });
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    spacedText(dl, fonts_.caps, ImVec2(x0 + S(4), start.y), kFaint, "R\xC3\x89" "CEMMENT JOU\xC3\x89S", S(1.1f));
    const float cw = S(250), ch = S(86), gap = S(12);
    const int perRow = std::max(1, int((x1 - x0 + gap) / (cw + gap)));
    const int n = std::min(int(list.size()), perRow);
    const float y = start.y + fonts_.caps->FontSize + S(10);
    for (int i = 0; i < n; i++) drawRecentCard(*list[size_t(i)].first, list[size_t(i)].second, x0 + float(i) * (cw + gap), y, cw, ch);
    ImGui::SetCursorScreenPos(ImVec2(x0, y + ch + S(18)));
    ImGui::Dummy(ImVec2(0, 0));
}

void LibraryUI::drawRecentCard(const GameEntry& g, const PlayStat& p, float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(x, y), b(x + w, y + h);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(("recent" + g.path).c_str());
    ImGui::InvisibleButton("##carte", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) selected_ = g.path;
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) launch(g);
    if (ImGui::BeginPopupContextItem("##ctx")) { gameContextMenu(g); ImGui::EndPopup(); }
    ImGui::PopID();
    const bool sel = selected_ == g.path;
    dl->AddRectFilled(a, b, sel ? rgb(0x22, 0x2B, 0x26) : (hov ? kHover : kCard), S(10));
    dl->AddRect(a, b, sel ? rgb(0x3D, 0xD6, 0x8C, 110) : kCardLine, S(10));
    const float is = S(54);
    const ImVec2 ia(x + S(14), y + (h - is) * 0.5f);
    dl->AddRectFilled(ia, ImVec2(ia.x + is, ia.y + is), rgb(0x24, 0x26, 0x2A), S(8));
    if (void* t = iconTexture(g)) dl->AddImage(ImTextureID(t), ImVec2(ia.x + S(3), ia.y + S(3)), ImVec2(ia.x + is - S(3), ia.y + is - S(3)));
    const float tx = ia.x + is + S(14);
    dl->PushClipRect(ImVec2(tx, y), ImVec2(b.x - S(10), b.y), true);
    text(dl, fonts_.medium, ImVec2(tx, y + S(16)), kText, g.title.c_str());
    const bool running = host_.isRunning(g.path);
    const std::string when = running ? std::string("En cours") : agoText(p.last);
    text(dl, fonts_.body, ImVec2(tx, y + S(36)), running ? kAccent : kDim, when.c_str());
    const std::string dur = durationText(p.seconds) + " de jeu";
    text(dl, fonts_.body, ImVec2(tx, y + S(55)), kFaint, dur.c_str());
    dl->PopClipRect();
}

void LibraryUI::gameContextMenu(const GameEntry& g) {
    ImGui::PushFont(fonts_.medium);
    ImGui::TextUnformatted(g.title.c_str());
    ImGui::PopFont();
    ImGui::Separator();
    const bool running = host_.isRunning(g.path);
    if (ImGui::MenuItem("Lancer", "Entr\xC3\xA9" "e", false, !running)) launch(g);
    if (ImGui::MenuItem("Arr\xC3\xAAter", nullptr, false, running)) host_.stop(g.path);
    if (ImGui::BeginMenu("Compatibilit\xC3\xA9")) {
        const Compat cur = lib_.compatOf(g);
        static const Compat order[] = { Compat::Perfect, Compat::Playable, Compat::InGame, Compat::Menus, Compat::Broken, Compat::Unknown };
        for (Compat c : order) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(compatColor(c)));
            if (ImGui::MenuItem(compatLabel(c), nullptr, cur == c)) lib_.setCompat(g, c);
            ImGui::PopStyleColor();
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Afficher dans l'explorateur")) host_.reveal(g.path);
    if (ImGui::MenuItem("Copier le chemin")) ImGui::SetClipboardText(g.path.c_str());
}

// Pied de page : ajout d'un dossier, rappel
void LibraryUI::drawFooter() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = S(62), y = vp->Pos.y + vp->Size.y - h, X = vp->Pos.x, W = vp->Size.x;
    const char* lbl = "Add New Game Directory";
    const ImVec2 ls = textSize(fonts_.medium, lbl);
    const ImVec2 a(X + S(58), y + (h - S(30)) * 0.5f);
    ImGui::SetCursorScreenPos(a);
    if (ImGui::InvisibleButton("##ajoutpied", ImVec2(S(34) + ls.x + S(12), S(30)))) addFolderDialog();
    const bool hov = ImGui::IsItemHovered();
    if (hov) dl->AddRectFilled(ImVec2(a.x - S(6), a.y - S(2)), ImVec2(a.x + S(40) + ls.x + S(12), a.y + S(32)), kHover, S(8));
    dl->AddRectFilled(ImVec2(a.x + S(2), a.y + S(4)), ImVec2(a.x + S(24), a.y + S(26)), kAccentDim, S(6));
    plusIcon(dl, ImVec2(a.x + S(13), a.y + S(15)), S(5), kAccent, S(1.8f));
    text(dl, fonts_.medium, ImVec2(a.x + S(34), a.y + (S(30) - ls.y) * 0.5f), kAccent, lbl);
    const char* r = "Les dossiers sont analys\xC3\xA9s automatiquement au d\xC3\xA9marrage";
    const ImVec2 rs = textSize(fonts_.body, r);
    if (X + W - S(30) - rs.x > a.x + S(60) + ls.x)
        text(dl, fonts_.body, ImVec2(X + W - S(30) - rs.x, y + (h - rs.y) * 0.5f), kFaint, r);
}

void LibraryUI::drawToast() {
    if (toast_.empty() || now_ > toastUntil_) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float maxW = vp->Size.x - S(80);
    const ImVec2 ts = fonts_.body->CalcTextSizeA(fonts_.body->FontSize, FLT_MAX, maxW, toast_.c_str());
    const float w = ts.x + S(54), h = ts.y + S(22);
    const float fade = float(std::min(1.0, (toastUntil_ - now_) * 3.0));
    const ImVec2 a(vp->Pos.x + (vp->Size.x - w) * 0.5f, vp->Pos.y + vp->Size.y - S(78) - h);
    dl->AddRectFilled(ImVec2(a.x, a.y + S(4)), ImVec2(a.x + w, a.y + h + S(6)), rgb(0, 0, 0, int(90 * fade)), S(12));
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), rgb(0x26, 0x28, 0x2C, int(250 * fade)), S(10));
    dl->AddRect(a, ImVec2(a.x + w, a.y + h), withAlpha(toastError_ ? kDanger : kAccent, int(120 * fade)), S(10));
    const ImU32 ic = withAlpha(toastError_ ? kDanger : kAccent, int(255 * fade));
    const ImVec2 c(a.x + S(20), a.y + h * 0.5f);
    dl->AddCircle(c, S(7), ic, 16, S(1.6f));
    if (toastError_) { dl->AddLine(ImVec2(c.x, c.y - S(3.5f)), ImVec2(c.x, c.y + S(1)), ic, S(1.6f)); dl->AddCircleFilled(ImVec2(c.x, c.y + S(3.5f)), S(1), ic); }
    else { dl->AddLine(ImVec2(c.x - S(3), c.y), ImVec2(c.x - S(0.5f), c.y + S(2.5f)), ic, S(1.6f)); dl->AddLine(ImVec2(c.x - S(0.5f), c.y + S(2.5f)), ImVec2(c.x + S(3.5f), c.y - S(2.5f)), ic, S(1.6f)); }
    dl->AddText(fonts_.body, fonts_.body->FontSize, ImVec2(a.x + S(36), a.y + S(11)), withAlpha(kText, int(255 * fade)), toast_.c_str(), nullptr, maxW);
}

void LibraryUI::drawModals() {
    host_.drawHostWindows(fonts_, s_);
    drawUpdateBanner();
    drawUpdateWindow();
    if (showAbout_) { ImGui::OpenPopup("\xC3\x80 propos"); showAbout_ = false; }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("\xC3\x80 propos", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushFont(fonts_.title);
        ImGui::TextUnformatted("ndsemu");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kDim));
        ImGui::TextUnformatted("Version " NDSEMU_VERSION);
        ImGui::TextUnformatted("\xC3\x89mulateur Nintendo DS \xC3\xA9" "crit en C++17.");
        ImGui::TextUnformatted("Interface : Dear ImGui (licence MIT).");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (ImGui::Button("Fermer", ImVec2(S(120), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// ---- mises a jour ----------------------------------------------------------------------------------------------
// Bandeau en bas a droite quand la recherche automatique trouve une nouvelle version
void LibraryUI::drawUpdateBanner() {
    Updater* u = host_.updater();
    if (!u || updateHidden_ || u->manual() || u->state() != Updater::State::Available) return;
    const ReleaseInfo r = u->release();
    if (r.version == lib_.skipVersion) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - S(20), vp->Pos.y + vp->Size.y - S(72)), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(12)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(rgb(0x26, 0x28, 0x2C)));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(withAlpha(kAccent, 140)));
    ImGui::Begin("##majbandeau", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PushFont(fonts_.medium);
    ImGui::Text("Nouvelle version %s disponible", r.version.c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kDim));
    ImGui::Text("Tu utilises la version %s.", u->currentVersion().c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(rgb(0x5A, 0xE6, 0xA2)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(rgb(0x2F, 0xB8, 0x76)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kOnAccent));
    if (ImGui::Button("Voir les nouveaut\xC3\xA9s")) { showUpdate_ = true; updateHidden_ = true; }
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    if (ImGui::Button("Plus tard")) updateHidden_ = true;
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

// Fenetre des mises a jour : nouveautes, installation avec progression, redemarrage
void LibraryUI::drawUpdateWindow() {
    Updater* u = host_.updater();
    if (!u) return;
    const char* kTitle = "Mise \xC3\xA0 jour";
    if (showUpdate_) { ImGui::OpenPopup(kTitle); showUpdate_ = false; }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(S(560), vp->Size.x - S(40)), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        ImGui::PopStyleVar(2);
        return;
    }
    const Updater::State st = u->state();
    const ReleaseInfo r = u->release();
    auto dim = [&](const std::string& t) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kDim));
        ImGui::TextWrapped("%s", t.c_str());
        ImGui::PopStyleColor();
    };
    auto primary = [&](const char* label) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(rgb(0x5A, 0xE6, 0xA2)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(rgb(0x2F, 0xB8, 0x76)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kOnAccent));
        const bool c = ImGui::Button(label);
        ImGui::PopStyleColor(4);
        return c;
    };
    bool close = false;
    ImGui::PushFont(fonts_.title);
    ImGui::TextUnformatted(st == Updater::State::Available || st == Updater::State::Downloading || st == Updater::State::Installing ||
                           st == Updater::State::Installed ? ("ndsemu " + r.version).c_str() : "Mises \xC3\xA0 jour");
    ImGui::PopFont();

    switch (st) {
    case Updater::State::Idle:
    case Updater::State::Checking:
        dim("Recherche d'une nouvelle version sur GitHub\xE2\x80\xA6");
        ImGui::Spacing();
        if (ImGui::Button("Fermer")) close = true;
        break;
    case Updater::State::UpToDate:
        dim("ndsemu est \xC3\xA0 jour (version " + u->currentVersion() + ").");
        ImGui::Spacing();
        if (ImGui::Button("Fermer") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
        break;
    case Updater::State::Failed:
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kDanger));
        ImGui::TextWrapped("La mise \xC3\xA0 jour a \xC3\xA9" "chou\xC3\xA9" "e : %s", u->error().c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (ImGui::Button("R\xC3\xA9" "essayer")) u->check(true);
        ImGui::SameLine();
        if (!r.page.empty() && ImGui::Button("Ouvrir la page sur GitHub")) u->openPage(r.page);
        ImGui::SameLine();
        if (ImGui::Button("Fermer") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
        break;
    case Updater::State::Available:
    case Updater::State::Downloading:
    case Updater::State::Installing: {
        dim("Tu utilises la version " + u->currentVersion() + ".");
        ImGui::Spacing();
        ImGui::PushFont(fonts_.medium);
        ImGui::TextUnformatted("Nouveaut\xC3\xA9s");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kField));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
        ImGui::BeginChild("##notes", ImVec2(0, S(220)), ImGuiChildFlags_AlwaysUseWindowPadding);
        const std::string notes = notesToText(r.notes);
        ImGui::TextWrapped("%s", notes.empty() ? "(pas de description pour cette version)" : notes.c_str());
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (st == Updater::State::Available) {
            const bool games = host_.anyGameRunning();
            if (games) dim("Ferme les jeux en cours avant d'installer (leur partie doit \xC3\xAAtre enregistr\xC3\xA9" "e).");
            else if (r.zipUrl.empty()) dim("Cette version n'a pas d'archive pour Windows : t\xC3\xA9l\xC3\xA9" "charge-la depuis sa page.");
            ImGui::BeginDisabled(games || r.zipUrl.empty());
            if (primary("Installer")) u->install();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Plus tard") || ImGui::IsKeyPressed(ImGuiKey_Escape)) { updateHidden_ = true; close = true; }
            ImGui::SameLine();
            if (ImGui::Button("Ignorer cette version")) { lib_.skipVersion = r.version; lib_.save(); updateHidden_ = true; close = true; }
            if (!r.page.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Page GitHub")) u->openPage(r.page);
            }
        } else {
            const float p = st == Updater::State::Installing ? 1.0f : u->progress();
            char label[64];
            if (st == Updater::State::Installing) std::snprintf(label, sizeof label, "Installation\xE2\x80\xA6");
            else std::snprintf(label, sizeof label, "T\xC3\xA9l\xC3\xA9" "chargement\xE2\x80\xA6 %d %%", int(p * 100.0f + 0.5f));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::ColorConvertU32ToFloat4(kAccent));
            ImGui::ProgressBar(p, ImVec2(-FLT_MIN, 0), label);
            ImGui::PopStyleColor();
        }
        break;
    }
    case Updater::State::Installed:
        dim("La version " + r.version + " est install\xC3\xA9" "e. Elle d\xC3\xA9marre d\xC3\xA8s que ndsemu est relanc\xC3\xA9.");
        ImGui::Spacing();
        if (primary("Red\xC3\xA9marrer maintenant")) u->restart();
        ImGui::SameLine();
        if (ImGui::Button("Plus tard")) close = true;
        break;
    }
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}
