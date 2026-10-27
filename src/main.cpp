#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <string>
#include <vector>
#include "core/log.hpp"
#include "core/memory.hpp"
#include "core/arm.hpp"
#include "core/nds_header.hpp"
#include "core/rom.hpp"
#include "core/savestate.hpp"
#include "core/emulator.hpp"
#include "core/video.hpp"
#ifdef _WIN32
#include "platform/window_win32.hpp"
#include "platform/audio_win32.hpp"
#include "ui/library.hpp"
#include <shellapi.h>
int runLibrary();                 // ui/library_win32.cpp : bibliotheque de jeux (lancee sans argument)
#endif

// Nom d'un registre materiel / d'une zone, pour rendre les statistiques lisibles.
static const char* ioName(uint32_t a) {
    switch (a) {
    case 0x04000000: return "DISPCNT";
    case 0x04000004: return "DISPSTAT/VCOUNT";
    case 0x04000180: return "IPCSYNC";
    case 0x04000184: return "IPCFIFOCNT";
    case 0x04000188: return "IPCFIFOSEND";
    case 0x04000204: return "EXMEMCNT";
    case 0x04000208: return "IME";
    case 0x04000210: return "IE";
    case 0x04000214: return "IF";
    case 0x04000240: return "VRAMCNT";
    case 0x04000304: return "POWCNT1";
    case 0x04100000: return "IPCFIFORECV";
    default: break;
    }
    switch (a >> 24) {
    case 4: return "I/O";
    case 5: return "palette";
    case 6: return "VRAM";
    case 7: return "OAM";
    default: return "";
    }
}

static const char* regionName(unsigned top) {
    switch (top) {
    case 0x00: return "ITCM/BIOS";
    case 0x01: return "ITCM";
    case 0x03: return "WRAM";
    case 0x04: return "I/O";
    case 0x05: return "palette";
    case 0x06: return "VRAM";
    case 0x07: return "OAM";
    case 0x08: case 0x09: return "slot GBA";
    case 0xFF: return "BIOS";
    default: return "?";
    }
}

// Journal des derniers acces aux registres materiels geres (ordre chronologique).
static void printIoLog(const char* who, const IBus& bus) {
    std::printf("\n--- Journal des acces registres %s (les plus recents en bas) ---\n", who);
    uint64_t n = bus.ioLogCount < IBus::kIoLog ? bus.ioLogCount : IBus::kIoLog;
    for (uint64_t i = bus.ioLogCount - n; i < bus.ioLogCount; i++) {
        const IoEvent& e = bus.ioLog[i % IBus::kIoLog];
        std::printf("%s%d %08X %s %08X\n", e.write ? "W" : "R", e.size * 8, e.addr, e.write ? "<-" : "->", e.value);
    }
}

// Resume des acces non geres d'un processeur.
static void printUnmapped(const char* who, const IBus& bus) {
    struct Region { size_t addrs = 0; uint64_t reads = 0, writes = 0; };
    std::map<unsigned, Region> regions;
    for (const auto& kv : bus.unmapped) {
        Region& r = regions[kv.first >> 24];
        r.addrs++;
        r.reads += kv.second.reads;
        r.writes += kv.second.writes;
    }
    std::printf("\n--- %s : acces non geres, par zone ---\n", who);
    if (regions.empty()) std::printf("(aucun)\n");
    for (const auto& kv : regions)
        std::printf("0x%02X......  %-10s adresses=%-6zu lectures=%-10llu ecritures=%llu\n",
            kv.first, regionName(kv.first), kv.second.addrs,
            (unsigned long long)kv.second.reads, (unsigned long long)kv.second.writes);

    // Les 20 adresses non gerees les plus sollicitees (celles qu'un processeur interroge en boucle)
    std::vector<std::pair<uint64_t, uint32_t>> top;
    for (const auto& kv : bus.unmapped) top.push_back({ kv.second.reads + kv.second.writes, kv.first });
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::printf("--- %s : les 20 adresses non gerees les plus sollicitees ---\n", who);
    for (size_t i = 0; i < top.size() && i < 20; i++) {
        const UnmappedStat& st = bus.unmapped.at(top[i].second);
        std::printf("%08X  lectures=%-9llu ecritures=%-9llu derniere ecriture=%08X  %s\n", top[i].second,
            (unsigned long long)st.reads, (unsigned long long)st.writes, st.lastValue, ioName(top[i].second));
    }
}

// Etat des moteurs 2D, de la capture et du moteur 3D (rapport final et instantane F12).
static void printVideoState(std::FILE* f, const System& sys) {
    std::fprintf(f, "\n--- Moteurs 2D (diagnostic du rendu) ---\n");
    std::fprintf(f, "POWCNT1=%04X VRAMCNT A..I =", sys.powcnt1);
    for (int i = 0; i < 9; i++) std::fprintf(f, " %02X", sys.vramCnt[i]);
    std::fprintf(f, "\n");
    for (int e = 0; e < 2; e++) {
        const uint32_t* r = sys.eng[e];
        std::fprintf(f, "Moteur %c : DISPCNT=%08X  BG0CNT=%04X BG1CNT=%04X BG2CNT=%04X BG3CNT=%04X\n",
            e == 0 ? 'A' : 'B', r[0], r[2] & 0xFFFF, r[2] >> 16, r[3] & 0xFFFF, r[3] >> 16);
        std::fprintf(f, "          BLDCNT=%04X BLDALPHA=%04X BLDY=%02X WINH=%08X WINV=%08X WININ/OUT=%08X LUMINOSITE=%08X\n",
            r[20] & 0xFFFF, r[20] >> 16, r[21] & 0xFF, r[16], r[17], r[18], r[27]);
        int active = 0, affine = 0, bitmapObj = 0;
        const uint8_t* oam = &sys.oam[e == 0 ? 0 : 0x400];
        for (int i = 0; i < 128; i++) {
            uint32_t a0 = oam[i * 8] | (oam[i * 8 + 1] << 8);
            if ((a0 & 0x300) == 0x200) continue;
            active++;
            if (a0 & 0x100) affine++;
            if (((a0 >> 10) & 3) == 3) bitmapObj++;
        }
        std::fprintf(f, "          defilement BG0..BG3 (H,V) = %03X,%03X %03X,%03X %03X,%03X %03X,%03X\n",
            r[4] & 0x1FF, (r[4] >> 16) & 0x1FF, r[5] & 0x1FF, (r[5] >> 16) & 0x1FF,
            r[6] & 0x1FF, (r[6] >> 16) & 0x1FF, r[7] & 0x1FF, (r[7] >> 16) & 0x1FF);
        std::fprintf(f, "          sprites actifs=%d dont a rotation/zoom=%d bitmap=%d\n", active, affine, bitmapObj);
    }
    std::fprintf(f, "Capture d'ecran : %llu image(s) capturee(s), dernier DISPCAPCNT=%08X (actuel %08X)\n",
        (unsigned long long)sys.captureCount, sys.lastCaptureCnt, sys.eng[0][25]);
    std::fprintf(f, "\n--- Moteur 3D ---\n");
    std::fprintf(f, "DISP3DCNT=%04X\n", sys.eng[0][24] & 0xFFFF);
    std::fprintf(f, "commandes=%llu echanges de tampons=%llu polygones dessines (derniere image)=%llu dont textures=%llu ombres (masques compris)=%llu commandes inconnues=%llu\n",
        (unsigned long long)sys.gx.commandCount, (unsigned long long)sys.gx.swapCount,
        (unsigned long long)sys.gx.polygonsDrawn, (unsigned long long)sys.gx.texturedPolygons,
        (unsigned long long)sys.gx.polygonsSkipped, (unsigned long long)sys.gx.unknownCommands);
    sys.gx.printStats(f);
}

int main(int argc, char** argv) {
    // Arguments : jeu.nds [instructions_ARM9_max] [images_entre_captures] [--fenetre]
    std::vector<char*> pos;
    std::vector<int> posIndex;    // position de chaque argument libre dans la ligne de commande
    bool windowMode = false;
    bool diagOn = false;          // --diag : surveillance du drapeau et journal FIFO (ralentit l'emulation)
    bool noCull = false;          // --sans-culling : dessine toutes les faces (pour comparer)
    bool cullFlip = false;        // --culling-inverse : inverse le sens "face avant" (si les modeles paraissent creux)
    bool reportOn = false;        // --rapport : affiche le rapport complet a la fermeture (mode fenetre) et ecrit poly_dump.txt
    bool forceHeadless = false;   // --sans-fenetre : emulation sans fenetre (tests, captures)
    bool noSound = false;         // --sans-son : pas de sortie audio
    bool recordSound = false;     // --enregistrer-son : ecrit le son produit dans son.wav (diagnostic)
    bool soundJournal = false;    // --journal-son : son_journal.txt (departs / arrets des canaux, messages ARM9 <-> ARM7)
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--fenetre") == 0) windowMode = true;
        else if (std::strcmp(argv[i], "--diag") == 0) diagOn = true;
        else if (std::strcmp(argv[i], "--sans-culling") == 0) noCull = true;
        else if (std::strcmp(argv[i], "--culling-inverse") == 0) cullFlip = true;
        else if (std::strcmp(argv[i], "--rapport") == 0) reportOn = true;
        else if (std::strcmp(argv[i], "--sans-fenetre") == 0) forceHeadless = true;
        else if (std::strcmp(argv[i], "--sans-son") == 0) noSound = true;
        else if (std::strcmp(argv[i], "--enregistrer-son") == 0) recordSound = true;
        else if (std::strcmp(argv[i], "--journal-son") == 0) soundJournal = true;
        else { pos.push_back(argv[i]); posIndex.push_back(i); }
    }
    // Utilisation grand public : un seul argument (une ROM glissee sur l'exe) -> on ouvre la fenetre et on joue.
    // L'attente d'une touche evite que la console se ferme avant qu'on ait pu lire un message.
    const bool friendly = argc <= 2;
    auto waitEnter = [&]() {
        if (!friendly) return;
        std::fprintf(stderr, "\nAppuyez sur Entree pour fermer.\n");
        std::getchar();
        };
#ifdef _WIN32
    if (!forceHeadless && pos.size() == 1) windowMode = true;
#else
    (void)forceHeadless; (void)noSound;                    // fenetre et son : Windows seulement pour l'instant
#endif
#ifdef _WIN32
    if (pos.empty() && !forceHeadless) return runLibrary();   // sans jeu : la bibliotheque
#endif
    if (pos.empty()) {
        std::fprintf(stderr, "ndsemu : glissez un fichier de jeu (.nds) sur ndsemu.exe pour lancer le jeu.\n"
            "Ligne de commande : %s jeu.nds [instructions_ARM9_max] [images_entre_captures] [--fenetre] [--sans-fenetre]\n"
            "                    [--sans-son] [--enregistrer-son] [--journal-son] [--rapport] [--diag]\n"
            "                    [--sans-culling] [--culling-inverse]\n", argv[0]);
        waitEnter();
        return 1;
    }

    // Chemin de la ROM. Sous Windows, argv est dans la page de code locale : un nom avec des caracteres hors de
    // cette page (japonais, emoji...) y est abime, on relit donc la ligne de commande en UTF-16.
    std::filesystem::path romPath(pos[0]);
#ifdef _WIN32
    {
        int wargc = 0;
        if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
            if (posIndex[0] < wargc) romPath = std::filesystem::path(wargv[posIndex[0]]);
            LocalFree(wargv);
        }
    }
#endif
    std::vector<uint8_t> rom;
    if (!loadFile(romPath, rom)) {
        LOG("impossible de lire %s", pos[0]);
        waitEnter();
        return 1;
    }

    NdsHeader h;
    std::string err;
    if (!parseNdsHeader(rom, h, err)) {
        LOG("ROM invalide : %s", err.c_str());
        waitEnter();
        return 1;
    }

    std::printf("Titre      : %s\n", h.title.c_str());
    std::printf("Code jeu   : %s\n", h.gameCode.c_str());
    std::printf("CRC header : %s\n", h.crcOk() ? "OK" : "INVALIDE");
    std::printf("ARM9       : rom 0x%08X -> ram 0x%08X, taille 0x%X, entree 0x%08X\n",
        h.arm9RomOffset, h.arm9RamAddr, h.arm9Size, h.arm9Entry);
    std::printf("ARM7       : rom 0x%08X -> ram 0x%08X, taille 0x%X, entree 0x%08X\n",
        h.arm7RomOffset, h.arm7RamAddr, h.arm7Size, h.arm7Entry);

    // La console : etat partage, une carte memoire par processeur, les deux processeurs. Ils sont crees une fois ;
    // "Reinitialiser" les detruit et les reconstruit sur place (les references restent valables), puis relance boot().
    System sys;
    Bus9 bus9(sys);
    Bus7 bus7(sys);
    ArmCpu cpu9(bus9, true, "ARM9");
    ArmCpu cpu7(bus7, false, "ARM7");

    // Fichier de sauvegarde : meme nom que la ROM, extension .sav
    const std::filesystem::path savePath = std::filesystem::path(romPath).replace_extension(".sav");
    // Firmware : celui que le jeu a modifie lors d'une partie precedente (firmware.bin a cote de l'exe), sinon le
    // firmware fabrique par l'emulateur
    const std::filesystem::path firmwarePath = std::filesystem::path(argv[0]).parent_path() / "firmware.bin";

    // Ecrit la sauvegarde si le jeu l'a modifiee depuis la derniere ecriture (renvoie vrai si un fichier a ete ecrit).
    std::vector<uint8_t> lastSaved;
    uint32_t lastSavedSize = 0;
    auto saveNow = [&]() -> bool {
        if (!sys.backupDirty) return false;
        uint32_t size = sys.saveFileSize();
        if (size == 0) return false;
        if (size > sys.backup.size()) size = uint32_t(sys.backup.size());
        std::vector<uint8_t> data(sys.backup.begin(), sys.backup.begin() + size);
        if (data == lastSaved) return false;
        std::ofstream f(savePath, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!f) return false;
        lastSaved = data;
        lastSavedSize = size;
        return true;
        };
    auto saveFirmware = [&]() {                                     // le jeu a ecrit dans le firmware : on le garde
        if (!sys.firmwareDirty) return;
        if (FILE* ff = std::fopen(firmwarePath.string().c_str(), "wb")) {
            std::fwrite(sys.firmware.data(), 1, sys.firmware.size(), ff);
            std::fclose(ff);
            sys.firmwareDirty = false;
            std::printf("Firmware modifie par le jeu, enregistre dans %s\n", firmwarePath.string().c_str());
        }
        };
    std::FILE* soundJournalFile = nullptr;
    if (soundJournal) {
        soundJournalFile = std::fopen("son_journal.txt", "w");
        if (soundJournalFile) std::printf("Journal du son : son_journal.txt\n");
    }

    // Mise sous tension : ce que fait le bootloader de la DS (copie du jeu en memoire, reglages, registres)
    auto boot = [&]() -> bool {
        sys.gx.cullEnabled = !noCull;
        sys.gx.cullFlip = cullFlip;
        sys.cartRom = &rom;                           // la cartouche : la ROM chargee
        sys.dmaBus[0] = &bus9;                        // le DMA copie la memoire de chaque processeur
        sys.dmaBus[1] = &bus7;
        sys.spu.journal = soundJournalFile;
        {
            std::vector<uint8_t> saved;
            if (loadFile(savePath, saved) && !saved.empty()) {
                for (size_t i = 0; i < saved.size() && i < sys.backup.size(); i++) sys.backup[i] = saved[i];
                sys.backupMaxWritten = uint32_t(saved.size() < sys.backup.size() ? saved.size() : sys.backup.size());
                // le type de puce se deduit de la taille du fichier : 512 o, 8 / 64 Ko, 128 Ko et plus
                sys.setSaveType(saved.size() <= 512 ? 1 : (saved.size() <= 65536 ? 2 : 3));
                std::printf("Sauvegarde existante chargee : %s (%zu octets)\n", savePath.u8string().c_str(), saved.size());
            }
        }
        if (!bus9.loadBlock(h.arm9RamAddr, &rom[h.arm9RomOffset], h.arm9Size) ||
            !bus7.loadBlock(h.arm7RamAddr, &rom[h.arm7RomOffset], h.arm7Size)) {
            LOG("echec du chargement des binaires en RAM");
            return false;
        }
        {
            std::vector<uint8_t> fw;
            if (loadFile(firmwarePath.string(), fw) && fw.size() == sys.firmware.size()) {
                sys.firmware = fw;
                std::printf("Firmware charge : %s\n", firmwarePath.string().c_str());
            }
        }
        ndsSetupBootMemory(bus9, rom, sys.cardChipId, sys.firmware);   // en-tete, identifiant de cartouche, reglages utilisateur
        // Au lancement d'un jeu, le firmware laisse les banques de VRAM C et D a l'ARM7 (0x06000000 et 0x06020000) :
        // certains jeux (Mario Kart DS...) y copient du code de l'ARM7 des le demarrage.
        bus9.write8(0x04000242, 0x82);
        bus9.write8(0x04000243, 0x8A);
        cpu9.reset(h.arm9Entry);
        cpu7.reset(h.arm7Entry);
        // Adresses frequentes et traces apres interruption : seulement quand un rapport sera affiche (couteux)
        cpu9.diagnostics = cpu7.diagnostics = !windowMode || reportOn || diagOn;
        return true;
        };
    if (!boot()) return 1;
    // Reinitialiser : comme eteindre et rallumer la console (la sauvegarde est ecrite avant)
    auto resetConsole = [&]() -> bool {
        saveNow();
        saveFirmware();
        cpu7.~ArmCpu();
        cpu9.~ArmCpu();
        bus7.~Bus7();
        bus9.~Bus9();
        sys.~System();
        new (&sys) System();
        new (&bus9) Bus9(sys);
        new (&bus7) Bus7(sys);
        new (&cpu9) ArmCpu(bus9, true, "ARM9");
        new (&cpu7) ArmCpu(bus7, false, "ARM7");
        lastSaved.clear();
        std::printf("Console reinitialisee.\n");
        return boot();
        };
    (void)resetConsole;                               // utilise par la fenetre (Windows)

    // Nombre max d'instructions ARM9 : 2e argument (par defaut 50 millions)
    const uint64_t kMaxSteps = (pos.size() >= 2) ? std::strtoull(pos[1], nullptr, 10) : 50000000ull;

    // Captures d'ecran : 3e argument = une image tous les N tours d'ecran (0 = seulement a la fin)
    const uint64_t kSnapEvery = (pos.size() >= 3) ? std::strtoull(pos[2], nullptr, 10) : 0;
    if (kSnapEvery) std::filesystem::create_directories("images");
    uint64_t lastFrame = 0, snapCount = 0;

    std::printf("\n--- Execution (ARM9 : 2 instructions pour 1 de l'ARM7) ---\n");
    const auto tStart = std::chrono::steady_clock::now();
    ArmCpu* failed = nullptr;
    std::vector<std::string> fifoLog;                 // DIAGNOSTIC : derniers messages IPC entre les deux processeurs
    std::vector<int16_t> recorded;                    // --enregistrer-son : tout le son produit (stereo)

    if (windowMode) {
#ifdef _WIN32
        // Mode interactif : une fenetre, le clavier, et la vitesse d'une vraie console (59,8 images par seconde)
        Window win;
        std::string winTitle = h.title.empty() ? std::string("ndsemu") : h.title;   // titre de la banniere s'il existe
        {
            GameEntry ge;
            if (Library::readRomInfo(romPath.u8string(), ge) && !ge.title.empty()) winTitle = ge.title;
        }
        if (!win.create((winTitle + " \xE2\x80\x94 ndsemu").c_str())) {
            std::fprintf(stderr, "Impossible d'ouvrir la fenetre.\n");
            return 1;
        }
        std::printf("Fenetre ouverte. Touches : fleches, Z=A, X=B, S=X, A=Y, Q=L, W=R, Entree=Start, Retour arriere=Select.\n"
                    "Raccourcis : P pause, Ctrl+R reinitialiser, M son, Ctrl+1..4 taille, F11 plein ecran, F12 instantane.\n");
        // Sauvegardes d'etat : etats/<nom du jeu>.etat1 a .etat4, a cote du programme
        const std::filesystem::path statesDir = std::filesystem::path(argv[0]).parent_path() / "etats";
        auto stateFile = [&](int slot) {
            return statesDir / std::filesystem::path(romPath.stem()).concat(".etat" + std::to_string(slot));
            };
        auto stateTarget = [&]() {
            SaveStateTarget t;
            t.sys = &sys; t.bus9 = &bus9; t.cpu9 = &cpu9; t.cpu7 = &cpu7;
            t.gameCode = h.gameCode; t.headerCrc = h.headerCrcStored;
            return t;
            };
        for (int slot = 1; slot <= Window::kSlots; slot++) {
            SaveStateInfo info;
            readStateInfo(stateFile(slot), info);
            win.setSlotInfo(slot, info.exists, info.time, info.thumb);
        }
        // Temps de jeu (affiche dans la bibliotheque) : compte quand le jeu n'est pas en pause, ecrit chaque minute
        const std::string playFile = (std::filesystem::path(argv[0]).parent_path() / "ndsemu_temps_de_jeu.ini").u8string();
        const std::string playKey = Library::gameKey(h.gameCode, h.headerCrcStored);
        Library::addPlayTime(playFile, playKey, 0, int64_t(std::time(nullptr)));   // "joue a l'instant"
        double playPending = 0;                                   // secondes pas encore ecrites
        auto flushPlayTime = [&]() {
            const int64_t whole = int64_t(playPending);
            if (whole <= 0) return;
            if (Library::addPlayTime(playFile, playKey, whole, int64_t(std::time(nullptr)))) playPending -= double(whole);
            };
        AudioOut audio;
        if (noSound) std::printf("Son desactive (--sans-son).\n");
        else if (audio.open(Spu::kSampleRate)) std::printf("Son : %d Hz, stereo.\n", audio.rate());
        else std::printf("Son : aucune sortie audio disponible, le jeu continue sans son.\n");
        std::vector<int16_t> soundBuf(2048 * 2);
        std::vector<uint32_t> frame;
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::microseconds(16716);
        auto next = clock::now();
        auto lastTitle = next;
        // DIAGNOSTIC : surveille le mot attendu par la boucle de l'ARM9
        const uint32_t kWatch = 0x020A5454;
        const uint32_t* watchPtr = reinterpret_cast<const uint32_t*>(&sys.mainRam[kWatch & 0x3FFFFF]);
        uint32_t watchLast = *watchPtr;
        int watchLines = 0;
        auto checkWatch = [&](const ArmCpu& who) {
            if (*watchPtr != watchLast && watchLines < 100) {
                std::printf("WATCH %08X : %08X -> %08X par %s a pc=%08X (cycle %llu)\n", kWatch,
                    watchLast, *watchPtr, who.name.c_str(), who.pc, (unsigned long long)sys.cycles);
                watchLines++;
            }
            watchLast = *watchPtr;
            };
        // DIAGNOSTIC : journal des messages du FIFO IPC (envois et lectures), 80 derniers conserves
        int fifoPrev[2] = { sys.fifo[0].count, sys.fifo[1].count };
        uint32_t syncPrev9 = sys.ipcSyncOut9, syncPrev7 = sys.ipcSyncOut7;
        auto addLog = [&](const char* txt) {
            fifoLog.push_back(txt);
            if (fifoLog.size() > 80) fifoLog.erase(fifoLog.begin());
            };
        auto checkAll = [&](const ArmCpu& who) {
            checkWatch(who);
            char buf[128];
            for (int c = 0; c < 2; c++) {
                const IpcFifo& f = sys.fifo[c];
                if (f.count > fifoPrev[c]) {
                    std::snprintf(buf, sizeof buf, "cycle %llu : %s envoie %08X",
                        (unsigned long long)sys.cycles, c == 0 ? "ARM9->ARM7" : "ARM7->ARM9",
                        f.data[(f.head + f.count - 1) & 15]);
                    addLog(buf);
                }
                else if (f.count < fifoPrev[c]) {
                    std::snprintf(buf, sizeof buf, "cycle %llu : %s lit un mot (reste %d)",
                        (unsigned long long)sys.cycles, c == 0 ? "ARM7" : "ARM9", f.count);
                    addLog(buf);
                }
                fifoPrev[c] = f.count;
            }
            if (sys.ipcSyncOut9 != syncPrev9 || sys.ipcSyncOut7 != syncPrev7) {
                std::snprintf(buf, sizeof buf, "cycle %llu : IPCSYNC sortie ARM9=%X ARM7=%X",
                    (unsigned long long)sys.cycles, sys.ipcSyncOut9, sys.ipcSyncOut7);
                addLog(buf);
                syncPrev9 = sys.ipcSyncOut9; syncPrev7 = sys.ipcSyncOut7;
            }
            };
        int shown = 0;
        // PERFORMANCE : temps moyen par image (ms) passe dans les processeurs, le rendu, l'affichage et l'attente
        double tCpu = 0, tRender = 0, tPresent = 0, tWait = 0;
        int perfFrames = 0, perfLines = 0;
        auto msBetween = [](clock::time_point a, clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
            };
        while (win.pump() && !failed) {
            if (win.takeResetRequest()) {                              // Jeu > Reinitialiser (Ctrl+R)
                if (!resetConsole()) break;
                watchPtr = reinterpret_cast<const uint32_t*>(&sys.mainRam[kWatch & 0x3FFFFF]);
                watchLast = *watchPtr;
            }
            if (const int slot = win.takeSaveRequest()) {               // Maj+F1..F4 ou menu : sauvegarder l'etat
                std::string stateErr;
                if (frame.empty()) renderScreens(sys, frame);
                if (saveStateFile(stateFile(slot), stateTarget(), frame, stateErr)) {
                    win.setSlotInfo(slot, true, int64_t(std::time(nullptr)), frame);
                    win.notify("\xC3\x89tat " + std::to_string(slot) + " sauvegard\xC3\xA9");
                } else win.notify("Sauvegarde de l'\xC3\xA9tat " + std::to_string(slot) + " impossible : " + stateErr, true);
            }
            if (const int slot = win.takeLoadRequest()) {               // F1..F4 ou menu : charger l'etat
                std::string stateErr;
                if (loadStateFile(stateFile(slot), stateTarget(), stateErr)) {
                    renderScreens(sys, frame);
                    next = clock::now();
                    watchPtr = reinterpret_cast<const uint32_t*>(&sys.mainRam[kWatch & 0x3FFFFF]);
                    watchLast = *watchPtr;
                    win.notify("\xC3\x89tat " + std::to_string(slot) + " charg\xC3\xA9");
                } else win.notify("\xC3\x89tat " + std::to_string(slot) + " : " + stateErr, true);
            }
            if (win.paused()) {                                        // en pause : on garde l'image, sans emuler ni jouer de son
                win.present(frame.empty() ? nullptr : frame.data(), 256, 384);
                next = clock::now() + period;
                std::this_thread::sleep_for(period);
                continue;
            }
            {
                const uint16_t keys = win.keyInput();
                if (keys != sys.keyInput && sys.spu.journal) {          // journal du son : reperes de ce que fait le joueur
                    static const char* kName[10] = {"A", "B", "Select", "Start", "droite", "gauche", "haut", "bas", "R", "L"};
                    std::string down;
                    for (int b = 0; b < 10; b++) if (!(keys & (1u << b))) { down += ' '; down += kName[b]; }
                    sys.spu.note(sys.cycles, "---- touches :%s", down.empty() ? " (aucune)" : down.c_str());
                }
                sys.keyInput = keys;
            }
            sys.keyXY = win.keyXY();
            {                                                          // ecran tactile : clic gauche sur l'ecran du bas
                int tx = 0, ty = 0;
                sys.touchDown = win.touch(tx, ty);
                if (sys.touchDown) {
                    sys.touchX = uint16_t(tx);
                    sys.touchY = uint16_t(ty);
                    sys.keyXY = uint8_t(sys.keyXY & ~0x40u);           // KEYXY bit 6 = 0 : stylet pose
                }
            }
            const uint64_t target = sys.cycles + 560190;               // une image de la console
            const auto t0 = clock::now();
            while (sys.cycles < target && !failed) {
                const uint64_t left = target - sys.cycles;
                // Les deux processeurs attendent une interruption : rien ne bouge avant le prochain evenement
                if (cpu9.halted && cpu7.halted && !sys.haltRequest[0] && !sys.haltRequest[1] &&
                    !bus9.wakePending() && !bus7.wakePending()) {
                    uint32_t skip = sys.cyclesToEvent();
                    if (skip > left) skip = uint32_t(left);
                    sys.advance(skip);
                    continue;
                }
                // Par paquets : le temps (ecran, minuteries) n'avance qu'a la fin du paquet, qui s'arrete
                // exactement sur le prochain evenement, ou plus tot si le programme touche une minuterie.
                uint32_t chunk = sys.cyclesToEvent();
                if (chunk > 32) chunk = 32;
                if (chunk > left) chunk = uint32_t(left);
                uint32_t done = 0;
                while (done < chunk) {
                    if (!cpu9.step()) { failed = &cpu9; break; }
                    if (diagOn) checkAll(cpu9);
                    if (!cpu9.step()) { failed = &cpu9; break; }
                    if (diagOn) checkAll(cpu9);
                    if (!cpu7.step()) { failed = &cpu7; break; }
                    if (diagOn) checkAll(cpu7);
                    done++;
                    if (sys.eventDirty) break;
                }
                sys.eventDirty = false;
                if (failed) break;
                sys.advance(done);
            }
            const auto t1 = clock::now();
            // Son de cette image (environ 547 echantillons) vers la carte son ; sans sortie, on vide simplement
            const float vol = win.volume();
            for (size_t n; (n = sys.spu.takeSamples(soundBuf.data(), 2048)) > 0; ) {
                if (recordSound && recorded.size() < size_t(Spu::kSampleRate) * 2 * 300)   // 5 minutes au plus (avant le volume)
                    recorded.insert(recorded.end(), soundBuf.begin(), soundBuf.begin() + std::ptrdiff_t(n * 2));
                if (vol < 0.999f)                                                 // volume de la fenetre (menu Son)
                    for (size_t i = 0; i < n * 2; i++) soundBuf[i] = int16_t(float(soundBuf[i]) * vol);
                audio.push(soundBuf.data(), n);
            }
            renderScreens(sys, frame);
            if (win.takeSnapshotRequest()) {                              // F12 : instantane de diagnostic
                static int snapNo = 0;
                char base[32];
                std::snprintf(base, sizeof base, "etat_%03d", ++snapNo);
                const std::string b = base;
                if (std::FILE* sf = std::fopen((b + ".txt").c_str(), "w")) {
                    std::fprintf(sf, "Instantane %d, image emulee %llu\n", snapNo, (unsigned long long)(sys.cycles / 560190));
                    printVideoState(sf, sys);
                    std::fprintf(sf, "\n--- Polygones de la derniere image 3D ---\n");
                    sys.gx.dumpPolygons(sf);
                    std::fclose(sf);
                }
                writePng((b + ".png").c_str(), frame, 256, 384);
                std::vector<uint32_t> img3d(256 * 192);                   // 3D seule : magenta = transparent
                const uint32_t* g = sys.gx.frame();
                for (size_t i = 0; i < img3d.size(); i++) img3d[i] = (g[i] >> 24) ? (g[i] & 0xFFFFFF) : 0xFF00FFu;
                writePng((b + "_3d.png").c_str(), img3d, 256, 192);
                std::printf("Instantane ecrit : %s.txt, %s.png, %s_3d.png\n", base, base, base);
            }
            const auto t2 = clock::now();
            win.present(frame.data(), 256, 384);
            const auto t3 = clock::now();
            shown++;
            next += period;
            const auto now = clock::now();
            if (now < next) std::this_thread::sleep_until(next);
            else if (now - next > std::chrono::milliseconds(100)) next = now;   // trop en retard : on ne rattrape pas
            const auto t4 = clock::now();
            tCpu += msBetween(t0, t1); tRender += msBetween(t1, t2);
            tPresent += msBetween(t2, t3); tWait += msBetween(t3, t4);
            perfFrames++;
            playPending += 0.016716;                                   // une image jouee (pas en pause)
            if (playPending >= 60.0) flushPlayTime();
            if (now - lastTitle > std::chrono::seconds(1)) {
                saveNow();                                                // sauvegarde automatique (si le jeu a ecrit quelque chose)
                const double n = perfFrames ? perfFrames : 1;
                const int audioMs = audio.isOpen() ? audio.latencyMs() : 0;
                char status[96], perf[256];
                std::snprintf(status, sizeof status, "%d images/s", shown);
                if (audio.isOpen())
                    std::snprintf(perf, sizeof perf, "%d images/s\nprocesseurs %.1f ms\nrendu       %.1f ms\naffichage   %.1f ms\nson : retard %d ms\n      pertes %u, coupures %u",
                        shown, tCpu / n, tRender / n, tPresent / n, audioMs, audio.dropped, audio.underruns);
                else
                    std::snprintf(perf, sizeof perf, "%d images/s\nprocesseurs %.1f ms\nrendu       %.1f ms\naffichage   %.1f ms\nson : aucun",
                        shown, tCpu / n, tRender / n, tPresent / n);
                if ((reportOn || diagOn) && audio.isOpen() && perfLines < 120)
                    std::printf("SON retard %d ms, pertes %u, coupures %u\n", audioMs, audio.dropped, audio.underruns);
                win.setStatus(status);
                win.setPerf(perf);
                if ((reportOn || diagOn) && perfLines++ < 120)
                    std::printf("PERF %d images/s : cpu %.2f ms, rendu %.2f ms, affichage %.2f ms, attente %.2f ms (budget 16.72 ms) | 3D : %llu polygones (%llu textures), %llu echanges\n",
                        shown, tCpu / n, tRender / n, tPresent / n, tWait / n,
                        (unsigned long long)sys.gx.polygonsDrawn, (unsigned long long)sys.gx.texturedPolygons,
                        (unsigned long long)sys.gx.swapCount);
                tCpu = tRender = tPresent = tWait = 0; perfFrames = 0;
                shown = 0;
                lastTitle = now;
            }
        }
        flushPlayTime();                                               // le reste du temps de jeu
#else
        std::fprintf(stderr, "Le mode fenetre n'existe que sous Windows pour l'instant.\n");
#endif
    }

    uint64_t steps = 0;
    while (!windowMode && steps < kMaxSteps && !failed) {
        // Paquets de cycles (voir plus haut) : 1 cycle de l'horloge de 33,5 MHz par instruction ARM7 (approximation)
        uint32_t chunk = sys.cyclesToEvent();
        if (chunk > 32) chunk = 32;
        const uint64_t itersLeft = (kMaxSteps - steps + 1) / 2;
        if (chunk > itersLeft) chunk = uint32_t(itersLeft);
        uint32_t done = 0;
        while (done < chunk) {
            if (!cpu9.step()) { failed = &cpu9; break; }
            if (!cpu9.step()) { failed = &cpu9; break; }
            if (!cpu7.step()) { failed = &cpu7; break; }
            done++;
            steps += 2;
            if (sys.eventDirty) break;
        }
        sys.eventDirty = false;
        if (failed) break;
        sys.advance(done);
        if (kSnapEvery) {                            // nouvelle image ecoulee ?
            uint64_t frame = sys.cycles / 560190;
            if (frame != lastFrame) {
                lastFrame = frame;
                if (frame % kSnapEvery == 0) {
                    std::vector<uint32_t> img;
                    renderScreens(sys, img);
                    char name[64];
                    std::snprintf(name, sizeof name, "images/ecran_%05llu.png", (unsigned long long)frame);
                    if (writePng(name, img, 256, 384)) snapCount++;
                }
            }
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();

    const bool saved = saveNow();                                   // la sauvegarde est toujours ecrite en premier
    if (sys.spu.journal) { std::fclose(sys.spu.journal); sys.spu.journal = nullptr; }
    saveFirmware();
    if (recordSound) {                                              // son.wav : 16 bits stereo
        if (FILE* wf = std::fopen("son.wav", "wb")) {
            auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, wf); };
            auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, wf); };
            const uint32_t bytes = uint32_t(recorded.size() * 2);
            std::fwrite("RIFF", 1, 4, wf); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, wf);
            w32(16); w16(1); w16(2); w32(Spu::kSampleRate); w32(Spu::kSampleRate * 4); w16(4); w16(16);
            std::fwrite("data", 1, 4, wf); w32(bytes);
            if (!recorded.empty()) std::fwrite(recorded.data(), 2, recorded.size(), wf);
            std::fclose(wf);
            std::printf("Son enregistre dans son.wav (%.1f s)\n", double(recorded.size()) / 2 / Spu::kSampleRate);
        }
    }
    bool fullReport = !windowMode || reportOn || diagOn;
    const bool friendError = failed && !fullReport;
    std::string errFile;
    if (friendError) {                                              // erreur chez un testeur : rapport complet dans un fichier
        const std::filesystem::path errPath = std::filesystem::path(argv[0]).parent_path() / "ndsemu_erreur.txt";   // a cote de l'exe
        if (std::freopen(errPath.string().c_str(), "w", stdout)) { fullReport = true; errFile = errPath.string(); }
    }
    if (fullReport) {
        if (saved) std::printf("Sauvegarde ecrite : %s (%u octets)\n", savePath.u8string().c_str(), lastSavedSize);
        if (failed) std::printf("\nARRET sur %s : %s\n", failed->name.c_str(), failed->error.c_str());
        else        std::printf("\nLimite d'instructions atteinte (pas d'erreur).\n");

        cpu9.dump();
        cpu7.dump();
        cpu9.printHistory();
        cpu7.printHistory();
        cpu9.printHotSpots();
        cpu7.printHotSpots();
        printIoLog("ARM9", bus9);
        printIoLog("ARM7", bus7);
        {
            std::vector<uint32_t> rgb;
            renderScreens(sys, rgb);
            if (writePng("ecran.png", rgb, 256, 384)) std::printf("\nImage des deux ecrans ecrite dans ecran.png\n");
        }
        // Sauvegarde : ecrite seulement si le jeu a change quelque chose
        static const char* kSaveType[4] = {"pas encore reconnue", "petite EEPROM (512 o)", "EEPROM (8 / 64 Ko)", "flash / grande EEPROM (128 Ko et plus)"};
        std::printf("\nSauvegarde : puce %s, %d octet(s) d'adresse ; %u octet(s) ecrits au plus haut ; commandes SPI (/ = fin de commande) :\n",
            kSaveType[sys.saveAddrBytes & 3], sys.saveAddrBytes, sys.backupMaxWritten);
        for (size_t i = 0; i < sys.backupLog.size() && i < 160; i++) {
            std::printf("%02X%s", sys.backupLog[i] & 0xFF, (sys.backupLog[i] & 0x100) ? " / " : " ");
        }
        std::printf("\n");
        if (cpu7.writeIrqTrace("trace_arm7.txt")) std::printf("\nTrace ARM7 ecrite dans trace_arm7.txt\n");
        if (cpu9.writeIrqTrace("trace_arm9.txt")) std::printf("Trace ARM9 ecrite dans trace_arm9.txt\n");
        std::printf("\nIPCSYNC : sortie ARM9=%X, sortie ARM7=%X, WRAMCNT=%u\n",
            sys.ipcSyncOut9, sys.ipcSyncOut7, sys.wramCnt);
        std::printf("Temps emule : %llu cycles (%.2f images), ligne %u\n",
            (unsigned long long)sys.cycles, double(sys.cycles) / 560190.0, sys.vcount);
        std::printf("Duree reelle : %.1f s (%.2f images emulees par seconde ; une vraie DS en fait 59,8)\n",
            seconds, seconds > 0 ? double(sys.cycles) / 560190.0 / seconds : 0.0);
        if (snapCount) std::printf("%llu captures ecrites dans le dossier images\n", (unsigned long long)snapCount);
        std::printf("Interruptions ARM9 : IME=%u IE=%08X IF=%08X | ARM7 : IME=%u IE=%08X IF=%08X\n",
            sys.irq[0].ime, sys.irq[0].ie, sys.irq[0].iflag,
            sys.irq[1].ime, sys.irq[1].ie, sys.irq[1].iflag);
        std::printf("FIFO IPC : %d mot(s) en attente ARM9->ARM7, %d mot(s) ARM7->ARM9 ; IPCFIFOCNT ARM9=%04X ARM7=%04X\n",
            sys.fifo[0].count, sys.fifo[1].count, sys.fifoCnt[0], sys.fifoCnt[1]);
        {
            uint32_t d9 = cpu9.dtcmRegDebug() & 0xFFFFF000u;
            std::printf("Gestionnaires d'IRQ du jeu : ARM9 = %08X (drapeaux BIOS %08X), ARM7 = %08X (drapeaux BIOS %08X)\n",
                bus9.read32(d9 + 0x3FFC), bus9.read32(d9 + 0x3FF8),
                bus7.read32(0x0380FFFC), bus7.read32(0x0380FFF8));
        }
        printUnmapped("ARM9", bus9);
        printUnmapped("ARM7", bus7);
        // DIAGNOSTIC : messages IPC recents, code de la boucle bloquante et donnees voisines du drapeau
        std::printf("\n--- Derniers evenements FIFO / IPCSYNC (les plus recents en bas) ---\n");
        for (const std::string& line : fifoLog) std::printf("%s\n", line.c_str());
        {
            auto dumpRange = [&](const char* title, uint32_t from, uint32_t to) {
                std::printf("\n--- %s ---\n", title);
                for (uint32_t a = from; a <= to; a += 4) std::printf("%08X: %08X\n", a, bus9.read32(a));
                };
            dumpRange("Code ARM9 : fonction de la boucle (01FF90C0..01FF9170)", 0x01FF90C0, 0x01FF9170);
            dumpRange("Code ARM9 : fonction rappelee (01FF8700..01FF8760)", 0x01FF8700, 0x01FF8760);
            dumpRange("Code ARM9 : fonction appelee (01FFA240..01FFA3C0)", 0x01FFA240, 0x01FFA3C0);
            dumpRange("Variables (02074CC0..02074CF0)", 0x02074CC0, 0x02074CF0);
            dumpRange("Donnees autour du drapeau (020A5440..020A546C)", 0x020A5440, 0x020A546C);
            printVideoState(stdout, sys);
            if (std::FILE* pf = std::fopen("poly_dump.txt", "w")) {          // liste des polygones de la derniere image 3D
                sys.gx.dumpPolygons(pf);
                std::fclose(pf);
            }
            {
                const Spu& sp = sys.spu;
                int active = 0;
                for (const Spu::Channel& c : sp.ch) if (c.cnt & 0x80000000u) active++;
                std::printf("\n--- Son ---\n");
                std::printf("SOUNDCNT=%04X (bit 15 = actif, volume %u) POWCNT2=%04X SOUNDBIAS=%03X | canaux en cours=%d, departs=%llu\n",
                    sp.soundCnt, sp.soundCnt & 0x7F, sys.powcnt2, sp.soundBias, active, (unsigned long long)sp.channelStarts);
                std::printf("echantillons produits=%llu dont non nuls=%llu, coupes par SOUNDCNT/POWCNT2=%llu, plus forte valeur=%d (sur 32767)\n",
                    (unsigned long long)sp.framesMixed, (unsigned long long)sp.framesAudible,
                    (unsigned long long)sp.framesMuted, sp.peak);
                for (int i = 0; i < 16; i++) {
                    const Spu::Channel& c = sp.ch[i];
                    if (!c.cnt && !c.sad) continue;
                    std::printf("  canal %2d : CNT=%08X SAD=%08X TMR=%04X PNT=%04X LEN=%06X\n", i, c.cnt, c.sad, c.tmr, c.pnt, c.len);
                }
            }
            std::printf("\n--- Etat du lecteur de cartouche ---\n");
            std::printf("ROMCTRL=%08X AUXSPICNT=%04X EXMEMCNT=%04X occupe=%d\n",
                sys.romCtrl, sys.auxSpiCnt, sys.exmemcnt, sys.cardBusy ? 1 : 0);
        }
    }
    if (friendError) {
        std::fflush(stdout);
        std::fprintf(stderr, "\nL'emulateur s'est arrete sur une erreur : %s\n"
            "Un rapport a ete ecrit dans le fichier %s\n(a envoyer a la personne qui a fait l'emulateur).\n",
            failed->error.c_str(), errFile.empty() ? "(ecriture impossible)" : errFile.c_str());
        waitEnter();
        return 2;                                                   // la bibliotheque le signale
    }
    return 0;
}