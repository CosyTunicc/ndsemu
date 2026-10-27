#include "core/arm.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static inline uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

static constexpr uint32_t FLAG_N = 1u << 31;
static constexpr uint32_t FLAG_Z = 1u << 30;
static constexpr uint32_t FLAG_C = 1u << 29;
static constexpr uint32_t FLAG_V = 1u << 28;

// Index de banque d'un mode (-1 si mode invalide).
static int bankIndex(uint32_t mode) {
    switch (mode) {
    case 0x10: case 0x1F: return 0;   // User / System
    case 0x11: return 1;              // FIQ
    case 0x12: return 2;              // IRQ
    case 0x13: return 3;              // Supervisor
    case 0x17: return 4;              // Abort
    case 0x1B: return 5;              // Undefined
    default:   return -1;
    }
}

static const char* modeName(uint32_t mode) {
    switch (mode) {
    case 0x10: return "usr"; case 0x11: return "fiq"; case 0x12: return "irq";
    case 0x13: return "svc"; case 0x17: return "abt"; case 0x1B: return "und";
    case 0x1F: return "sys"; default: return "???";
    }
}

void ArmCpu::reset(uint32_t entry) {
    for (auto& x : r) x = 0;
    for (auto& x : spsr) x = 0;
    for (auto& x : bankR13) x = 0;
    for (auto& x : bankR14) x = 0;
    for (auto& x : fiqR8_12) x = 0;
    for (auto& x : usrR8_12) x = 0;
    cp15Control = 0x00012078;
    cp15Dtcm = cp15Itcm = 0;
    bus.configureTcm(cp15Control, cp15Dtcm, cp15Itcm);
    pc = entry;
    cpsr = 0xDF;  // mode System, IRQ et FIQ desactives (etat apres le bootloader)
    error.clear();
}

void ArmCpu::setReg(unsigned n, uint32_t v) {
    if (n == 15) { pc = v & ~3u; branched = true; }
    else r[n] = v;
}

bool ArmCpu::fail(const char* why) {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%s @0x%08X (mot 0x%08X)", why, lastPc, lastInstr);
    error = buf;
    return false;
}

// Change de mode : sauvegarde les registres du mode actuel, charge ceux du nouveau.
// (A appeler AVANT de modifier cpsr.)
void ArmCpu::switchMode(uint32_t newMode) {
    int o = bankIndex(cpsr & 0x1F), n = bankIndex(newMode);
    if (o == n) return;
    bankR13[o] = r[13];
    bankR14[o] = r[14];
    if (o == 1) {                       // on quitte FIQ : r8-r12 reprennent leurs valeurs normales
        for (int i = 0; i < 5; i++) { fiqR8_12[i] = r[8 + i]; r[8 + i] = usrR8_12[i]; }
    }
    if (n == 1) {                       // on entre en FIQ
        for (int i = 0; i < 5; i++) { usrR8_12[i] = r[8 + i]; r[8 + i] = fiqR8_12[i]; }
    }
    r[13] = bankR13[n];
    r[14] = bankR14[n];
}

// Registre r0-r14 du mode User/System, meme si on est dans un autre mode.
uint32_t& ArmCpu::userReg(unsigned i) {
    int idx = bankIndex(cpsr & 0x1F);
    if (i < 8 || idx == 0) return r[i];              // communs a tous les modes, ou on est deja en User/System
    if (i < 13) return idx == 1 ? usrR8_12[i - 8] : r[i];   // r8-r12 : banques seulement en mode FIQ
    return i == 13 ? bankR13[0] : bankR14[0];        // r13 et r14 : banques User sauvegardees
}

// Retour d'exception : CPSR <- SPSR du mode courant (avec changement de mode).
bool ArmCpu::returnFromException() {
    int idx = bankIndex(cpsr & 0x1F);
    if (idx <= 0) return fail("retour d'exception en mode User/System (pas de SPSR)");
    uint32_t nv = spsr[idx];
    if (bankIndex(nv & 0x1F) < 0) return fail("retour d'exception : SPSR avec un mode invalide");
    switchMode(nv & 0x1F);
    cpsr = nv;
    return true;
}

// Valeur d'une condition pour un etat des drapeaux NZCV (bits 3..0 = N, Z, C, V)
static constexpr bool condEval(uint32_t c, uint32_t f) {
    const bool N = f & 8, Z = f & 4, C = f & 2, V = f & 1;
    switch (c) {
    case 0x0: return Z;                 // EQ
    case 0x1: return !Z;                // NE
    case 0x2: return C;                 // CS/HS
    case 0x3: return !C;                // CC/LO
    case 0x4: return N;                 // MI
    case 0x5: return !N;                // PL
    case 0x6: return V;                 // VS
    case 0x7: return !V;                // VC
    case 0x8: return C && !Z;           // HI
    case 0x9: return !C || Z;           // LS
    case 0xA: return N == V;            // GE
    case 0xB: return N != V;            // LT
    case 0xC: return !Z && (N == V);    // GT
    case 0xD: return Z || (N != V);     // LE
    default:  return true;              // 0xE = AL
    }
}
// Table pre-calculee : bit f de kCond[c] = la condition c est vraie quand les drapeaux valent f
struct CondTable {
    uint16_t t[16] = {};
    constexpr CondTable() {
        for (uint32_t c = 0; c < 16; c++)
            for (uint32_t f = 0; f < 16; f++)
                if (condEval(c, f)) t[c] = uint16_t(t[c] | (1u << f));
    }
};
static constexpr CondTable kCond;

bool ArmCpu::cond(uint32_t c) const {
    return (kCond.t[c] >> (cpsr >> 28)) & 1;
}

void ArmCpu::setNZ(uint32_t res) {
    cpsr &= ~(FLAG_N | FLAG_Z);
    if (res & 0x80000000u) cpsr |= FLAG_N;
    if (res == 0) cpsr |= FLAG_Z;
}

// Decalage par une valeur immediate (types : 0 LSL, 1 LSR, 2 ASR, 3 ROR/RRX).
uint32_t ArmCpu::shiftImm(uint32_t v, uint32_t type, uint32_t amt, bool& carry) const {
    carry = (cpsr & FLAG_C) != 0;
    switch (type) {
    case 0:
        if (amt) { carry = (v >> (32 - amt)) & 1; v <<= amt; }
        return v;
    case 1:
        if (amt == 0) { carry = v >> 31; return 0; }         // LSR #32
        carry = (v >> (amt - 1)) & 1;
        return v >> amt;
    case 2:
        if (amt == 0) { carry = v >> 31; return uint32_t(int32_t(v) >> 31); }  // ASR #32
        carry = (v >> (amt - 1)) & 1;
        return uint32_t(int32_t(v) >> amt);
    default:
        if (amt == 0) {                                      // RRX
            bool c = carry;
            carry = v & 1;
            return (uint32_t(c) << 31) | (v >> 1);
        }
        carry = (v >> (amt - 1)) & 1;
        return ror32(v, amt);
    }
}

// Decalage dont la quantite est donnee par un registre (octet de poids faible de Rs).
// "carry" contient la retenue actuelle en entree et recoit la nouvelle retenue en sortie.
static uint32_t shiftReg(uint32_t v, uint32_t type, uint32_t amt, bool& carry) {
    if (amt == 0) return v;                       // aucun decalage : valeur et retenue inchangees
    switch (type) {
    case 0:                                   // LSL
        if (amt < 32) { carry = (v >> (32 - amt)) & 1; return v << amt; }
        if (amt == 32) { carry = v & 1; return 0; }
        carry = false; return 0;
    case 1:                                   // LSR
        if (amt < 32) { carry = (v >> (amt - 1)) & 1; return v >> amt; }
        if (amt == 32) { carry = v >> 31; return 0; }
        carry = false; return 0;
    case 2:                                   // ASR
        if (amt < 32) { carry = (v >> (amt - 1)) & 1; return uint32_t(int32_t(v) >> amt); }
        carry = v >> 31;
        return uint32_t(int32_t(v) >> 31);
    default:                                  // ROR
        amt &= 31;
        if (amt == 0) { carry = v >> 31; return v; }
        carry = (v >> (amt - 1)) & 1;
        return ror32(v, amt);
    }
}

// Entree dans le gestionnaire d'interruption : mode IRQ, IRQ masquees, saut au vecteur 0x18
// (vecteurs hauts 0xFFFF0000 sur l'ARM9 si le bit V du registre de controle est mis).
void ArmCpu::enterIrq() {
    irqCount++;
    if (diagnostics) {
        irqTrace.clear();                            // on repart d'une trace propre pour cette interruption
        traceSeen.clear();
        traceSkipped = 0;
        tracing = true;
    }
    uint32_t old = cpsr;
    switchMode(0x12);                       // sauvegarde r13/r14 du mode actuel, charge ceux du mode IRQ
    spsr[2] = old;
    cpsr = (old & ~0x3Fu) | 0x12u | 0x80u;  // mode IRQ, IRQ desactivees, etat Thumb efface
    r[14] = pc + 4;                         // adresse de retour + 4 : on revient par SUBS pc, lr, #4
    pc = ((v5 && (cp15Control & (1u << 13))) ? 0xFFFF0000u : 0u) + 0x18;
    branched = false;
}

uint32_t ArmCpu::biosIfAddr() const {
    return v5 ? ((cp15Dtcm & 0xFFFFF000u) + 0x3FF8u) : 0x0380FFF8u;
}

bool ArmCpu::step() {
    if (*halt_) { *halt_ = false; halted = true; }               // ecriture de HALTCNT
    const IrqCtl& ic = *irq_;
    const bool wake = (ic.ie & ic.iflag) != 0;
    if (wake && (ic.ime & 1) && !(cpsr & 0x80u)) {               // interruption demandee et autorisee
        halted = false;
        enterIrq();
        return true;
    }
    if (halted) {
        if (!wake) return true;                                  // on attend : rien a executer
        halted = false;                                          // reveille (IE & IF) meme si l'IRQ est masquee
    }

    lastPc = pc;
    const bool thumb = (cpsr & 0x20u) != 0;
    lastInstr = thumb ? bus.read16(pc) : bus.read32(pc);
    histPc[histCount % kHist] = lastPc;
    histInstr[histCount % kHist] = lastInstr;
    if (diagnostics && (histCount & 63) == 0) pcSamples[lastPc]++;
    if (tracing) {                                   // trace depuis la derniere interruption
        uint16_t& seen = traceSeen[lastPc];
        if (seen < 3 && irqTrace.size() < 1500) {
            seen++;
            traceSkipped = 0;
            irqTrace.push_back({ lastPc, lastInstr, r[0], r[1], r[2], r[3], cpsr });
        }
        else if (++traceSkipped > 20000) {
            tracing = false;                         // le processeur tourne en boucle : on arrete
        }
    }
    histCount++;
    uint32_t instr = lastInstr;
    branched = false;

    if (thumb) {
        bool okT = execThumb(uint16_t(instr));
        if (okT && !branched) pc += 2;
        return okT;
    }

    uint32_t c = instr >> 28;
    if (c == 0xF) {                                  // instructions "inconditionnelles" (ARMv5)
        if (v5 && ((instr >> 25) & 7) == 5) {        // BLX immediat : appel vers du code Thumb
            int32_t off = int32_t(instr << 8) >> 6;  // imm24 * 4
            if ((instr >> 24) & 1) off += 2;         // bit H : demi-mot supplementaire
            r[14] = pc + 4;
            enterThumbState(true);
            pc = pc + 8 + uint32_t(off);
            return true;
        }
        if (v5 && (instr & 0xFD70F000) == 0xF550F000) {   // PLD (prechargement de cache) : sans effet
            pc += 4;
            return true;
        }
        return fail("instruction inconditionnelle (cond=0xF) non geree");
    }
    if (!cond(c)) { pc += 4; return true; }   // condition fausse : on saute l'instruction

    bool ok;
    switch ((instr >> 25) & 7) {
    case 0: {
        if ((instr & 0x90) == 0x90) {                 // bit7 et bit4 : multiplication / halfword
            if ((instr & 0x60) == 0) {
                if ((instr & 0x0FC000F0) == 0x00000090)      ok = execMul(instr);
                else if ((instr & 0x0F8000F0) == 0x00800090) ok = execMulLong(instr);
                else if ((instr & 0x0FB00FF0) == 0x01000090) ok = execSwp(instr);
                else ok = fail("instruction multiplication/SWP non geree");
            }
            else {
                ok = execHalfword(instr);
            }
            break;
        }
        uint32_t op = (instr >> 21) & 0xF;
        bool S = (instr >> 20) & 1;
        ok = (op >= 8 && op <= 11 && !S) ? execMisc(instr) : execDataProc(instr);
        break;
    }
    case 1: {
        uint32_t op = (instr >> 21) & 0xF;
        bool S = (instr >> 20) & 1;
        ok = (op >= 8 && op <= 11 && !S) ? execMisc(instr) : execDataProc(instr);
        break;
    }
    case 2: ok = execLoadStore(instr); break;
    case 3: ok = (instr & 0x10) ? fail("instruction non definie/media") : execLoadStore(instr); break;
    case 4: ok = execBlock(instr); break;
    case 5: ok = execBranch(instr); break;
    case 7: ok = execCoproc(instr); break;
    default: ok = fail("type d'instruction non gere"); break;
    }
    if (ok && !branched) pc += 4;
    return ok;
}

bool ArmCpu::execDataProc(uint32_t instr) {
    bool I = (instr >> 25) & 1;
    uint32_t op = (instr >> 21) & 0xF;
    bool S = (instr >> 20) & 1;
    unsigned Rn = (instr >> 16) & 0xF, Rd = (instr >> 12) & 0xF;

    bool regShift = !I && (instr & 0x10);      // operande 2 decale par la valeur d'un registre
    // S avec Rd = PC (MOVS pc,lr ; SUBS pc,lr,#4...) : retour d'exception, CPSR <- SPSR
    const bool exceptionReturn = S && Rd == 15 && !(op >= 8 && op <= 11);
    const bool flagsWanted = S && !exceptionReturn;

    // Operande 2 + retenue du decaleur
    uint32_t op2;
    bool shC = (cpsr & FLAG_C) != 0;
    if (I) {
        uint32_t rot = ((instr >> 8) & 0xF) * 2;
        op2 = ror32(instr & 0xFF, rot);
        if (rot) shC = op2 >> 31;
    }
    else if (regShift) {
        unsigned Rm = instr & 0xF, Rs = (instr >> 8) & 0xF;
        if (Rs == 15) return fail("decalage par registre avec Rs=PC non gere");
        uint32_t v = (Rm == 15) ? pc + 12 : r[Rm];     // avec un decalage par registre, PC se lit +12
        op2 = shiftReg(v, (instr >> 5) & 3, r[Rs] & 0xFF, shC);
    }
    else {
        op2 = shiftImm(reg(instr & 0xF), (instr >> 5) & 3, (instr >> 7) & 0x1F, shC);
    }

    uint32_t a = (regShift && Rn == 15) ? pc + 12 : reg(Rn);
    uint32_t res = 0;
    bool logical = false, writeResult = true;
    uint64_t wide = 0;
    bool arith = false;
    uint32_t cin = (cpsr & FLAG_C) ? 1 : 0;
    // ~x sur 32 bits puis elargi a 64 bits (ecrit avec un XOR pour eviter l'avertissement MSVC)
    const uint64_t nOp2 = uint64_t(op2 ^ 0xFFFFFFFFu);
    const uint64_t nA = uint64_t(a ^ 0xFFFFFFFFu);

    switch (op) {
    case 0x0: res = a & op2; logical = true; break;                              // AND
    case 0x1: res = a ^ op2; logical = true; break;                              // EOR
    case 0x2: wide = uint64_t(a) + nOp2 + 1; arith = true; break;                // SUB
    case 0x3: wide = uint64_t(op2) + nA + 1; arith = true; break;                // RSB
    case 0x4: wide = uint64_t(a) + op2; arith = true; break;                     // ADD
    case 0x5: wide = uint64_t(a) + op2 + cin; arith = true; break;               // ADC
    case 0x6: wide = uint64_t(a) + nOp2 + cin; arith = true; break;              // SBC
    case 0x7: wide = uint64_t(op2) + nA + cin; arith = true; break;              // RSC
    case 0x8: res = a & op2; logical = true; writeResult = false; break;         // TST
    case 0x9: res = a ^ op2; logical = true; writeResult = false; break;         // TEQ
    case 0xA: wide = uint64_t(a) + nOp2 + 1; arith = true; writeResult = false; break;  // CMP
    case 0xB: wide = uint64_t(a) + op2; arith = true; writeResult = false; break;       // CMN
    case 0xC: res = a | op2; logical = true; break;                              // ORR
    case 0xD: res = op2; logical = true; break;                                  // MOV
    case 0xE: res = a & ~op2; logical = true; break;                             // BIC
    default:  res = ~op2; logical = true; break;                                 // MVN
    }

    if (arith) {
        res = uint32_t(wide);
        if (flagsWanted) {
            // operandes effectifs de l'addition x + y (y = ~op2 ou ~a pour les soustractions)
            uint32_t x, y;
            switch (op) {
            case 0x2: case 0x6: case 0xA: x = a;   y = ~op2; break;
            case 0x3: case 0x7:           x = op2; y = ~a;   break;
            default:                      x = a;   y = op2;  break;
            }
            cpsr &= ~(FLAG_C | FLAG_V);
            if (wide >> 32) cpsr |= FLAG_C;
            if (~(x ^ y) & (x ^ res) & 0x80000000u) cpsr |= FLAG_V;
        }
    }
    else if (logical && flagsWanted) {
        cpsr = (cpsr & ~FLAG_C) | (shC ? FLAG_C : 0);
    }
    if (flagsWanted) setNZ(res);
    if (exceptionReturn) {
        if (!returnFromException()) return false;
        pc = res & ((cpsr & 0x20u) ? ~1u : ~3u);      // le bit T vient du SPSR restaure
        branched = true;
        return true;
    }
    if (writeResult) setReg(Rd, res);
    return true;
}

// Instructions "diverses" : BX, BLX (reg), CLZ, MRS, MSR.
bool ArmCpu::execMisc(uint32_t instr) {
    if ((instr & 0x0FFFFFD0) == 0x012FFF10) {                     // BX / BLX (registre)
        bool link = (instr >> 5) & 1;
        if (link && !v5) return fail("BLX (registre) n'existe pas sur ARM7");
        uint32_t target = reg(instr & 0xF);
        if (link) r[14] = pc + 4;
        if (target & 1) { enterThumbState(true);  pc = target & ~1u; }
        else { enterThumbState(false); pc = target & ~3u; }
        branched = true;
        return true;
    }
    if ((instr & 0x0FFF0FF0) == 0x016F0F10) {                     // CLZ
        if (!v5) return fail("CLZ n'existe pas sur ARM7");
        unsigned Rd = (instr >> 12) & 0xF;
        uint32_t v = reg(instr & 0xF);
        uint32_t n = 0;
        if (v == 0) n = 32;
        else while (!(v & 0x80000000u)) { v <<= 1; n++; }
        if (Rd == 15) return fail("CLZ avec Rd=PC non gere");
        r[Rd] = n;
        return true;
    }
    if ((instr & 0x0FBF0FFF) == 0x010F0000) {                     // MRS
        unsigned Rd = (instr >> 12) & 0xF;
        if (Rd == 15) return fail("MRS avec Rd=PC non gere");
        if ((instr >> 22) & 1) {
            int idx = bankIndex(cpsr & 0x1F);
            if (idx <= 0) return fail("MRS SPSR en mode User/System");
            r[Rd] = spsr[idx];
        }
        else {
            r[Rd] = cpsr;
        }
        return true;
    }
    if ((instr & 0x0FB0FFF0) == 0x0120F000 || (instr & 0x0FB0F000) == 0x0320F000) {   // MSR
        bool useSpsr = (instr >> 22) & 1;
        uint32_t value;
        if (instr & 0x02000000) value = ror32(instr & 0xFF, ((instr >> 8) & 0xF) * 2);
        else                    value = reg(instr & 0xF);
        uint32_t fm = (instr >> 16) & 0xF;
        uint32_t mask = ((fm & 1) ? 0x000000FFu : 0) | ((fm & 2) ? 0x0000FF00u : 0) |
            ((fm & 4) ? 0x00FF0000u : 0) | ((fm & 8) ? 0xFF000000u : 0);
        if (useSpsr) {
            int idx = bankIndex(cpsr & 0x1F);
            if (idx <= 0) return fail("MSR SPSR en mode User/System");
            spsr[idx] = (spsr[idx] & ~mask) | (value & mask);
        }
        else {
            if ((cpsr & 0x1F) == 0x10) mask &= 0xFF000000u;   // mode User : drapeaux seulement
            mask &= ~0x20u;                                    // le bit Thumb n'est pas modifiable par MSR
            uint32_t nv = (cpsr & ~mask) | (value & mask);
            if (bankIndex(nv & 0x1F) < 0) return fail("MSR: mode invalide");
            switchMode(nv & 0x1F);
            cpsr = nv;
        }
        return true;
    }
    // Extension "DSP" de l'ARMv5TE (ARM9 seulement) : additions saturantes et multiplications 16 bits.
    if (v5) {
        const uint32_t hi8 = (instr >> 20) & 0xFF, lo4 = (instr >> 4) & 0xF;
        const unsigned fA = (instr >> 16) & 0xF, fB = (instr >> 12) & 0xF, rs = (instr >> 8) & 0xF, rm = instr & 0xF;
        const bool isDspOp = hi8 == 0x10 || hi8 == 0x12 || hi8 == 0x14 || hi8 == 0x16;
        auto half = [](uint32_t v, bool top) -> int32_t { return top ? int32_t(int16_t(v >> 16)) : int32_t(int16_t(v)); };
        // addition 32 bits : le resultat est tronque, le drapeau Q (bit 27) signale un depassement
        auto addQ = [&](int32_t a, int32_t b) -> uint32_t {
            int64_t sum = int64_t(a) + b;
            if (sum != int64_t(int32_t(sum))) cpsr |= (1u << 27);
            return uint32_t(sum);
            };
        if (lo4 == 0x5 && isDspOp) {                                   // QADD, QSUB, QDADD, QDSUB
            const unsigned rn = fA, rd = fB;
            if (rd == 15 || rm == 15 || rn == 15) return fail("instruction saturante avec PC non geree");
            auto sat = [&](int64_t v) -> int32_t {
                if (v > INT32_MAX) { cpsr |= (1u << 27); return INT32_MAX; }
                if (v < INT32_MIN) { cpsr |= (1u << 27); return INT32_MIN; }
                return int32_t(v);
                };
            int64_t b = int32_t(r[rn]);
            if (hi8 == 0x14 || hi8 == 0x16) b = sat(b * 2);            // QDADD / QDSUB : l'operande est doublee (avec saturation)
            const int64_t a = int32_t(r[rm]);
            r[rd] = uint32_t(sat((hi8 == 0x10 || hi8 == 0x14) ? a + b : a - b));
            return true;
        }
        if ((lo4 & 0x9) == 0x8 && isDspOp) {                           // multiplications 16 bits
            const bool x = (lo4 >> 1) & 1, y = (lo4 >> 2) & 1;
            if (fA == 15 || rs == 15 || rm == 15) return fail("multiplication 16 bits avec PC non geree");
            switch (hi8) {
            case 0x10:                                             // SMLAxy : Rd = Rm.x * Rs.y + Rn
                if (fB == 15) return fail("SMLAxy avec Rn=PC non gere");
                r[fA] = addQ(half(r[rm], x) * half(r[rs], y), int32_t(r[fB]));
                return true;
            case 0x12: {                                           // SMLAWy (x=0) / SMULWy (x=1) : (Rm * Rs.y) >> 16
                int64_t p = (int64_t(int32_t(r[rm])) * half(r[rs], y)) >> 16;
                if (x) r[fA] = uint32_t(p);
                else   r[fA] = addQ(int32_t(p), int32_t(r[fB]));
                return true;
            }
            case 0x14: {                                           // SMLALxy : RdHi:RdLo += Rm.x * Rs.y
                if (fB == 15) return fail("SMLALxy avec RdLo=PC non gere");
                uint64_t acc = (uint64_t(r[fA]) << 32) | r[fB];
                acc += uint64_t(int64_t(half(r[rm], x)) * half(r[rs], y));
                r[fB] = uint32_t(acc);
                r[fA] = uint32_t(acc >> 32);
                return true;
            }
            default:                                               // SMULxy : Rd = Rm.x * Rs.y
                r[fA] = uint32_t(half(r[rm], x) * half(r[rs], y));
                return true;
            }
        }
    }
    return fail("instruction diverse non geree (BKPT, SMLAxy sur ARM7...)");
}

bool ArmCpu::execMul(uint32_t instr) {
    bool A = (instr >> 21) & 1, S = (instr >> 20) & 1;
    unsigned Rd = (instr >> 16) & 0xF, Rn = (instr >> 12) & 0xF;
    unsigned Rs = (instr >> 8) & 0xF, Rm = instr & 0xF;
    if (Rd == 15 || Rm == 15 || Rs == 15 || (A && Rn == 15)) return fail("MUL/MLA avec PC non gere");
    uint32_t res = r[Rm] * r[Rs] + (A ? r[Rn] : 0);
    r[Rd] = res;
    if (S) setNZ(res);
    return true;
}

// UMULL / UMLAL / SMULL / SMLAL : multiplication 32x32 -> 64 bits
bool ArmCpu::execMulLong(uint32_t instr) {
    bool sgn = (instr >> 22) & 1, acc = (instr >> 21) & 1, S = (instr >> 20) & 1;
    unsigned hi = (instr >> 16) & 0xF, lo = (instr >> 12) & 0xF;
    unsigned Rs = (instr >> 8) & 0xF, Rm = instr & 0xF;
    if (hi == 15 || lo == 15 || Rs == 15 || Rm == 15 || hi == lo)
        return fail("multiplication longue avec registres invalides (PC ou RdHi=RdLo)");
    uint64_t res;
    if (sgn) res = uint64_t(int64_t(int32_t(r[Rm])) * int64_t(int32_t(r[Rs])));
    else     res = uint64_t(r[Rm]) * uint64_t(r[Rs]);
    if (acc) res += (uint64_t(r[hi]) << 32) | r[lo];
    r[lo] = uint32_t(res);
    r[hi] = uint32_t(res >> 32);
    if (S) {
        cpsr &= ~(FLAG_N | FLAG_Z);
        if (res >> 63) cpsr |= FLAG_N;
        if (res == 0) cpsr |= FLAG_Z;
    }
    return true;
}

// SWP / SWPB : echange atomique entre un registre et la memoire (verrous du systeme)
bool ArmCpu::execSwp(uint32_t instr) {
    bool B = (instr >> 22) & 1;
    unsigned Rn = (instr >> 16) & 0xF, Rd = (instr >> 12) & 0xF, Rm = instr & 0xF;
    if (Rn == 15 || Rd == 15 || Rm == 15) return fail("SWP avec PC non gere");
    uint32_t addr = r[Rn], newVal = r[Rm], old;
    if (B) {
        old = bus.read8(addr);
        bus.write8(addr, uint8_t(newVal));
    }
    else {
        old = ror32(bus.read32(addr), (addr & 3) * 8);
        bus.write32(addr, newVal);
    }
    r[Rd] = old;
    return true;
}

bool ArmCpu::execHalfword(uint32_t instr) {
    bool P = (instr >> 24) & 1, U = (instr >> 23) & 1, Imm = (instr >> 22) & 1;
    bool W = (instr >> 21) & 1, L = (instr >> 20) & 1;
    unsigned Rn = (instr >> 16) & 0xF, Rd = (instr >> 12) & 0xF, sh = (instr >> 5) & 3;
    if (Rd == 15) return fail("transfert halfword avec Rd=PC non gere");

    uint32_t off = Imm ? (((instr >> 4) & 0xF0) | (instr & 0xF)) : reg(instr & 0xF);
    uint32_t base = reg(Rn);
    uint32_t withOff = U ? base + off : base - off;
    uint32_t addr = P ? withOff : base;

    if (!L && sh >= 2) {                             // LDRD (sh = 2) / STRD (sh = 3) : Rd et Rd+1, ARMv5TE
        if (!v5) return fail("LDRD/STRD n'existe pas sur ARM7");
        if ((Rd & 1) || Rd == 14) return fail("LDRD/STRD avec un registre impair (ou r14) non gere");
        const uint32_t lo = r[Rd], hi = r[Rd + 1];   // lus AVANT le write-back
        if ((!P || W) && Rn != 15) r[Rn] = withOff;
        // (adresse alignee sur 4 par les lectures / ecritures 32 bits)
        if (sh == 2) { r[Rd] = bus.read32(addr); r[Rd + 1] = bus.read32(addr + 4); }
        else         { bus.write32(addr, lo);   bus.write32(addr + 4, hi); }
        return true;
    }

    uint32_t storeVal = r[Rd];                       // lu AVANT le write-back
    if ((!P || W) && Rn != 15) r[Rn] = withOff;

    if (L) {
        uint32_t v;
        if (sh == 1)      v = ror32(bus.read16(addr), (addr & 1) * 8);          // LDRH
        else if (sh == 2) v = uint32_t(int32_t(int8_t(bus.read8(addr))));        // LDRSB
        else              v = uint32_t(int32_t(int16_t(bus.read16(addr))));      // LDRSH
        r[Rd] = v;
    }
    else {
        bus.write16(addr, uint16_t(storeVal));                                   // STRH
    }
    return true;
}

bool ArmCpu::execLoadStore(uint32_t instr) {
    bool I = (instr >> 25) & 1, P = (instr >> 24) & 1, U = (instr >> 23) & 1;
    bool B = (instr >> 22) & 1, W = (instr >> 21) & 1, L = (instr >> 20) & 1;
    unsigned Rn = (instr >> 16) & 0xF, Rd = (instr >> 12) & 0xF;

    uint32_t offset;
    if (!I) {
        offset = instr & 0xFFF;
    }
    else {
        bool dummy;
        offset = shiftImm(reg(instr & 0xF), (instr >> 5) & 3, (instr >> 7) & 0x1F, dummy);
    }

    uint32_t base = reg(Rn);
    uint32_t withOffset = U ? base + offset : base - offset;
    uint32_t addr = P ? withOffset : base;
    uint32_t storeVal = (Rd == 15) ? pc + 12 : r[Rd];   // lu AVANT le write-back
    if ((!P || W) && Rn != 15) r[Rn] = withOffset;

    if (L) {
        uint32_t v;
        if (B) v = bus.read8(addr);
        else   v = ror32(bus.read32(addr), (addr & 3) * 8);   // lecture non alignee = rotation
        if (v5 && Rd == 15) {                          // ARMv5 : le bit 0 choisit ARM / Thumb
            if (v & 1) { enterThumbState(true);  pc = v & ~1u; }
            else { enterThumbState(false); pc = v & ~3u; }
            branched = true;
        }
        else {
            setReg(Rd, v);
        }
    }
    else {
        if (B) bus.write8(addr, uint8_t(storeVal));
        else   bus.write32(addr, storeVal);
    }
    return true;
}

// LDM / STM (empiler / depiler plusieurs registres).
bool ArmCpu::execBlock(uint32_t instr) {
    bool P = (instr >> 24) & 1, U = (instr >> 23) & 1, S = (instr >> 22) & 1;
    bool W = (instr >> 21) & 1, L = (instr >> 20) & 1;
    unsigned Rn = (instr >> 16) & 0xF;
    uint32_t list = instr & 0xFFFF;

    if (Rn == 15) return fail("LDM/STM avec Rn=PC non gere");
    if (list == 0) return fail("LDM/STM avec liste vide non gere");

    // Bit S : - LDM avec PC dans la liste : retour d'exception (CPSR <- SPSR)
    //         - sinon : transfert des registres du mode User, meme depuis un autre mode
    const bool restore = S && L && (list & 0x8000u);
    const bool userBank = S && !restore;
    // (Avec S et write-back, le registre de base mis a jour est celui du mode courant : le SDK Nintendo
    //  l'utilise pour sauvegarder le contexte d'un thread, avec une base dans r0-r7, commune a tous les modes.)

    unsigned count = 0;
    for (unsigned i = 0; i < 16; i++) if (list & (1u << i)) count++;

    uint32_t base = r[Rn];
    uint32_t total = 4 * count;
    uint32_t addr = U ? (P ? base + 4 : base) : (P ? base - total : base - total + 4);
    uint32_t wb = U ? base + total : base - total;
    uint32_t rawPc = 0;

    for (unsigned i = 0; i < 16; i++) {
        if (!(list & (1u << i))) continue;
        if (L) {
            uint32_t v = bus.read32(addr);
            if (i == 15) {
                if (restore)                  rawPc = v;
                else if (v5 && (v & 1)) { enterThumbState(true); pc = v & ~1u; branched = true; }
                else { setReg(15, v); }
            }
            else if (userBank) {
                userReg(i) = v;
            }
            else {
                r[i] = v;
            }
        }
        else {
            uint32_t v = (i == 15) ? pc + 12 : (userBank ? userReg(i) : r[i]);
            bus.write32(addr, v);
        }
        addr += 4;
    }
    // Write-back (en cas de LDM avec Rn dans la liste, la valeur chargee l'emporte)
    if (W && !(L && (list & (1u << Rn)))) r[Rn] = wb;

    if (restore) {                                   // retour d'exception : fait APRES le write-back
        if (!returnFromException()) return false;
        pc = rawPc & ((cpsr & 0x20u) ? ~1u : ~3u);
        branched = true;
    }
    return true;
}

bool ArmCpu::execBranch(uint32_t instr) {
    int32_t off = int32_t(instr << 8) >> 6;     // imm24 etendu en signe, multiplie par 4
    if ((instr >> 24) & 1) r[14] = pc + 4;      // BL : adresse de retour
    pc = pc + 8 + uint32_t(off);
    branched = true;
    return true;
}

bool ArmCpu::cp15Read(unsigned crn, unsigned crm, unsigned opc2, uint32_t& out) {
    switch (crn) {
    case 0:
        if (crm != 0) return false;
        if (opc2 == 0) { out = 0x41059461; return true; }   // identifiant (ARM946E-S)
        if (opc2 == 1) { out = 0x0F0D2112; return true; }   // type de cache
        if (opc2 == 2) { out = 0x00140180; return true; }   // taille des TCM
        return false;
    case 1:
        if (crm == 0 && opc2 == 0) { out = cp15Control; return true; }
        return false;
    case 9:
        if (crm == 1 && opc2 == 0) { out = cp15Dtcm; return true; }
        if (crm == 1 && opc2 == 1) { out = cp15Itcm; return true; }
        out = 0; return true;
    case 2: case 3: case 5: case 6: case 7: case 13:
        out = 0; return true;      // MPU / caches : pas emules, lecture = 0
    default:
        return false;
    }
}

bool ArmCpu::cp15Write(unsigned crn, unsigned crm, unsigned opc2, uint32_t v) {
    switch (crn) {
    case 1:
        if (crm == 0 && opc2 == 0) {
            cp15Control = (v & 0x000FF085u) | 0x00000078u;
            bus.configureTcm(cp15Control, cp15Dtcm, cp15Itcm);
            return true;
        }
        return false;
    case 9:
        if (crm == 1 && (opc2 == 0 || opc2 == 1)) {
            if (opc2 == 0) cp15Dtcm = v; else cp15Itcm = v;
            bus.configureTcm(cp15Control, cp15Dtcm, cp15Itcm);
            return true;
        }
        return true;               // verrouillage de cache : ignore
    case 7:
        if ((crm == 0 && opc2 == 4) || (crm == 8 && opc2 == 2)) halted = true;   // attente d'interruption
        return true;               // autres operations de cache : ignorees
    case 2: case 3: case 5: case 6: case 13:
        return true;               // MPU : ignoree
    default:
        return false;
    }
}

bool ArmCpu::execCoproc(uint32_t instr) {
    if (((instr >> 24) & 0xF) == 0xF) return hleSwi((instr >> 16) & 0xFF);   // SWI : numero = bits 16-23
    if (!v5) return fail("instruction coprocesseur sur ARM7 (non gere)");
    if (!(instr & 0x10)) return fail("CDP (operation de coprocesseur) non gere");
    if (((instr >> 8) & 0xF) != 15) return fail("coprocesseur autre que CP15 non gere");
    bool L = (instr >> 20) & 1;
    unsigned opc1 = (instr >> 21) & 7, crn = (instr >> 16) & 0xF, rd = (instr >> 12) & 0xF;
    unsigned opc2 = (instr >> 5) & 7, crm = instr & 0xF;
    if (opc1 != 0) return fail("CP15 avec opcode1 != 0 non gere");
    if (rd == 15) return fail("MRC/MCR avec Rd=PC non gere");
    if (L) {
        uint32_t v;
        if (!cp15Read(crn, crm, opc2, v)) return fail("CP15: registre en lecture non gere");
        r[rd] = v;
    }
    else {
        if (!cp15Write(crn, crm, opc2, r[rd])) return fail("CP15: registre en ecriture non gere");
    }
    return true;
}

void ArmCpu::printHistory() const {
    std::printf("\n--- Dernieres instructions %s ---\n", name.c_str());
    uint64_t n = histCount < kHist ? histCount : kHist;
    for (uint64_t i = histCount - n; i < histCount; i++)
        std::printf("%08X: %08X\n", histPc[i % kHist], histInstr[i % kHist]);
}

bool ArmCpu::writeIrqTrace(const char* path) const {
    if (irqTrace.empty()) return false;
    FILE* f = std::fopen(path, "w");
    if (!f) return false;
    std::fprintf(f, "Trace %s depuis la derniere interruption (chaque adresse notee 3 fois au plus)\n", name.c_str());
    std::fprintf(f, "adresse:  mot       r0       r1       r2       r3       cpsr\n");
    for (const TraceEntry& e : irqTrace)
        std::fprintf(f, "%08X: %08X  %08X %08X %08X %08X  %08X\n", e.pc, e.instr, e.r0, e.r1, e.r2, e.r3, e.cpsr);
    std::fclose(f);
    return true;
}

void ArmCpu::printHotSpots() const {
    std::vector<std::pair<uint32_t, uint32_t>> v;
    for (const auto& kv : pcSamples) v.push_back({ kv.second, kv.first });
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::printf("\n--- %s : adresses les plus frequentes (1 echantillon = 64 instructions) ---\n", name.c_str());
    for (size_t i = 0; i < v.size() && i < 10; i++)
        std::printf("%08X : %u\n", v[i].second, v[i].first);
}

void ArmCpu::dump() const {
    std::printf("\n[%s] %llu instructions\n", name.c_str(), (unsigned long long)histCount);
    for (int i = 0; i < 12; i += 4)
        std::printf("r%-2d=%08X r%-2d=%08X r%-2d=%08X r%-2d=%08X\n",
            i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
    std::printf("r12=%08X sp =%08X lr =%08X pc =%08X\n", r[12], r[13], r[14], pc);
    std::printf("interruptions prises : %llu ; bit I (IRQ masquees) = %d, bit F = %d\n",
        (unsigned long long)irqCount, (cpsr >> 7) & 1, (cpsr >> 6) & 1);
    std::printf("cpsr=%08X [%c%c%c%c] mode %s%s\n", cpsr,
        cpsr & FLAG_N ? 'N' : '-', cpsr & FLAG_Z ? 'Z' : '-',
        cpsr & FLAG_C ? 'C' : '-', cpsr & FLAG_V ? 'V' : '-', modeName(cpsr & 0x1F),
        (cpsr & 0x20u) ? " (Thumb)" : "");
    if (halted) std::printf("(en attente d'une interruption)\n");
}


// ====================================================================== Thumb

// a + b + carryIn ; met a jour N, Z, C, V si "flags". Une soustraction a - b s'ecrit a + ~b + 1.
uint32_t ArmCpu::addWithFlags(uint32_t a, uint32_t b, uint32_t carryIn, bool flags) {
    uint64_t wide = uint64_t(a) + uint64_t(b) + carryIn;
    uint32_t res = uint32_t(wide);
    if (flags) {
        cpsr &= ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V);
        if (res & 0x80000000u) cpsr |= FLAG_N;
        if (res == 0) cpsr |= FLAG_Z;
        if (wide >> 32) cpsr |= FLAG_C;
        if (~(a ^ b) & (a ^ res) & 0x80000000u) cpsr |= FLAG_V;
    }
    return res;
}

bool ArmCpu::execThumb(uint16_t op) {
    const uint32_t tpc = pc + 4;      // en Thumb, PC se lit "adresse de l'instruction + 4"

    switch (op >> 13) {
    case 0: {                         // 000 : decalage immediat, addition/soustraction
        if (((op >> 11) & 3) != 3) {                       // format 1 : LSL / LSR / ASR immediat
            unsigned type = (op >> 11) & 3, amt = (op >> 6) & 0x1F, Rs = (op >> 3) & 7, Rd = op & 7;
            bool c;
            uint32_t res = shiftImm(r[Rs], type, amt, c);
            r[Rd] = res;
            cpsr = (cpsr & ~FLAG_C) | (c ? FLAG_C : 0);
            setNZ(res);
        }
        else {                                           // format 2 : ADDS / SUBS
            bool imm = (op >> 10) & 1, sub = (op >> 9) & 1;
            unsigned n = (op >> 6) & 7, Rs = (op >> 3) & 7, Rd = op & 7;
            uint32_t b = imm ? n : r[n];
            r[Rd] = sub ? addWithFlags(r[Rs], ~b, 1, true) : addWithFlags(r[Rs], b, 0, true);
        }
        return true;
    }

    case 1: {                         // 001 : MOVS / CMP / ADDS / SUBS avec immediat 8 bits
        unsigned o = (op >> 11) & 3, Rd = (op >> 8) & 7;
        uint32_t imm = op & 0xFF;
        switch (o) {
        case 0:  r[Rd] = imm; setNZ(imm); break;
        case 1:  addWithFlags(r[Rd], ~imm, 1, true); break;
        case 2:  r[Rd] = addWithFlags(r[Rd], imm, 0, true); break;
        default: r[Rd] = addWithFlags(r[Rd], ~imm, 1, true); break;
        }
        return true;
    }

    case 2: {                         // 010 : ALU, registres hauts, chargement relatif a PC, load/store registre
        if ((op >> 10) == 0x10) {                          // format 4 : operations ALU
            unsigned o = (op >> 6) & 0xF, Rs = (op >> 3) & 7, Rd = op & 7;
            uint32_t a = r[Rd], b = r[Rs], res = 0;
            bool writeBack = true, c = (cpsr & FLAG_C) != 0;
            uint32_t cin = c ? 1u : 0u;
            switch (o) {
            case 0x0: res = a & b; break;                                           // AND
            case 0x1: res = a ^ b; break;                                           // EOR
            case 0x2: res = shiftReg(a, 0, b & 0xFF, c); cpsr = (cpsr & ~FLAG_C) | (c ? FLAG_C : 0); break;  // LSL
            case 0x3: res = shiftReg(a, 1, b & 0xFF, c); cpsr = (cpsr & ~FLAG_C) | (c ? FLAG_C : 0); break;  // LSR
            case 0x4: res = shiftReg(a, 2, b & 0xFF, c); cpsr = (cpsr & ~FLAG_C) | (c ? FLAG_C : 0); break;  // ASR
            case 0x5: r[Rd] = addWithFlags(a, b, cin, true); return true;           // ADC
            case 0x6: r[Rd] = addWithFlags(a, ~b, cin, true); return true;          // SBC
            case 0x7: res = shiftReg(a, 3, b & 0xFF, c); cpsr = (cpsr & ~FLAG_C) | (c ? FLAG_C : 0); break;  // ROR
            case 0x8: res = a & b; writeBack = false; break;                        // TST
            case 0x9: r[Rd] = addWithFlags(0, ~b, 1, true); return true;            // NEG
            case 0xA: addWithFlags(a, ~b, 1, true); return true;                    // CMP
            case 0xB: addWithFlags(a, b, 0, true); return true;                     // CMN
            case 0xC: res = a | b; break;                                           // ORR
            case 0xD: res = a * b; break;                                           // MUL
            case 0xE: res = a & ~b; break;                                          // BIC
            default:  res = ~b; break;                                              // MVN
            }
            if (writeBack) r[Rd] = res;
            setNZ(res);
            return true;
        }
        if ((op >> 10) == 0x11) {                          // format 5 : registres hauts, BX / BLX
            unsigned o = (op >> 8) & 3;
            bool h1 = (op >> 7) & 1, h2 = (op >> 6) & 1;
            unsigned Rd = (op & 7) | (h1 ? 8u : 0u), Rs = ((op >> 3) & 7) | (h2 ? 8u : 0u);
            uint32_t vs = (Rs == 15) ? tpc : r[Rs];
            uint32_t vd = (Rd == 15) ? tpc : r[Rd];
            switch (o) {
            case 0: {                                  // ADD (sans modifier les drapeaux)
                uint32_t res = vd + vs;
                if (Rd == 15) { pc = res & ~1u; branched = true; }
                else r[Rd] = res;
                break;
            }
            case 1: addWithFlags(vd, ~vs, 1, true); break;           // CMP
            case 2:                                    // MOV (sans modifier les drapeaux)
                if (Rd == 15) { pc = vs & ~1u; branched = true; }
                else r[Rd] = vs;
                break;
            default:                                   // BX / BLX
                if (h1) {
                    if (!v5) return fail("BLX (registre) Thumb n'existe pas sur ARM7");
                    r[14] = (pc + 2) | 1u;
                }
                if (vs & 1) { pc = vs & ~1u; }
                else { enterThumbState(false); pc = vs & ~3u; }
                branched = true;
                break;
            }
            return true;
        }
        if ((op >> 11) == 0x09) {                          // format 6 : LDR Rd, [PC, #imm]
            unsigned Rd = (op >> 8) & 7;
            r[Rd] = bus.read32((tpc & ~3u) + (op & 0xFFu) * 4);
            return true;
        }
        // 0101 : formats 7 et 8, load/store avec registre d'offset
        unsigned Ro = (op >> 6) & 7, Rb = (op >> 3) & 7, Rd = op & 7;
        uint32_t addr = r[Rb] + r[Ro];
        if (!(op & 0x200)) {                               // format 7 : STR / STRB / LDR / LDRB
            bool L = (op >> 11) & 1, B = (op >> 10) & 1;
            if (L) r[Rd] = B ? bus.read8(addr) : ror32(bus.read32(addr), (addr & 3) * 8);
            else if (B) bus.write8(addr, uint8_t(r[Rd]));
            else        bus.write32(addr, r[Rd]);
        }
        else {                                           // format 8 : STRH / LDSB / LDRH / LDSH
            switch ((op >> 10) & 3) {
            case 0: bus.write16(addr, uint16_t(r[Rd])); break;
            case 1: r[Rd] = uint32_t(int32_t(int8_t(bus.read8(addr)))); break;
            case 2: r[Rd] = ror32(bus.read16(addr), (addr & 1) * 8); break;
            default: r[Rd] = uint32_t(int32_t(int16_t(bus.read16(addr)))); break;
            }
        }
        return true;
    }

    case 3: {                         // 011 : format 9, load/store avec offset immediat
        bool B = (op >> 12) & 1, L = (op >> 11) & 1;
        uint32_t off = (op >> 6) & 0x1F;
        unsigned Rb = (op >> 3) & 7, Rd = op & 7;
        uint32_t addr = r[Rb] + (B ? off : off * 4);
        if (L) r[Rd] = B ? bus.read8(addr) : ror32(bus.read32(addr), (addr & 3) * 8);
        else if (B) bus.write8(addr, uint8_t(r[Rd]));
        else        bus.write32(addr, r[Rd]);
        return true;
    }

    case 4: {                         // 100 : halfword immediat (1000) ou relatif a SP (1001)
        bool L = (op >> 11) & 1;
        if ((op >> 12) == 8) {                             // format 10 : STRH / LDRH
            uint32_t off = ((op >> 6) & 0x1F) * 2;
            unsigned Rb = (op >> 3) & 7, Rd = op & 7;
            uint32_t addr = r[Rb] + off;
            if (L) r[Rd] = ror32(bus.read16(addr), (addr & 1) * 8);
            else   bus.write16(addr, uint16_t(r[Rd]));
        }
        else {                                           // format 11 : STR / LDR relatif a SP
            unsigned Rd = (op >> 8) & 7;
            uint32_t addr = r[13] + (op & 0xFFu) * 4;
            if (L) r[Rd] = ror32(bus.read32(addr), (addr & 3) * 8);
            else   bus.write32(addr, r[Rd]);
        }
        return true;
    }

    case 5: {                         // 101 : adresse, ajustement de SP, push / pop
        if ((op >> 12) == 0xA) {                           // format 12 : ADD Rd, PC/SP, #imm
            unsigned Rd = (op >> 8) & 7;
            uint32_t base = (op & 0x0800) ? r[13] : (tpc & ~3u);
            r[Rd] = base + (op & 0xFFu) * 4;
            return true;
        }
        if ((op & 0xFF00) == 0xB000) {                     // format 13 : ADD / SUB SP, #imm
            uint32_t off = (op & 0x7Fu) * 4;
            if (op & 0x80) r[13] -= off; else r[13] += off;
            return true;
        }
        if ((op & 0xF600) == 0xB400) {                     // format 14 : PUSH / POP
            bool L = (op >> 11) & 1, R = (op >> 8) & 1;
            uint32_t list = op & 0xFF;
            unsigned count = R ? 1 : 0;
            for (unsigned i = 0; i < 8; i++) if (list & (1u << i)) count++;
            if (!L) {                                      // PUSH
                uint32_t sp = r[13] - 4 * count, addr = sp;
                for (unsigned i = 0; i < 8; i++)
                    if (list & (1u << i)) { bus.write32(addr, r[i]); addr += 4; }
                if (R) bus.write32(addr, r[14]);
                r[13] = sp;
            }
            else {                                       // POP
                uint32_t addr = r[13];
                for (unsigned i = 0; i < 8; i++)
                    if (list & (1u << i)) { r[i] = bus.read32(addr); addr += 4; }
                uint32_t v = 0;
                if (R) { v = bus.read32(addr); addr += 4; }
                r[13] = addr;
                if (R) {
                    if (v5) {                              // ARMv5 : le bit 0 choisit ARM / Thumb
                        if (v & 1) pc = v & ~1u;
                        else { enterThumbState(false); pc = v & ~3u; }
                    }
                    else {
                        pc = v & ~1u;
                    }
                    branched = true;
                }
            }
            return true;
        }
        return fail("instruction Thumb diverse non geree (BKPT, CPS...)");
    }

    case 6: {                         // 110 : LDMIA / STMIA, branchement conditionnel, SWI
        if ((op >> 12) == 0xC) {                           // format 15 : LDMIA / STMIA
            bool L = (op >> 11) & 1;
            unsigned Rb = (op >> 8) & 7;
            uint32_t list = op & 0xFF;
            if (list == 0) return fail("LDMIA/STMIA Thumb avec liste vide non gere");
            uint32_t addr = r[Rb];
            for (unsigned i = 0; i < 8; i++) {
                if (!(list & (1u << i))) continue;
                if (L) r[i] = bus.read32(addr);
                else   bus.write32(addr, r[i]);
                addr += 4;
            }
            if (!(L && (list & (1u << Rb)))) r[Rb] = addr;   // write-back
            return true;
        }
        unsigned cc = (op >> 8) & 0xF;                     // format 16 / 17
        if (cc == 0xF) return hleSwi(op & 0xFF);                 // SWI Thumb : numero = octet de poids faible
        if (cc == 0xE) return fail("instruction Thumb non definie (cond=0xE)");
        if (cond(cc)) {
            int32_t off = int32_t(int8_t(op & 0xFF)) * 2;
            pc = tpc + uint32_t(off);
            branched = true;
        }
        return true;
    }

    default: {                        // 111 : branchements
        switch (op >> 11) {
        case 0x1C: {                                   // format 18 : B
            int32_t off = int32_t(uint32_t(op & 0x7FF) << 21) >> 20;
            pc = tpc + uint32_t(off);
            branched = true;
            return true;
        }
        case 0x1E: {                                   // format 19, 1re moitie de BL
            int32_t hi = int32_t(uint32_t(op & 0x7FF) << 21) >> 9;
            r[14] = tpc + uint32_t(hi);
            return true;
        }
        case 0x1F: {                                   // 2e moitie de BL
            uint32_t target = r[14] + ((op & 0x7FFu) << 1);
            r[14] = (pc + 2) | 1u;
            pc = target & ~1u;
            branched = true;
            return true;
        }
        case 0x1D: {                                   // 2e moitie de BLX (ARMv5) : passe en mode ARM
            if (!v5) return fail("BLX (Thumb) n'existe pas sur ARM7");
            uint32_t target = (r[14] + ((op & 0x7FFu) << 1)) & ~3u;
            r[14] = (pc + 2) | 1u;
            enterThumbState(false);
            pc = target;
            branched = true;
            return true;
        }
        default:
            return fail("instruction Thumb non geree");
        }
    }
    }
}


// ====================================================================== SWI (BIOS emule)
//
// On n'execute pas le vrai BIOS : les fonctions courantes sont reproduites ici.
// Les registres non cites comme "resultat" sont laisses intacts (le vrai BIOS en modifie
// certains, mais un jeu ne doit pas compter dessus).

bool ArmCpu::hleSwi(unsigned num) {
    switch (num) {
    case 0x03:                                       // WaitByLoop : attente de r0 tours de boucle
        r[0] = 0;                                    // (le temps n'est pas encore emule)
        return true;

    case 0x09: {                                     // Divide : r0 / r1 -> r0 quotient, r1 reste, r3 |quotient|
        int32_t n = int32_t(r[0]), d = int32_t(r[1]);
        if (d == 0) return fail("SWI Divide : division par zero");
        int32_t q, m;
        if (n == INT32_MIN && d == -1) { q = INT32_MIN; m = 0; }
        else { q = n / d; m = n % d; }
        r[0] = uint32_t(q);
        r[1] = uint32_t(m);
        r[3] = (q < 0) ? (0u - uint32_t(q)) : uint32_t(q);
        return true;
    }

    case 0x0D: {                                     // Sqrt : r0 = racine carree entiere de r0
        uint32_t v = r[0], res = 0;
        for (uint32_t bit = 1u << 15; bit; bit >>= 1) {
            uint32_t t = res | bit;
            if (uint64_t(t) * t <= v) res = t;
        }
        r[0] = res;
        return true;
    }

    case 0x0B: {                                     // CpuSet : copie / remplissage de 16 ou 32 bits
        uint32_t src = r[0], dst = r[1], ctl = r[2];
        uint32_t count = ctl & 0x1FFFFF;
        bool fill = (ctl >> 24) & 1, word = (ctl >> 26) & 1;
        if (word) {
            src &= ~3u; dst &= ~3u;
            uint32_t v = fill ? bus.read32(src) : 0;
            for (uint32_t i = 0; i < count; i++) {
                if (!fill) v = bus.read32(src + 4 * i);
                bus.write32(dst + 4 * i, v);
            }
        }
        else {
            src &= ~1u; dst &= ~1u;
            uint16_t v = fill ? bus.read16(src) : uint16_t(0);
            for (uint32_t i = 0; i < count; i++) {
                if (!fill) v = bus.read16(src + 2 * i);
                bus.write16(dst + 2 * i, v);
            }
        }
        return true;
    }

    case 0x0C: {                                     // CpuFastSet : copie / remplissage par mots (multiple de 8)
        uint32_t src = r[0] & ~3u, dst = r[1] & ~3u, ctl = r[2];
        uint32_t count = ((ctl & 0x1FFFFF) + 7) & ~7u;
        bool fill = (ctl >> 24) & 1;
        uint32_t v = fill ? bus.read32(src) : 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!fill) v = bus.read32(src + 4 * i);
            bus.write32(dst + 4 * i, v);
        }
        return true;
    }

    case 0x0E: {                                     // GetCRC16 : r0 = valeur initiale, r1 = adresse, r2 = taille
        uint32_t crc = r[0] & 0xFFFF, addr = r[1], len = r[2];
        for (uint32_t i = 0; i < len; i++) {
            crc ^= bus.read8(addr + i);
            for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001u : (crc >> 1);
        }
        r[0] = crc;
        return true;
    }

    case 0x0F:                                       // IsDebugger : r0 = 0 (console du commerce)
        r[0] = 0;
        return true;

    case 0x10: {                                     // BitUnPack : r0 source, r1 destination, r2 = description
        // description : longueur source (16 bits), taille d'un element source et destination en bits, decalage
        // ajoute (bits 0-30) et bit 31 = l'ajouter aussi aux elements nuls. Ecrit par mots de 32 bits.
        const uint32_t info = r[2];
        const uint32_t srcLen = bus.read16(info), srcW = bus.read8(info + 2), dstW = bus.read8(info + 3);
        const uint32_t offs = bus.read32(info + 4);
        const bool zeroToo = (offs >> 31) & 1;
        const uint32_t addVal = offs & 0x7FFFFFFFu;
        if (!(srcW == 1 || srcW == 2 || srcW == 4 || srcW == 8) || !(dstW == 1 || dstW == 2 || dstW == 4 ||
              dstW == 8 || dstW == 16 || dstW == 32))
            return fail("SWI BitUnPack : tailles d'elements invalides");
        uint32_t src = r[0], dst = r[1] & ~3u, outWord = 0, outBits = 0;
        const uint32_t dstMask = dstW == 32 ? 0xFFFFFFFFu : ((1u << dstW) - 1);
        for (uint32_t i = 0; i < srcLen; i++) {
            const uint8_t byte = bus.read8(src + i);
            for (uint32_t b = 0; b < 8; b += srcW) {
                uint32_t v = (byte >> b) & ((1u << srcW) - 1);
                if (v || zeroToo) v += addVal;
                outWord |= (v & dstMask) << outBits;
                outBits += dstW;
                if (outBits >= 32) { bus.write32(dst, outWord); dst += 4; outWord = 0; outBits = 0; }
            }
        }
        return true;
    }

    case 0x12: case 0x15: {                          // LZ77 / RLE en ecriture 16 bits (VRAM) : on decompresse dans un
        std::vector<uint8_t> out;                    // tampon, puis on ecrit par demi-mots
        const uint32_t header = bus.read32(r[0]);
        const uint32_t size = header >> 8;
        if (num == 0x12 && (header & 0xF0) != 0x10) return fail("SWI LZ77 (16 bits) : en-tete de donnees invalide");
        if (num == 0x15 && (header & 0xF0) != 0x30) return fail("SWI RLE (16 bits) : en-tete de donnees invalide");
        out.reserve(size + 1);
        uint32_t src = r[0] + 4;
        while (out.size() < size) {
            if (num == 0x12) {
                uint32_t flags = bus.read8(src++);
                for (int b = 0; b < 8 && out.size() < size; b++, flags <<= 1) {
                    if (flags & 0x80) {
                        const uint32_t b0 = bus.read8(src++), b1 = bus.read8(src++);
                        const uint32_t len = (b0 >> 4) + 3, disp = (((b0 & 0xF) << 8) | b1) + 1;
                        for (uint32_t k = 0; k < len && out.size() < size; k++)
                            out.push_back(disp <= out.size() ? out[out.size() - disp] : uint8_t(0));
                    } else {
                        out.push_back(bus.read8(src++));
                    }
                }
            } else {
                const uint32_t f = bus.read8(src++);
                if (f & 0x80) {
                    const uint8_t v = bus.read8(src++);
                    for (uint32_t k = 0; k < (f & 0x7F) + 3 && out.size() < size; k++) out.push_back(v);
                } else {
                    for (uint32_t k = 0; k < (f & 0x7F) + 1 && out.size() < size; k++) out.push_back(bus.read8(src++));
                }
            }
        }
        if (out.size() & 1) out.push_back(0);
        const uint32_t dst = r[1] & ~1u;
        for (size_t k = 0; k < out.size(); k += 2) bus.write16(dst + uint32_t(k), uint16_t(out[k] | (out[k + 1] << 8)));
        return true;
    }

    case 0x13: {                                     // HuffUnComp : r0 source, r1 destination (ecrit par mots de 32 bits)
        const uint32_t src = r[0];
        const uint32_t header = bus.read32(src);
        if ((header & 0xF0) != 0x20) return fail("SWI Huffman : en-tete de donnees invalide");
        const uint32_t dataBits = header & 0xF;      // 4 ou 8 bits par valeur
        if (dataBits != 4 && dataBits != 8) return fail("SWI Huffman : taille de valeur invalide");
        const uint32_t size = header >> 8;
        const uint32_t treeBase = src + 4;           // 1er octet = taille de l'arbre / 2 - 1, puis la racine
        const uint32_t root = treeBase + 1;
        uint32_t bits = treeBase + (uint32_t(bus.read8(treeBase)) + 1) * 2;   // flux de bits apres l'arbre
        uint32_t node = root, dst = r[1] & ~3u, written = 0, outWord = 0, outBits = 0;
        while (written < size) {
            const uint32_t word = bus.read32(bits);
            bits += 4;
            for (int bit = 31; bit >= 0 && written < size; bit--) {
                const uint32_t dir = (word >> bit) & 1;
                const uint8_t nv = bus.read8(node);
                // enfants : a (noeud & ~1) + 2 x (decalage + 1) ; bit 7 / bit 6 = l'enfant 0 / 1 est une valeur
                const uint32_t child = (node & ~1u) + (nv & 0x3Fu) * 2 + 2 + dir;
                const bool isData = dir ? (nv & 0x40) != 0 : (nv & 0x80) != 0;
                if (!isData) { node = child; continue; }
                outWord |= uint32_t(bus.read8(child) & ((1u << dataBits) - 1)) << outBits;
                outBits += dataBits;
                node = root;
                if (outBits == 32) { bus.write32(dst, outWord); dst += 4; written += 4; outWord = 0; outBits = 0; }
            }
        }
        return true;
    }

    case 0x16: case 0x17: case 0x18: {               // filtres differentiels : chaque valeur = precedente + ecart
        const uint32_t header = bus.read32(r[0]);
        if ((header & 0xF0) != 0x80) return fail("SWI Diff : en-tete de donnees invalide");
        const uint32_t size = header >> 8;
        uint32_t src = r[0] + 4, dst = r[1];
        if (num == 0x18) {                           // 16 bits -> 16 bits
            uint16_t acc = 0;
            for (uint32_t k = 0; k + 1 < size + 1 && k < size; k += 2) {
                acc = uint16_t(acc + bus.read16(src + k));
                bus.write16(dst + k, acc);
            }
        } else {                                     // 8 bits -> ecriture 8 bits (0x16) ou par demi-mots (0x17)
            uint8_t acc = 0, lowByte = 0;
            for (uint32_t k = 0; k < size; k++) {
                acc = uint8_t(acc + bus.read8(src + k));
                if (num == 0x16) bus.write8(dst + k, acc);
                else if (k & 1) bus.write16((dst + k - 1) & ~1u, uint16_t(lowByte | (acc << 8)));
                else lowByte = acc;
            }
            if (num == 0x17 && (size & 1)) bus.write16((dst + size - 1) & ~1u, lowByte);
        }
        return true;
    }

    case 0x11: {                                     // LZ77UnCompReadNormalWrite8bit : r0 source, r1 destination
        uint32_t src = r[0], dst = r[1];
        uint32_t header = bus.read32(src);
        if ((header & 0xF0) != 0x10) return fail("SWI LZ77 : en-tete de donnees invalide");
        uint32_t size = header >> 8, out = 0;
        src += 4;
        while (out < size) {
            uint32_t flags = bus.read8(src++);
            for (int b = 0; b < 8 && out < size; b++, flags <<= 1) {
                if (flags & 0x80) {                  // bloc compresse : reference vers des octets deja ecrits
                    uint32_t b0 = bus.read8(src++), b1 = bus.read8(src++);
                    uint32_t len = (b0 >> 4) + 3, disp = (((b0 & 0xF) << 8) | b1) + 1;
                    for (uint32_t k = 0; k < len && out < size; k++, out++)
                        bus.write8(dst + out, bus.read8(dst + out - disp));
                }
                else {                             // octet brut
                    bus.write8(dst + out, bus.read8(src++));
                    out++;
                }
            }
        }
        return true;
    }

    case 0x14: {                                     // RLUnCompReadNormalWrite8bit : r0 source, r1 destination
        uint32_t src = r[0], dst = r[1];
        uint32_t header = bus.read32(src);
        if ((header & 0xF0) != 0x30) return fail("SWI RLE : en-tete de donnees invalide");
        uint32_t size = header >> 8, out = 0;
        src += 4;
        while (out < size) {
            uint32_t f = bus.read8(src++);
            if (f & 0x80) {                          // repetition d'un octet
                uint32_t len = (f & 0x7F) + 3;
                uint8_t v = bus.read8(src++);
                for (uint32_t k = 0; k < len && out < size; k++, out++) bus.write8(dst + out, v);
            }
            else {                                 // recopie d'octets bruts
                uint32_t len = (f & 0x7F) + 1;
                for (uint32_t k = 0; k < len&& out < size; k++, out++) bus.write8(dst + out, bus.read8(src++));
            }
        }
        return true;
    }

             // Tables de sons du BIOS de l'ARM7 : le vrai BIOS les contient en ROM. On les recalcule ici.
             // (Sinus et hauteurs : formules habituelles. Volume : COURBE APPROCHEE, a corriger quand le son sera emule.)
    case 0x1A: {                                     // GetSineTable : r0 = indice (0-63) -> sinus sur 16 bits
        if (v5) return fail("SWI 0x1A n'existe pas sur ARM9");
        double v = std::sin(double(r[0] & 0x3F) * 3.14159265358979323846 / 128.0) * 32768.0;
        long n = std::lround(v);
        r[0] = uint32_t(n > 0x7FFF ? 0x7FFF : n);
        return true;
    }
    case 0x1B: {                                     // GetPitchTable : r0 = indice (0-767) -> 65536 * (2^(i/768) - 1)
        if (v5) return fail("SWI 0x1B n'existe pas sur ARM9");
        double v = 65536.0 * (std::pow(2.0, double(r[0] % 768) / 768.0) - 1.0);
        long n = std::lround(v);
        r[0] = uint32_t(n > 0xFFFF ? 0xFFFF : n);
        return true;
    }
    case 0x1C: {                                     // GetVolumeTable : r0 = indice (0-723) -> volume sur 7 bits (approximation)
        if (v5) return fail("SWI 0x1C n'existe pas sur ARM9");
        double idx = double(r[0] > 723 ? 723 : r[0]);
        double v = (r[0] == 0) ? 0.0 : 127.0 * std::pow(2.0, (idx - 723.0) / 96.0);
        r[0] = uint32_t(std::lround(v));
        return true;
    }

    case 0x08:                                       // SoundBias (ARM7) : fait varier SOUNDBIAS progressivement
        return true;                                 // le son n'est pas encore emule : sans effet

    case 0x06:                                       // Halt : attend la prochaine interruption
        halted = true;
        return true;

    case 0x04: case 0x05: {                          // IntrWait (r0 = effacer d'abord, r1 = masque) / VBlankIntrWait
        uint32_t discard = (num == 5) ? 1u : r[0];
        uint32_t mask = (num == 5) ? 1u : r[1];
        uint32_t flagAddr = biosIfAddr();             // mot ou le gestionnaire du jeu note les interruptions recues
        if (!inIntrWait) {
            inIntrWait = true;
            if (discard) bus.write32(flagAddr, bus.read32(flagAddr) & ~mask);
            bus.forceImeOn();                         // comme le vrai BIOS : interruptions autorisees pendant l'attente
            cpsr &= ~0x80u;
        }
        uint32_t flags = bus.read32(flagAddr);
        if (flags & mask) {                           // l'interruption attendue est arrivee
            bus.write32(flagAddr, flags & ~mask);
            inIntrWait = false;
            return true;                              // le SWI est termine : on passe a l'instruction suivante
        }
        halted = true;                                // sinon on dort, et on re-executera ce SWI au reveil
        branched = true;                              // (pc reste sur le SWI)
        return true;
    }

    default: {
        char buf[64];
        std::snprintf(buf, sizeof buf, "SWI 0x%02X non gere", num);
        return fail(buf);
    }
    }
}