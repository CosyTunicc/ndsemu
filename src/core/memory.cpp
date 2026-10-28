#include "core/memory.hpp"
#include "core/video.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

// ---------------------------------------------------------------- IBus (commun)

void IBus::noteUnmapped(uint32_t addr, bool write, uint32_t value) {
    UnmappedStat& s = unmapped[addr & ~3u];
    if (write) { s.writes++; s.lastValue = value; }
    else       { s.reads++; }
}

// Cache des pages (voir memory.hpp) ----------------------------------------

// Une page de 4 Ko passe par le chemin rapide seulement si c'est de la memoire d'un seul tenant (le premier et
// le dernier octet se suivent) et si les ecritures 8 bits y sont normales. Sinon elle est notee "chemin lent".
uint8_t* IBus::fillPage(uint32_t a) {
    std::unique_ptr<PageEntry[]>& r = regions[a >> 24];
    if (!r) r.reset(new PageEntry[4096]);
    PageEntry& e = r[(a >> 12) & 0xFFF];
    const uint32_t base = a & ~0xFFFu;
    uint8_t* p0 = ptr(base);
    const bool contiguous = p0 && ptr(base + 0xFFF) == p0 + 0xFFF && !byteWriteIgnored(base);
    e.gen = *memGen;
    e.host = contiguous ? p0 : nullptr;
    return e.host ? e.host + (a & 0xFFF) : nullptr;
}

// Chemins lents : registres, palettes / OAM, zones vides ---------------------

uint8_t IBus::slowRead8(uint32_t a) {
    if (uint8_t* p = ptr(a)) return *p;
    uint32_t v;
    if (ioRead(a, 1, v)) { logIo(a, 1, v, false); return uint8_t(v); }
    noteUnmapped(a, false, 0);
    return 0;
}

uint16_t IBus::slowRead16(uint32_t a) {
    if (uint8_t* p = ptr(a)) { uint16_t v; std::memcpy(&v, p, 2); return v; }
    uint32_t v;
    if (ioRead(a, 2, v)) { logIo(a, 2, v, false); return uint16_t(v); }
    noteUnmapped(a, false, 0);
    return 0;
}

uint32_t IBus::slowRead32(uint32_t a) {
    if (uint8_t* p = ptr(a)) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    uint32_t v;
    if (ioRead(a, 4, v)) { logIo(a, 4, v, false); return v; }
    noteUnmapped(a, false, 0);
    return 0;
}

void IBus::slowWrite8(uint32_t a, uint8_t v) {
    if (byteWriteIgnored(a)) return;
    if (uint8_t* p = ptr(a)) { *p = v; return; }
    if (ioWrite(a, 1, v)) { logIo(a, 1, v, true); return; }
    noteUnmapped(a, true, v);
}

void IBus::slowWrite16(uint32_t a, uint16_t v) {
    if (uint8_t* p = ptr(a)) { std::memcpy(p, &v, 2); return; }
    if (ioWrite(a, 2, v)) { logIo(a, 2, v, true); return; }
    noteUnmapped(a, true, v);
}

void IBus::slowWrite32(uint32_t a, uint32_t v) {
    if (uint8_t* p = ptr(a)) { std::memcpy(p, &v, 4); return; }
    if (ioWrite(a, 4, v)) { logIo(a, 4, v, true); return; }
    noteUnmapped(a, true, v);
}

bool IBus::loadBlock(uint32_t addr, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        uint8_t* p = ptr(addr + static_cast<uint32_t>(i));
        if (!p) return false;
        *p = data[i];
    }
    return true;
}

// ---------------------------------------------------------------- outils

// Extrait de "reg" (valeur du registre 32 bits) la partie lue par un acces de "size" octets.
static uint32_t ioExtract(uint32_t reg, uint32_t addr, int size) {
    unsigned sh = (addr & 3) * 8;
    if (size == 4) return reg;
    if (size == 2) return (reg >> sh) & 0xFFFF;
    return (reg >> sh) & 0xFF;
}

// Ecriture de "size" octets : calcule la valeur decalee a sa place dans le mot et le masque.
static void ioPlace(uint32_t addr, int size, uint32_t value, uint32_t& v32, uint32_t& mask) {
    unsigned sh = (addr & 3) * 8;
    if (size == 4)      { v32 = value;        mask = 0xFFFFFFFFu; }
    else if (size == 2) { v32 = value << sh;  mask = 0xFFFFu << sh; }
    else                { v32 = value << sh;  mask = 0xFFu << sh; }
}

static uint32_t mergeBits(uint32_t old, uint32_t v, uint32_t mask) { return (old & ~mask) | (v & mask); }

// Ecrit une instruction ARM (little-endian) dans une table de vecteurs.
static void putWord(std::vector<uint8_t>& mem, uint32_t offset, uint32_t w) {
    std::memcpy(&mem[offset], &w, 4);
}

// ---------------------------------------------------------------- System : temps, ecran, minuteries

uint32_t System::ipcSyncRead(int cpu) const {
    uint32_t mine  = cpu == 0 ? ipcSyncOut9 : ipcSyncOut7;
    uint32_t other = cpu == 0 ? ipcSyncOut7 : ipcSyncOut9;
    bool en = cpu == 0 ? ipcIrqEnable9 : ipcIrqEnable7;
    return (other & 0xF) | ((mine & 0xF) << 8) | (en ? 0x4000u : 0u);
}

uint32_t System::dispstatRead(int cpu) const {
    uint32_t v = dispstatCfg[cpu] & 0xFFB8u;
    if (vcount >= 192 && vcount < 262) v |= 1;                 // V-Blank
    if (inHblank) v |= 2;                                      // H-Blank
    if (vcount == uint32_t(lyc(cpu))) v |= 4;                  // ligne de comparaison atteinte
    return v;
}

// ---- Memoire video --------------------------------------------------------

System::System() {
    static const size_t kBankSize[9] = {128 * 1024, 128 * 1024, 128 * 1024, 128 * 1024,
                                        64 * 1024, 16 * 1024, 16 * 1024, 32 * 1024, 16 * 1024};
    for (int i = 0; i < 9; i++) vram[i].assign(kBankSize[i], 0);
    rebuildVramMap();
    buildFirmware();
}

// Recalcule ce que chaque processeur voit de la VRAM, d'apres les registres VRAMCNT.
void System::rebuildVramMap() {
    static const uint32_t kRegionPages[5] = {32, 8, 16, 8, 41};          // pages de 16 Ko par region
    static const uint32_t kLcdc[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
    memGen++;                                                            // les caches de pages des bus sont perimes
    for (auto& r : vramMap9) for (auto& p : r) p = nullptr;
    for (auto& p : vramMap7) p = nullptr;

    auto map9 = [&](int region, uint32_t base, int bank) {
        uint32_t pages = uint32_t(vram[bank].size() >> 14);
        for (uint32_t p = 0; p < pages; p++) {
            uint32_t pg = (base >> 14) + p;
            if (region < 4) pg %= kRegionPages[region];
            else if (pg >= kRegionPages[4]) continue;
            vramMap9[region][pg] = vram[bank].data() + (size_t(p) << 14);
        }
    };
    auto map7 = [&](uint32_t base, int bank) {
        uint32_t pages = uint32_t(vram[bank].size() >> 14);
        for (uint32_t p = 0; p < pages; p++)
            vramMap7[((base >> 14) + p) % 16] = vram[bank].data() + (size_t(p) << 14);
    };

    for (int b = 0; b < 9; b++) {
        uint8_t c = vramCnt[b];
        if (!(c & 0x80)) continue;                              // banque desactivee
        int mst = c & 7, ofs = (c >> 3) & 3;
        if (mst == 0) { map9(4, kLcdc[b], b); continue; }       // acces direct (LCDC)
        switch (b) {
            case 0: case 1:                                     // A, B : fonds A / sprites A
                if (mst == 1) map9(0, 0x20000u * ofs, b);
                else if (mst == 2) map9(2, 0x20000u * (ofs & 1), b);
                break;
            case 2:                                             // C : fonds A / ARM7 / fonds B
                if (mst == 1) map9(0, 0x20000u * ofs, b);
                else if (mst == 2) map7(0x20000u * (ofs & 1), b);
                else if (mst == 4) map9(1, 0, b);
                break;
            case 3:                                             // D : fonds A / ARM7 / sprites B
                if (mst == 1) map9(0, 0x20000u * ofs, b);
                else if (mst == 2) map7(0x20000u * (ofs & 1), b);
                else if (mst == 4) map9(3, 0, b);
                break;
            case 4:                                             // E : fonds A / sprites A
                if (mst == 1) map9(0, 0, b);
                else if (mst == 2) map9(2, 0, b);
                break;
            case 5: case 6: {                                   // F, G : fonds A / sprites A (16 Ko, miroir 32 Ko plus loin)
                uint32_t base = (ofs & 1) * 0x4000u + (ofs >> 1) * 0x10000u;
                if (mst == 1) { map9(0, base, b); map9(0, base + 0x8000u, b); }
                else if (mst == 2) { map9(2, base, b); map9(2, base + 0x8000u, b); }
                break;
            }
            case 7:                                             // H : fonds B (32 Ko, miroir en +0x10000)
                if (mst == 1) { map9(1, 0, b); map9(1, 0x10000, b); }
                break;
            case 8:                                             // I : fonds B / sprites B (16 Ko, miroirs)
                if (mst == 1) for (uint32_t o : {0x8000u, 0xC000u, 0x18000u, 0x1C000u}) map9(1, o, b);
                else if (mst == 2) for (uint32_t o = 0; o < 0x20000u; o += 0x4000u) map9(3, o, b);   // repetee sur 128 Ko
                break;
        }
    }
}

// ---- Diviseur / racine carree de l'ARM9 -----------------------------------

void System::divCompute() {
    const int mode = divCnt & 3;                    // 0 : 32/32, 1 : 64/32, 2 : 64/64 (3 = comme 1)
    int64_t n = divNumer, d = divDenom;
    if (mode == 0) { n = int32_t(n); d = int32_t(d); }
    else if (mode != 2) { d = int32_t(d); }
    divCnt = uint16_t(divCnt & ~0x4000u);
    if (d == 0) {                                   // division par zero : drapeau d'erreur, resultat +-1
        divCnt |= 0x4000;
        divResult = (n < 0) ? 1 : -1;
        divRem = n;
    } else if (n == INT64_MIN && d == -1) {
        divResult = INT64_MIN;
        divRem = 0;
    } else {
        divResult = n / d;
        divRem = n % d;
    }
}

void System::sqrtCompute() {
    uint64_t p = (sqrtCnt & 1) ? sqrtParam : (sqrtParam & 0xFFFFFFFFull);
    uint64_t r = uint64_t(std::sqrt(double(p)));
    while (r > 0 && r * r > p) r--;
    while (r < 0xFFFFFFFFull && (r + 1) * (r + 1) <= p) r++;
    sqrtResult = uint32_t(r);
}

// ---- DMA ------------------------------------------------------------------

static int dmaTiming(int cpu, uint32_t cnt) { return cpu == 0 ? int((cnt >> 27) & 7) : int((cnt >> 28) & 3); }

void System::dmaRun(int cpu, int ch) {
    DmaChan& d = dma[cpu][ch];
    IBus* bus = dmaBus[cpu];
    uint32_t cnt = d.cnt;
    if (!bus) { d.cnt &= ~0x80000000u; return; }

    uint32_t count = cnt & (cpu == 0 ? 0x1FFFFFu : (ch == 3 ? 0xFFFFu : 0x3FFFu));
    if (count == 0) count = cpu == 0 ? 0x200000u : (ch == 3 ? 0x10000u : 0x4000u);
    bool word = (cnt >> 26) & 1;
    int dctl = int((cnt >> 21) & 3), sctl = int((cnt >> 23) & 3);
    int step = word ? 4 : 2;
    uint32_t src = d.curSad, dst = d.curDad;
    for (uint32_t i = 0; i < count; i++) {
        if (word) bus->write32(dst, bus->read32(src));
        else      bus->write16(dst, bus->read16(src));
        src += (sctl == 1) ? uint32_t(-step) : (sctl == 2 ? 0u : uint32_t(step));
        dst += (dctl == 1) ? uint32_t(-step) : (dctl == 2 ? 0u : uint32_t(step));
    }
    d.curSad = src;
    d.curDad = dst;
    if (cnt & (1u << 30)) raise(cpu, IRQ_DMA0 << ch);          // IRQ de fin de transfert

    bool repeat = (cnt >> 25) & 1;
    if (!repeat || dmaTiming(cpu, cnt) == 0 || (cpu == 0 && dmaTiming(cpu, cnt) == 7)) d.cnt &= ~0x80000000u;   // transfert unique (ou vers la file 3D) : termine
    else if (dctl == 3) d.curDad = d.dad;                      // repetition : la destination est rechargee
}

// Declenche les DMA "au moment de" : 1 = debut du V-Blank, 2 = debut du H-Blank (ARM9).
void System::dmaTrigger(int timing) {
    for (int c = 0; c < 2; c++)
        for (int ch = 0; ch < 4; ch++) {
            DmaChan& d = dma[c][ch];
            if ((d.cnt & 0x80000000u) && dmaTiming(c, d.cnt) == timing) dmaRun(c, ch);
        }
}

// reg : 0 = source, 1 = destination, 2 = controle (nombre + options)
void System::dmaWrite(int cpu, int ch, int reg, uint32_t value, uint32_t mask) {
    DmaChan& d = dma[cpu][ch];
    if (reg == 0) { d.sad = mergeBits(d.sad, value, mask) & 0x0FFFFFFFu; return; }
    if (reg == 1) { d.dad = mergeBits(d.dad, value, mask) & 0x0FFFFFFFu; return; }
    bool wasOn = (d.cnt & 0x80000000u) != 0;
    d.cnt = mergeBits(d.cnt, value, mask);
    bool on = (d.cnt & 0x80000000u) != 0;
    if (!wasOn && on) {                                        // activation : on memorise les adresses
        d.curSad = d.sad;
        d.curDad = d.dad;
        int t = dmaTiming(cpu, d.cnt);
        bool now = (t == 0) || (cpu == 0 && (t == 5 || t == 7)) || (cpu == 1 && t == 2);   // immediat, cartouche, ou file 3D (toujours prete)
        if (now) dmaRun(cpu, ch);
    }
}

// ---- Memoire de sauvegarde (puce SPI de la cartouche) ---------------------
//
// Trois familles de puces, qui different par le nombre d'octets d'adresse :
//   1 : petite EEPROM de 512 octets (le 9e bit d'adresse est le bit 3 de la commande : 03/0B lire, 02/0A ecrire)
//   2 : EEPROM / FRAM de 8 ou 64 Ko (03 lire, 02 ecrire)
//   3 : grande EEPROM ou memoire flash de 128 Ko a 8 Mo (03 / 0B lire, 02 / 0A ecrire une page,
//       DB / D8 effacer une page / un secteur, 9F identifiant)
// Communes a toutes : 06 / 04 autoriser / interdire l'ecriture, 05 lire l'etat (bit 1 = ecriture autorisee).
// Le type vient de la taille du fichier .sav (setSaveType), sinon il est reconnu a la 1re lecture du jeu : le
// SDK commence par lire un seul octet, donc la commande fait 1 + adresse + 1 octets.
// "hold" = la puce reste selectionnee apres cet octet (la commande continue).

void System::setSaveType(int addrBytes) {
    saveAddrBytes = addrBytes;
    saveMask = addrBytes == 1 ? 0x1FFu : (addrBytes == 2 ? 0xFFFFu : uint32_t(backup.size() - 1));
}

// Taille du .sav : celle de la puce (512 o ; 8 ou 64 Ko ; 256 ou 512 Ko), d'apres les adresses deja ecrites
uint32_t System::saveFileSize() const {
    switch (saveAddrBytes) {
    case 1:  return 512;
    case 2:  return backupMaxWritten <= 8192 ? 8192u : 65536u;
    case 3:  return backupMaxWritten <= 256 * 1024 ? 256u * 1024 : 512u * 1024;
    default: return 0;                                         // jamais utilisee : rien a enregistrer
    }
}

uint8_t System::backupTransfer(uint8_t b, bool hold) {
    uint8_t resp = 0xFF;
    const int ab = saveAddrBytes;                              // 0 = pas encore reconnu
    if (spiCmd < 0) {                                          // premier octet : la commande
        spiCmd = b;
        spiAddr = 0;
        spiA8 = 0;
        spiLen = 1;
        spiDummy = false;
        if (ab == 1 && ((b & 0xF7) == 0x03 || (b & 0xF7) == 0x02)) {   // petite EEPROM : bit 3 = 9e bit d'adresse
            spiA8 = (b & 0x08) ? 0x100u : 0u;
            spiCmd = b & 0xF7;
        }
        const bool addressed = spiCmd == 0x03 || spiCmd == 0x02 || ((spiCmd == 0x0B || spiCmd == 0x0A ||
                                spiCmd == 0xDB || spiCmd == 0xD8) && ab == 3);
        spiAddrLeft = addressed ? ab : 0;
        spiDummy = (spiCmd == 0x0B && ab == 3);                // lecture rapide : un octet vide apres l'adresse
        if (b == 0x06) spiWel = true;
        else if (b == 0x04) spiWel = false;
    } else {
        spiLen++;
        if (ab == 0) {                                         // type pas encore reconnu : puce vierge
            if (spiCmd == 0x05) resp = spiWel ? 0x02 : 0x00;
        } else if (spiAddrLeft > 0) {                          // octets d'adresse
            spiAddr = (spiAddr << 8) | b;
            if (--spiAddrLeft == 0) spiAddr |= spiA8;
        } else if (spiDummy) {
            spiDummy = false;
        } else if (spiCmd == 0x03 || spiCmd == 0x0B) {         // lecture
            resp = backup[spiAddr & saveMask];
            spiAddr = (spiAddr + 1) & saveMask;
        } else if (spiCmd == 0x05) {                           // etat : bit 1 = ecriture autorisee, bit 0 = occupee (jamais)
            resp = spiWel ? 0x02 : 0x00;
        } else if (spiCmd == 0x9F && ab == 3) {                // identifiant d'une flash de 256 Ko
            static const uint8_t kId[3] = {0x20, 0x40, 0x12};
            resp = kId[spiAddr < 3 ? spiAddr : 2];
            spiAddr++;
        } else if (spiCmd == 0x02 || spiCmd == 0x0A) {         // ecriture, en bouclant dans la page
            if (spiWel) {
                const uint32_t page = ab == 1 ? 16u : (ab == 2 ? 128u : 256u);
                const uint32_t a = spiAddr & saveMask;
                if (backup[a] != b) { backup[a] = b; backupDirty = true; }
                if (a + 1 > backupMaxWritten) backupMaxWritten = a + 1;
                spiAddr = (a & ~(page - 1)) | ((a + 1) & (page - 1));
            }
        }
    }
    if (backupLog.size() < 600) backupLog.push_back(uint16_t(b | (hold ? 0 : 0x100)));
    if (!hold) {                                               // fin de commande
        if (ab == 0 && spiCmd == 0x03 && spiLen >= 3)          // 1re lecture : on en deduit la taille des adresses
            setSaveType(std::min(3, std::max(1, spiLen - 2)));
        if (ab == 3 && spiWel && spiAddrLeft == 0) {           // effacements (flash) : remis a 0xFF
            const uint32_t a = spiAddr & saveMask;
            if (spiCmd == 0xDB) { std::memset(&backup[a & ~0xFFu], 0xFF, 0x100); backupDirty = true; }
            if (spiCmd == 0xD8) { std::memset(&backup[a & ~0xFFFFu], 0xFF, 0x10000); backupDirty = true; }
        }
        if (spiCmd == 0x02 || spiCmd == 0x0A || spiCmd == 0xDB || spiCmd == 0xD8) spiWel = false;   // l'autorisation retombe
        spiCmd = -1;
    }
    return resp;
}

// ---- Bus SPI de l'ARM7 ----------------------------------------------------

// Firmware fabrique : on ne fournit que ce que les jeux lisent vraiment, c'est-a-dire l'en-tete (position
// des reglages utilisateur) et les reglages eux-memes, en deux copies (le systeme garde celle dont le
// compteur est le plus grand). Le reste vaut 0xFF, comme une memoire flash effacee.
void System::buildFirmware() {
    firmware.assign(0x40000, 0xFF);
    std::memset(firmware.data(), 0, 0x200);
    firmware[0x1D] = 0xFF;                                     // type de console : DS
    const uint16_t settingsOfs = uint16_t(0x3FE00 / 8);        // position des reglages, en unites de 8 octets
    firmware[0x20] = uint8_t(settingsOfs & 0xFF); firmware[0x21] = uint8_t(settingsOfs >> 8);

    for (int copy = 0; copy < 2; copy++) {
        uint8_t* u = &firmware[0x3FE00 + copy * 0x100];
        std::memset(u, 0, 0x100);
        auto put16 = [&](int o, uint16_t v) { u[o] = uint8_t(v); u[o + 1] = uint8_t(v >> 8); };
        put16(0x00, 5);                                        // version
        u[0x02] = 0;                                           // couleur preferee
        u[0x03] = 1; u[0x04] = 1;                              // anniversaire : 1er janvier
        const char* nick = "NDSEMU";
        int n = 0;
        for (; nick[n]; n++) put16(0x06 + 2 * n, uint16_t(nick[n]));
        put16(0x1A, uint16_t(n));                              // longueur du pseudo
        // Calibrage de l'ecran tactile : deux points (valeur du capteur 12 bits <-> pixel).
        // Capteur = pixel x 16 : (0, 0) -> (0, 0) et (0xFF0, 0xBF0) -> (255, 191).
        put16(0x58, 0x0000); put16(0x5A, 0x0000); u[0x5C] = 0;   u[0x5D] = 0;
        put16(0x5E, 0x0FF0); put16(0x60, 0x0BF0); u[0x62] = 255; u[0x63] = 191;
        put16(0x64, 0x0001);                                   // langue : 1 = anglais (2 = francais)
        put16(0x70, uint16_t(copy));                           // compteur : la 2e copie est la plus recente
        uint16_t crc = 0xFFFF;                                 // CRC-16 des 0x70 premiers octets
        for (int i = 0; i < 0x70; i++) {
            crc ^= u[i];
            for (int k = 0; k < 8; k++) crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0xA001) : uint16_t(crc >> 1);
        }
        put16(0x72, crc);
    }
}

void System::spiRelease() {
    pmIndex = -1;
    if (fwCmd >= 0 && fwAddrLeft == 0 && fwWel) {               // fin d'une commande d'ecriture : elle s'execute
        const uint32_t a = fwAddr & 0x3FFFF;
        if (fwCmd == 0xDB) {                                     // effacement d'une page (256 octets)
            std::memset(&firmware[a & ~0xFFu], 0xFF, 0x100);
            firmwareDirty = true;
        } else if (fwCmd == 0xD8) {                              // effacement d'un secteur (64 Ko)
            std::memset(&firmware[a & ~0xFFFFu], 0xFF, 0x10000);
            firmwareDirty = true;
        }
    }
    if (fwCmd == 0x0A || fwCmd == 0x02 || fwCmd == 0xDB || fwCmd == 0xD8) fwWel = false;   // l'autorisation retombe
    fwCmd = -1;
    fwAddrLeft = 0;
    fwDummy = false;
}

uint8_t System::spiTransfer(uint8_t b) {
    uint8_t out = 0;
    switch ((spiCnt >> 8) & 3) {
    case 0:                                                    // gestion de l'alimentation
        if (pmIndex < 0) {                                     // 1er octet : bit 7 = lecture, bits 0-6 = registre
            pmIndex = b;
        } else {
            const int reg = pmIndex & 7;
            if (pmIndex & 0x80) out = pmReg[reg];
            else pmReg[reg] = b;                               // (bit 6 du registre 0 = eteindre : ignore)
        }
        break;
    case 1:                                                    // memoire firmware (flash SPI de 256 Ko)
        // Commandes : 03 lire, 0B lire vite, 05 etat, 06 / 04 autoriser / interdire l'ecriture, 9F identifiant,
        // 0A ecrire une page, 02 programmer (les bits ne peuvent que passer a 0), DB / D8 effacer page / secteur.
        // Les ecritures ne marchent qu'apres 06, et l'autorisation retombe a la fin de chacune.
        if (fwCmd < 0) {
            fwCmd = b;
            fwAddr = 0;
            fwAddrLeft = (b == 0x03 || b == 0x0B || b == 0x0A || b == 0x02 || b == 0xDB || b == 0xD8) ? 3 : 0;
            fwDummy = (b == 0x0B);
            if (b == 0x06) fwWel = true;
            else if (b == 0x04) fwWel = false;
        } else if (fwAddrLeft > 0) {
            fwAddr = (fwAddr << 8) | b;
            fwAddrLeft--;
        } else if (fwDummy) {
            fwDummy = false;
        } else if (fwCmd == 0x03 || fwCmd == 0x0B) {
            out = firmware[fwAddr & 0x3FFFF];
            fwAddr++;
        } else if (fwCmd == 0x05) {
            out = fwWel ? 0x02 : 0x00;                         // etat : bit 1 = ecriture autorisee, bit 0 = occupee (jamais)
        } else if (fwCmd == 0x9F) {
            static const uint8_t kId[3] = {0x20, 0x40, 0x12};  // identifiant de la puce
            out = kId[fwAddr < 3 ? fwAddr : 2];
            fwAddr++;
        } else if ((fwCmd == 0x0A || fwCmd == 0x02) && fwWel) {   // octets a ecrire, en bouclant dans la page de 256 octets
            uint8_t& cell = firmware[fwAddr & 0x3FFFF];
            const uint8_t nv = fwCmd == 0x0A ? b : uint8_t(cell & b);
            if (cell != nv) { cell = nv; firmwareDirty = true; }
            fwAddr = (fwAddr & ~0xFFu) | ((fwAddr + 1) & 0xFFu);
        } else {
            out = 0xFF;
        }
        break;
    case 2: {                                                  // ecran tactile : le resultat sort sur les 2 octets suivants
        out = tscPos == 1 ? uint8_t(tscResult >> 5) : (tscPos == 2 ? uint8_t(tscResult << 3) : uint8_t(0));
        if (b & 0x80) {                                        // octet de commande : bits 4-6 = mesure demandee
            switch ((b >> 4) & 7) {
            case 1: tscResult = touchDown ? uint16_t(touchY << 4) : 0xFFF; break;   // Y
            case 5: tscResult = touchDown ? uint16_t(touchX << 4) : 0x000; break;   // X
            case 3: tscResult = touchDown ? 0x200 : 0x000; break;                   // pression Z1
            case 4: tscResult = touchDown ? 0xE00 : 0xFFF; break;                   // pression Z2
            default: tscResult = 0x800; break;                                      // temperature, batterie...
            }
            if (b & 0x08) tscResult &= 0xFF0;                  // mode 8 bits
            tscPos = 1;
        } else if (tscPos) {
            tscPos = tscPos < 2 ? tscPos + 1 : 0;
        }
        break;
    }
    default:
        out = 0xFF;
        break;
    }
    if (!(spiCnt & 0x0800)) spiRelease();                      // bit 11 = 0 : fin de la commande apres cet octet
    return out;
}

// ---- Horloge temps reel (Seiko S-35199A) -----------------------------------
//
// Chaque bit passe pendant que l'horloge (bit 1) est basse, poids faible d'abord. Le 1er octet est la commande :
// code fixe 0110, numero du registre sur 3 bits, puis le sens (1 = lecture). La puce accepte aussi l'ordre
// inverse des bits ; on le remet dans l'ordre "registre en bits 4-6, lecture en bit 7".

static uint8_t toBcd(int v) { return uint8_t(((v / 10) << 4) | (v % 10)); }

void System::rtcByteIn(uint8_t b) {
    if (rtcInPos == 0) {                                       // octet de commande
        if ((b & 0xF0) == 0x60) {                              // envoye poids fort d'abord : on le retourne
            uint8_t rev = 0;
            for (int i = 0; i < 8; i++) { rev = uint8_t((rev >> 1) | (b & 0x80)); b = uint8_t(b << 1); }
            b = rev;
        }
        rtcCmd = b;
        std::memset(rtcOut, 0, sizeof rtcOut);
        if (rtcCmd & 0x80) {                                   // lecture : on prepare la reponse
            const std::time_t now = std::time(nullptr);
            std::tm t{};
#ifdef _WIN32
            localtime_s(&t, &now);
#else
            localtime_r(&now, &t);
#endif
            int hour = t.tm_hour;
            uint8_t hourByte;
            if (rtcStatus1 & 0x02) hourByte = toBcd(hour);                  // mode 24 heures
            else hourByte = toBcd(hour % 12);                                // mode 12 heures
            if (hour >= 12) hourByte |= 0x40;                                // bit "apres-midi"
            switch (rtcCmd & 0x70) {
            case 0x00: rtcOut[0] = rtcStatus1; break;
            case 0x40: rtcOut[0] = rtcStatus2; break;
            case 0x20:                                          // date et heure : annee, mois, jour, jour de semaine, h, min, s
                rtcOut[0] = toBcd(t.tm_year % 100);
                rtcOut[1] = toBcd(t.tm_mon + 1);
                rtcOut[2] = toBcd(t.tm_mday);
                rtcOut[3] = uint8_t(t.tm_wday);                // 0 = dimanche
                rtcOut[4] = hourByte;
                rtcOut[5] = toBcd(t.tm_min);
                rtcOut[6] = toBcd(t.tm_sec);
                break;
            case 0x60:                                          // heure seule
                rtcOut[0] = hourByte;
                rtcOut[1] = toBcd(t.tm_min);
                rtcOut[2] = toBcd(t.tm_sec);
                break;
            case 0x10: std::memcpy(rtcOut, rtcAlarm[0], 3); break;
            case 0x50: std::memcpy(rtcOut, rtcAlarm[1], 3); break;
            case 0x30: rtcOut[0] = rtcAdjust; break;
            default:   rtcOut[0] = rtcFree; break;
            }
        }
        return;
    }
    if (rtcCmd & 0x80) return;                                 // octets envoyes pendant une lecture : ignores
    const int pos = rtcInPos - 1;                              // ecriture : octets de donnees
    switch (rtcCmd & 0x70) {
    case 0x00: if (pos == 0) rtcStatus1 = uint8_t((rtcStatus1 & 0xF1) | (b & 0x0E)); break;   // bit 0 = remise a zero : ignore
    case 0x40: if (pos == 0) rtcStatus2 = b; break;
    case 0x10: if (pos < 3) rtcAlarm[0][pos] = b; break;
    case 0x50: if (pos < 3) rtcAlarm[1][pos] = b; break;
    case 0x30: if (pos == 0) rtcAdjust = b; break;
    case 0x70: if (pos == 0) rtcFree = b; break;
    default: break;                                            // reglage de la date / heure : on garde celle du PC
    }
}

void System::rtcWrite(uint16_t v) {
    if (v & 0x0004) {                                          // puce selectionnee
        if (!(rtcIo & 0x0004)) {                               // vient d'etre selectionnee : nouvelle commande
            rtcIn = 0; rtcInBit = 0; rtcInPos = 0;
            rtcOutBit = 0; rtcOutPos = 0;
            rtcCmd = 0;
        } else if (!(v & 0x0002)) {                            // horloge basse : un bit passe
            if (v & 0x0010) {                                  // le processeur envoie
                rtcIn = uint8_t(rtcIn | ((v & 1) << rtcInBit));
                if (++rtcInBit == 8) { rtcByteIn(rtcIn); rtcIn = 0; rtcInBit = 0; rtcInPos++; }
            } else {                                           // la puce repond
                const bool bit = (rtcOut[rtcOutPos] >> rtcOutBit) & 1;
                rtcIo = uint16_t((rtcIo & ~1u) | (bit ? 1u : 0u));
                if (++rtcOutBit == 8) { rtcOutBit = 0; if (rtcOutPos < 7) rtcOutPos++; }
            }
        }
    }
    // Bit 0 : impose par le processeur s'il est en sortie, sinon c'est la valeur fournie par la puce
    if (v & 0x0010) rtcIo = v;
    else rtcIo = uint16_t((rtcIo & 0x0001) | (v & 0xFFFE));
}

// ---- Cartouche ------------------------------------------------------------
//
// On suppose la cartouche deja initialisee (comme apres le demarrage reel) : les commandes sont
// recues en clair et les donnees renvoyees en clair. Commandes gerees : B7 (lecture), B8 / 90
// (identifiant), 00 (en-tete), 9F (vide) ; le reste renvoie 0xFF.

void System::cardStart() {
    uint32_t sizeField = (romCtrl >> 24) & 7;
    size_t len = sizeField == 0 ? 0 : (sizeField == 7 ? 4 : (size_t(0x100) << sizeField));
    cardBuf.assign(len, 0xFF);
    static const std::vector<uint8_t> kNoRom;
    const std::vector<uint8_t>& rom = cartRom ? *cartRom : kNoRom;
    switch (cardCmd[0]) {
        case 0xB7: {                                           // lecture de donnees
            uint32_t a = (uint32_t(cardCmd[1]) << 24) | (uint32_t(cardCmd[2]) << 16) |
                         (uint32_t(cardCmd[3]) << 8) | cardCmd[4];
            if (a < 0x8000) a = 0x8000 + (a & 0x1FF);          // la zone protegee n'est pas lisible
            for (size_t i = 0; i < len; i++) {
                size_t idx = size_t(a & ~0xFFFu) + ((a + i) & 0xFFFu);   // la lecture boucle sur 4 Ko
                cardBuf[i] = idx < rom.size() ? rom[idx] : uint8_t(0xFF);
            }
            break;
        }
        case 0xB8: case 0x90:                                  // identifiant de la puce
            for (size_t i = 0; i < len; i++) cardBuf[i] = uint8_t(cardChipId >> (8 * (i & 3)));
            break;
        case 0x00:                                             // en-tete
            for (size_t i = 0; i < len; i++) cardBuf[i] = (i & 0x1FF) < rom.size() ? rom[i & 0x1FF] : uint8_t(0xFF);
            break;
        default:
            break;
    }
    cardPos = 0;
    cardBusy = len > 0;
    if (!cardBusy && (auxSpiCnt & 0x4000)) raise((exmemcnt & 0x0800) ? 1 : 0, IRQ_CARD);
}

uint32_t System::cardRomCtrlRead() const {
    uint32_t v = romCtrl & 0x7F7FFFFFu;
    if (cardBusy) {
        v |= 0x80000000u;                                      // transfert en cours
        if (cardPos < cardBuf.size()) v |= 0x00800000u;        // un mot est pret a etre lu
    }
    return v;
}

uint32_t System::cardReadWord() {
    if (!cardBusy || cardPos >= cardBuf.size()) return 0xFFFFFFFFu;
    uint32_t w = 0;
    for (int i = 0; i < 4; i++)
        w |= uint32_t(cardPos + i < cardBuf.size() ? cardBuf[cardPos + i] : uint8_t(0xFF)) << (8 * i);
    cardPos += 4;
    if (cardPos >= cardBuf.size()) {                           // dernier mot lu : transfert termine
        cardBusy = false;
        if (auxSpiCnt & 0x4000) raise((exmemcnt & 0x0800) ? 1 : 0, IRQ_CARD);
    }
    return w;
}

// ---- FIFO IPC -------------------------------------------------------------

uint32_t System::fifoCntRead(int cpu) const {
    const IpcFifo& snd = fifo[cpu];
    const IpcFifo& rcv = fifo[1 - cpu];
    uint32_t v = fifoCnt[cpu] & 0xC404u;                       // bits 2, 10, 14 (erreur) et 15 (activee)
    if (snd.count == 0)  v |= 0x0001;                          // file d'envoi vide
    if (snd.count == 16) v |= 0x0002;                          // file d'envoi pleine
    if (rcv.count == 0)  v |= 0x0100;                          // file de reception vide
    if (rcv.count == 16) v |= 0x0200;                          // file de reception pleine
    return v;
}

void System::fifoCntWrite(int cpu, uint32_t v) {
    uint16_t old = fifoCnt[cpu];
    IpcFifo& snd = fifo[cpu];
    const IpcFifo& rcv = fifo[1 - cpu];
    if (v & 0x0008) { snd.count = 0; snd.head = 0; }           // vider la file d'envoi
    uint16_t nw = old;
    nw = uint16_t((nw & ~0x0004) | (v & 0x0004));              // IRQ "envoi vide"
    nw = uint16_t((nw & ~0x0400) | (v & 0x0400));              // IRQ "reception non vide"
    nw = uint16_t((nw & ~0x8000) | (v & 0x8000));              // FIFO activee
    if (v & 0x4000) nw = uint16_t(nw & ~0x4000);               // ecrire 1 efface l'erreur
    fifoCnt[cpu] = nw;
    if (!(old & 0x0004) && (nw & 0x0004) && snd.count == 0) raise(cpu, IRQ_IPCSENDEMPTY);
    if (!(old & 0x0400) && (nw & 0x0400) && rcv.count > 0)  raise(cpu, IRQ_IPCRECV);
}

void System::fifoSend(int cpu, uint32_t word) {
    if (!(fifoCnt[cpu] & 0x8000)) return;                      // FIFO desactivee : ecriture ignoree
    IpcFifo& snd = fifo[cpu];
    if (snd.count >= 16) { fifoCnt[cpu] |= 0x4000; return; }   // file pleine : erreur
    snd.data[(snd.head + snd.count) & 15] = word;
    snd.count++;
    spu.note(cycles, "FIFO %s %08X", cpu == 0 ? "ARM9 -> ARM7" : "ARM7 -> ARM9", word);   // journal du son (si active)
    // L'autre processeur recoit une IRQ si sa file de reception passe de vide a non vide
    if (snd.count == 1 && (fifoCnt[1 - cpu] & 0x8400) == 0x8400) raise(1 - cpu, IRQ_IPCRECV);
}

uint32_t System::fifoRecv(int cpu) {
    IpcFifo& rcv = fifo[1 - cpu];
    if (!(fifoCnt[cpu] & 0x8000)) return fifoLast[cpu];
    if (rcv.count == 0) { fifoCnt[cpu] |= 0x4000; return fifoLast[cpu]; }   // lecture d'une file vide : erreur
    uint32_t w = rcv.data[rcv.head];
    rcv.head = (rcv.head + 1) & 15;
    rcv.count--;
    fifoLast[cpu] = w;
    // L'envoyeur recoit une IRQ quand sa file d'envoi vient de se vider
    if (rcv.count == 0 && (fifoCnt[1 - cpu] & 0x0004)) raise(1 - cpu, IRQ_IPCSENDEMPTY);
    return w;
}

void System::tickTimer(int cpu, int i) {
    Timer& t = timers[cpu][i];
    t.counter++;
    if (t.counter == 0) {                                      // debordement
        t.counter = t.reload;
        if (t.ctl & 0x40) raise(cpu, IRQ_TIMER0 << i);
        if (i < 3 && (timers[cpu][i + 1].ctl & 0x84) == 0x84)  // le timer suivant compte les debordements
            tickTimer(cpu, i + 1);
    }
}

// Debut du V-Blank : le moteur 3D dessine sa liste de polygones, avec les textures et palettes que le systeme
// voit a cet instant (banques A a D en mode texture, E / F / G en mode palette de textures).
void System::gxVBlank() {
    GxTexMem mem;
    for (int b = 0; b < 4; b++)
        if ((vramCnt[b] & 0x87) == 0x83 && vram[b].size() >= 0x20000) mem.tex[(vramCnt[b] >> 3) & 3] = vram[b].data();
    if ((vramCnt[4] & 0x87) == 0x83 && vram[4].size() >= 0x10000)
        for (int i = 0; i < 4; i++) mem.pal[i] = vram[4].data() + i * 0x4000;
    static const int kSlot[4] = {0, 1, 4, 5};                     // fentes de palette occupees par F et G selon OFS
    for (int b = 5; b <= 6; b++)
        if ((vramCnt[b] & 0x87) == 0x83 && vram[b].size() >= 0x4000) mem.pal[kSlot[(vramCnt[b] >> 3) & 3]] = vram[b].data();
    gx.disp3dcnt = uint16_t(eng[0][24] & 0xFFFF);                   // DISP3DCNT (contours, brouillard, toon...)
    gx.onVBlank(mem);
}

// Ligne suivante. La capture d'ecran demandee avant la ligne 0 couvre toute l'image : comme le rendu 2D se fait
// d'un bloc, elle est faite au debut du V-Blank, avant que la 3D n'affiche l'image suivante.
void System::newLine() {
    lineCycle = 0;
    inHblank = false;
    vcount = (vcount + 1) % 263;
    if (vcount == 0) captureArmed = (eng[0][25] & 0x80000000u) != 0;
    if (vcount == 192) {
        gx.finish();                                           // mode differe : l'image 3D dessinee devient visible
        vblankReached = true;
        for (int c = 0; c < 2; c++) if (dispstatCfg[c] & 0x08) raise(c, IRQ_VBLANK);
        if (captureArmed && (eng[0][25] & 0x80000000u)) {
            displayCapture(*this);
            lastCaptureCnt = eng[0][25];
            captureCount++;
            eng[0][25] &= ~0x80000000u;                        // fin de la capture : le bit d'activation retombe
        }
        captureArmed = false;
        gxVBlank();                                            // un echange de tampons 3D devient visible
        dmaTrigger(1);                                         // DMA "au V-Blank"
    }
    for (int c = 0; c < 2; c++)
        if ((dispstatCfg[c] & 0x20) && vcount == uint32_t(lyc(c))) raise(c, IRQ_VCOUNT);
}

// Prochain evenement : debut de H-Blank, fin de ligne, ou debordement d'une minuterie en marche.
uint32_t System::cyclesToEvent() const {
    static const uint32_t kDiv[4] = {1, 64, 256, 1024};
    uint32_t d = lineCycle < 1536 ? 1536 - lineCycle : 2130 - lineCycle;
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 4; i++) {
            const Timer& t = timers[c][i];
            if (!(t.ctl & 0x80)) continue;                 // arretee
            if ((t.ctl & 4) && i > 0) continue;            // en mode cascade : suit la minuterie precedente
            const uint32_t div = kDiv[t.ctl & 3];
            const uint32_t toTick = div > t.accum ? div - t.accum : 1;          // cycles jusqu'au prochain "tic"
            const uint64_t toOverflow = uint64_t(toTick) + uint64_t(0x10000u - t.counter - 1u) * div;
            if (toOverflow < d) d = uint32_t(toOverflow);
        }
    }
    return d;
}

// Fait avancer le temps de n cycles en sautant d'evenement en evenement : le resultat est exactement celui
// de n appels a advanceSlow(1) (verifie par un test), mais sans parcourir chaque cycle.
void System::advance(uint32_t n) {
    static const uint32_t kDiv[4] = {1, 64, 256, 1024};
    while (n) {
        uint32_t k = cyclesToEvent();
        if (k > n) k = n;
        cycles += k;

        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 4; i++) {
                Timer& t = timers[c][i];
                if (!(t.ctl & 0x80)) continue;
                if ((t.ctl & 4) && i > 0) continue;
                const uint32_t div = kDiv[t.ctl & 3];
                const uint32_t toTick = div > t.accum ? div - t.accum : 1;
                uint32_t ticks = 0;
                if (k >= toTick) {
                    const uint32_t rest = k - toTick;
                    ticks = 1 + rest / div;
                    t.accum = rest % div;
                } else {
                    t.accum += k;
                }
                if (!ticks) continue;
                if (uint32_t(t.counter) + ticks < 0x10000u) t.counter = uint16_t(t.counter + ticks);
                else for (uint32_t j = 0; j < ticks; j++) tickTimer(c, i);          // debordement (au dernier tic)
            }
        }

        lineCycle += k;
        if (lineCycle == 1536) {                               // debut du H-Blank (256 points x 6 cycles)
            inHblank = true;
            if (vcount < 192) {
                for (int c = 0; c < 2; c++) if (dispstatCfg[c] & 0x10) raise(c, IRQ_HBLANK);
                dmaTrigger(2);                                 // DMA "au H-Blank"
            }
        }
        if (lineCycle == 2130) {                               // ligne suivante (355 points x 6 cycles)
            newLine();
        }
        n -= k;
    }
    spu.run(cycles);                                           // le son produit les echantillons dus (souvent aucun)
}

void System::advanceSlow(uint32_t n) {
    static const uint32_t kDiv[4] = {1, 64, 256, 1024};
    for (uint32_t k = 0; k < n; k++) {
        cycles++;

        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 4; i++) {
                Timer& t = timers[c][i];
                if (!(t.ctl & 0x80)) continue;                 // arretee
                if ((t.ctl & 4) && i > 0) continue;            // en mode cascade
                if (++t.accum >= kDiv[t.ctl & 3]) { t.accum = 0; tickTimer(c, i); }
            }
        }

        lineCycle++;
        if (lineCycle == 1536) {                               // debut du H-Blank (256 points x 6 cycles)
            inHblank = true;
            if (vcount < 192) {
                for (int c = 0; c < 2; c++) if (dispstatCfg[c] & 0x10) raise(c, IRQ_HBLANK);
                dmaTrigger(2);                                 // DMA "au H-Blank"
            }
        }
        if (lineCycle == 2130) {                               // ligne suivante (355 points x 6 cycles)
            newLine();
        }
    }
    spu.run(cycles);
}

bool System::ioRead32(int cpu, uint32_t addr, uint32_t& v) {
    if (cpu == 1 && addr >= 0x04000400 && addr < 0x04000520) { v = spu.read32(addr, cycles); return true; }   // son
    if (cpu == 0 && addr >= 0x04000620 && addr < 0x040006A4) {          // resultats des tests et matrices du moteur 3D
        if (addr < 0x04000630) {                                        // POS_RESULT : x, y, z, w
            v = uint32_t(gx.posResult[(addr - 0x04000620) >> 2]);
        } else if (addr < 0x04000638) {                                 // VEC_RESULT : 3 x 16 bits
            const uint32_t i = (addr - 0x04000630) >> 1;
            v = uint32_t(uint16_t(i < 3 ? gx.vecResult[i] : int16_t(0))) |
                (uint32_t(uint16_t(i + 1 < 3 ? gx.vecResult[i + 1] : int16_t(0))) << 16);
        } else if (addr >= 0x04000640 && addr < 0x04000680) {           // CLIPMTX_RESULT : 16 mots
            int32_t mtx[16];
            gx.readClipMatrix(mtx);
            v = uint32_t(mtx[(addr - 0x04000640) >> 2]);
        } else if (addr >= 0x04000680) {                                // VECMTX_RESULT : 9 mots
            int32_t mtx[9];
            gx.readVecMatrix(mtx);
            const uint32_t i = (addr - 0x04000680) >> 2;
            v = i < 9 ? uint32_t(mtx[i]) : 0u;
        } else v = 0;
        return true;
    }
    switch (addr) {
        case 0x04000004: v = dispstatRead(cpu) | (vcount << 16); return true;   // DISPSTAT + VCOUNT
        case 0x04000100: case 0x04000104: case 0x04000108: case 0x0400010C: {   // minuteries
            const Timer& t = timers[cpu][(addr - 0x04000100) / 4];
            v = t.counter | (uint32_t(t.ctl) << 16);
            return true;
        }
        case 0x04000130: v = keyInput; return true;            // KEYINPUT (+ KEYCNT = 0)
        case 0x04000134:                                       // RCNT + KEYXY (ARM7 seulement)
            if (cpu != 1) return false;
            v = (rcnt7 & 0xFFFF) | (uint32_t(keyXY) << 16);
            return true;
        case 0x04000180: v = ipcSyncRead(cpu); return true;
        case 0x040000B0: case 0x040000B4: case 0x040000B8:          // DMA : source, destination, controle
        case 0x040000BC: case 0x040000C0: case 0x040000C4:
        case 0x040000C8: case 0x040000CC: case 0x040000D0:
        case 0x040000D4: case 0x040000D8: case 0x040000DC: {
            uint32_t o = addr - 0x040000B0;
            v = (o % 12 == 8) ? dma[cpu][o / 12].cnt : 0;       // seul le registre de controle se relit
            return true;
        }
        case 0x040000E0: case 0x040000E4: case 0x040000E8: case 0x040000EC:   // DMAxFILL (ARM9)
            if (cpu != 0) return false;
            v = dmaFill[(addr - 0x040000E0) / 4];
            return true;
        case 0x04000280: case 0x04000290: case 0x04000294: case 0x04000298: case 0x0400029C:
        case 0x040002A0: case 0x040002A4: case 0x040002A8: case 0x040002AC:
        case 0x040002B0: case 0x040002B4: case 0x040002B8: case 0x040002BC:
            if (cpu != 0) return false;                         // diviseur et racine carree (ARM9)
            switch (addr) {
                case 0x04000280: v = divCnt; break;
                case 0x04000290: v = uint32_t(divNumer); break;
                case 0x04000294: v = uint32_t(uint64_t(divNumer) >> 32); break;
                case 0x04000298: v = uint32_t(divDenom); break;
                case 0x0400029C: v = uint32_t(uint64_t(divDenom) >> 32); break;
                case 0x040002A0: v = uint32_t(divResult); break;
                case 0x040002A4: v = uint32_t(uint64_t(divResult) >> 32); break;
                case 0x040002A8: v = uint32_t(divRem); break;
                case 0x040002AC: v = uint32_t(uint64_t(divRem) >> 32); break;
                case 0x040002B0: v = sqrtCnt; break;
                case 0x040002B4: v = sqrtResult; break;
                case 0x040002B8: v = uint32_t(sqrtParam); break;
                default:         v = uint32_t(sqrtParam >> 32); break;
            }
            return true;
        case 0x04000600:                                        // GXSTAT : moteur 3D (pas encore emule)
            if (cpu != 0) return false;
            // file de commandes vide, moteur libre (bit 0 : test en cours = 0) ; bit 1 : resultat de BOX_TEST
            v = 0x06000000u | (gx.boxTestResult ? 2u : 0u) | (uint32_t(gxIrqMode) << 30);
            return true;
        case 0x04000240:
            if (cpu == 1) {                                     // ARM7 : VRAMSTAT + WRAMSTAT
                uint32_t vs = ((vramCnt[2] & 0x87) == 0x82 ? 1u : 0u) | ((vramCnt[3] & 0x87) == 0x82 ? 2u : 0u);
                v = vs | (uint32_t(wramCnt) << 8);
            } else {
                v = vramCnt[0] | (uint32_t(vramCnt[1]) << 8) | (uint32_t(vramCnt[2]) << 16) | (uint32_t(vramCnt[3]) << 24);
            }
            return true;
        case 0x04000248:
            if (cpu != 0) return false;
            v = vramCnt[7] | (uint32_t(vramCnt[8]) << 8);
            return true;
        case 0x04000304:
            v = cpu == 0 ? powcnt1 : powcnt2;                   // POWCNT1 (ARM9) / POWCNT2 (ARM7)
            return true;
        case 0x04000138:                                        // RTC (ARM7)
            if (cpu != 1) return false;
            v = rtcIo;
            return true;
        case 0x040001C0:                                        // SPICNT + SPIDATA (ARM7) ; bit 7 "occupe" toujours a 0
            if (cpu != 1) return false;
            v = (spiCnt & 0xCF03u) | (uint32_t(spiOut) << 16);
            return true;
        case 0x040001A0: v = (auxSpiCnt & 0xE043u) | (uint32_t(spiData) << 16); return true;   // AUXSPICNT + AUXSPIDATA
        case 0x040001A4: v = cardRomCtrlRead(); return true;        // ROMCTRL
        case 0x040001A8: case 0x040001AC: v = 0; return true;       // octets de commande (ecriture seule)
        case 0x04100010: v = cardReadWord(); return true;           // donnees de la cartouche
        case 0x04000204: v = exmemcnt; return true;                 // EXMEMCNT
        case 0x04000184: v = fifoCntRead(cpu); return true;     // IPCFIFOCNT
        case 0x04000188: v = 0; return true;                    // IPCFIFOSEND (ecriture seule)
        case 0x04100000: v = fifoRecv(cpu); return true;        // IPCFIFORECV : lire retire un mot de la file
        case 0x04000208: v = irq[cpu].ime; return true;
        case 0x04000210: v = irq[cpu].ie; return true;
        case 0x04000214: v = irq[cpu].iflag; return true;
        case 0x04000244:                                        // VRAMCNT_E a G + WRAMCNT
            if (cpu != 0) return false;
            v = vramCnt[4] | (uint32_t(vramCnt[5]) << 8) | (uint32_t(vramCnt[6]) << 16) | (uint32_t(wramCnt) << 24);
            return true;
        case 0x04000300: v = 1; return true;                   // POSTFLG : demarrage termine
        default: break;
    }
    if (cpu == 0 && addr < 0x04001070 && (addr & 3) == 0) {                 // registres des moteurs 2D
        if (addr >= 0x04000000 && addr < 0x04000070 && addr != 0x04000004) { v = eng[0][(addr - 0x04000000) / 4]; return true; }
        if (addr >= 0x04001000 && addr < 0x04001070) { v = eng[1][(addr - 0x04001000) / 4]; return true; }
    }
    return false;
}

bool System::ioWrite32(int cpu, uint32_t addr, uint32_t v, uint32_t mask) {
    if (cpu == 1 && addr >= 0x04000400 && addr < 0x04000520) { spu.write32(addr, v, mask, cycles); return true; }   // son
    if (cpu == 0 && addr >= 0x04000400 && addr < 0x04000600) {      // moteur 3D : file de commandes et ports directs
        if (mask == 0xFFFFFFFFu) {
            if (addr < 0x04000440) gx.writeFifo(v);                 // GXFIFO : commandes empaquetees
            else gx.writePort((addr - 0x04000400) >> 2, v);         // un port par commande
        }
        return true;
    }
    if (cpu == 0 && addr >= 0x04000330 && addr < 0x040003C0) {          // registres d'affichage 3D (contours, brouillard...)
        uint32_t cur;
        std::memcpy(&cur, &gx.dispRegs[addr - 0x04000330], 4);
        cur = mergeBits(cur, v, mask);
        std::memcpy(&gx.dispRegs[addr - 0x04000330], &cur, 4);
        if (addr != 0x04000350 && addr != 0x04000354) return true;
    }
    switch (addr) {
        case 0x04000350:                                        // CLEAR_COLOR
            if (cpu != 0) return false;
            gx.clearColor = mergeBits(gx.clearColor, v, mask);
            return true;
        case 0x04000354:                                        // CLEAR_DEPTH
            if (cpu != 0) return false;
            gx.clearDepth = mergeBits(gx.clearDepth, v, mask) & 0x7FFF;
            return true;
        case 0x04000004:                                       // DISPSTAT (VCOUNT en lecture seule)
            if (mask & 0xFFFF)
                dispstatCfg[cpu] = uint16_t(mergeBits(dispstatCfg[cpu], v, mask & 0xFFFF) & 0xFFB8u);
            return true;
        case 0x04000100: case 0x04000104: case 0x04000108: case 0x0400010C: {
            Timer& t = timers[cpu][(addr - 0x04000100) / 4];
            eventDirty = true;                                  // la prochaine echeance a peut-etre change
            if (mask & 0xFFFF) t.reload = uint16_t(mergeBits(t.reload, v, mask & 0xFFFF));
            if (mask & 0xFFFF0000u) {
                bool wasOn = (t.ctl & 0x80) != 0;
                t.ctl = uint16_t(mergeBits(uint32_t(t.ctl) << 16, v, mask & 0xFFFF0000u) >> 16);
                if (!wasOn && (t.ctl & 0x80)) { t.counter = t.reload; t.accum = 0; }   // demarrage
            }
            return true;
        }
        case 0x04000134:
            if (cpu != 1) return false;
            if (mask & 0xFFFF) rcnt7 = mergeBits(rcnt7, v, mask & 0xFFFF);
            return true;
        case 0x04000180: {                                     // IPCSYNC
            if (!(mask & 0xFFFF)) return true;
            uint32_t nv = mergeBits(ipcSyncRead(cpu), v, mask & 0xFFFF);
            uint32_t out = (nv >> 8) & 0xF;
            bool en = (nv >> 14) & 1;
            if (cpu == 0) { ipcSyncOut9 = out; ipcIrqEnable9 = en; }
            else          { ipcSyncOut7 = out; ipcIrqEnable7 = en; }
            bool otherEn = cpu == 0 ? ipcIrqEnable7 : ipcIrqEnable9;
            if ((v & mask & 0x2000) && otherEn) raise(1 - cpu, IRQ_IPCSYNC);   // bit 13 : envoyer une IRQ
            return true;
        }
        case 0x040001A0:
            if (mask & 0xFFFF) {
                auxSpiCnt = uint16_t(mergeBits(auxSpiCnt, v, mask & 0xFFFF) & 0xE043u);
                if (!(auxSpiCnt & 0x8000)) spiCmd = -1;                          // desactive : la commande est abandonnee
            }
            if ((mask & 0x00FF0000u) && (auxSpiCnt & 0xA000) == 0xA000)          // octet envoye a la puce de sauvegarde
                spiData = backupTransfer(uint8_t(v >> 16), (auxSpiCnt & 0x40) != 0);
            return true;
        case 0x04000138:                                        // RTC (ARM7)
            if (cpu != 1) return false;
            if (mask & 0xFF) rtcWrite(uint16_t(mergeBits(rtcIo, v, mask & 0xFFFF)));
            return true;
        case 0x040001C0:                                        // SPICNT + SPIDATA (ARM7)
            if (cpu != 1) return false;
            if (mask & 0xFFFF) {
                const uint16_t old = spiCnt;
                spiCnt = uint16_t(mergeBits(spiCnt, v, mask & 0xFFFF) & 0xCF03u);
                // bus coupe ou autre peripherique choisi : la commande en cours est abandonnee
                if (!(spiCnt & 0x8000) || ((old ^ spiCnt) & 0x0300)) spiRelease();
            }
            if ((mask & 0x00FF0000u) && (spiCnt & 0x8000)) {     // ecrire SPIDATA envoie un octet
                spiOut = spiTransfer(uint8_t(v >> 16));
                if (spiCnt & 0x4000) raise(1, IRQ_SPI);
            }
            return true;
        case 0x040001A4:
            romCtrl = mergeBits(romCtrl, v, mask) & 0x7F7FFFFFu;
            if ((v & mask & 0x80000000u)) cardStart();                  // bit 31 : demarre le transfert
            return true;
        case 0x040001A8: case 0x040001AC: {
            int base = (addr == 0x040001A8) ? 0 : 4;
            for (int i = 0; i < 4; i++)
                if ((mask >> (8 * i)) & 0xFF) cardCmd[base + i] = uint8_t(v >> (8 * i));
            return true;
        }
        case 0x04000204:
            if (cpu == 0 && (mask & 0xFFFF)) exmemcnt = uint16_t(mergeBits(exmemcnt, v, mask & 0xFFFF));
            return true;
        case 0x04000184:
            if (mask & 0xFFFF) fifoCntWrite(cpu, v & 0xFFFF);
            return true;
        case 0x04000188:
            if (mask == 0xFFFFFFFFu) fifoSend(cpu, v);          // seules les ecritures 32 bits sont valides
            return true;
        case 0x04000208: irq[cpu].ime = mergeBits(irq[cpu].ime, v, mask) & 1; return true;
        case 0x04000210: irq[cpu].ie = mergeBits(irq[cpu].ie, v, mask); return true;
        case 0x04000214:                                                          // ecrire 1 acquitte
            irq[cpu].iflag &= ~(v & mask);
            if (cpu == 0 && (gxIrqMode == 1 || gxIrqMode == 2)) raise(0, IRQ_GXFIFO);   // condition toujours vraie : l'IRQ se releve
            return true;
        case 0x040000B0: case 0x040000B4: case 0x040000B8:          // DMA
        case 0x040000BC: case 0x040000C0: case 0x040000C4:
        case 0x040000C8: case 0x040000CC: case 0x040000D0:
        case 0x040000D4: case 0x040000D8: case 0x040000DC: {
            uint32_t o = addr - 0x040000B0;
            dmaWrite(cpu, int(o / 12), int((o % 12) / 4), v, mask);
            return true;
        }
        case 0x040000E0: case 0x040000E4: case 0x040000E8: case 0x040000EC: {   // DMAxFILL (ARM9)
            if (cpu != 0) return false;
            uint32_t& r = dmaFill[(addr - 0x040000E0) / 4];
            r = mergeBits(r, v, mask);
            return true;
        }
        case 0x04000280:                                        // DIVCNT
            if (cpu != 0) return false;
            if (mask & 0xFFFF) { divCnt = uint16_t((divCnt & ~3u) | (v & 3u)); divCompute(); }
            return true;
        case 0x04000290: case 0x04000294: case 0x04000298: case 0x0400029C: {   // dividende / diviseur
            if (cpu != 0) return false;
            int64_t& reg = (addr < 0x04000298) ? divNumer : divDenom;
            uint64_t u = uint64_t(reg);
            if (addr & 4) u = (u & 0xFFFFFFFFull) | (uint64_t(mergeBits(uint32_t(u >> 32), v, mask)) << 32);
            else          u = (u & 0xFFFFFFFF00000000ull) | mergeBits(uint32_t(u), v, mask);
            reg = int64_t(u);
            divCompute();
            return true;
        }
        case 0x040002A0: case 0x040002A4: case 0x040002A8: case 0x040002AC:
            return cpu == 0;                                    // resultats : lecture seule
        case 0x040002B0:                                        // SQRTCNT
            if (cpu != 0) return false;
            if (mask & 0xFFFF) { sqrtCnt = uint16_t(v & 1); sqrtCompute(); }
            return true;
        case 0x040002B4:
            return cpu == 0;
        case 0x040002B8: case 0x040002BC: {                     // SQRT_PARAM
            if (cpu != 0) return false;
            uint64_t u = sqrtParam;
            if (addr & 4) u = (u & 0xFFFFFFFFull) | (uint64_t(mergeBits(uint32_t(u >> 32), v, mask)) << 32);
            else          u = (u & 0xFFFFFFFF00000000ull) | mergeBits(uint32_t(u), v, mask);
            sqrtParam = u;
            sqrtCompute();
            return true;
        }
        case 0x04000600:                                        // GXSTAT : seul le mode d'IRQ (bits 30-31) est retenu
            if (cpu != 0) return false;
            if (mask & 0xC0000000u) {
                gxIrqMode = uint8_t((v >> 30) & 3);
                // La file de commandes 3D n'est pas emulee : elle est toujours vide, donc la condition
                // "moins qu'a moitie pleine" (mode 1) ou "vide" (mode 2) est vraie des qu'on la demande.
                if (gxIrqMode == 1 || gxIrqMode == 2) raise(0, IRQ_GXFIFO);
            }
            return true;
        case 0x04000240:                                        // VRAMCNT_A a D
            if (cpu != 0) return false;
            for (int i = 0; i < 4; i++) if ((mask >> (8 * i)) & 0xFF) vramCnt[i] = uint8_t((v >> (8 * i)) & 0x9F);
            rebuildVramMap();
            return true;
        case 0x04000244:                                        // VRAMCNT_E a G, WRAMCNT
            if (cpu != 0) return false;
            for (int i = 0; i < 3; i++) if ((mask >> (8 * i)) & 0xFF) vramCnt[4 + i] = uint8_t((v >> (8 * i)) & 0x9F);
            if (mask & 0xFF000000u) wramCnt = uint8_t((v >> 24) & 3);
            rebuildVramMap();
            return true;
        case 0x04000248:                                        // VRAMCNT_H, VRAMCNT_I
            if (cpu != 0) return false;
            if (mask & 0xFF)   vramCnt[7] = uint8_t(v & 0x9F);
            if (mask & 0xFF00) vramCnt[8] = uint8_t((v >> 8) & 0x9F);
            rebuildVramMap();
            return true;
        case 0x04000304:                                        // POWCNT1 (ARM9) / POWCNT2 (ARM7)
            if (mask & 0xFFFF) {
                if (cpu == 0) powcnt1 = uint16_t(mergeBits(powcnt1, v, mask & 0xFFFF) & 0x820F);
                else          powcnt2 = uint16_t(mergeBits(powcnt2, v, mask & 0xFFFF) & 0x0003);
            }
            return true;
        case 0x04000300:                                       // POSTFLG + HALTCNT (ARM7)
            if (cpu == 1 && (mask & 0xFF00) && (((v >> 14) & 3) == 2)) haltRequest[1] = true;
            return true;
        default: break;
    }
    if (cpu == 0 && addr < 0x04001070 && (addr & 3) == 0) {                 // registres des moteurs 2D
        if (addr >= 0x04000000 && addr < 0x04000070 && addr != 0x04000004) {
            uint32_t& r = eng[0][(addr - 0x04000000) / 4];
            r = mergeBits(r, v, mask);
            return true;
        }
        if (addr >= 0x04001000 && addr < 0x04001070) {
            uint32_t& r = eng[1][(addr - 0x04001000) / 4];
            r = mergeBits(r, v, mask);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- ARM9

Bus9::Bus9(System& s) : sys(s) {
    memGen = &sys.memGen;
    irqCtl = &sys.irq[0];
    haltFlag = &sys.haltRequest[0];
    // Vecteur d'IRQ emule (a 0xFFFF0018), comme le vrai BIOS : sauvegarde les registres, lit l'adresse
    // du gestionnaire du jeu dans la DTCM (base + 0x3FFC), l'appelle, puis restaure et revient.
    const uint32_t code[] = {
        0xE92D500F,   // 18: STMFD sp!, {r0-r3, r12, lr}
        0xEE190F11,   // 1C: MRC p15, 0, r0, c9, c1, 0     (registre de la DTCM)
        0xE1A00620,   // 20: MOV r0, r0, LSR #12
        0xE1A00600,   // 24: MOV r0, r0, LSL #12           (base de la DTCM)
        0xE2800901,   // 28: ADD r0, r0, #0x4000
        0xE28FE000,   // 2C: ADD lr, pc, #0                (adresse de retour = 0x34)
        0xE510F004,   // 30: LDR pc, [r0, #-4]             (gestionnaire du jeu)
        0xE8BD500F,   // 34: LDMFD sp!, {r0-r3, r12, lr}
        0xE25EF004,   // 38: SUBS pc, lr, #4               (retour de l'interruption)
    };
    for (unsigned i = 0; i < sizeof code / sizeof code[0]; i++) putWord(bios, 0x18 + 4 * i, code[i]);
}

void Bus9::configureTcm(uint32_t control, uint32_t dtcmReg, uint32_t itcmReg) {
    // Taille de zone = 512 << champ (bits 1 a 5), valable a partir de la valeur 3 (4 Ko).
    auto regionSize = [](uint32_t reg) -> uint64_t {
        uint32_t field = (reg >> 1) & 0x1F;
        return field >= 3 ? (uint64_t(512) << field) : 0;
    };
    sys.memGen++;                                                // la carte memoire de l'ARM9 change
    dtcmOn = (control & (1u << 16)) != 0;
    itcmOn = (control & (1u << 18)) != 0;
    dtcmBase = dtcmReg & 0xFFFFF000u;
    dtcmRegion = regionSize(dtcmReg);
    itcmRegion = regionSize(itcmReg);
}

uint8_t* Bus9::ptr(uint32_t addr) {
    if (dtcmOn && addr >= dtcmBase && uint64_t(addr) < dtcmBase + dtcmRegion)
        return &dtcm[(addr - dtcmBase) & (dtcm.size() - 1)];     // DTCM : 16 Ko repetes
    if (itcmOn && uint64_t(addr) < itcmRegion)
        return &itcm[addr & (itcm.size() - 1)];                  // ITCM : 32 Ko repetes
    if ((addr >> 12) == 0xFFFF0)
        return &bios[addr & 0xFFF];                              // vecteurs d'exception emules

    switch (addr >> 24) {
        case 0x02:                                               // RAM principale (4 Mo repetes)
            return &sys.mainRam[addr & (sys.mainRam.size() - 1)];
        case 0x05:                                               // palettes (2 Ko repetes)
            return &sys.palette[addr & 0x7FF];
        case 0x07:                                               // OAM (2 Ko repetes)
            return &sys.oam[addr & 0x7FF];
        case 0x06: {                                             // memoire video
            int region = int((addr >> 21) & 7);
            if (region > 4) return nullptr;
            uint32_t off = addr & 0x1FFFFF;
            if (region == 0) off &= 0x7FFFF;                     // fonds A : 512 Ko
            else if (region == 1 || region == 3) off &= 0x1FFFF; // fonds B, sprites B : 128 Ko
            else if (region == 2) off &= 0x3FFFF;                // sprites A : 256 Ko
            uint32_t pg = off >> 14;
            if (pg >= 41) return nullptr;
            uint8_t* p = sys.vramMap9[region][pg];
            return p ? p + (off & 0x3FFF) : nullptr;
        }
        case 0x03:                                               // WRAM partagee
            switch (sys.wramCnt) {
                case 0: return &sys.sharedWram[addr & 0x7FFF];            // tout pour l'ARM9
                case 1: return &sys.sharedWram[0x4000 + (addr & 0x3FFF)]; // 2e moitie
                case 2: return &sys.sharedWram[addr & 0x3FFF];            // 1ere moitie
                default: return nullptr;                                  // tout pour l'ARM7
            }
        default:
            return nullptr;
    }
}

bool Bus9::ioRead(uint32_t addr, int size, uint32_t& value) {
    if ((addr >> 25) == 4 || (addr >> 24) == 0x0A) { value = 0xFFFFFFFFu; return true; }   // slot GBA vide
    uint32_t w;
    if (!sys.ioRead32(0, addr & ~3u, w)) return false;
    value = ioExtract(w, addr, size);
    return true;
}

bool Bus9::ioWrite(uint32_t addr, int size, uint32_t value) {
    if ((addr >> 25) == 4 || (addr >> 24) == 0x0A) return true;   // ecritures dans le slot GBA : ignorees
    uint32_t v, mask;
    ioPlace(addr, size, value, v, mask);
    return sys.ioWrite32(0, addr & ~3u, v, mask);
}

// ---------------------------------------------------------------- ARM7

Bus7::Bus7(System& s) : sys(s) {
    sys.spu.bus = this;                                          // le son lit ses echantillons dans la memoire de l'ARM7
    sys.spu.power = &sys.powcnt2;
    memGen = &sys.memGen;
    irqCtl = &sys.irq[1];
    haltFlag = &sys.haltRequest[1];
    // Meme vecteur d'IRQ emule que sur l'ARM9, mais le gestionnaire du jeu est a l'adresse fixe 0x0380FFFC
    // (lue via 0x04000000 - 4, qui est un miroir de la WRAM de l'ARM7).
    const uint32_t code[] = {
        0xE92D500F,   // 18: STMFD sp!, {r0-r3, r12, lr}
        0xE3A00301,   // 1C: MOV r0, #0x04000000
        0xE28FE000,   // 20: ADD lr, pc, #0                (adresse de retour = 0x28)
        0xE510F004,   // 24: LDR pc, [r0, #-4]             (gestionnaire du jeu)
        0xE8BD500F,   // 28: LDMFD sp!, {r0-r3, r12, lr}
        0xE25EF004,   // 2C: SUBS pc, lr, #4
    };
    for (unsigned i = 0; i < sizeof code / sizeof code[0]; i++) putWord(bios, 0x18 + 4 * i, code[i]);
}

uint8_t* Bus7::ptr(uint32_t addr) {
    switch (addr >> 24) {
        case 0x00:
            return addr < bios.size() ? &bios[addr] : nullptr;
        case 0x02:
            return &sys.mainRam[addr & (sys.mainRam.size() - 1)];
        case 0x06: {                                                        // VRAM allouee a l'ARM7 (banques C, D)
            uint32_t off = addr & 0x3FFFF;
            uint8_t* p = sys.vramMap7[off >> 14];
            return p ? p + (off & 0x3FFF) : nullptr;
        }
        case 0x03:
            if (addr >= 0x03800000) return &sys.arm7Wram[addr & 0xFFFF];   // WRAM ARM7 (64 Ko repetes)
            switch (sys.wramCnt) {                                          // WRAM partagee
                case 0: return &sys.arm7Wram[addr & 0xFFFF];                // rien d'alloue : on voit la WRAM ARM7
                case 1: return &sys.sharedWram[addr & 0x3FFF];              // 1ere moitie
                case 2: return &sys.sharedWram[0x4000 + (addr & 0x3FFF)];   // 2e moitie
                default: return &sys.sharedWram[addr & 0x7FFF];             // tout pour l'ARM7
            }
        default:
            return nullptr;
    }
}

bool Bus7::ioRead(uint32_t addr, int size, uint32_t& value) {
    if ((addr >> 25) == 4 || (addr >> 24) == 0x0A) { value = 0xFFFFFFFFu; return true; }   // slot GBA vide
    uint32_t w;
    if (!sys.ioRead32(1, addr & ~3u, w)) return false;
    value = ioExtract(w, addr, size);
    return true;
}

bool Bus7::ioWrite(uint32_t addr, int size, uint32_t value) {
    if ((addr >> 25) == 4 || (addr >> 24) == 0x0A) return true;   // ecritures dans le slot GBA : ignorees
    uint32_t v, mask;
    ioPlace(addr, size, value, v, mask);
    return sys.ioWrite32(1, addr & ~3u, v, mask);
}
