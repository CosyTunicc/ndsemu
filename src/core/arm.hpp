#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/memory.hpp"

// Interpreteur ARM (mode ARM 32 bits uniquement), utilise pour l'ARM9 (ARMv5) et l'ARM7 (ARMv4T).
//
// Gere :
//   - traitement de donnees (MOV, ADD, SUB, CMP, AND, ORR...), decalages immediats
//   - LDR/STR (mot, octet), LDRH/STRH/LDRSB/LDRSH, LDM/STM
//   - B, BL, BX, BLX (registre), CLZ, MUL, MLA
//   - MRS / MSR, modes du processeur avec registres "banques" (r13/r14, FIQ r8-r12, SPSR)
//   - coprocesseur CP15 (registre de controle, ID, TCM : valeurs stockees seulement)
// Gere aussi : mode Thumb (jeu d'instructions 16 bits) et decalages par registre.
// Pas encore gere : multiplications longues, SWP, SWI,
// exceptions/interruptions, LDM/STM avec S, saturations (QADD...), TCM reellement mappees.
class ArmCpu {
public:
    // armv5 = true pour l'ARM9 (CLZ, BLX, CP15...), false pour l'ARM7.
    ArmCpu(IBus& b, bool armv5, const char* cpuName) : name(cpuName), bus(b), v5(armv5) {
        static IrqCtl noIrq;
        static bool noHalt = false;
        irq_ = b.irqCtl ? b.irqCtl : &noIrq;          // lus a chaque instruction : pas d'appel virtuel
        halt_ = b.haltFlag ? b.haltFlag : &noHalt;
    }

    // Diagnostic (echantillonnage des adresses frequentes, trace apres chaque interruption) : utile pour les
    // rapports, mais couteux. main le coupe pour le jeu normal.
    bool diagnostics = true;

    void reset(uint32_t entry);
    template <class Ar> void serialize(Ar& a);   // sauvegarde d'etat (savestate.cpp)

    bool step();            // execute une instruction ; false si non geree (voir error)
    void dump() const;      // affiche les registres
    void printHistory() const;   // affiche les dernieres instructions executees
    void printHotSpots() const;  // affiche les adresses ou le processeur passe le plus de temps
    uint64_t irqCount = 0;       // nombre d'interruptions prises en compte
    // Trace des instructions executees depuis la derniere interruption (boucles tronquees)
    bool writeIrqTrace(const char* path) const;
    uint32_t dtcmRegDebug() const { return cp15Dtcm; }

    std::string name;

    uint32_t pc = 0;        // adresse de l'instruction COURANTE (r15 lu = pc + 8)
    uint32_t r[15] = {};    // r0..r14 (vue du mode courant)
    uint32_t cpsr = 0;

    bool halted = false;        // en attente d'une interruption (Halt)
    uint32_t lastPc = 0, lastInstr = 0;
    uint64_t instructionCount() const { return histCount; }
    std::string error;

private:
    IBus& bus;
    bool v5;
    IrqCtl* irq_ = nullptr;
    bool* halt_ = nullptr;
    static constexpr unsigned kHist = 32;
    uint32_t histPc[kHist] = {}, histInstr[kHist] = {};
    uint64_t histCount = 0;
    std::map<uint32_t, uint32_t> pcSamples;          // une instruction sur 64 est echantillonnee

    struct TraceEntry { uint32_t pc, instr, r0, r1, r2, r3, cpsr; };
    std::vector<TraceEntry> irqTrace;
    std::unordered_map<uint32_t, uint16_t> traceSeen;   // combien de fois chaque adresse a deja ete notee
    bool tracing = false;
    uint32_t traceSkipped = 0;
    bool branched = false;

    // Registres banques. Index de banque : 0 usr/sys, 1 fiq, 2 irq, 3 svc, 4 abt, 5 und.
    uint32_t spsr[6] = {};
    uint32_t bankR13[6] = {}, bankR14[6] = {};
    uint32_t fiqR8_12[5] = {}, usrR8_12[5] = {};

    // CP15 (valeurs conservees)
    uint32_t cp15Control = 0x00012078;
    uint32_t cp15Dtcm = 0, cp15Itcm = 0;

    uint32_t reg(unsigned n) const { return n == 15 ? pc + 8 : r[n]; }
    void setReg(unsigned n, uint32_t v);
    bool cond(uint32_t c) const;
    uint32_t shiftImm(uint32_t v, uint32_t type, uint32_t amt, bool& carry) const;
    void setNZ(uint32_t res);
    bool fail(const char* why);
    void switchMode(uint32_t newMode);
    bool cp15Read(unsigned crn, unsigned crm, unsigned opc2, uint32_t& out);
    bool cp15Write(unsigned crn, unsigned crm, unsigned opc2, uint32_t v);

    bool execDataProc(uint32_t instr);
    bool execMisc(uint32_t instr);
    bool execMul(uint32_t instr);
    bool execMulLong(uint32_t instr);
    bool execSwp(uint32_t instr);
    bool execHalfword(uint32_t instr);
    bool execLoadStore(uint32_t instr);
    bool execBlock(uint32_t instr);
    bool execBranch(uint32_t instr);
    bool execCoproc(uint32_t instr);
    bool hleSwi(unsigned num);
    void enterIrq();                        // prise en compte d'une interruption (vecteur 0x18)
    uint32_t biosIfAddr() const;            // mot "interruptions recues" du BIOS, utilise par IntrWait
    bool inIntrWait = false;
    uint32_t& userReg(unsigned i);          // registre du mode User/System (r0-r14)
    bool returnFromException();             // CPSR = SPSR du mode courant (MOVS pc,lr / LDM^)      // appels au BIOS (SWI) emules directement, sans le BIOS reel

    // Mode Thumb (instructions de 16 bits, bit T = bit 5 du CPSR)
    bool execThumb(uint16_t op);
    uint32_t addWithFlags(uint32_t a, uint32_t b, uint32_t carryIn, bool flags);
    void enterThumbState(bool thumb) { if (thumb) cpsr |= 0x20u; else cpsr &= ~0x20u; }
};
