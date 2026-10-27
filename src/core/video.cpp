#include "core/video.hpp"
#include <algorithm>
#include <cstdio>

namespace {

// ------------------------------------------------------------------ acces a la memoire video

// Octet de VRAM, vu par un moteur : region 0 = fonds A, 1 = fonds B, 2 = sprites A, 3 = sprites B, 4 = acces direct.
uint8_t vramByte(const System& s, int region, uint32_t off) {
    static const uint32_t kMask[4] = {0x7FFFF, 0x1FFFF, 0x3FFFF, 0x1FFFF};
    if (region < 4) off &= kMask[region];
    uint32_t pg = off >> 14;
    if (pg >= 41) return 0;
    const uint8_t* p = s.vramMap9[region][pg];
    return p ? p[off & 0x3FFF] : uint8_t(0);
}

uint16_t vramHalf(const System& s, int region, uint32_t off) {
    return uint16_t(vramByte(s, region, off) | (vramByte(s, region, off + 1) << 8));
}

// Couleur 15 bits (BGR) -> 0x00RRGGBB
uint32_t rgb555(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

uint16_t palEntry(const System& s, uint32_t byteOffset) {
    return uint16_t(s.palette[byteOffset & 0x7FF] | (s.palette[(byteOffset + 1) & 0x7FF] << 8));
}

// Palettes etendues : une "fente" fait 8 Ko = 16 palettes de 256 couleurs (512 octets chacune).
// Fonds du moteur A : banque E (4 fentes) ou F / G (2 fentes chacune) en mode 4 ; moteur B : banque H en mode 2.
const uint8_t* extBgPalSlot(const System& s, int e, int slot) {
    if (e == 0) {
        if ((s.vramCnt[4] & 0x87) == 0x84 && !s.vram[4].empty()) return s.vram[4].data() + size_t(slot) * 0x2000;
        for (int b = 5; b <= 6; b++) {
            const uint8_t c = s.vramCnt[b];
            if ((c & 0x87) != 0x84 || s.vram[b].empty()) continue;
            const int first = (c & 8) ? 2 : 0;                  // OFS : fentes 0-1 ou 2-3
            if (slot >= first && slot < first + 2) return s.vram[b].data() + size_t(slot - first) * 0x2000;
        }
        return nullptr;
    }
    if ((s.vramCnt[7] & 0x87) == 0x82 && !s.vram[7].empty()) return s.vram[7].data() + size_t(slot) * 0x2000;
    return nullptr;
}

// Sprites : moteur A = banque F ou G en mode 5, moteur B = banque I en mode 3.
const uint8_t* extObjPal(const System& s, int e) {
    if (e == 0) {
        for (int b = 5; b <= 6; b++)
            if ((s.vramCnt[b] & 0x87) == 0x85 && !s.vram[b].empty()) return s.vram[b].data();
        return nullptr;
    }
    if ((s.vramCnt[8] & 0x87) == 0x83 && !s.vram[8].empty()) return s.vram[8].data();
    return nullptr;
}

uint16_t extColor(const uint8_t* pal, uint32_t palNum, uint32_t idx) {
    if (!pal) return 0;                                         // pas de banque : le materiel lit du noir
    const uint32_t off = palNum * 512 + idx * 2;
    return uint16_t(pal[off] | (pal[off + 1] << 8));
}

// Type d'un fond selon le mode : 0 = en tuiles, 1 = rotation/zoom (entrees de 8 bits),
// 2 = etendu (entrees de 16 bits ou bitmap), -1 = absent.
int bgKind(uint32_t mode, int n) {
    if (n < 2) return mode <= 6 ? 0 : -1;
    if (n == 2) {
        switch (mode) {
            case 0: case 1: case 3: return 0;
            case 2: case 4: return 1;
            case 5: return 2;
            default: return -1;
        }
    }
    switch (mode) {
        case 0: return 0;
        case 1: case 2: return 1;
        case 3: case 4: case 5: return 2;
        default: return -1;
    }
}

// ------------------------------------------------------------------ un pixel de fond ou de sprite
struct Pixel { bool opaque = false; bool semi = false; uint32_t color = 0; int prio = 4; uint8_t alpha = 0; };   // alpha : sprites bitmap (1..15), 0 = EVA / EVB

// Pixel de ligne : 0 = transparent ; sinon bit 24 leve + couleur 0xRRGGBB.
constexpr uint32_t kOpq = 0x01000000u;

// Ce dont le rendu d'un moteur a besoin, prepare une fois par image.
struct Ctx {
    const System& s;
    int e;
    uint32_t dispcnt;
    uint32_t pal[512];          // palette du moteur deja convertie : [0..255] fonds, [256..511] sprites
    Ctx(const System& sys, int eng) : s(sys), e(eng), dispcnt(sys.eng[eng][0]) {
        const uint32_t base = eng == 0 ? 0u : 0x400u;
        for (uint32_t i = 0; i < 512; i++) pal[i] = rgb555(palEntry(sys, base + i * 2));
    }
};

// Pointeur vers un octet de VRAM vu par un moteur (nullptr si rien n'est mappe).
inline const uint8_t* vramPtr(const System& s, int region, uint32_t off) {
    static const uint32_t kMask[4] = {0x7FFFF, 0x1FFFF, 0x3FFFF, 0x1FFFF};
    if (region < 4) off &= kMask[region];
    const uint32_t pg = off >> 14;
    if (pg >= 41) return nullptr;
    const uint8_t* p = s.vramMap9[region][pg];
    return p ? p + (off & 0x3FFF) : nullptr;
}

// Fond en tuiles : une ligne entiere (256 pixels), par tranches d'une tuile.
void renderTextLine(const Ctx& c, int n, int y, uint32_t* out) {
    const System& s = c.s;
    const int e = c.e;
    const uint32_t dispcnt = c.dispcnt;
    const uint32_t bgcnt = (s.eng[e][2 + (n >> 1)] >> (16 * (n & 1))) & 0xFFFF;
    const uint32_t scroll = s.eng[e][4 + n];
    const uint32_t hofs = scroll & 0x1FF, vofs = (scroll >> 16) & 0x1FF;

    uint32_t charBase = ((bgcnt >> 2) & 0xF) * 0x4000;
    uint32_t mapBase = ((bgcnt >> 8) & 0x1F) * 0x800;
    if (e == 0) {                                   // le moteur A a des decalages supplementaires dans DISPCNT
        charBase += ((dispcnt >> 24) & 7) * 0x10000;
        mapBase += ((dispcnt >> 27) & 7) * 0x10000;
    }
    const bool color256 = (bgcnt >> 7) & 1;
    const uint32_t size = (bgcnt >> 14) & 3;
    const uint32_t w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    const uint32_t py = (uint32_t(y) + vofs) & (h - 1);
    const uint32_t ty = py >> 3, fy0 = py & 7;

    const bool ext = color256 && (dispcnt & 0x40000000u);          // palettes etendues des fonds
    const uint8_t* extPal = nullptr;
    if (ext) {
        int slot = n;
        if (n < 2 && (bgcnt & 0x2000)) slot += 2;                   // BG0 / BG1 : le bit 13 choisit la fente 2 / 3
        extPal = extBgPalSlot(s, e, slot);
    }

    uint32_t x = 0;
    while (x < 256) {
        const uint32_t px = (x + hofs) & (w - 1);
        const uint32_t tx = px >> 3, startFx = px & 7;
        const uint32_t run = (8 - startFx < 256 - x) ? 8 - startFx : 256 - x;
        uint32_t block = 0;                          // bloc de 32 x 32 tuiles contenant ce point
        if (size == 1) block = tx >> 5;
        else if (size == 2) block = ty >> 5;
        else if (size == 3) block = (tx >> 5) + (ty >> 5) * 2;
        const uint8_t* mp = vramPtr(s, e, mapBase + block * 0x800 + (((ty & 31) * 32) + (tx & 31)) * 2);
        const uint32_t entry = mp ? uint32_t(mp[0] | (mp[1] << 8)) : 0;
        const uint32_t tile = entry & 0x3FF;
        const uint32_t fy = (entry & 0x800) ? 7 - fy0 : fy0;          // symetrie verticale
        const bool hflip = (entry & 0x400) != 0;                      // symetrie horizontale
        if (color256) {
            const uint8_t* tp = vramPtr(s, e, charBase + tile * 64 + fy * 8);
            for (uint32_t i = 0; i < run; i++) {
                uint32_t fx = startFx + i;
                if (hflip) fx = 7 - fx;
                const uint32_t idx = tp ? tp[fx] : 0u;
                out[x + i] = !idx ? 0u : (kOpq | (ext ? rgb555(extColor(extPal, entry >> 12, idx)) : c.pal[idx]));
            }
        } else {
            const uint8_t* tp = vramPtr(s, e, charBase + tile * 32 + fy * 4);
            const uint32_t palBase = (entry >> 12) * 16;
            for (uint32_t i = 0; i < run; i++) {
                uint32_t fx = startFx + i;
                if (hflip) fx = 7 - fx;
                const uint32_t b = tp ? tp[fx >> 1] : 0u;
                const uint32_t idx = (fx & 1) ? (b >> 4) : (b & 15);
                out[x + i] = !idx ? 0u : (kOpq | c.pal[palBase + idx]);
            }
        }
        x += run;
    }
}

// Fond 0 en 3D (moteur A) : image dessinee par le moteur 3D, decalee horizontalement par BG0HOFS (avec signe).
// L'opacite du pixel 3D (0..31) est rangee dans les bits 25-29 : la composition s'en sert pour le melange.
void render3dLine(const Ctx& c, int y, uint32_t* out) {
    const uint32_t* src = c.s.gx.frame() + size_t(y) * 256;
    // BG0HOFS est un decalage sur 9 bits avec signe : 0x1D2 = -46 (l'image 3D est poussee de 46 points a droite)
    int hofs = int(c.s.eng[0][4] & 0x1FF);
    if (hofs >= 256) hofs -= 512;
    for (int x = 0; x < 256; x++) {
        const int sx = x + hofs;
        const uint32_t p = (sx >= 0 && sx < 256) ? src[sx] : 0u;
        const uint32_t a = (p >> 24) & 31;
        out[x] = a ? ((a << 25) | kOpq | (p & 0xFFFFFFu)) : 0u;
    }
}

// Fond a rotation / zoom (BG2 et BG3) : entrees de 8 bits, ou etendu (entrees de 16 bits, bitmap 256 couleurs
// ou bitmap en couleurs directes). Le point de depart de l'image est (X, Y) ; chaque pixel a droite ajoute
// (PA, PC), chaque ligne vers le bas ajoute (PB, PD), en virgule fixe 8 bits.
Pixel affineBgPixel(const System& s, int e, int n, int x, int y, bool extended) {
    const uint32_t dispcnt = s.eng[e][0];
    const uint32_t bgcnt = (s.eng[e][2 + (n >> 1)] >> (16 * (n & 1))) & 0xFFFF;
    const int base = (n == 2) ? 8 : 12;
    const int32_t pa = int16_t(s.eng[e][base] & 0xFFFF), pb = int16_t(s.eng[e][base] >> 16);
    const int32_t pc = int16_t(s.eng[e][base + 1] & 0xFFFF), pd = int16_t(s.eng[e][base + 1] >> 16);
    const int32_t rx = int32_t(s.eng[e][base + 2] << 4) >> 4;     // 28 bits signes
    const int32_t ry = int32_t(s.eng[e][base + 3] << 4) >> 4;
    int32_t px = int32_t((int64_t(rx) + int64_t(pb) * y + int64_t(pa) * x) >> 8);
    int32_t py = int32_t((int64_t(ry) + int64_t(pd) * y + int64_t(pc) * x) >> 8);
    const bool wrap = (bgcnt >> 13) & 1;

    Pixel p;
    p.prio = int(bgcnt & 3);
    uint32_t charBase = ((bgcnt >> 2) & 0xF) * 0x4000;
    uint32_t mapBase = ((bgcnt >> 8) & 0x1F) * 0x800;
    if (e == 0) {
        charBase += ((dispcnt >> 24) & 7) * 0x10000;
        mapBase += ((dispcnt >> 27) & 7) * 0x10000;
    }

    if (extended && (bgcnt & 0x80)) {               // bitmap
        static const uint32_t kBw[4] = {128, 256, 512, 512}, kBh[4] = {128, 256, 256, 512};
        const uint32_t bw = kBw[(bgcnt >> 14) & 3], bh = kBh[(bgcnt >> 14) & 3];
        if (wrap) { px &= int32_t(bw - 1); py &= int32_t(bh - 1); }
        else if (px < 0 || py < 0 || px >= int32_t(bw) || py >= int32_t(bh)) return p;
        const uint32_t bmpBase = ((bgcnt >> 8) & 0x1F) * 0x4000 + (e == 0 ? ((dispcnt >> 27) & 7) * 0x10000 : 0u);
        if (bgcnt & 4) {                            // couleurs directes : le bit 15 doit etre a 1
            const uint16_t c = vramHalf(s, e, bmpBase + (uint32_t(py) * bw + uint32_t(px)) * 2);
            if (!(c & 0x8000)) return p;
            p.color = rgb555(c);
        } else {                                    // 256 couleurs, palette standard
            const uint8_t idx = vramByte(s, e, bmpBase + uint32_t(py) * bw + uint32_t(px));
            if (!idx) return p;
            p.color = rgb555(palEntry(s, (e == 0 ? 0u : 0x400u) + idx * 2u));
        }
        p.opaque = true;
        return p;
    }

    const int32_t size = int32_t(128u << ((bgcnt >> 14) & 3));
    if (wrap) { px &= size - 1; py &= size - 1; }
    else if (px < 0 || py < 0 || px >= size || py >= size) return p;
    const uint32_t tx = uint32_t(px) >> 3, ty = uint32_t(py) >> 3, perRow = uint32_t(size) >> 3;
    uint32_t fx = uint32_t(px) & 7, fy = uint32_t(py) & 7;
    uint32_t idx;
    if (extended) {                                 // entrees de 16 bits, comme un fond en tuiles
        const uint32_t entry = vramHalf(s, e, mapBase + (ty * perRow + tx) * 2);
        if (entry & 0x400) fx = 7 - fx;
        if (entry & 0x800) fy = 7 - fy;
        idx = vramByte(s, e, charBase + (entry & 0x3FF) * 64 + fy * 8 + fx);
        if (!idx) return p;
        if (dispcnt & 0x40000000u) p.color = rgb555(extColor(extBgPalSlot(s, e, n), entry >> 12, idx));
        else                       p.color = rgb555(palEntry(s, (e == 0 ? 0u : 0x400u) + idx * 2u));
    } else {                                        // entrees de 8 bits
        const uint32_t tile = vramByte(s, e, mapBase + ty * perRow + tx);
        idx = vramByte(s, e, charBase + tile * 64 + fy * 8 + fx);
        if (!idx) return p;
        p.color = rgb555(palEntry(s, (e == 0 ? 0u : 0x400u) + idx * 2u));
    }
    p.opaque = true;
    return p;
}

// Fond a rotation / etendu : une ligne entiere. Les bitmaps passent par affineBgPixel (rares) ;
// les fonds en tuiles gardent la derniere tuile lue tant que le point reste dedans.
void renderAffineLine(const Ctx& c, int n, int y, bool extended, uint32_t* out) {
    const System& s = c.s;
    const int e = c.e;
    const uint32_t dispcnt = c.dispcnt;
    const uint32_t bgcnt = (s.eng[e][2 + (n >> 1)] >> (16 * (n & 1))) & 0xFFFF;
    if (extended && (bgcnt & 0x80)) {               // bitmap
        for (int x = 0; x < 256; x++) {
            const Pixel p = affineBgPixel(s, e, n, x, y, true);
            out[x] = p.opaque ? (kOpq | p.color) : 0u;
        }
        return;
    }
    const int base = (n == 2) ? 8 : 12;
    const int32_t pa = int16_t(s.eng[e][base] & 0xFFFF), pb = int16_t(s.eng[e][base] >> 16);
    const int32_t pc = int16_t(s.eng[e][base + 1] & 0xFFFF), pd = int16_t(s.eng[e][base + 1] >> 16);
    const int32_t rx = int32_t(s.eng[e][base + 2] << 4) >> 4;     // 28 bits signes
    const int32_t ry = int32_t(s.eng[e][base + 3] << 4) >> 4;
    const bool wrap = (bgcnt >> 13) & 1;
    uint32_t charBase = ((bgcnt >> 2) & 0xF) * 0x4000;
    uint32_t mapBase = ((bgcnt >> 8) & 0x1F) * 0x800;
    if (e == 0) {
        charBase += ((dispcnt >> 24) & 7) * 0x10000;
        mapBase += ((dispcnt >> 27) & 7) * 0x10000;
    }
    const int32_t size = int32_t(128u << ((bgcnt >> 14) & 3));
    const uint32_t perRow = uint32_t(size) >> 3;
    const bool ext = extended && (dispcnt & 0x40000000u);
    const uint8_t* extPal = ext ? extBgPalSlot(s, e, n) : nullptr;

    int64_t fx64 = int64_t(rx) + int64_t(pb) * y;
    int64_t fy64 = int64_t(ry) + int64_t(pd) * y;
    uint32_t cachedKey = 0xFFFFFFFFu;
    uint32_t entry = 0;
    const uint8_t* tileData = nullptr;              // 64 octets de la tuile courante
    for (int x = 0; x < 256; x++, fx64 += pa, fy64 += pc) {
        int32_t px = int32_t(fx64 >> 8), py = int32_t(fy64 >> 8);
        if (wrap) { px &= size - 1; py &= size - 1; }
        else if (px < 0 || py < 0 || px >= size || py >= size) { out[x] = 0; continue; }
        const uint32_t tx = uint32_t(px) >> 3, ty = uint32_t(py) >> 3;
        uint32_t fx = uint32_t(px) & 7, fy = uint32_t(py) & 7;
        const uint32_t key = ty * perRow + tx;
        if (key != cachedKey) {
            cachedKey = key;
            if (extended) {
                const uint8_t* mp = vramPtr(s, e, mapBase + key * 2);
                entry = mp ? uint32_t(mp[0] | (mp[1] << 8)) : 0u;
                tileData = vramPtr(s, e, charBase + (entry & 0x3FF) * 64);
            } else {
                const uint8_t* mp = vramPtr(s, e, mapBase + key);
                entry = 0;
                tileData = vramPtr(s, e, charBase + (mp ? uint32_t(*mp) : 0u) * 64);
            }
        }
        if (extended) {
            if (entry & 0x400) fx = 7 - fx;
            if (entry & 0x800) fy = 7 - fy;
        }
        const uint32_t idx = tileData ? tileData[fy * 8 + fx] : 0u;
        if (!idx) { out[x] = 0; continue; }
        out[x] = kOpq | ((ext) ? rgb555(extColor(extPal, entry >> 12, idx)) : c.pal[idx]);
    }
}

// ------------------------------------------------------------------ sprites
// objWin : recoit 1 la ou un sprite en mode "fenetre" a un point non transparent (fenetre des sprites).
void renderSprites(const Ctx& c, int y, Pixel* line, uint8_t* objWin) {
    const System& s = c.s;
    const int e = c.e;
    static const int kW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static const int kH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    const uint32_t dispcnt = c.dispcnt;
    const bool map1D = (dispcnt >> 4) & 1;
    const uint32_t boundary = 32u << ((dispcnt >> 20) & 3);   // pas entre sprites en mode 1D
    const int region = e == 0 ? 2 : 3;
    const uint32_t oamBase = e == 0 ? 0u : 0x400u;
    const uint8_t* objExt = (dispcnt & 0x80000000u) ? extObjPal(s, e) : nullptr;   // palettes etendues des sprites

    for (int i = 127; i >= 0; i--) {                 // du dernier au premier : le plus petit numero passe devant
        const uint8_t* o = &s.oam[oamBase + i * 8];
        uint32_t a0 = o[0] | (o[1] << 8), a1 = o[2] | (o[3] << 8), a2 = o[4] | (o[5] << 8);
        const bool affine = (a0 & 0x100) != 0;       // sprite a rotation / zoom
        if (!affine && (a0 & 0x200)) continue;       // sprite desactive (bits 8-9 = 10)
        const uint32_t shape = (a0 >> 14) & 3, sz = (a1 >> 14) & 3;
        if (shape == 3) continue;
        const uint32_t objMode = (a0 >> 10) & 3;     // 0 normal, 1 semi-transparent, 2 fenetre, 3 bitmap
        const bool bitmap = objMode == 3;            // sprite bitmap : couleurs directes 15 bits, bit 15 = opaque
        const uint32_t bmpAlpha = (a2 >> 12) & 15;   // son opacite : 0 = invisible, 15 = opaque
        if (bitmap && bmpAlpha == 0) continue;
        const int w = kW[shape][sz], h = kH[shape][sz];
        // Zone occupee a l'ecran : la taille du sprite, ou le double avec le bit "taille double" (rotation seulement)
        const bool dbl = affine && (a0 & 0x200);
        const int bw = dbl ? 2 * w : w, bh = dbl ? 2 * h : h;
        const int sy = int(a0 & 0xFF);
        const int row = (y - sy) & 0xFF;
        if (row >= bh) continue;
        int sx = int(a1 & 0x1FF);
        if (sx >= 256) sx -= 512;
        const bool color256 = (a0 >> 13) & 1;
        const int prio = int((a2 >> 10) & 3);
        const uint32_t tile = a2 & 0x3FF, pal = (a2 >> 12) & 15;

        // Point (u, v) de l'image du sprite (0 <= u < w, 0 <= v < h) -> couleur ; faux si transparent
        auto texel = [&](int u, int v, uint32_t& color) -> bool {
            if (bitmap) {
                // DISPCNT bit 6 : rangement 1D (pas de 128 ou 256 octets selon le bit 22) ; sinon 2D, l'image
                // fait 128 (bit 5 = 0) ou 256 points de large et le numero de tuile donne la colonne et la ligne
                uint32_t addr;
                if (dispcnt & 0x40) addr = tile * (128u << ((dispcnt >> 22) & 1)) + (uint32_t(v) * uint32_t(w) + uint32_t(u)) * 2;
                else if (dispcnt & 0x20) addr = ((tile & 0x1F) << 4) + ((tile & 0x3E0) << 8) + (uint32_t(v) * 256 + uint32_t(u)) * 2;
                else addr = ((tile & 0x0F) << 4) + ((tile & 0x3F0) << 7) + (uint32_t(v) * 128 + uint32_t(u)) * 2;
                const uint16_t px = vramHalf(s, region, addr);
                if (!(px & 0x8000)) return false;
                color = rgb555(px);
                return true;
            }
            const uint32_t tx = uint32_t(u >> 3), ty = uint32_t(v >> 3), fx = uint32_t(u & 7), fy = uint32_t(v & 7);
            const uint32_t addr = map1D ? tile * boundary + (ty * uint32_t(w >> 3) + tx) * (color256 ? 64u : 32u)
                                        : (tile + ty * 32 + tx * (color256 ? 2u : 1u)) * 32u;
            if (color256) {
                const uint32_t idx = vramByte(s, region, addr + fy * 8 + fx);
                if (!idx) return false;
                color = objExt ? rgb555(extColor(objExt, pal, idx)) : c.pal[256 + idx];
            } else {
                const uint8_t b = vramByte(s, region, addr + fy * 4 + (fx >> 1));
                const uint32_t idx = (fx & 1) ? uint32_t(b >> 4) : uint32_t(b & 15);
                if (!idx) return false;
                color = c.pal[256 + pal * 16 + idx];
            }
            return true;
        };
        auto plot = [&](int px, uint32_t color) {
            if (objMode == 2) { objWin[px] = 1; return; }      // sprite fenetre : invisible, il ne fait que delimiter
            Pixel& d = line[px];
            if (!d.opaque || prio <= d.prio) {
                d.opaque = true; d.color = color; d.prio = prio;
                d.semi = objMode == 1 || (bitmap && bmpAlpha < 15);
                d.alpha = bitmap ? uint8_t(bmpAlpha) : uint8_t(0);
            }
        };

        if (!affine) {
            const bool hflip = (a1 >> 12) & 1, vflip = (a1 >> 13) & 1;
            const int v = vflip ? h - 1 - row : row;
            for (int cx = 0; cx < w; cx++) {
                const int px = sx + cx;
                if (px < 0 || px >= 256) continue;
                uint32_t color;
                if (texel(hflip ? w - 1 - cx : cx, v, color)) plot(px, color);
            }
            continue;
        }

        // Rotation / zoom : les 4 parametres PA, PB, PC, PD (virgule fixe 8.8) sont ranges dans l'OAM, groupe n
        // (bits 9-13 de attr1), dans le 4e demi-mot des entrees 4n a 4n+3. Le centre de la zone correspond au
        // centre de l'image ; un point a (dx, dy) du centre lit l'image en (PA dx + PB dy, PC dx + PD dy).
        const uint32_t grp = oamBase + ((a1 >> 9) & 31) * 32;
        auto param = [&](int k) { return int32_t(int16_t(s.oam[grp + k * 8 + 6] | (s.oam[grp + k * 8 + 7] << 8))); };
        const int32_t pa = param(0), pb = param(1), pc = param(2), pd = param(3);
        const int32_t dy = row - bh / 2;
        int32_t u = pa * (-bw / 2) + pb * dy + ((w / 2) << 8);     // position dans l'image (8.8) au 1er point
        int32_t v = pc * (-bw / 2) + pd * dy + ((h / 2) << 8);
        for (int cx = 0; cx < bw; cx++, u += pa, v += pc) {
            const int px = sx + cx;
            if (px < 0 || px >= 256) continue;
            const int iu = u >> 8, iv = v >> 8;
            if (iu < 0 || iv < 0 || iu >= w || iv >= h) continue;   // hors de l'image : transparent
            uint32_t color;
            if (texel(iu, iv, color)) plot(px, color);
        }
    }
}

// ------------------------------------------------------------------ effets de couleur (BLDCNT)
// Les couleurs sont en 0xRRGGBB (8 bits par canal). Coefficients EVA, EVB, EY sur 16 (16 = 100 %).
inline uint32_t blendAlpha(uint32_t a, uint32_t b, uint32_t eva, uint32_t evb) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t ch = (((a >> sh) & 255) * eva + ((b >> sh) & 255) * evb) >> 4;
        out |= (ch > 255 ? 255u : ch) << sh;
    }
    return out;
}
inline uint32_t brighten(uint32_t c, uint32_t ey) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) { const uint32_t ch = (c >> sh) & 255; out |= (ch + (((255 - ch) * ey) >> 4)) << sh; }
    return out;
}
inline uint32_t darken(uint32_t c, uint32_t ey) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) { const uint32_t ch = (c >> sh) & 255; out |= (ch - ((ch * ey) >> 4)) << sh; }
    return out;
}
// Pixel 3D (opacite a sur 31) pose sur la couche du dessous
inline uint32_t blend3d(uint32_t top, uint32_t below, uint32_t a) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8)
        out |= ((((top >> sh) & 255) * (a + 1) + ((below >> sh) & 255) * (31 - a)) >> 5) << sh;
    return out;
}

// Une fenetre rectangulaire : registre H (x2 = octet bas, x1 = octet haut) ou V. Si x1 > x2, la zone fait le tour.
inline bool inWinRange(uint32_t reg16, uint32_t p) {
    const uint32_t lo = (reg16 >> 8) & 0xFF, hi = reg16 & 0xFF;
    return lo <= hi ? (p >= lo && p < hi) : (p >= lo || p < hi);
}

// ------------------------------------------------------------------ un moteur complet
void renderEngine(const System& s, int e, uint32_t* out /* 256 x 192 */, bool masterBrightness = true) {
    const Ctx c(s, e);
    const uint32_t dispcnt = c.dispcnt;
    const uint32_t white = 0xFFFFFF;
    const uint32_t mode = (dispcnt >> 16) & 3;
    const bool enabled = e == 0 ? (s.powcnt1 & 2) : (s.powcnt1 & 0x200);
    const uint32_t bgMode = dispcnt & 7;
    const uint32_t backdrop = c.pal[0];

    // Fonds visibles, du plus prioritaire au moins prioritaire (a priorite egale : le plus petit numero d'abord)
    int order[4], orderPrio[4], kinds[4], count = 0;
    for (int prio = 0; prio < 4; prio++) {
        for (int n = 0; n < 4; n++) {
            if (!(dispcnt & (0x100u << n))) continue;
            int kind = bgKind(bgMode, n);
            if (kind < 0) continue;
            if (n == 0 && e == 0 && (dispcnt & 8)) kind = 3;   // BG0 du moteur A en 3D
            if (int((s.eng[e][2 + (n >> 1)] >> (16 * (n & 1))) & 3) != prio) continue;
            order[count] = n; orderPrio[count] = prio; kinds[count] = kind; count++;
        }
    }

    // Effets de couleur : BLDCNT (0x50), BLDALPHA (0x52), BLDY (0x54)
    const uint32_t bldcnt = s.eng[e][20] & 0xFFFF, bldalpha = s.eng[e][20] >> 16;
    const uint32_t effect = (bldcnt >> 6) & 3;                      // 0 aucun, 1 transparence, 2 eclaircir, 3 assombrir
    const uint32_t target1 = bldcnt & 0x3F, target2 = (bldcnt >> 8) & 0x3F;   // bits : BG0..BG3, sprites, couleur de fond
    auto cap16 = [](uint32_t v) { return v > 16 ? 16u : v; };
    const uint32_t eva = cap16(bldalpha & 31), evb = cap16((bldalpha >> 8) & 31), ey = cap16(s.eng[e][21] & 31);

    // Fenetres : WIN0H/WIN1H (0x40), WIN0V/WIN1V (0x44), WININ/WINOUT (0x48)
    const bool win0On = (dispcnt & 0x2000) != 0, win1On = (dispcnt & 0x4000) != 0, objWinOn = (dispcnt & 0x8000) != 0;
    const bool anyWin = win0On || win1On || objWinOn;
    const uint32_t win0h = s.eng[e][16] & 0xFFFF, win1h = s.eng[e][16] >> 16;
    const uint32_t win0v = s.eng[e][17] & 0xFFFF, win1v = s.eng[e][17] >> 16;
    const uint32_t winIn = s.eng[e][18] & 0xFFFF, winOut = s.eng[e][18] >> 16;
    // Par fenetre, 6 bits : BG0..BG3 visibles, sprites visibles, effets de couleur autorises

    uint32_t bgLine[4][256];
    for (int y = 0; y < 192; y++) {
        uint32_t* row = out + y * 256;
        if (!enabled || (dispcnt & 0x80) || mode == 0) { for (int x = 0; x < 256; x++) row[x] = white; continue; }
        if (mode == 2 && e == 0) {                   // affichage direct d'une banque de VRAM
            uint32_t bank = (dispcnt >> 18) & 3;
            for (int x = 0; x < 256; x++)
                row[x] = rgb555(vramHalf(s, 4, bank * 0x20000 + (uint32_t(y) * 256 + x) * 2));
            continue;
        }
        if (mode == 3) { for (int x = 0; x < 256; x++) row[x] = 0; continue; }   // memoire principale : pas gere

        Pixel sprites[256];
        uint8_t objWin[256] = {};
        if (dispcnt & 0x1000) renderSprites(c, y, sprites, objWin);

        for (int k = 0; k < count; k++) {
            if (kinds[k] == 0)      renderTextLine(c, order[k], y, bgLine[k]);
            else if (kinds[k] == 3) render3dLine(c, y, bgLine[k]);
            else                    renderAffineLine(c, order[k], y, kinds[k] == 2, bgLine[k]);
        }

        const bool in0V = win0On && inWinRange(win0v, uint32_t(y));
        const bool in1V = win1On && inWinRange(win1v, uint32_t(y));

        for (int x = 0; x < 256; x++) {
            // Fenetre dont depend ce point (WIN0 l'emporte sur WIN1, qui l'emporte sur la fenetre des sprites)
            uint32_t wc = 0x3F;
            if (anyWin) {
                if (in0V && inWinRange(win0h, uint32_t(x)))      wc = winIn & 0x3F;
                else if (in1V && inWinRange(win1h, uint32_t(x))) wc = (winIn >> 8) & 0x3F;
                else if (objWinOn && objWin[x])                  wc = (winOut >> 8) & 0x3F;
                else                                             wc = winOut & 0x3F;
            }

            // Les deux couches visibles les plus prioritaires. id : 0..3 fonds, 4 sprites, 5 couleur de fond.
            uint32_t col[2] = { backdrop, backdrop };
            int id[2] = { 5, 5 };
            int a3d = -1;                                        // opacite 3D si la couche du dessus est la 3D
            bool semi = false;                                   // sprite semi-transparent au-dessus
            uint32_t semiAlpha = 0;                              // sprite bitmap : son opacite (sinon EVA / EVB)
            int found = 0;
            const Pixel& sp = sprites[x];
            const bool spVisible = sp.opaque && (wc & 0x10);
            bool spDone = !spVisible;
            for (int k = 0; k < count && found < 2; k++) {
                if (!spDone && sp.prio <= orderPrio[k]) {        // a priorite egale, le sprite passe devant le fond
                    spDone = true;
                    if (found == 0) { semi = sp.semi; semiAlpha = sp.alpha; }
                    col[found] = sp.color; id[found] = 4; found++;
                    if (found == 2) break;
                }
                if (!(wc & (1u << order[k]))) continue;          // fond masque par la fenetre
                const uint32_t v = bgLine[k][x];
                if (!v) continue;
                if (found == 0 && kinds[k] == 3) a3d = int((v >> 25) & 31);
                col[found] = v & 0xFFFFFFu; id[found] = order[k]; found++;
            }
            if (found < 2 && !spDone) {                          // sprite derriere tous les fonds visibles
                if (found == 0) { semi = sp.semi; semiAlpha = sp.alpha; }
                col[found] = sp.color; id[found] = 4; found++;
            }

            uint32_t color = col[0];
            const bool secondIsTarget2 = (target2 >> id[1]) & 1;
            if (semi && secondIsTarget2) {
                // sprite semi-transparent : toujours en transparence, quel que soit BLDCNT
                if (semiAlpha) color = blendAlpha(col[0], col[1], semiAlpha + 1, 15 - semiAlpha);
                else           color = blendAlpha(col[0], col[1], eva, evb);
            } else if (a3d >= 0 && secondIsTarget2) {
                // la 3D se melange avec la couche du dessous selon sa propre opacite, jamais avec EVA / EVB :
                // un pixel 3D opaque (31) reste donc opaque meme si BLDCNT demande la transparence pour BG0
                if (a3d < 31) color = blend3d(col[0], col[1], uint32_t(a3d));
            } else if ((wc & 0x20) && ((target1 >> id[0]) & 1)) {
                switch (effect) {
                case 1: if (secondIsTarget2) color = blendAlpha(col[0], col[1], eva, evb); break;
                case 2: color = brighten(col[0], ey); break;
                case 3: color = darken(col[0], ey); break;
                default: break;
                }
            }
            row[x] = color;
        }

        // Luminosite generale (fondus au blanc / au noir)
        if (!masterBrightness) continue;
        uint32_t bright = s.eng[e][27];
        uint32_t factor = bright & 31, bmode = (bright >> 14) & 3;
        if (factor > 16) factor = 16;
        if (bmode == 1 || bmode == 2) {
            for (int x = 0; x < 256; x++) {
                uint32_t c2 = row[x], res = 0;
                for (int sh = 16; sh >= 0; sh -= 8) {
                    uint32_t ch = (c2 >> sh) & 255;
                    ch = (bmode == 1) ? ch + ((255 - ch) * factor) / 16 : ch - (ch * factor) / 16;
                    res |= ch << sh;
                }
                row[x] = res;
            }
        }
    }
}

// ------------------------------------------------------------------ PNG
uint32_t crcTable[256];
bool crcReady = false;
void crcInit() {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crcTable[n] = c;
    }
    crcReady = true;
}
uint32_t crc32(const uint8_t* d, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; i++) crc = crcTable[(crc ^ d[i]) & 255] ^ (crc >> 8);
    return crc;
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, uint32_t(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    put32(out, crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
}

}  // namespace

// ------------------------------------------------------------------ capture d'ecran
// DISPCAPCNT : bits 0-4 EVA, 8-12 EVB, 16-17 banque de destination (A..D), 18-19 decalage d'ecriture (x 32 Ko),
// 20-21 taille (128x128, 256x64, 256x128, 256x192), 24 source A (0 = image du moteur A, 1 = 3D seule),
// 25 source B (0 = VRAM, 1 = memoire principale), 26-27 decalage de lecture, 29-30 choix (A, B, melange).
// La source B en VRAM est la banque choisie par DISPCNT (bits 18-19), la meme que l'affichage direct.
// Le resultat est ecrit en couleurs 15 bits, bit 15 = pixel opaque.
void displayCapture(System& s) {
    const uint32_t cnt = s.eng[0][25];
    std::vector<uint8_t>& dst = s.vram[(cnt >> 16) & 3];
    if (dst.size() < 0x20000) return;
    static const int kW[4] = {128, 256, 256, 256}, kH[4] = {128, 64, 128, 192};
    const int w = kW[(cnt >> 20) & 3], h = kH[(cnt >> 20) & 3];
    const uint32_t sel = (cnt >> 29) & 3;
    const uint32_t eva = std::min<uint32_t>(cnt & 31, 16), evb = std::min<uint32_t>((cnt >> 8) & 31, 16);
    const bool needA = sel != 1, needB = sel != 0;

    std::vector<uint32_t> imgA;
    const bool only3d = (cnt >> 24) & 1;
    if (needA && !only3d) { imgA.resize(256 * 192); renderEngine(s, 0, imgA.data(), false); }
    const uint32_t* frame3d = s.gx.frame();

    const std::vector<uint8_t>& srcB = s.vram[(s.eng[0][0] >> 18) & 3];
    const bool bFromVram = !((cnt >> 25) & 1) && srcB.size() >= 0x20000;
    const uint32_t readOfs = ((cnt >> 26) & 3) * 0x8000, writeOfs = ((cnt >> 18) & 3) * 0x8000;

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint32_t i = uint32_t(y) * 256 + uint32_t(x);
            // source A en 15 bits + opacite
            uint32_t a = 0, aOn = 0;
            if (needA) {
                if (only3d) {
                    const uint32_t p = frame3d[i];
                    aOn = ((p >> 24) & 31) ? 1 : 0;
                    a = ((p >> 19) & 31) | (((p >> 11) & 31) << 5) | (((p >> 3) & 31) << 10);
                } else {
                    const uint32_t p = imgA[i];
                    aOn = 1;
                    a = ((p >> 19) & 31) | (((p >> 11) & 31) << 5) | (((p >> 3) & 31) << 10);
                }
            }
            uint32_t b = 0, bOn = 0;
            if (needB && bFromVram) {
                const uint32_t o = (readOfs + i * 2) & 0x1FFFF;
                b = uint32_t(srcB[o] | (srcB[o + 1] << 8));
                bOn = b >> 15;
                b &= 0x7FFF;
            }
            uint32_t out;
            if (sel == 0)      out = a | (aOn << 15);
            else if (sel == 1) out = b | (bOn << 15);
            else {
                out = 0;
                for (int sh = 0; sh < 15; sh += 5) {
                    const uint32_t ca = (a >> sh) & 31, cb = (b >> sh) & 31;
                    const uint32_t v = (ca * aOn * eva + cb * bOn * evb + 8) >> 4;
                    out |= std::min<uint32_t>(v, 31) << sh;
                }
                out |= ((aOn && eva) || (bOn && evb)) ? 0x8000u : 0u;
            }
            const uint32_t o = (writeOfs + i * 2) & 0x1FFFF;
            dst[o] = uint8_t(out);
            dst[o + 1] = uint8_t(out >> 8);
        }
}

void renderScreens(const System& sys, std::vector<uint32_t>& rgb) {
    rgb.resize(256 * 384);
    const bool aOnTop = (sys.powcnt1 & 0x8000) != 0;      // bit 15 : moteur A sur l'ecran du haut
    renderEngine(sys, 0, rgb.data() + (aOnTop ? 0 : 256 * 192));
    renderEngine(sys, 1, rgb.data() + (aOnTop ? 256 * 192 : 0));
}

bool writePng(const char* path, const std::vector<uint32_t>& rgb, int width, int height) {
    if (!crcReady) crcInit();
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (1 + width * 3));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);                                  // filtre "aucun"
        for (int x = 0; x < width; x++) {
            uint32_t c = rgb[size_t(y) * width + x];
            raw.push_back(uint8_t(c >> 16)); raw.push_back(uint8_t(c >> 8)); raw.push_back(uint8_t(c));
        }
    }
    // flux zlib avec blocs "stockes" (sans compression)
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    while (pos < raw.size() || pos == 0) {
        size_t n = raw.size() - pos;
        if (n > 65535) n = 65535;
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put32(ihdr, uint32_t(width)); put32(ihdr, uint32_t(height));
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}
