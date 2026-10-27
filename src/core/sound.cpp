#include "core/sound.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include "core/memory.hpp"

static uint32_t mergeBits(uint32_t old, uint32_t v, uint32_t mask) { return (old & ~mask) | (v & mask); }
static int32_t clamp16(int64_t v) { return int32_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

// Duree d'un echantillon en cycles de 33,5 MHz : la minuterie avance a 16,76 MHz et deborde a 0x10000
static uint32_t samplePeriod(uint16_t tmr) { return (0x10000u - tmr) * 2u; }

// Tables de l'ADPCM IMA (identiques sur la console)
static const int8_t kAdpcmIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static const uint16_t kAdpcmStep[89] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x0010, 0x0011, 0x0013, 0x0015,
    0x0017, 0x0019, 0x001C, 0x001F, 0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F, 0x009D, 0x00AD, 0x00BE, 0x00D1,
    0x00E6, 0x00FD, 0x0117, 0x0133, 0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583, 0x0610, 0x06AB, 0x0756, 0x0812,
    0x08E0, 0x09C3, 0x0ABD, 0x0BD0, 0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B, 0x3BB9, 0x41B2, 0x4844, 0x4F7E,
    0x5771, 0x602F, 0x69CE, 0x7462, 0x7FFF};

uint64_t Spu::sampleCount(uint32_t format, uint32_t words) {
    switch (format) {
    case 0:  return uint64_t(words) * 4;                       // PCM 8 bits : 4 echantillons par mot
    case 1:  return uint64_t(words) * 2;                       // PCM 16 bits
    case 2:  return words > 1 ? uint64_t(words - 1) * 8 : 0;   // ADPCM : un mot d'en-tete, puis 8 par mot
    default: return 0;                                         // onde carree / bruit : sans fin
    }
}

// Une ligne du journal : heure de la console (secondes.millisecondes depuis le demarrage), puis le message
void Spu::note(uint64_t now, const char* fmt, ...) {
    if (!journal || journalLines >= 200000) return;
    journalLines++;
    const double t = double(now) / 33513982.0;
    std::fprintf(journal, "%9.3f  ", t);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(journal, fmt, ap);
    va_end(ap);
    std::fputc('\n', journal);
}

size_t Spu::takeSamples(int16_t* dst, size_t maxFrames) {
    const size_t frames = std::min(maxFrames, output.size() / 2);
    std::copy(output.begin(), output.begin() + std::ptrdiff_t(frames * 2), dst);
    output.erase(output.begin(), output.begin() + std::ptrdiff_t(frames * 2));
    return frames;
}

// ---------------------------------------------------------------- canaux

int32_t Spu::fetch(const Channel& c, uint32_t pos) const {
    if (!bus) return 0;
    if (((c.cnt >> 29) & 3) == 0) return int32_t(int8_t(bus->read8(c.sad + pos))) << 8;   // PCM 8 bits
    return int32_t(int16_t(bus->read16(c.sad + pos * 2)));                                // PCM 16 bits
}

// ADPCM : decode le demi-octet "pos" (apres le mot d'en-tete) et met a jour la valeur et l'indice
void Spu::decodeAdpcm(Channel& c) {
    const uint8_t byte = bus ? bus->read8(c.sad + 4 + (c.pos >> 1)) : 0;
    const uint32_t data = (c.pos & 1) ? (byte >> 4) : (byte & 15);
    const int32_t step = kAdpcmStep[c.adpcmIndex];
    int32_t diff = step / 8;
    if (data & 1) diff += step / 4;
    if (data & 2) diff += step / 2;
    if (data & 4) diff += step;
    if (data & 8) c.cur = std::max(c.cur - diff, -0x7FFF);
    else          c.cur = std::min(c.cur + diff, 0x7FFF);
    c.adpcmIndex = std::min(88, std::max(0, c.adpcmIndex + kAdpcmIndex[data & 7]));
}

void Spu::startChannel(int i) {
    Channel& c = ch[i];
    channelStarts++;
    c.accum = 0;
    c.pos = 0;
    c.psgStep = 0;
    c.lfsr = 0x7FFF;
    c.loopSaved = false;
    c.cur = 0;
    switch ((c.cnt >> 29) & 3) {
    case 2: {                                                  // ADPCM : en-tete = valeur de depart + indice
        const uint32_t header = bus ? bus->read32(c.sad) : 0;
        c.cur = int16_t(header & 0xFFFF);
        c.adpcmIndex = std::min(88, int((header >> 16) & 0x7F));
        // l'en-tete n'est que le point de depart : le 1er echantillon entendu est le 1er demi-octet decode
        if (c.pnt <= 1) { c.loopCur = c.cur; c.loopIndex = c.adpcmIndex; c.loopSaved = true; }   // boucle au debut
        if (sampleCount(2, uint32_t(c.pnt) + c.len) > 0) decodeAdpcm(c);
        break;
    }
    case 3: break;                                             // onde carree / bruit : le 1er debordement donne la valeur
    default: c.cur = fetch(c, 0); break;
    }
}

void Spu::stepChannel(int i) {
    Channel& c = ch[i];
    const uint32_t format = (c.cnt >> 29) & 3, repeat = (c.cnt >> 27) & 3;
    if (format == 3) {
        if (i >= 8 && i <= 13) {                               // onde carree : (rapport + 1) huitiemes en haut
            c.psgStep = (c.psgStep + 1) & 7;
            const uint32_t duty = (c.cnt >> 24) & 7;
            c.cur = c.psgStep >= 7 - duty ? 0x7FFF : -0x7FFF;
        } else if (i >= 14) {                                  // bruit
            if (c.lfsr & 1) { c.lfsr = uint16_t((c.lfsr >> 1) ^ 0x6000); c.cur = -0x7FFF; }
            else            { c.lfsr = uint16_t(c.lfsr >> 1);            c.cur = 0x7FFF; }
        } else {
            c.cur = 0;                                         // canaux 0-7 : pas de generateur
        }
        return;
    }
    const uint32_t words = uint32_t(c.pnt) + c.len;
    const uint32_t total = uint32_t(sampleCount(format, words));
    const uint32_t loop = format == 2 ? (c.pnt ? (uint32_t(c.pnt) - 1) * 8 : 0) : uint32_t(sampleCount(format, c.pnt));
    auto endReached = [&]() -> bool {                          // vrai si le canal s'arrete
        if (repeat == 2 || total == 0 || loop >= total) {      // "une fois" (ou boucle impossible)
            c.cnt &= ~0x80000000u;
            c.cur = 0;
            note(nextSample, "canal %2d : fin des donnees (s'arrete tout seul)", i);
            return true;
        }
        c.pos = loop;                                          // boucle (et mode manuel : on boucle aussi)
        return false;
    };
    if (format == 2) {                                         // pos = demi-octet en train d'etre joue
        c.pos++;
        if (c.pos >= total) {
            if (endReached()) return;
            c.cur = c.loopCur; c.adpcmIndex = c.loopIndex;     // etat juste avant le point de boucle
        } else if (c.pos == loop && !c.loopSaved) {
            c.loopCur = c.cur; c.loopIndex = c.adpcmIndex; c.loopSaved = true;
        }
        decodeAdpcm(c);
        return;
    }
    c.pos++;
    if (c.pos >= total && endReached()) return;
    c.cur = fetch(c, c.pos);
}

// ---------------------------------------------------------------- mixage et capture

void Spu::mixOne() {
    static const int kShift[4] = {0, 1, 2, 4};
    int64_t mixL = 0, mixR = 0;
    int32_t ch1L = 0, ch1R = 0, ch3L = 0, ch3R = 0;
    for (int i = 0; i < 16; i++) {
        Channel& c = ch[i];
        if (!(c.cnt & 0x80000000u)) { c.out = 0; continue; }
        int32_t vol = int32_t(c.cnt & 0x7F);
        if (vol == 127) vol = 128;
        const int32_t v = ((c.cur * vol) >> 7) >> kShift[(c.cnt >> 8) & 3];
        c.out = v;
        int32_t pan = int32_t((c.cnt >> 16) & 0x7F);
        if (pan == 127) pan = 128;
        const int32_t l = (v * (128 - pan)) >> 7, r = (v * pan) >> 7;
        if (i == 1) { ch1L = l; ch1R = r; if (soundCnt & 0x1000) continue; }   // bit 12 : canal 1 hors du mixeur
        if (i == 3) { ch3L = l; ch3R = r; if (soundCnt & 0x2000) continue; }   // bit 13 : canal 3 hors du mixeur
        mixL += l;
        mixR += r;
    }
    lastMixL = clamp16(mixL);
    lastMixR = clamp16(mixR);

    // Sorties gauche / droite : mixeur, canal 1, canal 3 ou canaux 1 + 3
    auto select = [&](uint32_t sel, int64_t mix, int32_t c1, int32_t c3) -> int64_t {
        switch (sel) { case 1: return c1; case 2: return c3; case 3: return int64_t(c1) + c3; default: return mix; }
    };
    int64_t outL = select((soundCnt >> 8) & 3, mixL, ch1L, ch3L);
    int64_t outR = select((soundCnt >> 10) & 3, mixR, ch1R, ch3R);
    int64_t master = soundCnt & 0x7F;
    if (master == 127) master = 128;
    outL = (outL * master) >> 7;
    outR = (outR * master) >> 7;
    // Le convertisseur de la console sature a environ 4 canaux a plein volume : meme marge ici
    int16_t sl = int16_t(clamp16(outL / 4)), sr = int16_t(clamp16(outR / 4));
    if (!(soundCnt & 0x8000) || (power && !(*power & 1))) { sl = sr = 0; framesMuted++; }
    framesMixed++;
    if (sl || sr) framesAudible++;
    peak = std::max(peak, std::max(std::abs(int32_t(sl)), std::abs(int32_t(sr))));
    output.push_back(sl);
    output.push_back(sr);
    // Personne ne lit assez vite : on jette les plus vieux, par gros blocs (pas a chaque echantillon)
    if (output.size() > maxBuffered * 4)
        output.erase(output.begin(), output.begin() + std::ptrdiff_t(output.size() - maxBuffered * 2));
}

// Capture : au rythme de la minuterie du canal 1 (capture 0) ou 3 (capture 1), ecrit en memoire la sortie du
// mixeur (gauche / droite) ou celle du canal 0 / 2, en 16 ou 8 bits.
void Spu::runCapture(int i) {
    if (!(capCnt[i] & 0x80)) return;
    Capture& k = cap[i];
    const uint32_t period = samplePeriod(ch[i == 0 ? 1 : 3].tmr);
    const bool pcm8 = (capCnt[i] & 0x08) != 0;
    const uint32_t bytes = uint32_t(capLen[i] ? capLen[i] : 1) * 4;
    k.accum += kCyclesPerSample;
    while (k.accum >= period) {
        k.accum -= period;
        const int32_t v = (capCnt[i] & 0x02) ? ch[i == 0 ? 0 : 2].out : (i == 0 ? lastMixL : lastMixR);
        if (bus) {
            if (pcm8) bus->write8(capDad[i] + k.pos, uint8_t(int8_t(clamp16(v) >> 8)));
            else      bus->write16(capDad[i] + k.pos, uint16_t(int16_t(clamp16(v))));
        }
        k.pos += pcm8 ? 1 : 2;
        if (k.pos >= bytes) {
            k.pos = 0;
            if (capCnt[i] & 0x04) { capCnt[i] = uint8_t(capCnt[i] & 0x7F); return; }   // "une fois" : termine
        }
    }
}

void Spu::runSlow(uint64_t now) {
    while (nextSample <= now) {
        for (int i = 0; i < 16; i++) {                         // chaque canal avance au rythme de sa minuterie
            Channel& c = ch[i];
            if (!(c.cnt & 0x80000000u)) continue;
            const uint32_t period = samplePeriod(c.tmr);
            c.accum += kCyclesPerSample;
            while (c.accum >= period && (c.cnt & 0x80000000u)) {
                c.accum -= period;
                stepChannel(i);
            }
        }
        mixOne();
        runCapture(0);
        runCapture(1);
        nextSample += kCyclesPerSample;
    }
}

// ---------------------------------------------------------------- registres

uint32_t Spu::read32(uint32_t addr, uint64_t now) {
    run(now);
    const uint32_t o = addr - 0x04000400;
    if (o < 0x100) {
        // seul SOUNDxCNT se relit ; adresse, minuterie, boucle et longueur sont en ecriture seule
        return (o & 0xF) == 0 ? ch[o >> 4].cnt : 0u;
    }
    switch (addr) {
    case 0x04000500: return soundCnt;
    case 0x04000504: return soundBias;
    case 0x04000508: return uint32_t(capCnt[0]) | (uint32_t(capCnt[1]) << 8);
    case 0x04000510: return capDad[0];
    case 0x04000518: return capDad[1];
    default:         return 0;                                 // longueurs de capture : ecriture seule
    }
}

void Spu::write32(uint32_t addr, uint32_t v, uint32_t mask, uint64_t now) {
    run(now);
    const uint32_t o = addr - 0x04000400;
    if (o < 0x100) {
        Channel& c = ch[o >> 4];
        const int i = int(o >> 4);
        switch (o & 0xF) {
        case 0x0: {
            const bool wasOn = (c.cnt & 0x80000000u) != 0;
            c.cnt = mergeBits(c.cnt, v, mask) & 0xFF7F837Fu;   // bits inutilises a 0
            if ((c.cnt & 0x80000000u) && !wasOn) {
                startChannel(i);
                static const char* kFmt[4] = {"PCM8", "PCM16", "ADPCM", "PSG"};
                static const char* kRep[4] = {"manuel", "boucle", "une fois", "?"};
                note(now, "canal %2d : depart   %-5s %-8s vol=%3u pan=%3u SAD=%08X TMR=%04X PNT=%04X LEN=%06X", i,
                     kFmt[(c.cnt >> 29) & 3], kRep[(c.cnt >> 27) & 3], c.cnt & 0x7F, (c.cnt >> 16) & 0x7F,
                     c.sad, c.tmr, c.pnt, c.len);
            } else if (wasOn && !(c.cnt & 0x80000000u)) {
                note(now, "canal %2d : arrete par le jeu", i);
            }
            break;
        }
        case 0x4: c.sad = mergeBits(c.sad, v, mask) & 0x07FFFFFCu; break;
        case 0x8:
            if (mask & 0x0000FFFFu) c.tmr = uint16_t(mergeBits(c.tmr, v, mask));
            if (mask & 0xFFFF0000u) c.pnt = uint16_t(mergeBits(uint32_t(c.pnt) << 16, v, mask) >> 16);
            break;
        case 0xC: c.len = mergeBits(c.len, v, mask) & 0x003FFFFFu; break;
        }
        return;
    }
    switch (addr) {
    case 0x04000500: if (mask & 0xFFFF) soundCnt = uint16_t(mergeBits(soundCnt, v, mask) & 0xBF7F); break;
    case 0x04000504: if (mask & 0xFFFF) soundBias = uint16_t(mergeBits(soundBias, v, mask) & 0x3FF); break;
    case 0x04000508:
        for (int i = 0; i < 2; i++) {
            if (!((mask >> (8 * i)) & 0xFF)) continue;
            const uint8_t old = capCnt[i];
            capCnt[i] = uint8_t((v >> (8 * i)) & 0x8F);
            if ((capCnt[i] & 0x80) && !(old & 0x80)) {
                cap[i] = Capture();                            // depart : on repart du debut
                note(now, "capture %d : depart CNT=%02X DAD=%08X LEN=%04X", i, capCnt[i], capDad[i], capLen[i]);
            } else if ((old & 0x80) && !(capCnt[i] & 0x80)) {
                note(now, "capture %d : arretee par le jeu", i);
            }
        }
        break;
    case 0x04000510: capDad[0] = mergeBits(capDad[0], v, mask) & 0x07FFFFFCu; break;
    case 0x04000514: if (mask & 0xFFFF) capLen[0] = uint16_t(mergeBits(capLen[0], v, mask)); break;
    case 0x04000518: capDad[1] = mergeBits(capDad[1], v, mask) & 0x07FFFFFCu; break;
    case 0x0400051C: if (mask & 0xFFFF) capLen[1] = uint16_t(mergeBits(capLen[1], v, mask)); break;
    default: break;
    }
}
