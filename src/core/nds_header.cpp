#include "core/nds_header.hpp"
#include <cstring>

static uint32_t rd32(const std::vector<uint8_t>& d, size_t off) {
    uint32_t v;
    std::memcpy(&v, &d[off], 4);  // la DS est little-endian (comme x86/ARM64 hôte)
    return v;
}
static uint16_t rd16(const std::vector<uint8_t>& d, size_t off) {
    uint16_t v;
    std::memcpy(&v, &d[off], 2);
    return v;
}

// CRC-16 "MODBUS" utilisé par le header DS : init 0xFFFF, poly 0xA001.
static uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

static std::string readStr(const std::vector<uint8_t>& d, size_t off, size_t len) {
    std::string s(reinterpret_cast<const char*>(&d[off]), len);
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
}

bool parseNdsHeader(const std::vector<uint8_t>& rom, NdsHeader& h, std::string& err) {
    if (rom.size() < 0x200) {
        err = "fichier trop petit pour contenir un header NDS";
        return false;
    }
    h.title = readStr(rom, 0x000, 12);
    h.gameCode = readStr(rom, 0x00C, 4);
    h.makerCode = readStr(rom, 0x010, 2);
    h.unitCode = rom[0x012];
    h.romVersion = rom[0x01E];

    h.arm9RomOffset = rd32(rom, 0x020);
    h.arm9Entry     = rd32(rom, 0x024);
    h.arm9RamAddr   = rd32(rom, 0x028);
    h.arm9Size      = rd32(rom, 0x02C);

    h.arm7RomOffset = rd32(rom, 0x030);
    h.arm7Entry     = rd32(rom, 0x034);
    h.arm7RamAddr   = rd32(rom, 0x038);
    h.arm7Size      = rd32(rom, 0x03C);

    h.totalRomSize = rd32(rom, 0x080);
    h.headerSize   = rd32(rom, 0x084);

    h.headerCrcStored = rd16(rom, 0x15E);
    h.headerCrcComputed = crc16(rom.data(), 0x15E);

    // Vérifications de cohérence : les binaires doivent tenir dans le fichier.
    if (uint64_t(h.arm9RomOffset) + h.arm9Size > rom.size() ||
        uint64_t(h.arm7RomOffset) + h.arm7Size > rom.size()) {
        err = "les binaires ARM9/ARM7 dépassent la taille du fichier";
        return false;
    }
    return true;
}
