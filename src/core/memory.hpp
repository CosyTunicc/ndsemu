#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include "core/gx.hpp"
#include "core/sound.hpp"

// Statistiques des acces a des adresses non gerees (par mot de 4 octets).
struct UnmappedStat {
    uint64_t reads = 0, writes = 0;
    uint32_t lastValue = 0;     // derniere valeur ecrite
};

// Un acces a un registre materiel (pour le journal de diagnostic).
struct IoEvent { uint32_t addr = 0, value = 0; uint8_t size = 0; bool write = false; };

// Controleur d'interruptions d'un processeur : IME (interrupteur general),
// IE (interruptions autorisees), IF (interruptions en attente, on acquitte en ecrivant 1).
struct IrqCtl { uint32_t ime = 0, ie = 0, iflag = 0; };

// Une minuterie (4 par processeur).
struct Timer { uint16_t reload = 0, counter = 0, ctl = 0; uint32_t accum = 0; };

// Numeros de bits d'IE / IF
enum IrqBit : uint32_t {
    IRQ_VBLANK  = 1u << 0,
    IRQ_HBLANK  = 1u << 1,
    IRQ_VCOUNT  = 1u << 2,
    IRQ_TIMER0  = 1u << 3,     // les timers 1 a 3 sont les bits suivants
    IRQ_DMA0 = 1u << 8,            // les DMA 1 a 3 sont les bits suivants
    IRQ_IPCSYNC = 1u << 16,
    IRQ_IPCSENDEMPTY = 1u << 17,   // file d'envoi IPC devenue vide
    IRQ_IPCRECV = 1u << 18,        // file de reception IPC non vide
    IRQ_CARD = 1u << 19,           // transfert avec la cartouche termine
    IRQ_SPI = 1u << 23,            // transfert SPI termine (ARM7 seulement)
    IRQ_GXFIFO = 1u << 21,         // file de commandes 3D (ARM9 seulement) : moitie vide ou vide selon GXSTAT
};

// File IPC (FIFO) de 16 mots : un CPU y ecrit, l'autre y lit.
struct IpcFifo { uint32_t data[16] = {}; int head = 0, count = 0; };

class IBus;

// Un canal DMA (4 par processeur) : copie automatique de memoire.
struct DmaChan { uint32_t sad = 0, dad = 0, cnt = 0, curSad = 0, curDad = 0; };

// Etat de la console partage entre l'ARM9 et l'ARM7 : memoire, horloge, ecran, interruptions.
// Index de processeur : 0 = ARM9, 1 = ARM7.
struct System {
    std::vector<uint8_t> mainRam    = std::vector<uint8_t>(4 * 1024 * 1024, 0);  // 4 Mo
    std::vector<uint8_t> sharedWram = std::vector<uint8_t>(32 * 1024, 0);        // WRAM partagee 32 Ko
    std::vector<uint8_t> arm7Wram   = std::vector<uint8_t>(64 * 1024, 0);        // WRAM de l'ARM7 64 Ko
    uint8_t wramCnt = 3;                       // repartition de la WRAM partagee (0 a 3)

    // IPCSYNC : chaque CPU ecrit 4 bits que l'autre peut lire
    uint32_t ipcSyncOut9 = 0, ipcSyncOut7 = 0;
    bool ipcIrqEnable9 = false, ipcIrqEnable7 = false;

    // FIFO entre processeurs : fifo[c] est la file d'ENVOI du processeur c (reception de l'autre)
    IpcFifo fifo[2];
    Gx gx;                                     // moteur 3D (commandes, matrices, dessin des polygones)
    bool vblankReached = false;                // mis a vrai au debut de chaque V-Blank (fin d'image pour Emulator)
    uint8_t gxIrqMode = 0;                     // GXSTAT bits 30-31 : 0 = pas d'IRQ, 1 = file moins qu'a moitie pleine, 2 = file vide
    uint16_t fifoCnt[2] = {0, 0};              // bits 2, 10 et 15 reglables ; bit 14 = erreur
    uint32_t fifoLast[2] = {0, 0};             // dernier mot lu (renvoye quand la file est vide)

    // Lecteur de cartouche (slot NDS). La ROM est fournie par main ; les transferts sont instantanes.
    const std::vector<uint8_t>* cartRom = nullptr;
    uint16_t exmemcnt = 0;                     // EXMEMCNT : le bit 11 donne le processeur proprietaire du slot
    uint16_t auxSpiCnt = 0;                    // AUXSPICNT (le bit 14 autorise l'IRQ de fin de transfert)
    uint32_t romCtrl = 0;                      // ROMCTRL (0x040001A4)
    uint8_t cardCmd[8] = {};                   // octets de commande (0x040001A8 - 0x040001AF)
    std::vector<uint8_t> cardBuf;              // donnees de la reponse
    size_t cardPos = 0;
    bool cardBusy = false;
    uint32_t cardChipId = 0x00001FC2;          // identifiant de puce renvoye aux jeux

    // Memoire de sauvegarde : EEPROM serie (64 Ko, 2 octets d'adresse) sur le bus SPI de la cartouche.
    // Memoire de sauvegarde : jusqu'a 512 Ko (flash) ; le type de puce fixe la partie utilisee (voir memory.cpp)
    std::vector<uint8_t> backup = std::vector<uint8_t>(512 * 1024, 0xFF);
    int saveAddrBytes = 0;                     // octets d'adresse de la puce : 1, 2 ou 3 ; 0 = pas encore reconnu
    void setSaveType(int addrBytes);
    uint32_t saveFileSize() const;             // taille du fichier .sav a ecrire pour ce type de puce
    uint32_t backupMaxWritten = 0;             // plus haute adresse ecrite + 1
    bool backupDirty = false;                  // des donnees ont change : le fichier de sauvegarde est a reecrire
    std::vector<uint16_t> backupLog;           // premiers octets envoyes a la puce (bit 8 = fin de commande)
    uint8_t spiData = 0;                       // dernier octet recu de la puce (AUXSPIDATA)

    // ---- Bus SPI de l'ARM7 (SPICNT 0x040001C0, SPIDATA 0x040001C2) ----
    // Trois peripheriques : 0 = gestion de l'alimentation, 1 = memoire firmware, 2 = ecran tactile (TSC2046).
    // Les transferts sont instantanes (jamais "occupe").
    uint16_t spiCnt = 0;
    uint8_t spiOut = 0;                        // octet recu lors du dernier transfert
    std::vector<uint8_t> firmware;             // 256 Ko, fabrique au demarrage (pas de vrai firmware)
    static constexpr uint32_t kUserSettings = 0x3FF00;   // reglages utilisateur (copie la plus recente)
    // Ecran tactile, rempli par la plateforme : position en pixels de l'ecran du bas
    bool touchDown = false;
    uint16_t touchX = 0, touchY = 0;

    // ---- Son (ARM7, 0x04000400 - 0x0400051F) ----
    Spu spu;
    uint16_t powcnt2 = 1;                      // POWCNT2 (ARM7) : bit 0 = son allume (deja fait par le firmware au demarrage), bit 1 = wifi

    // ---- Horloge temps reel (RTC, registre 0x04000138 de l'ARM7) ----
    // L'ARM7 pilote la puce fil par fil : bit 0 = donnee, bit 1 = horloge, bit 2 = selection, bits 4-6 = sens.
    // La date et l'heure renvoyees sont celles du PC.
    uint16_t rtcIo = 0;

    // ---- Memoire video ----
    // Palettes (2 Ko : moteur A puis moteur B) et OAM (2 Ko : sprites des deux moteurs)
    std::vector<uint8_t> palette = std::vector<uint8_t>(2048, 0);
    std::vector<uint8_t> oam = std::vector<uint8_t>(2048, 0);
    // 9 banques de VRAM : A, B, C, D (128 Ko), E (64 Ko), F, G (16 Ko), H (32 Ko), I (16 Ko)
    std::vector<uint8_t> vram[9];
    uint8_t vramCnt[9] = {};                   // registres VRAMCNT_A a VRAMCNT_I
    // Ce que chaque processeur voit, par pages de 16 Ko. ARM9 : 0 fonds A, 1 fonds B, 2 sprites A,
    // 3 sprites B, 4 acces direct (LCDC). ARM7 : une seule zone de 256 Ko.
    uint8_t* vramMap9[5][41] = {};
    uint8_t* vramMap7[16] = {};
    uint32_t eng[2][28] = {};                  // registres des moteurs 2D A et B (0x00 a 0x6F)
    uint16_t powcnt1 = 0;                      // POWCNT1 (ARM9) : ecrans et moteurs allumes

    // ---- Diviseur et racine carree materiels de l'ARM9 (calcules instantanement) ----
    uint16_t divCnt = 0;
    int64_t divNumer = 0, divDenom = 0, divResult = 0, divRem = 0;
    uint16_t sqrtCnt = 0;
    uint64_t sqrtParam = 0;
    uint32_t sqrtResult = 0;

    // ---- DMA ----
    DmaChan dma[2][4];
    IBus* dmaBus[2] = {nullptr, nullptr};      // cartes memoire utilisees par le DMA de chaque processeur
    uint32_t dmaFill[4] = {0, 0, 0, 0};        // DMA0FILL..DMA3FILL (ARM9, 0x040000E0) : 16 octets libres pour le programme

    // ---- capture d'ecran (DISPCAPCNT, 0x04000064 = eng[0][25]) ----
    bool captureArmed = false;                 // la capture etait demandee au debut de l'image (ligne 0)
    uint64_t captureCount = 0;                 // images capturees (diagnostic)
    uint32_t lastCaptureCnt = 0;               // dernier DISPCAPCNT utilise (diagnostic)

    System();
    void rebuildVramMap();
    template <class Ar> void serialize(Ar& a);   // sauvegarde d'etat (savestate.cpp)

    uint32_t memGen = 0;                       // change a chaque modification de la carte memoire (caches des bus)

    IrqCtl irq[2];
    Timer timers[2][4];
    uint16_t dispstatCfg[2] = {0, 0};          // bits d'activation d'IRQ et ligne de comparaison, par CPU
    uint32_t vcount = 0;                       // ligne en cours (0 a 262)
    uint32_t lineCycle = 0;                    // cycle dans la ligne (2130 cycles par ligne)
    bool inHblank = false;
    uint64_t cycles = 0;                       // temps ecoule, en cycles de l'horloge de 33,5 MHz
    uint32_t rcnt7 = 0;
    uint16_t keyInput = 0x03FF;                // touches A,B,Select,Start,droite,gauche,haut,bas,R,L (0 = appuye)
    uint8_t keyXY = 0x4B;                      // touches X, Y... (0 = appuye) ; 0x4B = rien d'appuye, console ouverte
    bool haltRequest[2] = {false, false};      // demande d'attente d'interruption (HALTCNT)

    void advance(uint32_t n);                  // fait avancer le temps : ecran et minuteries
    void advanceSlow(uint32_t n);              // meme chose, un cycle a la fois (reference pour les tests)
    void gxVBlank();                           // debut du V-Blank : dessine l'image 3D si un echange de tampons est en attente
    void newLine();                            // passage a la ligne suivante (V-Blank, capture, interruptions)
    uint32_t cyclesToEvent() const;            // cycles avant le prochain evenement (ligne, debordement de minuterie), au moins 1
    bool eventDirty = false;                   // mis a 1 quand le programme change une minuterie : la boucle principale se resynchronise
    bool ioRead32(int cpu, uint32_t addr, uint32_t& value);                   // addr aligne sur 4
    bool ioWrite32(int cpu, uint32_t addr, uint32_t value, uint32_t mask);    // mask = octets ecrits

private:
    void tickTimer(int cpu, int i);
    void divCompute();
    void sqrtCompute();
    void dmaWrite(int cpu, int ch, int reg, uint32_t value, uint32_t mask);
    void dmaRun(int cpu, int ch);
    void dmaTrigger(int timing);               // 1 = V-Blank, 2 = H-Blank (ARM9)
    int spiCmd = -1;                           // commande SPI en cours (-1 : aucune)
    int spiAddrLeft = 0;
    uint32_t spiAddr = 0, spiA8 = 0;           // spiA8 : 9e bit d'adresse des petites EEPROM
    int spiLen = 0;                            // octets de la commande en cours (detection du type)
    bool spiDummy = false;                     // lecture rapide : octet vide a sauter
    bool spiWel = false;                       // ecriture autorisee (apres WREN)
    uint32_t saveMask = 0xFFFF;
    uint8_t backupTransfer(uint8_t byte, bool hold);
    // SPI de l'ARM7
    void buildFirmware();
    uint8_t spiTransfer(uint8_t byte);
    void spiRelease();                         // le peripherique est deselectionne : sa commande se termine
    uint8_t pmReg[8] = {0x0D, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00};   // registres de l'alimentation
    int pmIndex = -1;                          // registre selectionne (-1 : on attend l'octet d'index)
    int fwCmd = -1, fwAddrLeft = 0;            // commande firmware en cours
    uint32_t fwAddr = 0;
    bool fwDummy = false;                      // lecture rapide (0x0B) : un octet vide avant les donnees
    bool fwWel = false;                        // ecriture autorisee (commande 06)
public:
    bool firmwareDirty = false;                // le jeu a modifie le firmware (reglages Wi-Fi...)
private:
    uint16_t tscResult = 0;                    // derniere conversion du controleur tactile (12 bits)
    int tscPos = 0;                            // 1 = octet haut a envoyer, 2 = octet bas
    // RTC
    void rtcWrite(uint16_t v);
    void rtcByteIn(uint8_t b);
    uint8_t rtcCmd = 0, rtcIn = 0;
    int rtcInBit = 0, rtcInPos = 0, rtcOutBit = 0, rtcOutPos = 0;
    uint8_t rtcOut[8] = {};
    uint8_t rtcStatus1 = 0x02, rtcStatus2 = 0x00;      // statut 1 : bit 1 = mode 24 heures
    uint8_t rtcAlarm[2][3] = {}, rtcAdjust = 0, rtcFree = 0;
    void cardStart();
    void cardDmaKick();                        // DMA "cartouche" en attente : vide le transfert en cours
    uint32_t cardRomCtrlRead() const;
    uint32_t cardReadWord();
    uint32_t fifoCntRead(int cpu) const;
    void fifoCntWrite(int cpu, uint32_t v);
    void fifoSend(int cpu, uint32_t word);
    uint32_t fifoRecv(int cpu);
    void raise(int cpu, uint32_t bits) { irq[cpu].iflag |= bits; }
    uint32_t dispstatRead(int cpu) const;
    uint32_t ipcSyncRead(int cpu) const;
    int lyc(int cpu) const { return (dispstatCfg[cpu] >> 8) | ((dispstatCfg[cpu] & 0x80) << 1); }
};

// Bus memoire : ce que "voit" un processeur. L'ARM9 et l'ARM7 n'ont pas la meme carte memoire.
//
// Chemin rapide : un petit cache retient, pour les pages de 4 Ko deja rencontrees, l'adresse de la memoire
// correspondante (RAM, WRAM, TCM, VRAM...). Une page qui n'est pas de la memoire contigue (registres, palettes
// et OAM qui se repetent tous les 2 Ko, zones vides) est retenue comme "chemin lent". Le cache est vide des
// que la carte memoire change (System::memGen : VRAMCNT, WRAMCNT, TCM).
class IBus {
public:
    virtual ~IBus() = default;

    uint8_t read8(uint32_t a) {
        if (uint8_t* p = fastPtr(a)) return *p;
        return slowRead8(a);
    }
    uint16_t read16(uint32_t a) {
        a &= ~1u;                                                  // acces alignes
        if (uint8_t* p = fastPtr(a)) { uint16_t v; std::memcpy(&v, p, 2); return v; }
        return slowRead16(a);
    }
    uint32_t read32(uint32_t a) {
        a &= ~3u;
        if (uint8_t* p = fastPtr(a)) { uint32_t v; std::memcpy(&v, p, 4); return v; }
        return slowRead32(a);
    }
    void write8(uint32_t a, uint8_t v) {
        if (uint8_t* p = fastPtr(a)) { *p = v; return; }           // (palettes / OAM ne passent jamais par ici)
        slowWrite8(a, v);
    }
    void write16(uint32_t a, uint16_t v) {
        a &= ~1u;
        if (uint8_t* p = fastPtr(a)) { std::memcpy(p, &v, 2); return; }
        slowWrite16(a, v);
    }
    void write32(uint32_t a, uint32_t v) {
        a &= ~3u;
        if (uint8_t* p = fastPtr(a)) { std::memcpy(p, &v, 4); return; }
        slowWrite32(a, v);
    }

    // Copie un bloc dans la memoire a partir de addr (chargement des binaires).
    bool loadBlock(uint32_t addr, const uint8_t* data, size_t size);

    // Certaines zones ignorent les ecritures sur 8 bits (palettes et OAM de l'ARM9).
    virtual bool byteWriteIgnored(uint32_t /*addr*/) const { return false; }

    // Seul l'ARM9 a des TCM : par defaut, ne fait rien.
    virtual void configureTcm(uint32_t /*control*/, uint32_t /*dtcmReg*/, uint32_t /*itcmReg*/) {}

    // Interruptions vues par le processeur de ce bus
    virtual bool irqPending() const { return false; }     // IME actif et (IE & IF) non nul
    virtual bool wakePending() const { return false; }    // (IE & IF) non nul : reveille un processeur en attente
    virtual bool takeHaltRequest() { return false; }      // un HALTCNT a-t-il demande l'attente ?
    virtual void forceImeOn() {}
    // Pour le processeur, qui les consulte a chaque instruction sans appel virtuel :
    IrqCtl* irqCtl = nullptr;                             // controleur d'interruptions de ce processeur
    bool* haltFlag = nullptr;                             // demande d'attente (HALTCNT), remise a faux par le processeur

    std::map<uint32_t, UnmappedStat> unmapped;   // cle = adresse alignee sur 4 octets

    // Journal des 64 derniers acces a des registres materiels (diagnostic)
    static constexpr unsigned kIoLog = 64;
    IoEvent ioLog[kIoLog];
    uint64_t ioLogCount = 0;

protected:
    // Pointeur vers l'octet de memoire a cette adresse, ou nullptr si ce n'est pas de la RAM.
    virtual uint8_t* ptr(uint32_t addr) = 0;
    // Registres d'entree/sortie geres (retourne false si l'adresse n'est pas geree).
    virtual bool ioRead(uint32_t addr, int size, uint32_t& value) = 0;
    virtual bool ioWrite(uint32_t addr, int size, uint32_t value) = 0;

    const uint32_t* memGen = nullptr;                     // generation de la carte memoire (fournie par System)

private:
    // Table des pages de 4 Ko : une table de 4096 pages par zone de 16 Mo (octet haut de l'adresse), creee a la
    // premiere utilisation de la zone. Chaque case retient la generation de la carte memoire pour laquelle elle
    // a ete remplie : une case d'une ancienne generation est simplement recalculee.
    // host = memoire de la page, ou nullptr = chemin lent.
    struct PageEntry { uint32_t gen = 0xFFFFFFFFu; uint8_t* host = nullptr; };
    std::unique_ptr<PageEntry[]> regions[256];
    uint8_t* fastPtr(uint32_t a) {
        if (const PageEntry* r = regions[a >> 24].get()) {
            const PageEntry& e = r[(a >> 12) & 0xFFF];
            if (e.gen == *memGen) return e.host ? e.host + (a & 0xFFF) : nullptr;
        }
        return fillPage(a);
    }
    uint8_t* fillPage(uint32_t a);
    uint8_t  slowRead8(uint32_t a);
    uint16_t slowRead16(uint32_t a);
    uint32_t slowRead32(uint32_t a);
    void slowWrite8(uint32_t a, uint8_t v);
    void slowWrite16(uint32_t a, uint16_t v);
    void slowWrite32(uint32_t a, uint32_t v);
    void noteUnmapped(uint32_t addr, bool write, uint32_t value);
    void logIo(uint32_t addr, int size, uint32_t value, bool write) {
        IoEvent& e = ioLog[ioLogCount++ % kIoLog];
        e.addr = addr; e.value = value; e.size = uint8_t(size); e.write = write;
    }
};

// Carte memoire de l'ARM9 : TCM, RAM principale, WRAM partagee, vecteurs du "BIOS".
class Bus9 : public IBus {
public:
    explicit Bus9(System& s);
    template <class Ar> void serialize(Ar& a);   // sauvegarde d'etat (savestate.cpp)
    void configureTcm(uint32_t control, uint32_t dtcmReg, uint32_t itcmReg) override;
    bool byteWriteIgnored(uint32_t a) const override { return (a >> 24) == 0x05 || (a >> 24) == 0x07; }
    bool irqPending() const override { const IrqCtl& c = sys.irq[0]; return (c.ime & 1) && (c.ie & c.iflag); }
    bool wakePending() const override { return (sys.irq[0].ie & sys.irq[0].iflag) != 0; }
    bool takeHaltRequest() override { bool h = sys.haltRequest[0]; sys.haltRequest[0] = false; return h; }
    void forceImeOn() override { sys.irq[0].ime = 1; }

protected:
    uint8_t* ptr(uint32_t addr) override;
    bool ioRead(uint32_t addr, int size, uint32_t& value) override;
    bool ioWrite(uint32_t addr, int size, uint32_t value) override;

private:
    System& sys;
    std::vector<uint8_t> itcm = std::vector<uint8_t>(32 * 1024, 0);
    std::vector<uint8_t> dtcm = std::vector<uint8_t>(16 * 1024, 0);
    std::vector<uint8_t> bios = std::vector<uint8_t>(4 * 1024, 0);   // 0xFFFF0000 : vecteurs emules
    bool itcmOn = false, dtcmOn = false;
    uint64_t itcmRegion = 0;                     // taille de la zone vue par l'ITCM (base = 0)
    uint64_t dtcmBase = 0, dtcmRegion = 0;       // base et taille de la zone vue par la DTCM
};

// Carte memoire de l'ARM7 : RAM principale, WRAM partagee, WRAM de l'ARM7, vecteurs du "BIOS".
class Bus7 : public IBus {
public:
    explicit Bus7(System& s);
    bool irqPending() const override { const IrqCtl& c = sys.irq[1]; return (c.ime & 1) && (c.ie & c.iflag); }
    bool wakePending() const override { return (sys.irq[1].ie & sys.irq[1].iflag) != 0; }
    bool takeHaltRequest() override { bool h = sys.haltRequest[1]; sys.haltRequest[1] = false; return h; }
    void forceImeOn() override { sys.irq[1].ime = 1; }

protected:
    uint8_t* ptr(uint32_t addr) override;
    bool ioRead(uint32_t addr, int size, uint32_t& value) override;
    bool ioWrite(uint32_t addr, int size, uint32_t value) override;

private:
    System& sys;
    std::vector<uint8_t> bios = std::vector<uint8_t>(16 * 1024, 0);  // 0x00000000 : vecteurs emules
};
