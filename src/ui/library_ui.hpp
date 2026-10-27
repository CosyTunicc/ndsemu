#pragma once
#include <map>
#include <string>
#include "ui/library.hpp"

struct ImFont;
struct UiFonts;
class Updater;

// Ce que l'interface demande au systeme (Windows, ou l'apercu de test) : boites de dialogue, lancement du jeu,
// textures, fenetre sans cadre.
class LibraryHost {
public:
    virtual ~LibraryHost() = default;
    virtual std::string pickFolder() = 0;                                  // "" si annule
    virtual std::string pickRom() = 0;                                     // "" si annule
    virtual bool launch(const std::string& rom, const Library& lib, std::string& error) = 0;   // nouvelle fenetre
    virtual bool isRunning(const std::string& rom) = 0;
    virtual void stop(const std::string& rom) = 0;
    virtual std::string takeGameError() = 0;                               // un jeu lance s'est arrete sur une erreur
    virtual void reveal(const std::string& path) = 0;                      // montre le fichier / dossier dans l'explorateur
    virtual void* createTexture(const uint32_t* rgba, int w, int h) = 0;
    virtual void destroyTexture(void* tex) = 0;
    virtual void minimize() = 0;
    virtual void toggleMaximize() = 0;
    virtual bool isMaximized() = 0;
    virtual void close() = 0;
    virtual void openInputSettings() = 0;                                  // fenetre "Touches et manette"
    virtual void drawHostWindows(const UiFonts&, float /*scale*/) {}       // fenetres de l'hote (dans l'image de l'interface)
    virtual Updater* updater() { return nullptr; }                         // mises a jour (nullptr : pas prises en charge)
    virtual bool anyGameRunning() { return false; }
};

struct UiFonts {
    ImFont* body = nullptr;       // texte courant
    ImFont* medium = nullptr;     // titres de jeux, boutons
    ImFont* title = nullptr;      // titres des panneaux
    ImFont* caps = nullptr;       // en-tetes de colonnes (petites capitales) ; "small" est une macro de Windows
    ImFont* mono = nullptr;       // chemins, raccourcis
    ImFont* monoBold = nullptr;
};

// Chemins des polices et tailles : charge les polices dans l'atlas d'ImGui pour l'echelle donnee (1 = 96 ppp).
struct UiFontFiles { std::string regular, semibold, bold, mono, monoBold; };
UiFonts loadUiFonts(const UiFontFiles& files, float scale);
void applyDarkStyle(float scale);                         // theme sombre commun (bibliotheque et fenetre de jeu)

class LibraryUI {
public:
    LibraryUI(Library& lib, LibraryHost& host) : lib_(lib), host_(host) {}
    void setScale(float s) { s_ = s; }
    void setFonts(const UiFonts& f) { fonts_ = f; }
    void applyStyle();                                     // couleurs et arrondis d'ImGui
    void draw(double nowSeconds);                          // une image de l'interface (plein ecran)
    void onFilesDropped(const std::vector<std::string>& paths);
    void releaseTextures();
    const UiFonts& fonts() const { return fonts_; }

    // Pour la fenetre sans cadre : la barre de titre fait titleBarHeight pixels ; a droite de captionButtonsX,
    // ce sont les boutons (reduire, agrandir, fermer).
    float titleBarHeight = 0, captionButtonsX = 0;
    bool wantsClose = false;

private:
    Library& lib_;
    LibraryHost& host_;
    UiFonts fonts_;
    float s_ = 1.0f;
    float S(float v) const { return v * s_; }

    char search_[128] = {};
    bool focusSearch_ = false;
    std::string selected_;                                 // chemin du jeu selectionne
    std::map<std::string, std::pair<int64_t, void*>> textures_;   // icones (chemin -> date, texture)
    std::string toast_;
    bool toastError_ = false;
    double toastUntil_ = 0, now_ = 0;
    int openMenu_ = -1;
    bool showAbout_ = false;
    bool updateStarted_ = false;                           // recherche automatique faite (une fois par lancement)
    bool updateHidden_ = false;                            // bandeau "nouvelle version" ferme
    bool showUpdate_ = false;                              // ouvrir la fenetre des mises a jour

    void* iconTexture(const GameEntry& g);
    void notify(const std::string& text, bool error);
    void launch(const GameEntry& g);
    void addFolderDialog();
    void openRomDialog();

    void drawTitleBar();
    void drawMenuBar();
    void drawColumnHeaders(float x0, float x1);
    void drawContent();
    void drawFooter();
    void drawToast();
    void drawModals();
    void drawUpdateBanner();
    void drawUpdateWindow();
    bool drawFolderHeader(size_t index, GameFolder& f, int shown, float x0, float x1);
    void drawEmptyState(float x0, float x1, float y0, float y1, bool noFolder);
    void drawGameRow(const GameEntry& g, float x0, float x1);
    void drawGameTile(const GameEntry& g, float x, float y, float w, float h);
    void drawRecent(float x0, float x1);
    void drawRecentCard(const GameEntry& g, const PlayStat& p, float x, float y, float w, float h);
    void gameContextMenu(const GameEntry& g);
};
