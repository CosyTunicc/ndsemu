#include "core/emulator.hpp"
#include <fstream>
#include <new>
#include "core/rom.hpp"
#include "core/savestate.hpp"
#include "core/video.hpp"

void ndsSetupBootMemory(IBus& bus, const std::vector<uint8_t>& rom, uint32_t chipId, const std::vector<uint8_t>& firmware) {
    auto hdr16 = [&](size_t off) -> uint16_t { return uint16_t(rom[off] | (rom[off + 1] << 8)); };
    bus.loadBlock(0x027FFE00, rom.data(), 0x170);          // copie de l'en-tete de la cartouche
    bus.write32(0x027FF800, chipId);                       // identifiant de la cartouche (2 copies)
    bus.write32(0x027FF804, chipId);
    bus.write16(0x027FF808, hdr16(0x15E));                 // CRC de l'en-tete
    bus.write16(0x027FF80A, hdr16(0x06C));                 // CRC de la zone securisee
    bus.write16(0x027FF850, 0x5835);
    bus.write16(0x027FF880, 0x0007);
    bus.write16(0x027FF884, 0x0006);
    bus.write32(0x027FFC00, chipId);
    bus.write32(0x027FFC04, chipId);
    bus.write16(0x027FFC08, hdr16(0x15E));
    bus.write16(0x027FFC0A, hdr16(0x06C));
    bus.write16(0x027FFC10, 0x5835);
    bus.write16(0x027FFC30, 0xFFFF);
    bus.write16(0x027FFC40, 0x0001);                       // demarrage normal
    bus.loadBlock(0x027FFC80, &firmware[System::kUserSettings], 0x70);   // reglages de l'utilisateur
}

bool Emulator::load(const std::filesystem::path& rom, const std::filesystem::path& dataDir, std::string& error) {
    std::vector<uint8_t> data;
    if (!loadFile(rom, data)) { error = "fichier illisible"; return false; }
    NdsHeader h;
    if (!parseNdsHeader(data, h, error)) return false;
    if (size_t(h.arm9RomOffset) + h.arm9Size > data.size() || size_t(h.arm7RomOffset) + h.arm7Size > data.size()) {
        error = "ROM incomplete";
        return false;
    }
    rom_.swap(data);
    header_ = h;
    romPath_ = rom;
    savePath_ = std::filesystem::path(rom).replace_extension(".sav");
    dataDir_ = dataDir;
    firmwarePath_ = dataDir / "firmware.bin";
    lastSaved_.clear();
    if (!reset()) { error = error_; rom_.clear(); return false; }
    return true;
}

bool Emulator::boot() {
    sys_.cartRom = &rom_;
    sys_.gx.deferred = sys_.gx.async = async3d_;
    sys_.dmaBus[0] = &bus9_;
    sys_.dmaBus[1] = &bus7_;
    std::vector<uint8_t> saved;
    if (loadFile(savePath_, saved) && !saved.empty()) {
        for (size_t i = 0; i < saved.size() && i < sys_.backup.size(); i++) sys_.backup[i] = saved[i];
        sys_.backupMaxWritten = uint32_t(saved.size() < sys_.backup.size() ? saved.size() : sys_.backup.size());
        sys_.setSaveType(saved.size() <= 512 ? 1 : (saved.size() <= 65536 ? 2 : 3));
    }
    if (!bus9_.loadBlock(header_.arm9RamAddr, &rom_[header_.arm9RomOffset], header_.arm9Size) ||
        !bus7_.loadBlock(header_.arm7RamAddr, &rom_[header_.arm7RomOffset], header_.arm7Size)) {
        error_ = "chargement du jeu en memoire impossible";
        return false;
    }
    std::vector<uint8_t> fw;
    if (loadFile(firmwarePath_, fw) && fw.size() == sys_.firmware.size()) sys_.firmware = fw;
    ndsSetupBootMemory(bus9_, rom_, sys_.cardChipId, sys_.firmware);
    bus9_.write8(0x04000242, 0x82);                        // VRAM C et D a l'ARM7, comme au lancement d'un jeu
    bus9_.write8(0x04000243, 0x8A);
    cpu9_.reset(header_.arm9Entry);
    cpu7_.reset(header_.arm7Entry);
    cpu9_.diagnostics = cpu7_.diagnostics = false;
    return true;
}

bool Emulator::reset() {
    saveIfDirty();
    cpu7_.~ArmCpu();
    cpu9_.~ArmCpu();
    bus7_.~Bus7();
    bus9_.~Bus9();
    sys_.~System();
    new (&sys_) System();
    new (&bus9_) Bus9(sys_);
    new (&bus7_) Bus7(sys_);
    new (&cpu9_) ArmCpu(bus9_, true, "ARM9");
    new (&cpu7_) ArmCpu(bus7_, false, "ARM7");
    lastSaved_.clear();
    error_.clear();
    return boot();
}

void Emulator::setInput(uint16_t keyInput, uint8_t keyXY, bool touchDown, int touchX, int touchY) {
    sys_.keyInput = keyInput;
    sys_.keyXY = keyXY;
    sys_.touchDown = touchDown;
    if (touchDown) {
        sys_.touchX = uint16_t(touchX < 0 ? 0 : (touchX > 255 ? 255 : touchX));
        sys_.touchY = uint16_t(touchY < 0 ? 0 : (touchY > 191 ? 191 : touchY));
        sys_.keyXY = uint8_t(sys_.keyXY & ~0x40u);          // KEYXY bit 6 = 0 : stylet pose
    }
}

// Une image : meme boucle que la fenetre Windows (2 instructions ARM9 pour 1 ARM7, temps par paquets).
// Avec la 3D sur un autre coeur, l'image s'arrete au debut du V-Blank : l'image visible est alors complete et le
// dessin 3D lance a cet instant a toute l'image suivante pour se faire.
bool Emulator::runFrame() {
    if (!loaded()) return false;
    const uint64_t target = sys_.cycles + (async3d_ ? 2 * kCyclesPerFrame : kCyclesPerFrame);
    sys_.vblankReached = false;
    while (sys_.cycles < target && !(async3d_ && sys_.vblankReached)) {
        const uint64_t left = target - sys_.cycles;
        if (cpu9_.halted && cpu7_.halted && !sys_.haltRequest[0] && !sys_.haltRequest[1] &&
            !bus9_.wakePending() && !bus7_.wakePending()) {
            uint32_t skip = sys_.cyclesToEvent();
            if (skip > left) skip = uint32_t(left);
            sys_.advance(skip);
            continue;
        }
        uint32_t chunk = sys_.cyclesToEvent();
        if (chunk > 32) chunk = 32;
        if (chunk > left) chunk = uint32_t(left);
        uint32_t done = 0;
        while (done < chunk) {
            if (!cpu9_.step()) { error_ = "ARM9 : " + cpu9_.error; return false; }
            if (!cpu9_.step()) { error_ = "ARM9 : " + cpu9_.error; return false; }
            if (!cpu7_.step()) { error_ = "ARM7 : " + cpu7_.error; return false; }
            done++;
            if (sys_.eventDirty) break;
        }
        sys_.eventDirty = false;
        sys_.advance(done);
    }
    return true;
}

const std::vector<uint32_t>& Emulator::render() {
    renderScreens(sys_, frame_);
    return frame_;
}

size_t Emulator::takeAudio(int16_t* stereo, size_t maxFrames) { return sys_.spu.takeSamples(stereo, maxFrames); }

void Emulator::saveFirmware() {
    if (!sys_.firmwareDirty) return;
    std::error_code ec;
    std::filesystem::create_directories(dataDir_, ec);
    std::ofstream f(firmwarePath_, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(reinterpret_cast<const char*>(sys_.firmware.data()), std::streamsize(sys_.firmware.size()));
    if (f) sys_.firmwareDirty = false;
}

bool Emulator::saveIfDirty() {
    if (!loaded()) return false;
    saveFirmware();
    if (!sys_.backupDirty) return false;
    uint32_t size = sys_.saveFileSize();
    if (size == 0) return false;
    if (size > sys_.backup.size()) size = uint32_t(sys_.backup.size());
    std::vector<uint8_t> data(sys_.backup.begin(), sys_.backup.begin() + size);
    if (data == lastSaved_) return false;
    std::ofstream f(savePath_, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!f) return false;
    lastSaved_ = data;
    return true;
}

std::filesystem::path Emulator::statePath(int slot) const {
    return dataDir_ / "etats" / std::filesystem::path(romPath_.stem()).concat(".etat" + std::to_string(slot));
}

namespace {
SaveStateTarget makeTarget(System& s, Bus9& b, ArmCpu& c9, ArmCpu& c7, const NdsHeader& h) {
    SaveStateTarget t;
    t.sys = &s; t.bus9 = &b; t.cpu9 = &c9; t.cpu7 = &c7;
    t.gameCode = h.gameCode; t.headerCrc = h.headerCrcStored;
    return t;
}
}

bool Emulator::saveState(int slot, std::string& error) {
    if (frame_.empty()) render();
    return saveStateFile(statePath(slot), makeTarget(sys_, bus9_, cpu9_, cpu7_, header_), frame_, error);
}

bool Emulator::loadState(int slot, std::string& error) {
    if (!loadStateFile(statePath(slot), makeTarget(sys_, bus9_, cpu9_, cpu7_, header_), error)) return false;
    render();
    return true;
}
