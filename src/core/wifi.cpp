#include "core/wifi.hpp"
#include <cstring>

// Registres utilises (adresses dans le bloc de 4 Ko, cf. GBATEK "DS Wifi")
namespace {
enum : uint32_t {
    W_ID = 0x000, W_MODE_RST = 0x004, W_MODE_WEP = 0x006, W_IF = 0x010, W_IE = 0x012,
    W_RXBUF_BEGIN = 0x050, W_RXBUF_END = 0x052, W_TRX_POWER = 0x034, W_POWER_US = 0x036, W_POWER_TX = 0x038,
    W_POWERSTATE = 0x03C, W_POWERFORCE = 0x040, W_RANDOM = 0x044, W_POWERDOWNCTRL = 0x048,
    W_TXREQ_RESET = 0x0AC, W_TXREQ_SET = 0x0AE, W_TXREQ_READ = 0x0B0, W_TXBUSY = 0x0B6,
    W_US_COUNTCNT = 0x0E8, W_US_COUNT0 = 0x0F8, W_US_COUNT3 = 0x0FE,
    W_BB_CNT = 0x158, W_BB_WRITE = 0x15A, W_BB_READ = 0x15C, W_BB_BUSY = 0x15E, W_RF_BUSY = 0x180,
    W_RF_PINS = 0x19C, W_RF_STATUS = 0x214, W_IF_SET = 0x21C,
};
}

Wifi::Wifi() {
    io(W_ID) = 0xC340;                                         // puce de la DS Lite (voir le firmware fabrique)
    for (uint32_t r = 0x018; r < 0x01E; r += 2) io(r) = 0xFFFF;
    for (uint32_t r = 0x020; r < 0x026; r += 2) io(r) = 0xFFFF;
    io(W_POWER_US) = 0x0001;
    io(W_POWERSTATE) = 0x0200;                                 // bit 9 : recepteur eteint
    io(W_RXBUF_BEGIN) = 0x4000;
    io(W_RXBUF_END) = 0x4800;
    setStatus(9);
    bb_[0x00] = 0x6D;                                          // identifiant de la puce baseband
}

void Wifi::setStatus(int s) {
    static const uint16_t kPins[10] = { 0x04, 0x84, 0, 0x46, 0, 0x84, 0x87, 0, 0x46, 0x04 };
    io(W_RF_STATUS) = uint16_t(s);
    io(W_RF_PINS) = kPins[s];
}

void Wifi::setIrq(int bit) { io(W_IF) = uint16_t(io(W_IF) | (1u << bit)); }

bool Wifi::consumeIrqEdge() {
    const bool line = irq();
    const bool edge = line && !irqLine_;
    irqLine_ = line;
    return edge;
}

// Alimentation du recepteur : demandee par W_POWERFORCE, W_MODE_RST bit 0, W_POWERSTATE ou W_POWERDOWNCTRL
void Wifi::updatePower(uint64_t cycles) {
    const bool on = (io(W_POWERSTATE) & 0x0200) == 0 || powerOnAt_ != 0;
    bool want = on;
    if (io(W_POWERFORCE) & 0x8000) want = (io(W_POWERFORCE) & 1) == 0;
    else if (!(io(W_MODE_RST) & 1)) want = false;
    else if ((io(W_POWERSTATE) & 0x0202) == 0x0202 || (io(W_POWERDOWNCTRL) & 2)) want = true;
    else if ((io(W_POWERSTATE) & 0x0201) == 0x0001) want = (io(W_POWERDOWNCTRL) & 1) != 0;
    if (want == on) return;
    if (want) {
        io(W_TRX_POWER) = 1;
        setStatus(1);
        io(W_POWERSTATE) = uint16_t(io(W_POWERSTATE) | 0x0100);
        powerOnAt_ = cycles + 2048ull * 33513982ull / 1000000ull;     // ~2 ms
        setIrq(11);
    } else {
        io(W_TRX_POWER) = 0;
        setStatus(9);
        io(W_POWERSTATE) = uint16_t((io(W_POWERSTATE) & ~0x0101u) | 0x0200);
        powerOnAt_ = 0;
    }
}

void Wifi::update(uint64_t cycles) {
    if (powerOnAt_ && cycles >= powerOnAt_) {
        powerOnAt_ = 0;
        io(W_POWERSTATE) = 0;                                  // recepteur pret
        setStatus(1);
    }
}

uint16_t Wifi::read(uint32_t addr, uint64_t cycles) {
    update(cycles);
    uint32_t a = addr & 0x7FFE;
    uint16_t v;
    if (a >= 0x4000 && a < 0x6000) v = uint16_t(ram_[a & 0x1FFE] | (ram_[(a & 0x1FFE) + 1] << 8));
    else if (a >= 0x2000 && a < 0x4000) v = 0xFFFF;
    else {
        const uint32_t r = a & 0xFFF;
        if (r == W_RANDOM) {
            random_ = uint16_t((random_ & 1) ^ (((random_ & 0x3FF) << 1) | (random_ >> 10)));
            v = random_ & 0x7FF;
        } else if (r >= W_US_COUNT0 && r <= W_US_COUNT3) {
            const uint64_t us = (io(W_US_COUNTCNT) & 1) ? microseconds(cycles) - usBase_ : usFrozen_;
            v = uint16_t(us >> (8 * (r - W_US_COUNT0)));
        } else if (r == W_BB_READ) {
            v = bb_[io(W_BB_CNT) & 0xFF];
        } else if (r == W_BB_BUSY || r == W_RF_BUSY || r == W_TXBUSY) {
            v = 0;
        } else {
            v = io(r);
        }
    }
    if (logCount < kLogMax) log[logCount++] = { addr, v, false };
    return v;
}

void Wifi::write(uint32_t addr, uint16_t v, uint64_t cycles) {
    update(cycles);
    if (logCount < kLogMax) log[logCount++] = { addr, v, true };
    uint32_t a = addr & 0x7FFE;
    if (a >= 0x4000 && a < 0x6000) { ram_[a & 0x1FFE] = uint8_t(v); ram_[(a & 0x1FFE) + 1] = uint8_t(v >> 8); return; }
    if (a >= 0x1000) return;
    const uint32_t r = a & 0xFFF;
    switch (r) {
    case W_ID: case W_RF_STATUS: case W_RF_PINS: case W_TXREQ_READ: case W_BB_READ: return;   // lecture seule
    case W_MODE_RST:
        io(r) = uint16_t(v & 1);
        if (v & 0x4000) { io(W_RXBUF_BEGIN) = 0x4000; io(W_RXBUF_END) = 0x4800; }
        updatePower(cycles);
        return;
    case W_MODE_WEP: io(r) = uint16_t(v & 0x7F); updatePower(cycles); return;
    case W_IF: io(W_IF) = uint16_t(io(W_IF) & ~v); return;     // 1 = acquitter
    case W_IF_SET: io(W_IF) = uint16_t(io(W_IF) | v); return;
    case W_POWER_US: io(r) = uint16_t(v & 3); return;
    case W_POWER_TX: io(r) = uint16_t(v & 3); return;
    case W_POWERSTATE:
        io(r) = uint16_t((io(r) & 0x0300) | (v & 0x0003));
        updatePower(cycles);
        return;
    case W_POWERFORCE: io(r) = uint16_t(v & 0x8001); updatePower(cycles); return;
    case W_POWERDOWNCTRL: io(r) = uint16_t(v & 3); updatePower(cycles); return;
    case W_TXREQ_RESET: io(W_TXREQ_READ) = uint16_t(io(W_TXREQ_READ) & ~v); return;
    case W_TXREQ_SET: io(W_TXREQ_READ) = uint16_t(io(W_TXREQ_READ) | (v & 0x0F)); return;
    case W_US_COUNTCNT: {
        const bool wasOn = io(r) & 1, on = v & 1;
        if (wasOn && !on) usFrozen_ = microseconds(cycles) - usBase_;
        if (!wasOn && on) usBase_ = microseconds(cycles) - usFrozen_;
        io(r) = uint16_t(v & 1);
        return;
    }
    case W_BB_CNT:
        io(r) = v;
        if ((v >> 12) == 5) {                                  // ecriture dans un registre de la puce baseband
            const uint8_t idx = uint8_t(v & 0xFF);
            if (idx != 0x00 && idx != 0x5D && idx != 0x64) bb_[idx] = uint8_t(io(W_BB_WRITE));
        }
        return;
    default:
        if (r >= W_US_COUNT0 && r <= W_US_COUNT3) {            // reglage du compteur
            uint64_t us = (io(W_US_COUNTCNT) & 1) ? microseconds(cycles) - usBase_ : usFrozen_;
            const int sh = int(8 * (r - W_US_COUNT0));
            us = (us & ~(0xFFFFull << sh)) | (uint64_t(v) << sh);
            usFrozen_ = us;
            usBase_ = microseconds(cycles) - us;
            return;
        }
        io(r) = v;
        return;
    }
}
