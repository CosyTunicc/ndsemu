#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Infos utiles du header d'une ROM .nds (les 0x200 premiers octets).
// Offsets documentés dans GBATEK ("DS Cartridge Header").
struct NdsHeader {
    std::string title;      // 0x000, 12 octets
    std::string gameCode;   // 0x00C, 4 octets
    std::string makerCode;  // 0x010, 2 octets
    uint8_t unitCode = 0;   // 0x012 (0 = DS, 2 = DSi enhanced, 3 = DSi only)
    uint8_t romVersion = 0; // 0x01E

    uint32_t arm9RomOffset = 0, arm9Entry = 0, arm9RamAddr = 0, arm9Size = 0;
    uint32_t arm7RomOffset = 0, arm7Entry = 0, arm7RamAddr = 0, arm7Size = 0;

    uint32_t totalRomSize = 0;  // 0x080
    uint32_t headerSize = 0;    // 0x084 (normalement 0x4000)
    uint16_t headerCrcStored = 0;   // 0x15E
    uint16_t headerCrcComputed = 0; // calculé sur 0x000..0x15D
    bool crcOk() const { return headerCrcStored == headerCrcComputed; }
};

// Retourne false si les données sont trop petites ou incohérentes.
bool parseNdsHeader(const std::vector<uint8_t>& rom, NdsHeader& out, std::string& err);
