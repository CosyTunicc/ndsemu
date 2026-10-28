#pragma once
// La console complete, prete a l'emploi pour une interface (SDL, PS5...) : chargement de la ROM, demarrage,
// une image a la fois, sauvegarde du jeu, sauvegardes d'etat, reinitialisation. Aucune dependance au systeme.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "core/arm.hpp"
#include "core/memory.hpp"
#include "core/nds_header.hpp"

// Ce que le demarrage reel (BIOS + firmware) ecrit en fin de memoire principale avant de lancer le jeu.
void ndsSetupBootMemory(IBus& bus, const std::vector<uint8_t>& rom, uint32_t chipId, const std::vector<uint8_t>& firmware);

class Emulator {
public:
    static constexpr uint32_t kCyclesPerFrame = 560190;   // une image de la console (59,83 images/s)

    // dataDir : dossier des donnees de l'emulateur (firmware.bin, etats/). La sauvegarde du jeu (.sav) est a
    // cote de la ROM.
    bool load(const std::filesystem::path& rom, const std::filesystem::path& dataDir, std::string& error);
    bool loaded() const { return !rom_.empty(); }

    void setInput(uint16_t keyInput, uint8_t keyXY, bool touchDown, int touchX, int touchY);
    bool runFrame();                                        // false : arret sur une instruction non geree (error())
    const std::vector<uint32_t>& render();                  // les deux ecrans, 256 x 384, 0x00RRGGBB
    const std::vector<uint32_t>& lastFrame() const { return frame_; }
    size_t takeAudio(int16_t* stereo, size_t maxFrames);    // son produit (32 728 Hz, stereo)

    bool saveIfDirty();                                     // .sav (et firmware.bin) si le jeu a ecrit ; vrai si ecrit
    bool reset();                                           // comme eteindre et rallumer
    std::filesystem::path statePath(int slot) const;        // dataDir/etats/<nom du jeu>.etatN
    bool saveState(int slot, std::string& error);
    bool loadState(int slot, std::string& error);

    const NdsHeader& header() const { return header_; }
    const std::string& error() const { return error_; }
    const std::filesystem::path& romPath() const { return romPath_; }
    System& sys() { return sys_; }
    // 3D dessinee sur un autre coeur (vrai par defaut) ; a regler avant load()
    void setAsync3D(bool on) { async3d_ = on; }

private:
    bool boot();
    void saveFirmware();

    // Ordre important : construits dans cet ordre, reconstruits sur place par reset()
    System sys_;
    Bus9 bus9_{ sys_ };
    Bus7 bus7_{ sys_ };
    ArmCpu cpu9_{ bus9_, true, "ARM9" };
    ArmCpu cpu7_{ bus7_, false, "ARM7" };

    std::vector<uint8_t> rom_;
    NdsHeader header_;
    std::filesystem::path romPath_, savePath_, dataDir_, firmwarePath_;
    std::vector<uint8_t> lastSaved_;
    std::vector<uint32_t> frame_;
    std::string error_;
    bool async3d_ = true;
};
