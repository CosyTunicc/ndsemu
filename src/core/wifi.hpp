#pragma once
// Puce Wi-Fi de la DS (ARM7, 0x04800000), reduite a ce que les jeux verifient au demarrage du sans-fil : identifiant,
// registres relus, memoire de 8 Ko, puce "baseband" (BB) et radio (RF), compteur en microsecondes, mise sous
// tension du recepteur (avec l'interruption 11 au bout de ~2 ms). Aucune trame n'est jamais emise ni recue : la DS
// se comporte comme une console sans aucun reseau a portee. C'est ce qu'il faut pour que les jeux qui allument le
// Wi-Fi (Pokemon Diamant au menu "Continuer", par exemple) ne s'arretent pas sur "Erreur de communication".
#include <cstdint>

class Wifi {
public:
    Wifi();
    uint16_t read(uint32_t addr, uint64_t cycles);
    void write(uint32_t addr, uint16_t v, uint64_t cycles);
    void update(uint64_t cycles);             // evenements dus au temps (fin de la mise sous tension)
    bool irq() const { return (io_[0x010 / 2] & io_[0x012 / 2]) != 0; }   // W_IF & W_IE : ligne d'interruption (IRQ 24)
    bool consumeIrqEdge();                     // vrai si la ligne vient de passer a 1 (a signaler au processeur)

    // Journal des acces (diagnostic, --rapport)
    struct LogEntry { uint32_t addr; uint16_t value; bool write; };
    static constexpr int kLogMax = 400;
    LogEntry log[kLogMax];
    int logCount = 0;


private:
    uint16_t& io(uint32_t reg) { return io_[(reg & 0xFFF) >> 1]; }
    uint64_t microseconds(uint64_t cycles) const { return cycles * 1000000ull / 33513982ull; }
    void setStatus(int s);
    void updatePower(uint64_t cycles);
    void setIrq(int bit);

    uint16_t io_[0x800] = {};
    uint8_t ram_[0x2000] = {};
    uint8_t bb_[0x100] = {};
    uint64_t usBase_ = 0;                      // compteur microseconde = microseconds(cycles) - usBase_
    uint64_t usFrozen_ = 0;                    // valeur gardee quand le compteur est arrete
    uint64_t powerOnAt_ = 0;                   // 0 : pas de mise sous tension en cours, sinon instant (cycles) de la fin
    uint16_t random_ = 0x7EC;
    bool irqLine_ = false;
};
