#include "core/savestate.hpp"
#include <ctime>
#include <fstream>
#include "core/arm.hpp"
#include "core/gx.hpp"
#include "core/memory.hpp"
#include "core/sound.hpp"

// ---- ce que contient l'etat de chaque partie ---------------------------------------------------------------
// Ne sont pas sauvegardes : les pointeurs (refaits au chargement), les caches, les statistiques de diagnostic.

template <class Ar> void Spu::serialize(Ar& a) {
    a.io(ch); a.io(soundCnt); a.io(soundBias); a.io(capCnt); a.io(capDad); a.io(capLen);
    a.io(nextSample); a.io(cap); a.io(lastMixL); a.io(lastMixR);
    if (Ar::loading) output.clear();
}

template <class Ar> void Gx::serialize(Ar& a) {
    finish();                                             // dessin en cours termine et visible
    a.io(clearColor); a.io(clearDepth); a.io(boxTestResult); a.io(posResult); a.io(vecResult);
    a.io(commandCount); a.io(swapCount);
    a.io(packed_); a.io(packedPos_); a.io(inParams_); a.io(cmd_); a.io(need_); a.io(have_); a.io(params_);
    a.io(portBuf_); a.io(portHave_);
    a.io(mtxMode_); a.io(proj_); a.io(pos_); a.io(vec_); a.io(tex_); a.io(projStack_); a.io(posStack_); a.io(vecStack_);
    a.io(posSp_); a.io(clip_); a.io(clipDirty_);
    a.io(vx_); a.io(vy_); a.io(vz_); a.io(cr_); a.io(cg_); a.io(cb_); a.io(texS_); a.io(texT_);
    a.io(texParam_); a.io(texPal_); a.io(diffuse_);
    a.io(lightDir_); a.io(lightColor_); a.io(ambient_); a.io(specular_); a.io(emission_);
    a.io(polyAttrNext_); a.io(polyAttr_); a.io(viewport_); a.io(vtxType_); a.io(vtxCount_); a.io(asm_);
    a.io(building_); a.io(pending_); a.io(swapPending_);
    a.io(rejectedOffscreen_); a.io(droppedPolys_); a.io(pendingRejected_); a.io(pendingDropped_);
    a.io(culledPolys_); a.io(pendingCulled_);
    a.io(color_);                                         // l'image 3D affichee
    if (Ar::loading) front_ = color_;
    a.io(dispRegs);
    if (Ar::loading) texFast_.palKey = 0xFFFFFFFFu;       // palette deja convertie : a refaire
}

template <class Ar> void System::serialize(Ar& a) {
    a.io(mainRam); a.io(sharedWram); a.io(arm7Wram); a.io(wramCnt);
    a.io(ipcSyncOut9); a.io(ipcSyncOut7); a.io(ipcIrqEnable9); a.io(ipcIrqEnable7); a.io(fifo);
    gx.serialize(a);
    a.io(gxIrqMode); a.io(fifoCnt); a.io(fifoLast);
    a.io(exmemcnt); a.io(auxSpiCnt); a.io(romCtrl); a.io(cardCmd); a.io(cardBuf);
    uint64_t cp = cardPos; a.io(cp); cardPos = size_t(cp);
    a.io(cardBusy); a.io(cardChipId);
    a.io(backup); a.io(saveAddrBytes); a.io(backupMaxWritten); a.io(backupDirty); a.io(spiData);
    a.io(spiCnt); a.io(spiOut); a.io(firmware); a.io(touchDown); a.io(touchX); a.io(touchY);
    spu.serialize(a);
    a.io(powcnt2); a.io(rtcIo);
    a.io(palette); a.io(oam);
    for (auto& v : vram) a.io(v);
    a.io(vramCnt); a.io(eng); a.io(powcnt1);
    a.io(divCnt); a.io(divNumer); a.io(divDenom); a.io(divResult); a.io(divRem);
    a.io(sqrtCnt); a.io(sqrtParam); a.io(sqrtResult);
    a.io(dma); a.io(dmaFill); a.io(captureArmed);
    a.io(irq); a.io(timers); a.io(dispstatCfg); a.io(vcount); a.io(lineCycle); a.io(inHblank); a.io(cycles);
    a.io(rcnt7); a.io(keyInput); a.io(keyXY); a.io(haltRequest); a.io(eventDirty);
    a.io(spiCmd); a.io(spiAddrLeft); a.io(spiAddr); a.io(spiA8); a.io(spiLen); a.io(spiDummy); a.io(spiWel);
    a.io(saveMask);
    a.io(pmReg); a.io(pmIndex); a.io(fwCmd); a.io(fwAddrLeft); a.io(fwAddr); a.io(fwDummy); a.io(fwWel);
    a.io(firmwareDirty); a.io(tscResult); a.io(tscPos);
    a.io(rtcCmd); a.io(rtcIn); a.io(rtcInBit); a.io(rtcInPos); a.io(rtcOutBit); a.io(rtcOutPos); a.io(rtcOut);
    a.io(rtcStatus1); a.io(rtcStatus2); a.io(rtcAlarm); a.io(rtcAdjust); a.io(rtcFree);
}

template <class Ar> void Bus9::serialize(Ar& a) {
    a.io(itcm); a.io(dtcm); a.io(itcmOn); a.io(dtcmOn); a.io(itcmRegion); a.io(dtcmBase); a.io(dtcmRegion);
}

template <class Ar> void ArmCpu::serialize(Ar& a) {
    a.io(pc); a.io(r); a.io(cpsr); a.io(halted);
    a.io(spsr); a.io(bankR13); a.io(bankR14); a.io(fiqR8_12); a.io(usrR8_12);
    a.io(cp15Control); a.io(cp15Dtcm); a.io(cp15Itcm); a.io(inIntrWait); a.io(branched);
}

// ---- fichier ----------------------------------------------------------------------------------------------
// En-tete : "NDSEMUST", version, code du jeu, CRC de l'en-tete de la ROM, date, miniature 256 x 384, puis l'etat.
namespace {
constexpr char kMagic[8] = { 'N', 'D', 'S', 'E', 'M', 'U', 'S', 'T' };
constexpr uint32_t kVersion = 2;                       // 2 : registres d'affichage 3D (contours, brouillard)

struct Header {
    char magic[8];
    uint32_t version;
    char code[4];
    uint16_t crc;
    uint16_t pad;
    int64_t time;
};

template <class Ar> void body(Ar& a, const SaveStateTarget& t) {
    t.sys->serialize(a);
    t.bus9->serialize(a);
    t.cpu9->serialize(a);
    t.cpu7->serialize(a);
}

bool readAll(const std::filesystem::path& f, std::vector<uint8_t>& out) {
    std::ifstream in(f, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
constexpr size_t kThumbBytes = 256 * 384 * 4;
}  // namespace

bool saveStateFile(const std::filesystem::path& file, const SaveStateTarget& t, const std::vector<uint32_t>& screen,
                   std::string& error) {
    Header h = {};
    std::memcpy(h.magic, kMagic, 8);
    h.version = kVersion;
    for (int i = 0; i < 4; i++) h.code[i] = i < int(t.gameCode.size()) ? t.gameCode[size_t(i)] : ' ';
    h.crc = t.headerCrc;
    h.time = int64_t(std::time(nullptr));
    StateWriter w;
    w.raw(&h, sizeof h);
    std::vector<uint32_t> thumb(256 * 384, 0);
    if (screen.size() == thumb.size()) thumb = screen;
    w.raw(thumb.data(), kThumbBytes);
    body(w, t);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = std::filesystem::path(file).concat(".tmp");    // ecriture puis renommage :
    {                                                                                   // pas de fichier a moitie ecrit
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { error = "impossible d'\xC3\xA9" "crire le fichier"; return false; }
        out.write(reinterpret_cast<const char*>(w.data.data()), std::streamsize(w.data.size()));
        if (!out) { error = "disque plein ?"; return false; }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) { std::filesystem::remove(file, ec); std::filesystem::rename(tmp, file, ec); }
    if (ec) { error = "impossible de remplacer l'ancienne sauvegarde"; return false; }
    return true;
}

bool readStateInfo(const std::filesystem::path& file, SaveStateInfo& info) {
    info = SaveStateInfo();
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    Header h = {};
    in.read(reinterpret_cast<char*>(&h), sizeof h);
    if (!in || std::memcmp(h.magic, kMagic, 8) != 0) return false;
    info.thumb.resize(256 * 384);
    in.read(reinterpret_cast<char*>(info.thumb.data()), std::streamsize(kThumbBytes));
    if (!in) { info.thumb.clear(); return false; }
    info.exists = true;
    info.time = h.time;
    return true;
}

bool loadStateFile(const std::filesystem::path& file, const SaveStateTarget& t, std::string& error) {
    std::vector<uint8_t> d;
    if (!readAll(file, d)) { error = "emplacement vide"; return false; }
    if (d.size() < sizeof(Header) + kThumbBytes) { error = "fichier abim\xC3\xA9"; return false; }
    Header h;
    std::memcpy(&h, d.data(), sizeof h);
    if (std::memcmp(h.magic, kMagic, 8) != 0) { error = "ce n'est pas une sauvegarde d'\xC3\xA9tat"; return false; }
    if (h.version != kVersion) { error = "sauvegarde d'une autre version de ndsemu"; return false; }
    if (std::string(h.code, 4) != t.gameCode.substr(0, 4) || h.crc != t.headerCrc) { error = "sauvegarde d'un autre jeu"; return false; }
    const uint8_t* p = d.data() + sizeof(Header) + kThumbBytes;
    const size_t n = d.size() - sizeof(Header) - kThumbBytes;
    {
        StateReader check(p, n, true);
        body(check, t);
        if (!check.finished()) { error = "fichier abim\xC3\xA9 ou incompatible"; return false; }
    }
    StateReader r(p, n, false);
    body(r, t);
    t.sys->rebuildVramMap();                              // pages de VRAM, et caches des bus invalides
    t.sys->memGen++;
    return true;
}

template void Spu::serialize<StateWriter>(StateWriter&);
template void Spu::serialize<StateReader>(StateReader&);
template void Gx::serialize<StateWriter>(StateWriter&);
template void Gx::serialize<StateReader>(StateReader&);
template void System::serialize<StateWriter>(StateWriter&);
template void System::serialize<StateReader>(StateReader&);
template void Bus9::serialize<StateWriter>(StateWriter&);
template void Bus9::serialize<StateReader>(StateReader&);
template void ArmCpu::serialize<StateWriter>(StateWriter&);
template void ArmCpu::serialize<StateReader>(StateReader&);
