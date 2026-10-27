#pragma once
// Touches du clavier et manette, reglables. Manettes reconnues : Xbox et compatibles (XInput), PlayStation 5
// (DualSense, DualSense Edge) et PlayStation 4 (DualShock 4), en USB ou en Bluetooth (lues directement en HID). La configuration est gardee
// dans ndsemu_touches.ini a cote du programme ; la bibliotheque et la fenetre de jeu la partagent (une fenetre de
// jeu ouverte la relit quand le fichier change).
#ifdef _WIN32
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

struct UiFonts;

// Boutons de la console, dans l'ordre des bits de KEYINPUT (0..9), puis X et Y (KEYXY)
enum DsButton { BtnA, BtnB, BtnSelect, BtnStart, BtnRight, BtnLeft, BtnUp, BtnDown, BtnR, BtnL, BtnX, BtnY, BtnCount };

// Entrees de la manette : boutons XInput (bits 0..15) et quelques entrees analogiques traitees comme des boutons
enum : uint32_t {
    PadLT = 1u << 16, PadRT = 1u << 17,                                   // gachettes
    PadStickLeft = 1u << 18, PadStickRight = 1u << 19, PadStickUp = 1u << 20, PadStickDown = 1u << 21,   // stick gauche
    PadHome = 1u << 22, PadTouchpad = 1u << 23,                          // bouton PS, pave tactile (PlayStation)
};
// Les boutons PlayStation sont ranges a la place de leurs equivalents Xbox (meme position sur la manette) :
// Croix = A, Rond = B, Carre = X, Triangle = Y, L1 = LB, R1 = RB, L2 = LT, R2 = RT, Options = Start, Create = Back.

struct InputConfig {
    int key[BtnCount];                // touche du clavier (code VK_...)
    uint32_t pad[BtnCount];           // entree de la manette (un bit), 0 = aucune
    bool stickAsDpad = true;          // le stick gauche sert aussi de croix directionnelle

    InputConfig();                    // valeurs par defaut
    bool load();                      // false si le fichier n'existe pas (on garde les valeurs par defaut)
    void save() const;
};

const char* dsButtonName(int b);
std::string keyName(int vk);
std::string padInputName(uint32_t bit, bool playstation = false);
std::string inputConfigPath();
int64_t inputConfigStamp();          // date du fichier (pour voir s'il a change), 0 s'il n'existe pas

// Manette PlayStation lue en HID dans un fil d'execution a part (recherche toutes les 2 s, puis lecture continue)
class PlayStationPad {
public:
    PlayStationPad();
    ~PlayStationPad();
    bool connected() const { return connected_.load(); }
    uint32_t state() const { return state_.load(); }
    const char* model() const;                         // "DualSense", "DualShock 4"...
private:
    void run();
    std::atomic<bool> stop_{ false }, connected_{ false };
    std::atomic<uint32_t> state_{ 0 };
    std::atomic<int> model_{ 0 };
    std::thread thread_;
};

// Manette : XInput (premiere branchee) et PlayStation. poll() une fois par image.
class Gamepad {
public:
    void poll();
    bool connected() const { return index_ >= 0 || ps_.connected(); }
    bool playstation() const { return index_ < 0 && ps_.connected(); }
    const char* model() const { return index_ >= 0 ? "Xbox / XInput" : ps_.model(); }
    uint32_t state() const { return state_ | ps_.state(); }   // entrees enfoncees (bits ci-dessus)
private:
    int index_ = -1;
    uint32_t state_ = 0;
    int probeWait_ = 0;                                // les manettes absentes ne sont cherchees que de temps en temps
    PlayStationPad ps_;
};

// Fenetre "Touches et manette" (Dear ImGui), utilisee par la bibliotheque et par la fenetre de jeu.
class InputEditor {
public:
    void open() { request_ = true; }
    bool isOpen() const { return visible_; }
    // A appeler a chaque image (dans une image ImGui). Renvoie true quand la configuration vient d'etre enregistree.
    bool draw(const UiFonts& fonts, float scale);
private:
    InputConfig cfg_;
    Gamepad pad_;
    bool request_ = false, visible_ = false;
    int captureRow_ = -1, captureCol_ = -1;            // case en attente d'une touche (colonne 0 clavier, 1 manette)
    bool keyDownAtStart_[256] = {};
    uint32_t padAtStart_ = 0;
    std::string message_;
};
#endif
