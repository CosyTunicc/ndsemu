// ndsemu avec SDL2 : version pour la PS5 (programme maison lance par le Homebrew Launcher de websrv) et pour
// Linux (pour essayer sur PC). Liste des jeux, partie, menu en jeu (sauvegardes d'etat, disposition des ecrans),
// manette (DualSense sur PS5, toute manette reconnue par SDL ailleurs) et clavier / souris sur PC.
//
// Manette (disposition de la DS : le bouton de droite fait A) :
//   Rond = A   Croix = B   Triangle = X   Carre = Y   L1 / R1 = L / R   Options = Start   L2 = Select
//   croix directionnelle ou stick gauche = croix de la DS
//   stick droit = deplace le stylet sur l'ecran du bas, R2 (ou R3) = poser le stylet
//   clic du pave tactile = menu (pause, etats, disposition, reinitialiser, retour a la liste)
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "core/emulator.hpp"
#include "core/sound.hpp"
#include "core/video.hpp"
#include "imgui.h"
#include "platform/sdl/soft_imgui.hpp"
#include "ui/library.hpp"

namespace fs = std::filesystem;

namespace {
#include "platform/sdl/fonts_embedded.inc"

// ---- dossiers ---------------------------------------------------------------------------------------------
#ifdef __PROSPERO__
const char* kDataDir = "/data/ndsemu";
std::vector<std::string> romDirs() {
    std::vector<std::string> d = { "/data/ndsemu/roms", "/data/homebrew/ndsemu/roms" };
    for (int i = 0; i < 8; i++) {
        d.push_back("/mnt/usb" + std::to_string(i) + "/ndsemu");
        d.push_back("/mnt/usb" + std::to_string(i) + "/homebrew/ndsemu/roms");
    }
    for (int i = 0; i < 2; i++) d.push_back("/mnt/ext" + std::to_string(i) + "/ndsemu");
    return d;
}
#else
const char* kDataDir = "ndsemu_donnees";
std::vector<std::string> romDirs() { return { "roms", "ndsemu_donnees/roms" }; }
#endif

// ---- journal ----------------------------------------------------------------------------------------------
// PS5 : chaque etape du demarrage est ecrite dans /data/ndsemu/journal.txt ; les etapes importantes et les
// erreurs s'affichent aussi en notification sur la console (on voit ainsi si le programme a bien demarre).
#ifdef __PROSPERO__
struct NotifyRequest { char unused[45]; char message[3075]; };
extern "C" int sceKernelSendNotificationRequest(int, NotifyRequest*, size_t, int);
void notifyPs5(const std::string& text) {
    NotifyRequest req = {};
    std::snprintf(req.message, sizeof req.message, "%s", text.c_str());
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
#else
void notifyPs5(const std::string&) {}
#endif
void journal(const std::string& text, bool notify = false) {
    static bool first = true;
    std::error_code ec;
    fs::create_directories(kDataDir, ec);
    if (FILE* f = std::fopen((std::string(kDataDir) + "/journal.txt").c_str(), first ? "w" : "a")) {
        std::fprintf(f, "%s\n", text.c_str());
        std::fclose(f);
    }
    first = false;
    std::fprintf(stderr, "%s\n", text.c_str());
    if (notify) notifyPs5("ndsemu : " + text);
}

// ---- jeux trouves -----------------------------------------------------------------------------------------
struct Game { GameEntry info; SoftTexture icon; };

std::vector<std::unique_ptr<Game>> scanGames(const std::vector<std::string>& extra) {
    std::vector<std::unique_ptr<Game>> games;
    std::vector<std::string> dirs = romDirs();
    dirs.insert(dirs.end(), extra.begin(), extra.end());
    for (const std::string& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
            std::string ext = it->path().extension().string();
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".nds") continue;
            auto g = std::make_unique<Game>();
            if (!Library::readRomInfo(it->path().string(), g->info)) continue;
            bool dup = false;
            for (const auto& o : games) if (o->info.path == g->info.path) dup = true;
            if (dup) continue;
            if (g->info.hasIcon) { g->icon.w = 32; g->icon.h = 32; g->icon.px.assign(g->info.icon, g->info.icon + 32 * 32); }
            games.push_back(std::move(g));
        }
    }
    std::sort(games.begin(), games.end(), [](const auto& a, const auto& b) { return a->info.title < b->info.title; });
    return games;
}

// ---- reglages (disposition des ecrans) ---------------------------------------------------------------------
struct Settings { int layout = 0; int volume = 100; bool perf = false; };
Settings loadSettings() {
    Settings s;
    std::ifstream in(fs::path(kDataDir) / "reglages.ini");
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const int v = std::atoi(line.c_str() + eq + 1);
        if (line.compare(0, eq, "disposition") == 0) s.layout = std::clamp(v, 0, 2);
        if (line.compare(0, eq, "volume") == 0) s.volume = std::clamp(v, 0, 100);
        if (line.compare(0, eq, "performances") == 0) s.perf = v != 0;
    }
    return s;
}
void saveSettings(const Settings& s) {
    std::ofstream out(fs::path(kDataDir) / "reglages.ini", std::ios::trunc);
    out << "disposition=" << s.layout << "\nvolume=" << s.volume << "\nperformances=" << (s.perf ? 1 : 0) << "\n";
}
const char* layoutName(int l) {
    switch (l) {
        case 1: return "C\xC3\xB4te \xC3\xA0 c\xC3\xB4te";
        case 2: return "Grand \xC3\xA9" "cran du haut";
        default: return "L'un sous l'autre";
    }
}

// ---- dessin des ecrans de la console -------------------------------------------------------------------------
struct Rect { int x, y, w, h; };

// Copie une moitie de l'image (0 = haut, 1 = bas) dans un rectangle, agrandie au plus proche
void blitScreen(const std::vector<uint32_t>& frame, int half, const Rect& r, const SoftTarget& t) {
    if (frame.size() != 256 * 384 || r.w <= 0 || r.h <= 0) return;
    static std::vector<int> xmap;
    xmap.resize(size_t(r.w));
    for (int x = 0; x < r.w; x++) xmap[size_t(x)] = std::min(255, x * 256 / r.w);
    for (int y = 0; y < r.h; y++) {
        const int dy = r.y + y;
        if (dy < 0 || dy >= t.height) continue;
        const uint32_t* src = frame.data() + size_t(half * 192 + std::min(191, y * 192 / r.h)) * 256;
        uint32_t* dst = t.pixels + size_t(dy) * size_t(t.pitch);
        for (int x = 0; x < r.w; x++) {
            const int dx = r.x + x;
            if (dx < 0 || dx >= t.width) continue;
            const uint32_t c = src[xmap[size_t(x)]];
            dst[dx] = 0xFF000000u | (t.rgbOrder ? c : ((c & 0xFF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16)));
        }
    }
}

// Rectangles des deux ecrans selon la disposition
void layoutScreens(int layout, int W, int H, Rect& top, Rect& bottom) {
    if (layout == 1) {                                         // cote a cote
        const float k = std::min(float(W) / 512.0f, float(H) / 192.0f);
        const int w = int(256 * k), h = int(192 * k);
        const int x0 = (W - 2 * w) / 2, y0 = (H - h) / 2;
        top = { x0, y0, w, h };
        bottom = { x0 + w, y0, w, h };
    } else if (layout == 2) {                                  // grand ecran du haut, petit ecran du bas a droite
        const float k = std::min(float(W) * 0.75f / 256.0f, float(H) / 192.0f);
        const int w = int(256 * k), h = int(192 * k);
        const int sw = std::min(W - w, int(256 * k / 3.0f)), sh = sw * 192 / 256;
        const int x0 = (W - w - sw) / 2;
        top = { x0, (H - h) / 2, w, h };
        bottom = { x0 + w, (H - h) / 2 + h - sh, sw, sh };
    } else {                                                   // l'un sous l'autre
        const float k = std::min(float(W) / 256.0f, float(H) / 384.0f);
        const int w = int(256 * k), h = int(192 * k);
        const int x0 = (W - w) / 2, y0 = (H - 2 * h) / 2;
        top = { x0, y0, w, h };
        bottom = { x0, y0 + h, w, h };
    }
}

// ---- style de l'interface (meme theme sombre que sur PC) -----------------------------------------------------
void applyStyle(float s) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowPadding = ImVec2(24 * s, 20 * s);
    st.FramePadding = ImVec2(14 * s, 10 * s);
    st.ItemSpacing = ImVec2(10 * s, 10 * s);
    st.WindowRounding = 14 * s;
    st.FrameRounding = 10 * s;
    st.PopupRounding = 10 * s;
    st.ScrollbarSize = 12 * s;
    st.ScrollbarRounding = 6 * s;
    st.WindowBorderSize = 1;
    st.AntiAliasedLines = false;                                // plus rapide en rendu logiciel
    st.AntiAliasedFill = false;
    st.AntiAliasedLinesUseTex = false;
    ImVec4* c = st.Colors;
    auto rgb = [](int r, int g, int b, int a = 255) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f); };
    c[ImGuiCol_Text] = rgb(0xE8, 0xEA, 0xED);
    c[ImGuiCol_TextDisabled] = rgb(0x7A, 0x7F, 0x86);
    c[ImGuiCol_WindowBg] = rgb(0x1A, 0x1C, 0x1F);
    c[ImGuiCol_PopupBg] = rgb(0x23, 0x25, 0x29);
    c[ImGuiCol_Border] = rgb(0x31, 0x34, 0x39);
    c[ImGuiCol_FrameBg] = rgb(0x20, 0x22, 0x26);
    c[ImGuiCol_Button] = rgb(0x26, 0x29, 0x2D);
    c[ImGuiCol_ButtonHovered] = rgb(0x2E, 0x4A, 0x3C);
    c[ImGuiCol_ButtonActive] = rgb(0x3D, 0xD6, 0x8C, 120);
    c[ImGuiCol_Header] = rgb(0x26, 0x29, 0x2D);
    c[ImGuiCol_HeaderHovered] = rgb(0x2E, 0x4A, 0x3C);
    c[ImGuiCol_HeaderActive] = rgb(0x3D, 0xD6, 0x8C, 120);
    c[ImGuiCol_NavHighlight] = rgb(0x3D, 0xD6, 0x8C);
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(0x3A, 0x3D, 0x42);
    c[ImGuiCol_Separator] = rgb(0x2A, 0x2D, 0x31);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 150);
}

// ---- manette -----------------------------------------------------------------------------------------------
struct Pad {
    SDL_GameController* gc = nullptr;
    bool btn(SDL_GameControllerButton b) const { return gc && SDL_GameControllerGetButton(gc, b); }
    float axis(SDL_GameControllerAxis a) const {
        if (!gc) return 0;
        const float v = float(SDL_GameControllerGetAxis(gc, a)) / 32767.0f;
        return std::fabs(v) < 0.2f ? 0.0f : v;                  // zone morte
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    journal("d\xC3\xA9marrage", true);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        journal(std::string("erreur SDL_Init : ") + SDL_GetError(), true);
        // sans le son ou la manette, on essaie quand meme d'afficher l'image
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            journal(std::string("erreur SDL_Init (image seule) : ") + SDL_GetError(), true);
            return 1;
        }
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        SDL_InitSubSystem(SDL_INIT_AUDIO);
    }
    journal(std::string("SDL pr\xC3\xAAt, pilote vid\xC3\xA9o : ") + (SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?"));
    std::error_code ec;
    fs::create_directories(fs::path(kDataDir) / "roms", ec);
    Settings settings = loadSettings();

#ifdef __PROSPERO__
    SDL_Window* window = SDL_CreateWindow("ndsemu", 0, 0, 1920, 1080, SDL_WINDOW_FULLSCREEN);
#else
    SDL_Window* window = SDL_CreateWindow("ndsemu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_RESIZABLE);
#endif
    if (!window) { journal(std::string("erreur SDL_CreateWindow : ") + SDL_GetError(), true); SDL_Quit(); return 1; }
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        journal("fen\xC3\xAAtre " + std::to_string(w) + " x " + std::to_string(h));
    }

    // Son : 32 728 Hz stereo (SDL convertit pour la sortie)
    SDL_AudioSpec want = {}, have = {};
    want.freq = Spu::kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);
    journal(audio ? "son : " + std::to_string(have.freq) + " Hz" : std::string("pas de son : ") + SDL_GetError());

    // Interface : Dear ImGui, dessinee en logiciel dans la surface de la fenetre
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    int winW = 0, winH = 0;
    SDL_GetWindowSize(window, &winW, &winH);
    const float ui = std::max(1.0f, float(winH) / 720.0f);       // taille du texte : 1080p -> x1,5
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0152, 0x0153, 0x2013, 0x2026, 0x2190, 0x2193, 0x25B6, 0x25B6, 0x25C0, 0x25C0, 0 };
    ImFont* fBody = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kFontRegular), int(sizeof kFontRegular), 18.0f * ui, &cfg, ranges);
    ImFont* fBold = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kFontBold), int(sizeof kFontBold), 18.0f * ui, &cfg, ranges);
    ImFont* fTitle = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kFontBold), int(sizeof kFontBold), 30.0f * ui, &cfg, ranges);
    SoftTexture fontTex;
    softImGuiInit(fontTex);
    applyStyle(ui);

    std::vector<std::string> extraDirs;
    std::string directRom;
    for (int i = 1; i < argc; i++) {
        std::error_code e2;
        if (fs::is_directory(argv[i], e2)) extraDirs.push_back(argv[i]);
        else if (fs::is_regular_file(argv[i], e2)) directRom = argv[i];
    }
    std::vector<std::unique_ptr<Game>> games = scanGames(extraDirs);
    journal(std::to_string(games.size()) + " jeu(x) trouv\xC3\xA9(s), " + std::to_string(SDL_NumJoysticks()) + " manette(s)");

    std::unique_ptr<Emulator> emu;
    bool playing = false, menuOpen = false, quit = false;
    std::string toast, errorText, currentTitle;
    Uint64 toastUntil = 0;
    float curX = 128, curY = 96;                                 // stylet (ecran du bas)
    Uint64 cursorShownUntil = 0;
    Pad pad;
    bool prevTouchpad = false;
    int saveTimer = 0;
    const Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 last = SDL_GetPerformanceCounter(), emuLast = last;
    double emuClock = 0;
    double perfEmu = 0;                          // mesures (menu > Performances)
    int perfEmuFrames = 0, perfShown = 0;
    std::string perfText;
    Uint64 perfSince = SDL_GetPerformanceCounter();
    std::vector<int16_t> audioBuf(4096 * 2);

    auto notify = [&](const std::string& t) { toast = t; toastUntil = SDL_GetTicks64() + 2500; };
    auto startGame = [&](const std::string& path) {
        emu = std::make_unique<Emulator>();
        std::string err;
        journal("lancement : " + path);
        if (!emu->load(path, kDataDir, err)) { errorText = "Impossible de lancer le jeu : " + err; journal(errorText); emu.reset(); return; }
        playing = true;
        menuOpen = false;
        GameEntry info;
        currentTitle = Library::readRomInfo(path, info) ? info.title : fs::path(path).filename().string();
        if (audio) SDL_ClearQueuedAudio(audio);
        emuClock = 0;
    };
    auto stopGame = [&]() {
        if (emu) emu->saveIfDirty();
        emu.reset();
        playing = false;
        menuOpen = false;
        if (audio) SDL_ClearQueuedAudio(audio);
        games = scanGames(extraDirs);
    };
    if (!directRom.empty()) startGame(directRom);
    // Essais : NDSEMU_CAPTURE=fichier.png:N ecrit l'image affichee apres N images puis quitte
    std::string capFile;
    int capFrames = -1, frameNo = 0, shownFrames = 0;
    if (const char* c = std::getenv("NDSEMU_CAPTURE")) {
        const std::string v = c;
        const size_t colon = v.rfind(':');
        capFile = v.substr(0, colon);
        capFrames = colon == std::string::npos ? 60 : std::atoi(v.c_str() + colon + 1);
    }
    if (const char* m = std::getenv("NDSEMU_MENU")) menuOpen = playing && m[0] == '1';

    while (!quit) {
        // ---- evenements ----
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT: quit = true; break;
                case SDL_CONTROLLERDEVICEADDED:
                    if (!pad.gc) pad.gc = SDL_GameControllerOpen(e.cdevice.which);
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    if (pad.gc && SDL_GameControllerGetJoystick(pad.gc) == SDL_JoystickFromInstanceID(e.cdevice.which)) {
                        SDL_GameControllerClose(pad.gc);
                        pad.gc = nullptr;
                    }
                    break;
                case SDL_MOUSEMOTION: io.AddMousePosEvent(float(e.motion.x), float(e.motion.y)); break;
                case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                    if (e.button.button == SDL_BUTTON_LEFT) io.AddMouseButtonEvent(0, e.type == SDL_MOUSEBUTTONDOWN);
                    break;
                case SDL_MOUSEWHEEL: io.AddMouseWheelEvent(float(e.wheel.x), float(e.wheel.y)); break;
                case SDL_KEYDOWN: {
                    const SDL_Keycode k = e.key.keysym.sym;
                    const bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
                    if (k == SDLK_ESCAPE && playing) menuOpen = !menuOpen;
                    if (playing && !menuOpen && k >= SDLK_F1 && k <= SDLK_F4 && !e.key.repeat) {
                        const int slot = int(k - SDLK_F1) + 1;
                        std::string err;
                        if (shift) notify(emu->saveState(slot, err) ? "\xC3\x89tat " + std::to_string(slot) + " sauvegard\xC3\xA9" : "Erreur : " + err);
                        else notify(emu->loadState(slot, err) ? "\xC3\x89tat " + std::to_string(slot) + " charg\xC3\xA9" : "\xC3\x89tat " + std::to_string(slot) + " : " + err);
                    }
                    if (k == SDLK_UP) io.AddKeyEvent(ImGuiKey_UpArrow, true);
                    if (k == SDLK_DOWN) io.AddKeyEvent(ImGuiKey_DownArrow, true);
                    if (k == SDLK_RETURN) io.AddKeyEvent(ImGuiKey_Enter, true);
                    if (k == SDLK_SPACE) io.AddKeyEvent(ImGuiKey_Space, true);
                    break;
                }
                case SDL_KEYUP: {
                    const SDL_Keycode k = e.key.keysym.sym;
                    if (k == SDLK_UP) io.AddKeyEvent(ImGuiKey_UpArrow, false);
                    if (k == SDLK_DOWN) io.AddKeyEvent(ImGuiKey_DownArrow, false);
                    if (k == SDLK_RETURN) io.AddKeyEvent(ImGuiKey_Enter, false);
                    if (k == SDLK_SPACE) io.AddKeyEvent(ImGuiKey_Space, false);
                    break;
                }
                default: break;
            }
        }
        if (!pad.gc)                                             // manette deja branchee au demarrage
            for (int i = 0; i < SDL_NumJoysticks() && !pad.gc; i++)
                if (SDL_IsGameController(i)) pad.gc = SDL_GameControllerOpen(i);

        // Manette -> navigation dans l'interface (hors partie, ou menu ouvert)
        const bool uiNav = !playing || menuOpen;
        auto navKey = [&](ImGuiKey key, bool down) { io.AddKeyEvent(key, uiNav && down); };
        navKey(ImGuiKey_GamepadDpadUp, pad.btn(SDL_CONTROLLER_BUTTON_DPAD_UP) || pad.axis(SDL_CONTROLLER_AXIS_LEFTY) < -0.6f);
        navKey(ImGuiKey_GamepadDpadDown, pad.btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || pad.axis(SDL_CONTROLLER_AXIS_LEFTY) > 0.6f);
        navKey(ImGuiKey_GamepadDpadLeft, pad.btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT));
        navKey(ImGuiKey_GamepadDpadRight, pad.btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
        navKey(ImGuiKey_GamepadFaceDown, pad.btn(SDL_CONTROLLER_BUTTON_A));       // Croix : valider
        navKey(ImGuiKey_GamepadFaceRight, pad.btn(SDL_CONTROLLER_BUTTON_B));      // Rond : retour

        // Pave tactile : ouvre / ferme le menu en jeu
        const bool touchpad = pad.btn(SDL_CONTROLLER_BUTTON_TOUCHPAD);
        if (playing && touchpad && !prevTouchpad) menuOpen = !menuOpen;
        prevTouchpad = touchpad;

        // Temps ecoule -> nombre d'images de la console a calculer (au plus 2 d'un coup)
        int framesDue = 0;
        {
            const Uint64 t = SDL_GetPerformanceCounter();
            emuClock += double(t - emuLast) / double(freq);
            emuLast = t;
            if (emuClock > 0.1) emuClock = 0.1;
            const double period = 1.0 / 59.8261;
#ifndef __PROSPERO__
            if (playing && !menuOpen && emuClock < period) {            // PC : on attend la prochaine image
                SDL_Delay(Uint32((period - emuClock) * 1000.0));
                continue;
            }
#endif
            while (emuClock >= period && framesDue < 2) { emuClock -= period; framesDue++; }
            if (emuClock >= period) emuClock = 0;                       // trop en retard : on ne rattrape pas
            if (!playing || menuOpen) { framesDue = 0; emuClock = 0; }
        }
        SDL_Surface* surf = SDL_GetWindowSurface(window);
        if (!surf) { SDL_Delay(16); continue; }
        SDL_LockSurface(surf);
        SoftTarget target;
        target.pixels = static_cast<uint32_t*>(surf->pixels);
        target.width = surf->w;
        target.height = surf->h;
        target.pitch = surf->pitch / 4;
        target.rgbOrder = surf->format->Rshift == 16;
        for (int y = 0; y < surf->h; y++) std::fill_n(target.pixels + size_t(y) * size_t(target.pitch), surf->w, 0xFF101113u);

        // ---- partie ----
        if (playing && emu) {
            Rect rTop{}, rBot{};
            layoutScreens(settings.layout, surf->w, surf->h, rTop, rBot);
            if (!menuOpen) {
                const Uint8* ks = SDL_GetKeyboardState(nullptr);
                uint16_t keys = 0x03FF;
                uint8_t xy = 0x4B;
                auto press = [&](bool on, int bit) { if (on) keys = uint16_t(keys & ~(1u << bit)); };
                const float lx = pad.axis(SDL_CONTROLLER_AXIS_LEFTX), ly = pad.axis(SDL_CONTROLLER_AXIS_LEFTY);
                press(pad.btn(SDL_CONTROLLER_BUTTON_B) || ks[SDL_SCANCODE_Z], 0);                               // A
                press(pad.btn(SDL_CONTROLLER_BUTTON_A) || ks[SDL_SCANCODE_X], 1);                               // B
                press(pad.axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.5f || pad.btn(SDL_CONTROLLER_BUTTON_BACK) || ks[SDL_SCANCODE_BACKSPACE], 2);   // Select
                press(pad.btn(SDL_CONTROLLER_BUTTON_START) || ks[SDL_SCANCODE_RETURN], 3);                      // Start
                press(pad.btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > 0.5f || ks[SDL_SCANCODE_RIGHT], 4);
                press(pad.btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -0.5f || ks[SDL_SCANCODE_LEFT], 5);
                press(pad.btn(SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -0.5f || ks[SDL_SCANCODE_UP], 6);
                press(pad.btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > 0.5f || ks[SDL_SCANCODE_DOWN], 7);
                press(pad.btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) || ks[SDL_SCANCODE_W], 8);                  // R
                press(pad.btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) || ks[SDL_SCANCODE_Q], 9);                   // L
                if (pad.btn(SDL_CONTROLLER_BUTTON_Y) || ks[SDL_SCANCODE_S]) xy = uint8_t(xy & ~1u);          // X
                if (pad.btn(SDL_CONTROLLER_BUTTON_X) || ks[SDL_SCANCODE_A]) xy = uint8_t(xy & ~2u);          // Y

                // Stylet : stick droit + R2 / R3, ou souris sur l'ecran du bas
                const float rx = pad.axis(SDL_CONTROLLER_AXIS_RIGHTX), ry = pad.axis(SDL_CONTROLLER_AXIS_RIGHTY);
                if (rx != 0 || ry != 0) {
                    curX = std::clamp(curX + rx * 4.0f, 0.0f, 255.0f);
                    curY = std::clamp(curY + ry * 4.0f, 0.0f, 191.0f);
                    cursorShownUntil = SDL_GetTicks64() + 2000;
                }
                bool touch = pad.axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f || pad.btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
                if (touch) cursorShownUntil = SDL_GetTicks64() + 2000;
                int mx = 0, my = 0;
                if ((SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK) && mx >= rBot.x && mx < rBot.x + rBot.w &&
                    my >= rBot.y && my < rBot.y + rBot.h) {
                    touch = true;
                    curX = float((mx - rBot.x) * 256 / rBot.w);
                    curY = float((my - rBot.y) * 192 / rBot.h);
                }
                emu->setInput(keys, xy, touch, int(curX), int(curY));

                // Autant d'images de la console que de temps reel ecoule (59,83 par seconde) : si l'affichage
                // ralentit (30 images/s sur la PS5 si une image prend trop de temps), le jeu garde sa vitesse.
                bool failed = false;
                for (int k = 0; k < framesDue && !failed; k++) {
                    const Uint64 qa = SDL_GetPerformanceCounter();
                    if (!emu->runFrame()) { failed = true; break; }
                    perfEmu += double(SDL_GetPerformanceCounter() - qa) * 1000.0 / double(freq);
                    perfEmuFrames++;
                    for (size_t n; (n = emu->takeAudio(audioBuf.data(), 4096)) > 0;) {
                        if (!audio) continue;
                        // trop d'avance (plus de ~150 ms en attente) : on laisse tomber ce morceau
                        if (SDL_GetQueuedAudioSize(audio) > Uint32(Spu::kSampleRate * 4 * 15 / 100)) continue;
                        if (settings.volume < 100)
                            for (size_t i = 0; i < n * 2; i++) audioBuf[i] = int16_t(audioBuf[i] * settings.volume / 100);
                        SDL_QueueAudio(audio, audioBuf.data(), Uint32(n * 4));
                    }
                    if (++saveTimer >= 60) { saveTimer = 0; emu->saveIfDirty(); }
                }
                if (failed) {
                    errorText = "Le jeu s'est arr\xC3\xAAt\xC3\xA9 sur une erreur :\n" + emu->error();
                    journal(errorText);
                    stopGame();
                } else if (framesDue > 0) {
                    emu->render();
                }
            }
            if (emu) {
                blitScreen(emu->lastFrame(), 0, rTop, target);
                blitScreen(emu->lastFrame(), 1, rBot, target);
            }
        }

        // ---- interface ----
        const Uint64 now = SDL_GetPerformanceCounter();
        io.DeltaTime = std::max(1e-4f, float(double(now - last) / double(freq)));
        last = now;
        io.DisplaySize = ImVec2(float(surf->w), float(surf->h));
        ImGui::NewFrame();
        ImGui::PushFont(fBody);
        const float W = float(surf->w), H = float(surf->h);

        if (!playing) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(W, H));
            ImGui::Begin("##liste", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImGui::PushFont(fTitle);
            ImGui::TextUnformatted("ndsemu");
            ImGui::PopFont();
            ImGui::TextDisabled("\xC3\x89mulateur Nintendo DS");
            ImGui::Separator();
            if (games.empty()) {
                ImGui::Spacing();
                ImGui::PushFont(fBold);
                ImGui::TextUnformatted("Aucun jeu trouv\xC3\xA9.");
                ImGui::PopFont();
                ImGui::TextUnformatted("Mets tes fichiers .nds dans un de ces dossiers, puis relance ndsemu :");
#ifdef __PROSPERO__
                ImGui::BulletText("cl\xC3\xA9 USB : /ndsemu  (dossier \xC2\xAB ndsemu \xC2\xBB \xC3\xA0 la racine de la cl\xC3\xA9)");
                ImGui::BulletText("disque interne : /data/ndsemu/roms");
#else
                ImGui::BulletText("./roms  ou  ./ndsemu_donnees/roms");
#endif
            } else {
                const float rowH = 64 * ui;
                ImGui::BeginChild("##jeux", ImVec2(0, H - ImGui::GetCursorPosY() - 70 * ui));
                for (size_t i = 0; i < games.size(); i++) {
                    Game& g = *games[i];
                    ImGui::PushID(int(i));
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable("##jeu", false, 0, ImVec2(0, rowH))) startGame(g.info.path);
                    if (i == 0 && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const float is = rowH - 12 * ui;
                    if (!g.icon.px.empty())
                        dl->AddImage(ImTextureID(&g.icon), ImVec2(p.x + 8 * ui, p.y + 6 * ui), ImVec2(p.x + 8 * ui + is, p.y + 6 * ui + is));
                    dl->AddText(fBold, fBold->FontSize, ImVec2(p.x + is + 24 * ui, p.y + 10 * ui), IM_COL32(0xE8, 0xEA, 0xED, 255), g.info.title.c_str());
                    const std::string sub = (g.info.subtitle.empty() ? g.info.fileName : g.info.subtitle) + "  \xC2\xB7  " + g.info.gameCode;
                    dl->AddText(fBody, fBody->FontSize, ImVec2(p.x + is + 24 * ui, p.y + 36 * ui), IM_COL32(0x9A, 0xA0, 0xA6, 255), sub.c_str());
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }
            ImGui::SetCursorPosY(H - 56 * ui);
            ImGui::TextDisabled("Croix : jouer     Pav\xC3\xA9 tactile pendant le jeu : menu");
            ImGui::SameLine(W - 200 * ui);
            if (ImGui::Button("Quitter ndsemu")) quit = true;
            ImGui::End();
        } else if (menuOpen && emu) {
            ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(520 * ui, 0));
            ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::PushFont(fTitle);
            ImGui::TextUnformatted("Pause");
            ImGui::PopFont();
            ImGui::TextDisabled("%s", currentTitle.c_str());
            const ImVec2 bw(-FLT_MIN, 0);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            if (ImGui::Button("Reprendre", bw)) menuOpen = false;
            ImGui::Separator();
            for (int slot = 1; slot <= 4; slot++) {
                ImGui::PushID(slot);
                const std::string ls = "Sauvegarder l'\xC3\xA9tat " + std::to_string(slot);
                if (ImGui::Button(ls.c_str(), ImVec2(240 * ui, 0))) {
                    std::string err;
                    notify(emu->saveState(slot, err) ? "\xC3\x89tat " + std::to_string(slot) + " sauvegard\xC3\xA9" : "Erreur : " + err);
                    menuOpen = false;
                }
                ImGui::SameLine();
                const std::string ll = "Charger l'\xC3\xA9tat " + std::to_string(slot);
                const bool exists = fs::exists(emu->statePath(slot), ec);
                ImGui::BeginDisabled(!exists);
                if (ImGui::Button(ll.c_str(), ImVec2(-FLT_MIN, 0))) {
                    std::string err;
                    notify(emu->loadState(slot, err) ? "\xC3\x89tat " + std::to_string(slot) + " charg\xC3\xA9" : "\xC3\x89tat " + std::to_string(slot) + " : " + err);
                    menuOpen = false;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::Separator();
            const std::string lay = std::string("Disposition : ") + layoutName(settings.layout);
            if (ImGui::Button(lay.c_str(), bw)) { settings.layout = (settings.layout + 1) % 3; saveSettings(settings); }
            const std::string vol = "Volume : " + std::to_string(settings.volume) + " %";
            if (ImGui::Button(vol.c_str(), bw)) { settings.volume = settings.volume >= 100 ? 0 : settings.volume + 25; saveSettings(settings); }
            if (ImGui::Button(settings.perf ? "Performances : affich\xC3\xA9" "es" : "Performances : masqu\xC3\xA9" "es", bw)) { settings.perf = !settings.perf; saveSettings(settings); }
            if (ImGui::Button("R\xC3\xA9initialiser", bw)) { emu->reset(); menuOpen = false; notify("Console r\xC3\xA9initialis\xC3\xA9" "e"); }
            if (ImGui::Button("Retour \xC3\xA0 la liste des jeux", bw)) stopGame();
            ImGui::Spacing();
            ImGui::TextDisabled("Pav\xC3\xA9 tactile : reprendre");
            ImGui::End();
        }

        if (playing && !menuOpen && SDL_GetTicks64() < cursorShownUntil) {    // curseur du stylet
            Rect rTop{}, rBot{};
            layoutScreens(settings.layout, surf->w, surf->h, rTop, rBot);
            const float cx = float(rBot.x) + curX * float(rBot.w) / 256.0f, cy = float(rBot.y) + curY * float(rBot.h) / 192.0f;
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            fg->AddCircle(ImVec2(cx, cy), 9 * ui, IM_COL32(0, 0, 0, 200), 16, 4 * ui);
            fg->AddCircle(ImVec2(cx, cy), 9 * ui, IM_COL32(0x3D, 0xD6, 0x8C, 255), 16, 2 * ui);
        }
        // Performances : images affichees par seconde, temps de calcul d'une image de la console
        perfShown++;
        if (double(SDL_GetPerformanceCounter() - perfSince) >= double(freq)) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%d images/s  \xC2\xB7  \xC3\xA9mulation %.1f ms/image", perfShown,
                          perfEmuFrames ? perfEmu / perfEmuFrames : 0.0);
            perfText = buf;
            perfShown = 0; perfEmu = 0; perfEmuFrames = 0;
            perfSince = SDL_GetPerformanceCounter();
        }
        if (playing && settings.perf) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const ImVec2 ts = ImGui::CalcTextSize(perfText.c_str());
            fg->AddRectFilled(ImVec2(12 * ui, 12 * ui), ImVec2(28 * ui + ts.x, 28 * ui + ts.y), IM_COL32(0, 0, 0, 170), 6 * ui);
            fg->AddText(ImVec2(20 * ui, 20 * ui), IM_COL32(0xE8, 0xEA, 0xED, 255), perfText.c_str());
        }
        if (!toast.empty() && SDL_GetTicks64() < toastUntil) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const ImVec2 ts = ImGui::CalcTextSize(toast.c_str());
            const ImVec2 a((W - ts.x) * 0.5f - 18 * ui, H - ts.y - 60 * ui);
            fg->AddRectFilled(a, ImVec2(a.x + ts.x + 36 * ui, a.y + ts.y + 24 * ui), IM_COL32(0x23, 0x25, 0x29, 235), 10 * ui);
            fg->AddText(ImVec2(a.x + 18 * ui, a.y + 12 * ui), IM_COL32(0xE8, 0xEA, 0xED, 255), toast.c_str());
        }
        if (!errorText.empty()) {
            ImGui::OpenPopup("##erreur");
            ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("##erreur", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(errorText.c_str());
                ImGui::Spacing();
                if (ImGui::Button("OK", ImVec2(160 * ui, 0))) { errorText.clear(); ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
        }
        ImGui::PopFont();
        ImGui::Render();
        softImGuiRender(ImGui::GetDrawData(), target);
        if (capFrames >= 0 && ++frameNo >= capFrames) {
            std::vector<uint32_t> img(size_t(surf->w) * size_t(surf->h));
            for (int y = 0; y < surf->h; y++)
                for (int x = 0; x < surf->w; x++) {
                    const uint32_t c = target.pixels[size_t(y) * size_t(target.pitch) + size_t(x)];
                    img[size_t(y) * size_t(surf->w) + size_t(x)] = target.rgbOrder ? (c & 0xFFFFFFu) : (((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF));
                }
            writePng(capFile.c_str(), img, surf->w, surf->h);
            quit = true;
        }
        SDL_UnlockSurface(surf);
        const bool shown = SDL_UpdateWindowSurface(window) == 0;   // PS5 : attend l'affichage (60 images/s)
        if (++shownFrames <= 3 && !shown) journal(std::string("erreur d'affichage : ") + SDL_GetError(), true);
        if (shownFrames == 1 && shown) journal("premi\xC3\xA8re image affich\xC3\xA9" "e", true);

#ifndef __PROSPERO__
        if (!playing || menuOpen) SDL_Delay(15);                   // PC : interface a ~60 images/s
#endif
    }

    if (emu) emu->saveIfDirty();
    ImGui::DestroyContext();
    if (pad.gc) SDL_GameControllerClose(pad.gc);
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_DestroyWindow(window);
    SDL_Quit();
    journal("fin");
    return 0;
}
