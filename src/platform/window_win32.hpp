#pragma once
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "platform/input_win32.hpp"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11SamplerState;

// Fenetre de jeu : les deux ecrans de la console, agrandis, sous une barre de menus (theme sombre, Dear ImGui
// et Direct3D 11). Lit aussi le clavier et la souris.
//
// Touches du jeu (disposition habituelle des emulateurs) :
//   fleches = croix directionnelle   Z = A   X = B   S = X   A = Y
//   Q = L   W = R   Entree = Start   Retour arriere = Select
// Ce sont les valeurs par defaut : elles se changent dans Jeu > Touches et manette (ndsemu_touches.ini), qui
// regle aussi la manette (XInput : Xbox et compatibles).
// La souris (clic gauche maintenu sur l'ecran du bas) simule le stylet sur l'ecran tactile.
// Raccourcis : P pause, Ctrl+R reinitialiser, M couper le son, Ctrl+1..4 taille, F11 ou Alt+Entree plein ecran
// (Echap pour en sortir), F1..F4 charger un etat, Maj+F1..F4 sauvegarder, F12 instantane de diagnostic
// (etat_NNN.txt / .png / _3d.png).
// Les reglages (taille, lissage, disposition, volume...) sont gardes dans ndsemu_fenetre.ini a cote du programme.
class Window {
public:
    ~Window();
    bool create(const char* title);
    bool pump();                                           // traite les messages ; false quand la fenetre est fermee
    void present(const uint32_t* rgb, int width, int height);   // affiche une image 256 x 384 (0x00RRGGBB) et les menus
    void setTitle(const char* utf8Title);
    void setStatus(const std::string& status) { status_ = status; }       // a droite de la barre de menus
    void setPerf(const std::string& perf) { perf_ = perf; }               // details (Affichage > Performances)

    uint16_t keyInput() const;                             // boutons A,B,Select,Start,droite,gauche,haut,bas,R,L (0 = appuye)
    uint8_t keyXY() const;                                 // boutons X (bit 0) et Y (bit 1), 0 = appuye
    bool touch(int& x, int& y) const;                      // ecran tactile : x 0..255, y 0..191 ; false si non touche
    bool takeSnapshotRequest();                            // F12 (ou menu) depuis le dernier appel
    bool takeResetRequest();                               // Ctrl+R (ou menu) depuis le dernier appel
    // Sauvegardes d'etat : 4 emplacements. F1..F4 = charger, Maj+F1..F4 = sauvegarder (ou menu Jeu).
    static constexpr int kSlots = 4;
    int takeSaveRequest();                                 // 1..4, ou 0
    int takeLoadRequest();                                 // 1..4, ou 0
    void setSlotInfo(int slot, bool exists, int64_t time, const std::vector<uint32_t>& thumb);   // thumb : 256 x 384
    void notify(const std::string& text, bool error = false);   // message en bas de la fenetre
    bool paused() const { return paused_; }
    float volume() const { return muted_ ? 0.0f : float(volumePct_) / 100.0f; }

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);
    bool initGraphics();
    void releaseGraphics();
    void rebuildFonts();
    void drawUi();
    void setZoom(int z);
    void setFullscreen(bool on);
    void loadSettings();
    void saveSettings() const;
    int layoutW() const { return layout_ == 1 ? 512 : 256; }
    int layoutH() const { return layout_ == 1 ? 192 : 384; }

    HWND hwnd = nullptr;
    bool closed = false;
    bool keys[256] = {};
    bool mouseDown = false;
    int mouseX = 0, mouseY = 0;

    // Direct3D 11
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain* swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11Texture2D* tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    ID3D11SamplerState* point_ = nullptr;
    bool resize_ = false, fontsDirty_ = false;
    bool imguiReady_ = false;
    float scale_ = 1.0f;
    void* fonts_ = nullptr;                               // UiFonts (voir library_ui.hpp)
    std::vector<uint32_t> upload_;

    // Reglages
    int zoom_ = 2;                                        // taille : 1 a 4 fois
    bool smooth_ = false;                                 // lissage de l'image
    int layout_ = 0;                                      // 0 ecrans l'un sous l'autre, 1 cote a cote
    bool showPerf_ = false;
    int volumePct_ = 100;
    bool muted_ = false;
    bool paused_ = false;
    bool fullscreen_ = false;
    WINDOWPLACEMENT placement_ = {};
    bool snapshotRequest = false, resetRequest = false;
    int saveRequest_ = 0, loadRequest_ = 0;
    struct Slot { bool exists = false; int64_t time = 0; ID3D11ShaderResourceView* thumb = nullptr; } slots_[kSlots];
    std::string toast_;
    bool toastError_ = false;
    double toastUntil_ = 0;
    void drawSlotMenu(bool save);
    bool menuOpen_ = false;
    float barH_ = 0;
    float screenRect_[2][4] = {};                         // ecran du haut / du bas a l'affichage (x0, y0, x1, y1)
    std::string status_, perf_, title_;
    std::string settingsPath_;
    InputConfig input_;                                   // touches et manette
    Gamepad pad_;
    InputEditor inputEditor_;
    int64_t inputStamp_ = 0;
    int inputCheck_ = 0;
};
#endif
