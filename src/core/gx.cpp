#include "gx.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <chrono>

namespace {

    constexpr int32_t kOne = 0x1000;                // 1.0 en virgule fixe 20.12

    // Nombre de parametres de chaque commande (-1 : commande inconnue)
    int paramCount(uint32_t cmd) {
        switch (cmd) {
        case 0x00: return 0;
        case 0x10: return 1;  case 0x11: return 0;  case 0x12: return 1;  case 0x13: return 1;
        case 0x14: return 1;  case 0x15: return 0;  case 0x16: return 16; case 0x17: return 12;
        case 0x18: return 16; case 0x19: return 12; case 0x1A: return 9;  case 0x1B: return 3;
        case 0x1C: return 3;
        case 0x20: return 1;  case 0x21: return 1;  case 0x22: return 1;  case 0x23: return 2;
        case 0x24: return 1;  case 0x25: return 1;  case 0x26: return 1;  case 0x27: return 1;
        case 0x28: return 1;  case 0x29: return 1;  case 0x2A: return 1;  case 0x2B: return 1;
        case 0x30: return 1;  case 0x31: return 1;  case 0x32: return 1;  case 0x33: return 1;
        case 0x34: return 32;
        case 0x40: return 1;  case 0x41: return 0;
        case 0x50: return 1;
        case 0x60: return 1;
        case 0x70: return 3;  case 0x71: return 2;  case 0x72: return 1;
        default: return -1;
        }
    }

    void identity(int32_t* m) {
        std::memset(m, 0, 16 * sizeof(int32_t));
        m[0] = m[5] = m[10] = m[15] = kOne;
    }

    // m = s * m (lignes ; un sommet est un vecteur ligne multiplie a droite par la matrice)
    void mulMat(int32_t* m, const int32_t* s) {
        int32_t t[16];
        std::memcpy(t, m, sizeof t);
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                const int64_t sum = int64_t(s[r * 4 + 0]) * t[0 * 4 + c] + int64_t(s[r * 4 + 1]) * t[1 * 4 + c] +
                    int64_t(s[r * 4 + 2]) * t[2 * 4 + c] + int64_t(s[r * 4 + 3]) * t[3 * 4 + c];
                m[r * 4 + c] = int32_t(sum >> 12);
            }
        }
    }

    void from4x3(int32_t* s, const uint32_t* p) {
        for (int r = 0; r < 4; r++) {
            s[r * 4 + 0] = int32_t(p[r * 3 + 0]);
            s[r * 4 + 1] = int32_t(p[r * 3 + 1]);
            s[r * 4 + 2] = int32_t(p[r * 3 + 2]);
            s[r * 4 + 3] = (r == 3) ? kOne : 0;
        }
    }

    void from3x3(int32_t* s, const uint32_t* p) {
        identity(s);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) s[r * 4 + c] = int32_t(p[r * 3 + c]);
    }

    void scaleMat(int32_t* m, const uint32_t* p) {
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) m[r * 4 + c] = int32_t((int64_t(m[r * 4 + c]) * int32_t(p[r])) >> 12);
    }

    void translateMat(int32_t* m, const uint32_t* p) {
        for (int c = 0; c < 4; c++) {
            const int64_t add = int64_t(int32_t(p[0])) * m[0 * 4 + c] + int64_t(int32_t(p[1])) * m[1 * 4 + c] +
                int64_t(int32_t(p[2])) * m[2 * 4 + c];
            m[3 * 4 + c] += int32_t(add >> 12);
        }
    }

    uint8_t expand5(uint32_t c) { return uint8_t((c << 3) | (c >> 2)); }

    GxVtx lerpVtx(const GxVtx& a, const GxVtx& b, double t) {
        GxVtx o;
        auto L = [t](int32_t p, int32_t q) { return int32_t(std::lround(p + (double(q) - p) * t)); };
        o.x = L(a.x, b.x); o.y = L(a.y, b.y); o.z = L(a.z, b.z); o.w = L(a.w, b.w);
        o.r = uint8_t(L(a.r, b.r)); o.g = uint8_t(L(a.g, b.g)); o.b = uint8_t(L(a.b, b.b));
        o.s = L(a.s, b.s); o.t = L(a.t, b.t);
        return o;
    }

    // Decoupe un polygone contre un plan : 0 = proche (z + w >= 0), 1 = lointain (w - z >= 0)
    int clipPlane(const GxVtx* in, int n, GxVtx* out, int plane) {
        auto dist = [plane](const GxVtx& v) { return plane == 0 ? int64_t(v.z) + v.w : int64_t(v.w) - v.z; };
        int m = 0;
        for (int i = 0; i < n; i++) {
            const GxVtx& cur = in[i];
            const GxVtx& nxt = in[(i + 1) % n];
            const int64_t dc = dist(cur), dn = dist(nxt);
            if (dc >= 0 && m < 10) out[m++] = cur;
            if ((dc >= 0) != (dn >= 0) && m < 10) out[m++] = lerpVtx(cur, nxt, double(dc) / double(dc - dn));
        }
        return m;
    }

    struct ScreenVtx { float x, y, z, iw, r, g, b; };

}  // namespace

Gx::Gx() : color_(256 * 192, 0), depth_(256 * 192, 1.0f), stencil_(256 * 192, 0), polyId_(256 * 192, 0), pixFlags_(256 * 192, 0), below_(256 * 192, 0), cov_(256 * 192, 31), idBuf_(256 * 192, 0) {
    identity(proj_); identity(pos_); identity(vec_); identity(tex_);
    identity(projStack_); identity(clip_);
    for (int i = 0; i < 32; i++) { identity(posStack_[i]); identity(vecStack_[i]); }
    viewport_ = 0xBFFF0000u;
}

void Gx::writeFifo(uint32_t word) {
    if (!inParams_) {                        // un mot de quatre commandes
        packed_ = word;
        packedPos_ = 0;
        nextCommand();
        return;
    }
    params_[have_++] = word;
    if (have_ == need_) {
        inParams_ = false;
        exec(cmd_, params_);
        nextCommand();
    }
}

void Gx::nextCommand() {
    while (packedPos_ < 4) {
        const uint32_t cmd = (packed_ >> (8 * packedPos_)) & 0xFF;
        packedPos_++;
        const int need = paramCount(cmd);
        if (need < 0) { if (cmd) unknownCommands++; continue; }
        if (need == 0) { if (cmd) exec(cmd, nullptr); continue; }
        cmd_ = cmd; need_ = need; have_ = 0; inParams_ = true;
        return;
    }
}

void Gx::writePort(uint32_t cmd, uint32_t param) {
    cmd &= 0xFF;
    if (cmd >= 0x80) return;
    const int need = paramCount(cmd);
    if (need < 0) { unknownCommands++; return; }
    if (need == 0) { if (cmd) exec(cmd, nullptr); return; }
    int& n = portHave_[cmd];
    portBuf_[cmd][n++] = param;
    if (n == need) { n = 0; exec(cmd, portBuf_[cmd]); }
}

void Gx::updateClip() {
    if (!clipDirty_) return;
    std::memcpy(clip_, proj_, sizeof clip_);
    mulMat(clip_, pos_);                     // position puis projection
    clipDirty_ = false;
}

void Gx::exec(uint32_t cmd, const uint32_t* p) {
    commandCount++;
    // Matrice(s) visee(s) par le mode courant
    int32_t* target[2] = { nullptr, nullptr };
    switch (mtxMode_) {
    case 0: target[0] = proj_; break;
    case 1: target[0] = pos_; break;
    case 2: target[0] = pos_; target[1] = vec_; break;
    default: target[0] = tex_; break;
    }
    auto touched = [this]() { if (mtxMode_ != 3) clipDirty_ = true; };

    switch (cmd) {
    case 0x10: mtxMode_ = int(p[0] & 3); break;
    case 0x11:                                                   // MTX_PUSH
        if (mtxMode_ == 0) std::memcpy(projStack_, proj_, sizeof proj_);
        else if (mtxMode_ == 3) { /* pile de textures : non geree */ }
        else {
            if (posSp_ < 31) {
                std::memcpy(posStack_[posSp_], pos_, sizeof pos_);
                std::memcpy(vecStack_[posSp_], vec_, sizeof vec_);
                posSp_++;
            }
        }
        break;
    case 0x12: {                                                 // MTX_POP : n niveaux (6 bits signes)
        const int n = int(int32_t(p[0] << 26) >> 26);
        if (mtxMode_ == 0) std::memcpy(proj_, projStack_, sizeof proj_);
        else if (mtxMode_ != 3) {
            posSp_ -= n;
            if (posSp_ < 0) posSp_ = 0;
            if (posSp_ > 30) posSp_ = 30;
            std::memcpy(pos_, posStack_[posSp_], sizeof pos_);
            std::memcpy(vec_, vecStack_[posSp_], sizeof vec_);
        }
        touched();
        break;
    }
    case 0x13: {                                                 // MTX_STORE
        const int i = int(p[0] & 31);
        if (mtxMode_ == 0) std::memcpy(projStack_, proj_, sizeof proj_);
        else if (mtxMode_ != 3 && i < 31) {
            std::memcpy(posStack_[i], pos_, sizeof pos_);
            std::memcpy(vecStack_[i], vec_, sizeof vec_);
        }
        break;
    }
    case 0x14: {                                                 // MTX_RESTORE
        const int i = int(p[0] & 31);
        if (mtxMode_ == 0) std::memcpy(proj_, projStack_, sizeof proj_);
        else if (mtxMode_ != 3 && i < 31) {
            std::memcpy(pos_, posStack_[i], sizeof pos_);
            std::memcpy(vec_, vecStack_[i], sizeof vec_);
        }
        touched();
        break;
    }
    case 0x15:                                                   // MTX_IDENTITY
        for (int32_t* m : target) if (m) identity(m);
        touched();
        break;
    case 0x16:                                                   // MTX_LOAD_4x4
        for (int32_t* m : target) if (m) for (int i = 0; i < 16; i++) m[i] = int32_t(p[i]);
        touched();
        break;
    case 0x17: {                                                 // MTX_LOAD_4x3
        int32_t s[16];
        from4x3(s, p);
        for (int32_t* m : target) if (m) std::memcpy(m, s, sizeof s);
        touched();
        break;
    }
    case 0x18: {                                                 // MTX_MULT_4x4
        int32_t s[16];
        for (int i = 0; i < 16; i++) s[i] = int32_t(p[i]);
        for (int32_t* m : target) if (m) mulMat(m, s);
        touched();
        break;
    }
    case 0x19: {                                                 // MTX_MULT_4x3
        int32_t s[16];
        from4x3(s, p);
        for (int32_t* m : target) if (m) mulMat(m, s);
        touched();
        break;
    }
    case 0x1A: {                                                 // MTX_MULT_3x3
        int32_t s[16];
        from3x3(s, p);
        for (int32_t* m : target) if (m) mulMat(m, s);
        touched();
        break;
    }
    case 0x1B:                                                   // MTX_SCALE (jamais sur la matrice des vecteurs)
        if (target[0]) scaleMat(target[0], p);
        touched();
        break;
    case 0x1C:                                                   // MTX_TRANS
        for (int32_t* m : target) if (m) translateMat(m, p);
        touched();
        break;

    case 0x20:                                                   // COLOR
        cr_ = expand5(p[0] & 31); cg_ = expand5((p[0] >> 5) & 31); cb_ = expand5((p[0] >> 10) & 31);
        break;
    case 0x22: {                                                 // TEXCOORD : s et t en 12.4
        const int32_t s = int16_t(p[0] & 0xFFFF), t = int16_t(p[0] >> 16);
        if ((texParam_ >> 30) == 1) {                            // transformation par la matrice de textures
            texS_ = int32_t((int64_t(s) * tex_[0] + int64_t(t) * tex_[4] + int64_t(tex_[8]) + int64_t(tex_[12])) >> 12);
            texT_ = int32_t((int64_t(s) * tex_[1] + int64_t(t) * tex_[5] + int64_t(tex_[9]) + int64_t(tex_[13])) >> 12);
        }
        else { texS_ = s; texT_ = t; }
        break;
    }
    case 0x2A: texParam_ = p[0]; break;                          // TEXIMAGE_PARAM
    case 0x2B: texPal_ = p[0] & 0x1FFF; break;                   // PLTT_BASE
    case 0x23:                                                   // VTX_16
        vx_ = int16_t(p[0] & 0xFFFF); vy_ = int16_t(p[0] >> 16); vz_ = int16_t(p[1] & 0xFFFF);
        submitVertex();
        break;
    case 0x24:                                                   // VTX_10 : 3 x 10 bits, 6 bits de fraction
        vx_ = (int32_t(p[0] << 22) >> 22) * 64;
        vy_ = (int32_t((p[0] >> 10) << 22) >> 22) * 64;
        vz_ = (int32_t((p[0] >> 20) << 22) >> 22) * 64;
        submitVertex();
        break;
    case 0x25: vx_ = int16_t(p[0] & 0xFFFF); vy_ = int16_t(p[0] >> 16); submitVertex(); break;   // VTX_XY
    case 0x26: vx_ = int16_t(p[0] & 0xFFFF); vz_ = int16_t(p[0] >> 16); submitVertex(); break;   // VTX_XZ
    case 0x27: vy_ = int16_t(p[0] & 0xFFFF); vz_ = int16_t(p[0] >> 16); submitVertex(); break;   // VTX_YZ
    case 0x28:                                                   // VTX_DIFF : relatif au sommet precedent
        vx_ += int32_t(p[0] << 22) >> 22;
        vy_ += int32_t((p[0] >> 10) << 22) >> 22;
        vz_ += int32_t((p[0] >> 20) << 22) >> 22;
        submitVertex();
        break;
    case 0x29: polyAttrNext_ = p[0]; break;                      // POLYGON_ATTR (effectif au prochain BEGIN_VTXS)
    case 0x21: {                                                 // NORMAL : 3 x 10 bits (1 signe, 9 de fraction)
        const int64_t nx = (int32_t(p[0] << 22) >> 22) * 8, ny = (int32_t((p[0] >> 10) << 22) >> 22) * 8,
            nz = (int32_t((p[0] >> 20) << 22) >> 22) * 8;
        applyLighting(int32_t((nx * vec_[0] + ny * vec_[4] + nz * vec_[8]) >> 12),
            int32_t((nx * vec_[1] + ny * vec_[5] + nz * vec_[9]) >> 12),
            int32_t((nx * vec_[2] + ny * vec_[6] + nz * vec_[10]) >> 12));
        break;
    }
    case 0x30:                                                   // DIF_AMB
        diffuse_ = p[0] & 0x7FFF;
        ambient_ = uint16_t((p[0] >> 16) & 0x7FFF);
        if (p[0] & 0x8000) { cr_ = expand5(diffuse_ & 31); cg_ = expand5((diffuse_ >> 5) & 31); cb_ = expand5((diffuse_ >> 10) & 31); }
        break;
    case 0x31:                                                   // SPE_EMI (le speculaire n'est pas gere)
        specular_ = uint16_t(p[0] & 0x7FFF);
        emission_ = uint16_t((p[0] >> 16) & 0x7FFF);
        break;
    case 0x32: {                                                 // LIGHT_VECTOR : direction, transformee par la matrice des vecteurs
        const int i = int(p[0] >> 30);
        const int64_t dx = (int32_t(p[0] << 22) >> 22) * 8, dy = (int32_t((p[0] >> 10) << 22) >> 22) * 8,
            dz = (int32_t((p[0] >> 20) << 22) >> 22) * 8;
        lightDir_[i][0] = int32_t((dx * vec_[0] + dy * vec_[4] + dz * vec_[8]) >> 12);
        lightDir_[i][1] = int32_t((dx * vec_[1] + dy * vec_[5] + dz * vec_[9]) >> 12);
        lightDir_[i][2] = int32_t((dx * vec_[2] + dy * vec_[6] + dz * vec_[10]) >> 12);
        break;
    }
    case 0x33: lightColor_[p[0] >> 30] = uint16_t(p[0] & 0x7FFF); break;     // LIGHT_COLOR
    case 0x40:                                                   // BEGIN_VTXS
        vtxType_ = int(p[0] & 3);
        vtxCount_ = 0;
        polyAttr_ = polyAttrNext_;
        break;
    case 0x50:                                                   // SWAP_BUFFERS
        pending_ = std::move(building_);
        building_.clear();
        pendingRejected_ = rejectedOffscreen_; pendingDropped_ = droppedPolys_; pendingCulled_ = culledPolys_;
        rejectedOffscreen_ = 0; droppedPolys_ = 0; culledPolys_ = 0;
        swapPending_ = true;
        swapCount++;
        break;
    case 0x60: viewport_ = p[0]; break;                          // VIEWPORT
    case 0x70: {                                                 // BOX_TEST : la boite est-elle (en partie) dans le champ ?
        const int32_t bx = int16_t(p[0] & 0xFFFF), by = int16_t(p[0] >> 16), bz = int16_t(p[1] & 0xFFFF);
        const int32_t sw = int16_t(p[1] >> 16), sh = int16_t(p[2] & 0xFFFF), sd = int16_t(p[2] >> 16);
        // On transforme les 8 coins ; la boite est ecartee seulement si tous les coins sont du meme cote d'un
        // meme plan du volume de vue (test prudent : ne masque jamais un modele visible).
        int outside[6] = {};
        for (int i = 0; i < 8; i++) {
            int32_t c[4];
            transformPoint(bx + ((i & 1) ? sw : 0), by + ((i & 2) ? sh : 0), bz + ((i & 4) ? sd : 0), c);
            outside[0] += c[0] < -int64_t(c[3]);  outside[1] += c[0] > c[3];
            outside[2] += c[1] < -int64_t(c[3]);  outside[3] += c[1] > c[3];
            outside[4] += c[2] < -int64_t(c[3]);  outside[5] += c[2] > c[3];
        }
        boxTestResult = true;
        for (int k = 0; k < 6; k++) if (outside[k] == 8) boxTestResult = false;
        break;
    }
    case 0x71:                                                   // POS_TEST : position transformee (sans sommet)
        transformPoint(int16_t(p[0] & 0xFFFF), int16_t(p[0] >> 16), int16_t(p[1] & 0xFFFF), posResult);
        break;
    case 0x72: {                                                 // VEC_TEST : vecteur x matrice des vecteurs
        const int64_t nx = (int32_t(p[0] << 22) >> 22) * 8, ny = (int32_t((p[0] >> 10) << 22) >> 22) * 8,
            nz = (int32_t((p[0] >> 20) << 22) >> 22) * 8;
        vecResult[0] = int16_t((nx * vec_[0] + ny * vec_[4] + nz * vec_[8]) >> 12);
        vecResult[1] = int16_t((nx * vec_[1] + ny * vec_[5] + nz * vec_[9]) >> 12);
        vecResult[2] = int16_t((nx * vec_[2] + ny * vec_[6] + nz * vec_[10]) >> 12);
        break;
    }
    default: break;                                              // textures, lumieres, tests... : ignores pour l'instant
    }
}

// Couleur d'un sommet eclaire : emission + somme sur les lumieres actives de (ambiante + diffuse * max(0, -N.L)),
// chaque terme multiplie par la couleur de la lumiere. Normale et lumieres en virgule fixe 20.12.
void Gx::applyLighting(int32_t nx, int32_t ny, int32_t nz) {
    if (!(polyAttrNext_ & 0xF)) return;                              // aucune lumiere activee : on garde COLOR
    float out[3];
    for (int ch = 0; ch < 3; ch++) out[ch] = float((emission_ >> (5 * ch)) & 31) / 31.0f;
    for (int i = 0; i < 4; i++) {
        if (!(polyAttrNext_ & (1u << i))) continue;
        float d = -(float(nx) * float(lightDir_[i][0]) + float(ny) * float(lightDir_[i][1]) + float(nz) * float(lightDir_[i][2])) /
            (4096.0f * 4096.0f);
        d = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
        for (int ch = 0; ch < 3; ch++) {
            const float lc = float((lightColor_[i] >> (5 * ch)) & 31) / 31.0f;
            const float am = float((ambient_ >> (5 * ch)) & 31) / 31.0f;
            const float df = float((diffuse_ >> (5 * ch)) & 31) / 31.0f;
            out[ch] += am * lc + df * lc * d;
        }
    }
    auto to8 = [](float v) { return uint8_t((v > 1.0f ? 1.0f : v) * 255.0f + 0.5f); };
    cr_ = to8(out[0]); cg_ = to8(out[1]); cb_ = to8(out[2]);
}

void Gx::printStats(FILE* f) const {
    std::fprintf(f, "polygones=%d eclaires=%d translucides=%d ombres=%d | modes (modulation, decalque, toon, ombre) = %d %d %d %d | formats de texture 0..7 = %d %d %d %d %d %d %d %d\n",
        stats.polys, stats.lit, stats.translucent, stats.shadow, stats.mode[0], stats.mode[1], stats.mode[2], stats.mode[3],
        stats.fmt[0], stats.fmt[1], stats.fmt[2], stats.fmt[3], stats.fmt[4], stats.fmt[5], stats.fmt[6], stats.fmt[7]);
    std::fprintf(f, "generation de coordonnees (aucune, source, normale, sommet) = %d %d %d %d | textures absentes de la memoire=%d palettes absentes=%d infos compressees absentes=%d chemin lent=%d\n",
        stats.texgen[0], stats.texgen[1], stats.texgen[2], stats.texgen[3], stats.texMissing, stats.palMissing, stats.infoMissing, stats.slowPath);
    std::fprintf(f, "rejetes hors champ=%d perdus (liste pleine)=%d faces ecartees=%d | faces demandees (aucune, dos, face, les deux) = %d %d %d %d | pixels visibles : sens inverse=%d sens horaire=%d\n",
        stats.offscreen, stats.dropped, stats.culled, stats.renderFlags[0], stats.renderFlags[1], stats.renderFlags[2], stats.renderFlags[3],
        stats.winNeg, stats.winPos);
    std::fprintf(f, "VIEWPORT=%08X (x1=%u y1=%u x2=%u y2=%u) CLEAR_COLOR=%08X CLEAR_DEPTH=%04X\n", viewport_,
        viewport_ & 0xFF, (viewport_ >> 8) & 0xFF, (viewport_ >> 16) & 0xFF, (viewport_ >> 24) & 0xFF, clearColor, clearDepth);
}

void Gx::dumpPolygons(FILE* f) const {
    printStats(f);
    int idx = 0;
    for (const GxPoly& p : lastDrawn_) {
        const uint32_t fmt = (p.texParam >> 26) & 7;
        std::fprintf(f, "#%d n=%d attr=%08X (lumieres=%X mode=%u alpha=%u id=%u) tex: fmt=%u %dx%d adr=%05X pal=%04X param=%08X |",
            idx++, p.n, p.attr, p.attr & 0xF, (p.attr >> 4) & 3, (p.attr >> 16) & 31, (p.attr >> 24) & 63, fmt,
            8 << ((p.texParam >> 20) & 7), 8 << ((p.texParam >> 23) & 7), (p.texParam & 0xFFFF) * 8, p.texPal, p.texParam);
        for (int i = 0; i < p.n; i++)
            std::fprintf(f, " (%d,%d,%d,w%d rgb %02X%02X%02X st %d,%d)", p.v[i].x, p.v[i].y, p.v[i].z, p.v[i].w, p.v[i].r, p.v[i].g, p.v[i].b, p.v[i].s, p.v[i].t);
        std::fprintf(f, "\n");
    }
}

void Gx::transformPoint(int32_t px, int32_t py, int32_t pz, int32_t out[4]) {
    updateClip();
    const int64_t x = px, y = py, z = pz;
    for (int k = 0; k < 4; k++)
        out[k] = int32_t((x * clip_[k] + y * clip_[4 + k] + z * clip_[8 + k] + int64_t(kOne) * clip_[12 + k]) >> 12);
}

void Gx::readClipMatrix(int32_t* out16) {
    updateClip();
    std::memcpy(out16, clip_, 16 * sizeof(int32_t));
}

void Gx::readVecMatrix(int32_t* out9) const {
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) out9[r * 3 + k] = vec_[r * 4 + k];
}

void Gx::submitVertex() {
    updateClip();
    GxVtx v;
    const int64_t x = vx_, y = vy_, z = vz_;
    v.x = int32_t((x * clip_[0] + y * clip_[4] + z * clip_[8] + int64_t(kOne) * clip_[12]) >> 12);
    v.y = int32_t((x * clip_[1] + y * clip_[5] + z * clip_[9] + int64_t(kOne) * clip_[13]) >> 12);
    v.z = int32_t((x * clip_[2] + y * clip_[6] + z * clip_[10] + int64_t(kOne) * clip_[14]) >> 12);
    v.w = int32_t((x * clip_[3] + y * clip_[7] + z * clip_[11] + int64_t(kOne) * clip_[15]) >> 12);
    v.r = cr_; v.g = cg_; v.b = cb_;                                 // couleur courante (COLOR, ou resultat de l'eclairage)
    v.s = texS_; v.t = texT_;

    const int n = vtxCount_++;
    switch (vtxType_) {
    case 0:                                                      // triangles
        asm_[n % 3] = v;
        if (n % 3 == 2) { GxVtx t[3] = { asm_[0], asm_[1], asm_[2] }; emitPolygon(t, 3); }
        break;
    case 1:                                                      // quadrilateres
        asm_[n % 4] = v;
        if (n % 4 == 3) { GxVtx q[4] = { asm_[0], asm_[1], asm_[2], asm_[3] }; emitPolygon(q, 4); }
        break;
    case 2:                                                      // bande de triangles
        if (n == 0) asm_[0] = v;
        else if (n == 1) asm_[1] = v;
        else {
            const bool odd = ((n - 2) & 1) != 0;                 // un triangle sur deux est retourne pour garder le sens
            GxVtx t[3] = { odd ? asm_[1] : asm_[0], odd ? asm_[0] : asm_[1], v };
            emitPolygon(t, 3);
            asm_[0] = asm_[1];
            asm_[1] = v;
        }
        break;
    default:                                                     // bande de quadrilateres
        if (n == 0) asm_[0] = v;
        else if (n == 1) asm_[1] = v;
        else if ((n & 1) == 0) asm_[2] = v;
        else {
            GxVtx q[4] = { asm_[0], asm_[1], v, asm_[2] };
            emitPolygon(q, 4);
            asm_[0] = asm_[2];
            asm_[1] = v;
        }
        break;
    }
}

void Gx::emitPolygon(const GxVtx* v, int n) {
    GxVtx a[10], b[10];
    std::memcpy(a, v, sizeof(GxVtx) * size_t(n));
    int m = clipPlane(a, n, b, 0);
    if (m < 3) return;
    m = clipPlane(b, m, a, 1);
    if (m < 3) return;
    // Un polygone entierement hors de l'un des quatre plans lateraux n'est jamais visible : la console l'ecarte
    // avant de le stocker (il ne compte donc pas dans la memoire de polygones).
    auto allOutside = [&](auto&& test) { for (int i = 0; i < m; i++) if (!test(a[i])) return false; return true; };
    if (allOutside([](const GxVtx& q) { return q.x < -q.w; }) || allOutside([](const GxVtx& q) { return q.x > q.w; }) ||
        allOutside([](const GxVtx& q) { return q.y < -q.w; }) || allOutside([](const GxVtx& q) { return q.y > q.w; })) {
        rejectedOffscreen_++;
        return;
    }
    // Sens du polygone a l'ecran : aire signee (formule des lacets) sur (x / w, -y / w) ; negative = sens inverse des
    // aiguilles d'une montre vu par le joueur = face avant (convention d'OpenGL).
    int8_t facing = 0;
    {
        bool valid = true;
        double area = 0.0;
        for (int i = 0; i < m && valid; i++) {
            const GxVtx& p = a[i];
            const GxVtx& q = a[(i + 1) % m];
            if (p.w <= 0 || q.w <= 0) { valid = false; break; }
            const double px = double(p.x) / p.w, py = -double(p.y) / p.w, qx = double(q.x) / q.w, qy = -double(q.y) / q.w;
            area += px * qy - qx * py;
        }
        if (valid) facing = area < 0.0 ? int8_t(-1) : (area > 0.0 ? int8_t(1) : int8_t(0));
        if (cullEnabled && valid) {
            const bool front = cullFlip ? (facing > 0) : (facing < 0);
            const bool back = cullFlip ? (facing < 0) : (facing > 0);
            if ((front && !(polyAttr_ & 0x80)) || (back && !(polyAttr_ & 0x40)) || facing == 0) { culledPolys_++; return; }
        }
    }
    if (building_.size() >= 8192) { droppedPolys_++; return; }       // garde-fou (la console en garde au plus 2048)
    GxPoly poly;
    poly.n = m;
    poly.attr = polyAttr_;
    poly.texParam = texParam_;
    poly.texPal = texPal_;
    poly.facing = facing;
    std::memcpy(poly.v, a, sizeof(GxVtx) * size_t(m));
    building_.push_back(poly);
}

void Gx::onVBlank(const GxTexMem& mem) {
    if (!swapPending_) return;
    swapPending_ = false;
    texMem_ = mem;
    struct Timer {                                                    // temps de dessin de l'image 3D (mesures)
        uint64_t& acc;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timer() { acc += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()); }
    } timer{ renderNs };

    const uint32_t clearAlpha = (clearColor >> 16) & 31;
    // octet du haut = opacite 0..31 (0 : rien de dessine, le moteur 2D voit a travers)
    const uint32_t clearRgb = (clearAlpha == 0) ? 0u :
        ((clearAlpha << 24) | (uint32_t(expand5(clearColor & 31)) << 16) | (uint32_t(expand5((clearColor >> 5) & 31)) << 8) |
            expand5((clearColor >> 10) & 31));
    std::fill(color_.begin(), color_.end(), clearRgb);
    std::fill(depth_.begin(), depth_.end(), float(clearDepth & 0x7FFF) / 32767.0f);

    polygonsDrawn = 0;
    polygonsSkipped = 0;
    texturedPolygons = 0;
    stats = Stats();
    stats.offscreen = pendingRejected_;
    stats.dropped = pendingDropped_;
    stats.culled = pendingCulled_;
    std::fill(idBuf_.begin(), idBuf_.end(), uint16_t(0));
    std::fill(stencil_.begin(), stencil_.end(), uint8_t(0));
    std::fill(polyId_.begin(), polyId_.end(), uint8_t((clearColor >> 24) & 63));   // ID de l'arriere-plan
    std::fill(pixFlags_.begin(), pixFlags_.end(), uint8_t((clearColor & 0x8000) ? 2 : 0));   // l'arriere-plan peut avoir du brouillard
    std::fill(cov_.begin(), cov_.end(), uint8_t(31));
    lastWasMask_ = false;
    // Opaques d'abord, puis translucides dans l'ordre de soumission
    for (int pass = 0; pass < 2; pass++) {
        for (size_t pi = 0; pi < pending_.size(); pi++) {
            const GxPoly& poly = pending_[pi];
            curId_ = uint16_t(pi + 1);
            const uint32_t alpha = (poly.attr >> 16) & 31;
            const bool translucent = alpha > 0 && alpha < 31;
            if (translucent != (pass == 1)) continue;
            stats.polys++;
            stats.renderFlags[(poly.attr >> 6) & 3]++;
            stats.mode[(poly.attr >> 4) & 3]++;
            stats.fmt[(poly.texParam >> 26) & 7]++;
            stats.texgen[poly.texParam >> 30]++;
            if (poly.attr & 0xF) stats.lit++;
            if (translucent) stats.translucent++;
            if (((poly.attr >> 4) & 3) == 3) { polygonsSkipped++; stats.shadow++; }     // polygone d'ombre (voir rasterize)
            rasterize(poly);
            polygonsDrawn++;
            if (((poly.texParam >> 26) & 7) != 0) texturedPolygons++;
        }
    }
    edgeAndFog();
    for (size_t i = 0; i < idBuf_.size(); i++) {                       // quels polygones sont visibles : negatif ou positif ?
        const uint16_t id = idBuf_[i];
        if (!id) continue;
        const int8_t f = pending_[id - 1].facing;
        if (f < 0) stats.winNeg++; else if (f > 0) stats.winPos++;
    }
    lastDrawn_.swap(pending_);                                         // gardee pour le diagnostic
    pending_.clear();
}

// Passe finale sur l'image 3D terminee :
// - contours (DISP3DCNT bit 5) : un pixel de polygone opaque devient la couleur de contour de son groupe d'ID
//   (ID / 8, registres 0x330) si un voisin a un autre ID et se trouve derriere lui. Le fond compte comme un voisin
//   (ID et profondeur de CLEAR_COLOR / CLEAR_DEPTH) : c'est ce qui dessine le trait autour des personnages.
// - brouillard (bit 7) : la densite depend de la profondeur (table de 32 valeurs a partir de FOG_OFFSET, un pas
//   de 0x400 >> FOG_SHIFT) ; la couleur et l'opacite tendent vers FOG_COLOR (bit 6 : l'opacite seulement).
void Gx::edgeAndFog() {
    if (disp3dcnt & 0x20) {
        std::vector<uint32_t> out(color_);
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++) {
                const size_t i = size_t(y) * 256 + size_t(x);
                if (!(pixFlags_[i] & 1)) continue;
                const uint8_t id = polyId_[i];
                const float d = depth_[i];
                bool edge = false;
                const int nx[4] = { x - 1, x + 1, x, x }, ny[4] = { y, y, y - 1, y + 1 };
                for (int k = 0; k < 4 && !edge; k++) {
                    if (nx[k] < 0 || nx[k] > 255 || ny[k] < 0 || ny[k] > 191) continue;
                    const size_t j = size_t(ny[k]) * 256 + size_t(nx[k]);
                    if (polyId_[j] != id && d < depth_[j]) edge = true;
                }
                if (!edge) continue;
                const uint16_t ec = reg16(uint32_t(id >> 3) * 2);
                out[i] = (color_[i] & 0xFF000000u) | (uint32_t(expand5(ec & 31)) << 16) |
                         (uint32_t(expand5((ec >> 5) & 31)) << 8) | expand5((ec >> 10) & 31);
            }
        color_.swap(out);
    }
    if (disp3dcnt & 0x80) {
        const int shift = (disp3dcnt >> 8) & 15;
        const float step = float(0x400 >> shift);
        const float offset = float(reg16(0x2C) & 0x7FFF);
        const uint32_t fc = uint32_t(dispRegs[0x28] | (dispRegs[0x29] << 8) | (dispRegs[0x2A] << 16));
        const float fr = expand5(fc & 31), fg = expand5((fc >> 5) & 31), fb = expand5((fc >> 10) & 31);
        const float fa = float((fc >> 16) & 31);
        const bool alphaOnly = (disp3dcnt & 0x40) != 0;
        float table[32];
        for (int k = 0; k < 32; k++) { const int v = dispRegs[0x30 + k] & 127; table[k] = v == 127 ? 128.0f : float(v); }
        for (size_t i = 0; i < color_.size(); i++) {
            if (!(pixFlags_[i] & 2)) continue;
            const float t = (depth_[i] * 32767.0f - offset) / step - 1.0f;
            float dens;
            if (t <= 0) dens = table[0];
            else if (t >= 31) dens = table[31];
            else { const int k = int(t); const float u = t - float(k); dens = table[k] * (1 - u) + table[k + 1] * u; }
            const float f = dens / 128.0f;
            if (f <= 0) continue;
            const uint32_t c = color_[i];
            float a = float(c >> 24);
            float r = float((c >> 16) & 255), g = float((c >> 8) & 255), b = float(c & 255);
            if (!alphaOnly) { r += (fr - r) * f; g += (fg - g) * f; b += (fb - b) * f; }
            a += (fa - a) * f;
            const uint32_t ia = uint32_t(std::clamp(a + 0.5f, 0.0f, 31.0f));
            color_[i] = (ia << 24) | (uint32_t(r + 0.5f) << 16) | (uint32_t(g + 0.5f) << 8) | uint32_t(b + 0.5f);
        }
    }
    if (disp3dcnt & 0x10) {                                           // anticrenelage : bords melanges avec le dessous
        // Seuls les bords de la silhouette sont adoucis : un pixel dont un voisin est le fond ou un autre objet
        // (autre ID de polygone). Les jointures entre polygones d'un meme modele restent pleines.
        std::vector<uint8_t> sil(color_.size(), 0);
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++) {
                const size_t i = size_t(y) * 256 + size_t(x);
                if (!(pixFlags_[i] & 1) || cov_[i] >= 31) continue;
                const int nx[4] = { x - 1, x + 1, x, x }, ny[4] = { y, y, y - 1, y + 1 };
                for (int k = 0; k < 4; k++) {
                    if (nx[k] < 0 || nx[k] > 255 || ny[k] < 0 || ny[k] > 191) continue;
                    const size_t j = size_t(ny[k]) * 256 + size_t(nx[k]);
                    if (!(pixFlags_[j] & 1) || polyId_[j] != polyId_[i]) { sil[i] = 1; break; }
                }
            }
        for (size_t i = 0; i < color_.size(); i++) {
            if (!sil[i]) continue;
            const float c = float(cov_[i]) / 31.0f;
            const uint32_t top = color_[i], bel = below_[i];
            const float ta = float(top >> 24), ba = float(bel >> 24);
            uint32_t rgb = top & 0xFFFFFFu;
            if (ba > 0) {                                                 // quelque chose dessous : les couleurs se melangent
                uint32_t out = 0;
                for (int sh = 0; sh <= 16; sh += 8) {
                    const float v = float((top >> sh) & 255) * c + float((bel >> sh) & 255) * (1.0f - c);
                    out |= uint32_t(v + 0.5f) << sh;
                }
                rgb = out;
            }
            const uint32_t a = uint32_t(std::clamp(ta * c + ba * (1.0f - c) + 0.5f, 1.0f, 31.0f));   // fond vide : bord a demi transparent
            color_[i] = (a << 24) | rgb;
        }
    }
}

uint8_t Gx::texByte(uint32_t addr) const {
    const uint8_t* slot = texMem_.tex[(addr >> 17) & 3];
    return slot ? slot[addr & 0x1FFFF] : uint8_t(0);
}

uint16_t Gx::palHalf(uint32_t addr) const {
    addr &= 0x1FFFE;
    const uint32_t slot = addr >> 14;
    if (slot >= 6 || !texMem_.pal[slot]) return 0;
    const uint8_t* p = texMem_.pal[slot] + (addr & 0x3FFF);
    return uint16_t(p[0] | (p[1] << 8));
}

// Lit un texel (s, t en 12.4). Retourne faux si le polygone n'a pas de texture. a : opacite 0..31.
bool Gx::sampleTexture(const GxPoly& poly, int s, int t, uint8_t& r, uint8_t& g, uint8_t& b, int& a) const {
    const uint32_t fmt = (poly.texParam >> 26) & 7;
    if (fmt == 0) return false;
    const int W = 8 << ((poly.texParam >> 20) & 7), H = 8 << ((poly.texParam >> 23) & 7);
    auto wrap = [](int c, int size, bool repeat, bool flip) {
        if (repeat) {
            if (flip) { const int m = c & (2 * size - 1); return (m & size) ? (2 * size - 1 - m) : m; }
            return c & (size - 1);
        }
        return c < 0 ? 0 : (c >= size ? size - 1 : c);
        };
    const int u = wrap(s >> 4, W, (poly.texParam >> 16) & 1, (poly.texParam >> 18) & 1);
    const int v = wrap(t >> 4, H, (poly.texParam >> 17) & 1, (poly.texParam >> 19) & 1);
    const uint32_t base = (poly.texParam & 0xFFFF) * 8;
    const uint32_t texel = uint32_t(v) * uint32_t(W) + uint32_t(u);
    const bool color0Clear = (poly.texParam >> 29) & 1;
    uint16_t c16 = 0;
    a = 31;
    switch (fmt) {
    case 1: {                                                    // A3I5
        const uint8_t d = texByte(base + texel);
        c16 = palHalf(poly.texPal * 16 + (d & 31) * 2);
        const int a3 = d >> 5;
        a = a3 * 4 + a3 / 2;
        break;
    }
    case 2: {                                                    // 4 couleurs
        const int idx = (texByte(base + (texel >> 2)) >> ((texel & 3) * 2)) & 3;
        c16 = palHalf(poly.texPal * 8 + uint32_t(idx) * 2);
        if (idx == 0 && color0Clear) a = 0;
        break;
    }
    case 3: {                                                    // 16 couleurs
        const uint8_t d = texByte(base + (texel >> 1));
        const int idx = (texel & 1) ? (d >> 4) : (d & 15);
        c16 = palHalf(poly.texPal * 16 + uint32_t(idx) * 2);
        if (idx == 0 && color0Clear) a = 0;
        break;
    }
    case 4: {                                                    // 256 couleurs
        const uint8_t idx = texByte(base + texel);
        c16 = palHalf(poly.texPal * 16 + uint32_t(idx) * 2);
        if (idx == 0 && color0Clear) a = 0;
        break;
    }
    case 5: {                                                    // compresse par blocs de 4 x 4
        const uint32_t block = uint32_t(v >> 2) * uint32_t(W >> 2) + uint32_t(u >> 2);
        const uint32_t dataAddr = base + block * 4 + uint32_t(v & 3);
        const int ci = (texByte(dataAddr) >> ((u & 3) * 2)) & 3;
        const uint32_t slotBase = ((base >> 17) & 2) ? 0x60000u : 0x20000u;
        const uint32_t infoAddr = slotBase + (((base & 0x1FFFC) + block * 4) >> 1);
        const uint16_t info = uint16_t(texByte(infoAddr) | (texByte(infoAddr + 1) << 8));
        const uint32_t palAddr = poly.texPal * 16 + uint32_t(info & 0x3FFF) * 4;
        const uint16_t p0 = palHalf(palAddr), p1 = palHalf(palAddr + 2);
        auto mix = [](uint16_t x, uint16_t y, int wx, int wy) {
            int out = 0;
            for (int sh = 0; sh < 15; sh += 5) out |= ((((x >> sh) & 31) * wx + ((y >> sh) & 31) * wy) / (wx + wy)) << sh;
            return uint16_t(out);
            };
        switch (info >> 14) {
        case 0: c16 = ci == 0 ? p0 : ci == 1 ? p1 : ci == 2 ? palHalf(palAddr + 4) : p0; if (ci == 3) a = 0; break;
        case 1: c16 = ci == 0 ? p0 : ci == 1 ? p1 : ci == 2 ? mix(p0, p1, 1, 1) : p0; if (ci == 3) a = 0; break;
        case 2: c16 = palHalf(palAddr + uint32_t(ci) * 2); break;
        default: c16 = ci == 0 ? p0 : ci == 1 ? p1 : ci == 2 ? mix(p0, p1, 5, 3) : mix(p0, p1, 3, 5); break;
        }
        break;
    }
    case 6: {                                                    // A5I3
        const uint8_t d = texByte(base + texel);
        c16 = palHalf(poly.texPal * 16 + (d & 7) * 2);
        a = d >> 3;
        break;
    }
    default: {                                                   // 7 : couleurs directes, bit 15 = opaque
        const uint32_t addr = base + texel * 2;
        c16 = uint16_t(texByte(addr) | (texByte(addr + 1) << 8));
        a = (c16 & 0x8000) ? 31 : 0;
        break;
    }
    }
    r = expand5(c16 & 31); g = expand5((c16 >> 5) & 31); b = expand5((c16 >> 10) & 31);
    return true;
}

// Prepare la texture d'un polygone : pointeur sur les donnees et palette convertie en couleurs 8 bits.
void Gx::prepareTexture(const GxPoly& poly, TexFast& tf) const {
    const uint32_t fmt = (poly.texParam >> 26) & 7;
    tf.fmt = fmt;
    tf.W = 8 << ((poly.texParam >> 20) & 7);
    tf.H = 8 << ((poly.texParam >> 23) & 7);
    tf.repS = (poly.texParam >> 16) & 1; tf.repT = (poly.texParam >> 17) & 1;
    tf.flipS = (poly.texParam >> 18) & 1; tf.flipT = (poly.texParam >> 19) & 1;
    tf.clear0 = (poly.texParam >> 29) & 1;

    // Donnees : utilisables directement si toute la texture tient dans une fente de 128 Ko
    const uint32_t base = (poly.texParam & 0xFFFF) * 8;
    const uint32_t bytes = fmt == 7 ? uint32_t(tf.W * tf.H * 2) : fmt == 2 ? uint32_t(tf.W * tf.H / 4) :
        fmt == 3 ? uint32_t(tf.W * tf.H / 2) : uint32_t(tf.W * tf.H);
    const uint8_t* slot = texMem_.tex[(base >> 17) & 3];
    tf.data = (fmt != 5 && slot && (base & 0x1FFFF) + bytes <= 0x20000) ? slot + (base & 0x1FFFF) : nullptr;
    // diagnostic : memoire de texture ou de palette absente, ou chemin lent
    Stats& st = const_cast<Stats&>(stats);
    if (!slot) st.texMissing++;
    if (!tf.data) st.slowPath++;
    if (fmt == 5 && !texMem_.tex[((base >> 17) & 2) ? 3 : 1]) st.infoMissing++;
    if (fmt != 0 && fmt != 7) {
        const uint32_t pa = poly.texPal * (fmt == 2 ? 8u : 16u);
        if ((pa >> 14) >= 6 || !texMem_.pal[pa >> 14]) st.palMissing++;
    }

    const int entries = fmt == 1 ? 32 : fmt == 2 ? 4 : fmt == 3 ? 16 : fmt == 4 ? 256 : fmt == 6 ? 8 : 0;
    if (entries) {
        const uint32_t key = (fmt << 16) | poly.texPal;
        if (tf.palKey != key) {
            const uint32_t palAddr = poly.texPal * (fmt == 2 ? 8u : 16u);
            for (int i = 0; i < entries; i++) {
                const uint16_t c16 = palHalf(palAddr + uint32_t(i) * 2);
                tf.pal32[i] = (uint32_t(expand5(c16 & 31)) << 16) | (uint32_t(expand5((c16 >> 5) & 31)) << 8) | expand5((c16 >> 10) & 31);
            }
            tf.palKey = key;
        }
    }
}

void Gx::rasterize(const GxPoly& poly) {
    const int vx1 = int(viewport_ & 0xFF), vy1 = int((viewport_ >> 8) & 0xFF);
    const int vx2 = int((viewport_ >> 16) & 0xFF), vy2 = int((viewport_ >> 24) & 0xFF);
    const float width = float(vx2 - vx1 + 1), height = float(vy2 - vy1 + 1);
    const float top = float(191 - vy2);

    struct SV { float x, y, z, iw, r, g, b, s, t; };
    SV sv[10];
    for (int i = 0; i < poly.n; i++) {
        const GxVtx& v = poly.v[i];
        const float w = float(v.w > 0 ? v.w : 1);
        sv[i].x = float(vx1) + (float(v.x) + w) * width / (2.0f * w);
        sv[i].y = top + (-float(v.y) + w) * height / (2.0f * w);
        sv[i].z = (float(v.z) / w + 1.0f) * 0.5f;
        sv[i].iw = 1.0f / w;
        sv[i].r = v.r; sv[i].g = v.g; sv[i].b = v.b;
        sv[i].s = float(v.s); sv[i].t = float(v.t);
    }

    const uint32_t alphaReg = (poly.attr >> 16) & 31;
    const int polyAlpha = (alphaReg == 0) ? 31 : int(alphaReg);       // 0 = fil de fer : dessine plein pour l'instant
    const bool writeDepthTranslucent = (poly.attr & 0x800) != 0;
    const uint32_t mode = (poly.attr >> 4) & 3;                       // 0 modulation, 1 decalque, 2 toon / surbrillance, 3 ombre
    const bool fogOn = (poly.attr & 0x8000) != 0;                     // POLYGON_ATTR bit 15 : brouillard
    // Ombres (mode 3) : un volume d'ombre est dessine en deux temps. Les polygones d'ID 0 (masque) ne dessinent rien :
    // ils marquent le pochoir la ou le test de profondeur ECHOUE (le sol est a l'interieur du volume). Puis les
    // polygones d'ombre (ID 1..63) ne se dessinent que sur les pixels marques, sauf sur un objet de meme ID
    // (un kart ne recoit pas sa propre ombre). Le pochoir est efface au debut d'une nouvelle serie de masques.
    const uint32_t polyIdNum = (poly.attr >> 24) & 63;
    const int shadowKind = (mode != 3) ? 0 : (polyIdNum == 0 ? 1 : 2);
    if (shadowKind == 1) {
        if (!lastWasMask_) std::fill(stencil_.begin(), stencil_.end(), uint8_t(0));
        lastWasMask_ = true;
    } else if (shadowKind == 2) {
        lastWasMask_ = false;
    }
    const bool textured = shadowKind != 1 && ((poly.texParam >> 26) & 7) != 0;
    // Anticrenelage (DISP3DCNT bit 4) : les polygones opaques sont dessines un demi-pixel plus large, et chaque pixel
    // du bord retient la part couverte (distance au bord) ; la passe finale le melange avec ce qu'il recouvre.
    const bool aaPoly = (disp3dcnt & 0x10) && shadowKind == 0 && polyAlpha == 31;
    TexFast& tf = texFast_;
    if (textured) prepareTexture(poly, tf);
    bool flat = true;                                                 // meme couleur a tous les sommets : pas d'interpolation
    for (int i = 1; i < poly.n; i++)
        if (poly.v[i].r != poly.v[0].r || poly.v[i].g != poly.v[0].g || poly.v[i].b != poly.v[0].b) flat = false;

    for (int k = 1; k + 1 < poly.n; k++) {                             // triangulation en eventail
        const SV& a = sv[0];
        SV b = sv[k], c = sv[k + 1];
        float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (area == 0.0f) continue;
        const bool swapped = area < 0.0f;
        if (swapped) { std::swap(b, c); area = -area; }
        // Cotes du triangle qui sont de vrais bords du polygone (les diagonales de l'eventail n'en sont pas) :
        // cote 0 = b-c (toujours un bord), cote 1 = c-a, cote 2 = a-b (a echanger si b et c l'ont ete)
        const bool firstTri = k == 1, lastTri = k + 2 == poly.n;
        const bool realEdge[3] = { true, swapped ? firstTri : lastTri, swapped ? lastTri : firstTri };

        const int grow = 0;
        const int minY = std::max(0, int(std::floor(std::min({ a.y, b.y, c.y }))) - grow);
        const int maxY = std::min(191, int(std::ceil(std::max({ a.y, b.y, c.y }))) + grow);
        const int minX = std::max(0, int(std::floor(std::min({ a.x, b.x, c.x }))) - grow);
        const int maxX = std::min(255, int(std::ceil(std::max({ a.x, b.x, c.x }))) + grow);
        if (minX > maxX || minY > maxY) continue;

        // poids de a et de b : fonctions lineaires de (x, y) ; le poids de c est le complement
        const float inv = 1.0f / area;
        const float aA = (b.y - c.y) * inv, aB = (c.x - b.x) * inv, aC = (b.x * c.y - b.y * c.x) * inv;
        const float bA = (c.y - a.y) * inv, bB = (a.x - c.x) * inv, bC = (c.x * a.y - c.y * a.x) * inv;
        // longueur du gradient de chaque poids : poids / longueur = distance (en pixels) au cote oppose
        const float gEdge[3] = { std::sqrt(aA * aA + aB * aB), std::sqrt(bA * bA + bB * bB),
                                 std::sqrt((aA + bA) * (aA + bA) + (aB + bB) * (aB + bB)) };
        const float invG[3] = { gEdge[0] > 0 ? 1.0f / gEdge[0] : 0.0f, gEdge[1] > 0 ? 1.0f / gEdge[1] : 0.0f, gEdge[2] > 0 ? 1.0f / gEdge[2] : 0.0f };
        // Le polygone n'est pas elargi : comme sur la console, l'anticrenelage ne touche que les pixels qu'il couvre
        // deja (un elargissement faisait deborder la texture et la profondeur sur les polygones voisins)
        const float aaGrow[3] = { 0.0f, 0.0f, 0.0f };
        const float gx3[3] = { aA, bA, -(aA + bA) }, gy3[3] = { aB, bB, -(aB + bB) };   // gradients des poids

        // Une grandeur f vaut f2 + w0 * (f0 - f2) + w1 * (f1 - f2) : plan en (x, y), pas en x constant
        struct Plane { float c0, gx, gy; };
        auto plane = [&](float f0, float f1, float f2) {
            const float d0 = f0 - f2, d1 = f1 - f2;
            return Plane{ f2 + aC * d0 + bC * d1, aA * d0 + bA * d1, aB * d0 + bB * d1 };
            };
        const Plane pz = plane(a.z, b.z, c.z);
        const Plane pd = plane(a.iw, b.iw, c.iw);                       // denominateur de la perspective
        Plane pr{}, pg{}, pb{}, ps{}, pt{};
        if (!flat) {
            pr = plane(a.r * a.iw, b.r * b.iw, c.r * c.iw);
            pg = plane(a.g * a.iw, b.g * b.iw, c.g * c.iw);
            pb = plane(a.b * a.iw, b.b * b.iw, c.b * c.iw);
        }
        if (textured) {
            ps = plane(a.s * a.iw, b.s * b.iw, c.s * c.iw);
            pt = plane(a.t * a.iw, b.t * b.iw, c.t * c.iw);
        }
        const float flatR = poly.v[0].r, flatG = poly.v[0].g, flatB = poly.v[0].b;

        for (int y = minY; y <= maxY; y++) {
            const float py = float(y) + 0.5f;
            const float k0 = aC + aB * py, k1 = bC + bB * py, k2 = 1.0f - k0 - k1;
            // intervalle de x (centres de pixels) ou les trois poids sont positifs
            float lo = -1e30f, hi = 1e30f;
            bool empty = false;
            const float ks0[3] = { k0, k1, k2 }, ms[3] = { aA, bA, -(aA + bA) };
            // avec l'anticrenelage, un pixel est pris si son centre est a moins d'un demi-pixel du triangle
            const float ks[3] = { ks0[0] + 0.5f * gEdge[0] * aaGrow[0], ks0[1] + 0.5f * gEdge[1] * aaGrow[1], ks0[2] + 0.5f * gEdge[2] * aaGrow[2] };
            for (int e = 0; e < 3; e++) {
                if (ms[e] > 1e-12f) lo = std::max(lo, -ks[e] / ms[e]);
                else if (ms[e] < -1e-12f) hi = std::min(hi, -ks[e] / ms[e]);
                else if (ks[e] < 0.0f) empty = true;
            }
            if (empty || lo > hi) continue;
            const int xs = std::max(minX, int(std::ceil(lo - 0.5f)));
            const int xe = std::min(maxX, int(std::floor(hi - 0.5f)));
            if (xs > xe) continue;

            const float px0 = float(xs) + 0.5f;
            float z = pz.c0 + pz.gy * py + pz.gx * px0;
            float dn = pd.c0 + pd.gy * py + pd.gx * px0;
            float nr = pr.c0 + pr.gy * py + pr.gx * px0, ng = pg.c0 + pg.gy * py + pg.gx * px0, nb = pb.c0 + pb.gy * py + pb.gx * px0;
            float ns = ps.c0 + ps.gy * py + ps.gx * px0, nt = pt.c0 + pt.gy * py + pt.gx * px0;
            size_t idx = size_t(y) * 256 + size_t(xs);

            for (int x = xs; x <= xe; x++, idx++, z += pz.gx, dn += pd.gx, nr += pr.gx, ng += pg.gx, nb += pb.gx, ns += ps.gx, nt += pt.gx) {
                if (shadowKind == 1) {                                        // masque : marque la ou le test echoue
                    if (z > depth_[idx]) stencil_[idx] = 1;
                    continue;
                }
                if (shadowKind == 2 && (!stencil_[idx] || polyId_[idx] == polyIdNum)) continue;
                uint8_t covN = 31;
                // valeurs interpolees au pixel ; pour un pixel du bord elargi (centre hors du triangle), elles sont
                // prises au point le plus proche dans le triangle (sinon la texture deborde sur ses voisines)
                float sz = z, sdn = dn, snr = nr, sng = ng, snb = nb, sns = ns, snt = nt;
                if (aaPoly) {                                                 // part couverte : distance au bord le plus proche
                    const float pxc = float(x) + 0.5f;
                    float w[3] = { k0 + aA * pxc, k1 + bA * pxc, 0.0f };
                    w[2] = 1.0f - w[0] - w[1];
                    float d = 1e9f;
                    for (int e = 0; e < 3; e++) if (realEdge[e]) d = std::min(d, w[e] * invG[e]);
                    d += 0.5f;
                    if (d <= 0.0f) continue;
                    covN = d >= 1.0f ? uint8_t(31) : uint8_t(d * 31.0f + 0.5f);
                    if (covN == 0) continue;
                    if (w[0] < 0.0f || w[1] < 0.0f || w[2] < 0.0f) {
                        float qx = pxc, qy = py;
                        for (int pass2 = 0; pass2 < 2; pass2++)
                            for (int e = 0; e < 3; e++) {
                                const float we = (e == 0 ? aC + aA * qx + aB * qy
                                                : e == 1 ? bC + bA * qx + bB * qy
                                                : 1.0f - (aC + aA * qx + aB * qy) - (bC + bA * qx + bB * qy));
                                if (we >= 0.0f) continue;
                                const float g2 = gx3[e] * gx3[e] + gy3[e] * gy3[e];
                                if (g2 <= 0.0f) continue;
                                const float t = (-we + 1e-4f) / g2;
                                qx += gx3[e] * t; qy += gy3[e] * t;
                            }
                        auto at = [&](const Plane& pl) { return pl.c0 + pl.gx * qx + pl.gy * qy; };
                        sz = at(pz); sdn = at(pd);
                        if (!flat) { snr = at(pr); sng = at(pg); snb = at(pb); }
                        if (textured) { sns = at(ps); snt = at(pt); }
                    }
                }
                if (sz > depth_[idx]) {
                    continue;
                }
                const float pinv = 1.0f / (sdn > 1e-12f ? sdn : 1e-12f);
                float r, g, bl;
                if (flat) { r = flatR; g = flatG; bl = flatB; }
                else { r = snr * pinv; g = sng * pinv; bl = snb * pinv; }
                // Toon / surbrillance (mode 2) : la composante rouge du sommet choisit une couleur de la table toon
                float hr = 0, hg = 0, hb = 0;
                if (mode == 2) {
                    const int ti = std::clamp(int(r) >> 3, 0, 31);
                    const uint16_t tc = reg16(0x50 + uint32_t(ti) * 2);
                    const float cr = expand5(tc & 31), cg = expand5((tc >> 5) & 31), cb = expand5((tc >> 10) & 31);
                    if (disp3dcnt & 2) { hr = cr; hg = cg; hb = cb; }                     // surbrillance : ajoutee apres la texture
                    else { r = cr; g = cg; bl = cb; }                                   // toon : remplace la couleur
                }
                int alpha = polyAlpha;
                if (textured) {
                    float sf = sns * pinv, tfv = snt * pinv;
                    int s = int(sf); if (float(s) > sf) s--;                  // partie entiere vers le bas
                    int t = int(tfv); if (float(t) > tfv) t--;
                    uint32_t trgb;
                    int ta;
                    if (tf.data) {
                        // chemin rapide : coordonnees de texel, puis lecture directe
                        auto wrap = [](int cc, int size, bool repeat, bool flip) {
                            if (repeat) {
                                if (flip) { const int m = cc & (2 * size - 1); return (m & size) ? (2 * size - 1 - m) : m; }
                                return cc & (size - 1);
                            }
                            return cc < 0 ? 0 : (cc >= size ? size - 1 : cc);
                            };
                        const int u = wrap(s >> 4, tf.W, tf.repS, tf.flipS), v = wrap(t >> 4, tf.H, tf.repT, tf.flipT);
                        const uint32_t texel = uint32_t(v) * uint32_t(tf.W) + uint32_t(u);
                        ta = 31;
                        switch (tf.fmt) {
                        case 1: { const uint8_t d = tf.data[texel]; trgb = tf.pal32[d & 31]; const int a3 = d >> 5; ta = a3 * 4 + a3 / 2; break; }
                        case 2: { const int i = (tf.data[texel >> 2] >> ((texel & 3) * 2)) & 3; trgb = tf.pal32[i]; if (i == 0 && tf.clear0) ta = 0; break; }
                        case 3: { const uint8_t d = tf.data[texel >> 1]; const int i = (texel & 1) ? (d >> 4) : (d & 15); trgb = tf.pal32[i]; if (i == 0 && tf.clear0) ta = 0; break; }
                        case 4: { const uint8_t i = tf.data[texel]; trgb = tf.pal32[i]; if (i == 0 && tf.clear0) ta = 0; break; }
                        case 6: { const uint8_t d = tf.data[texel]; trgb = tf.pal32[d & 7]; ta = d >> 3; break; }
                        default: {
                            const uint16_t c16 = uint16_t(tf.data[texel * 2] | (tf.data[texel * 2 + 1] << 8));
                            trgb = (uint32_t(expand5(c16 & 31)) << 16) | (uint32_t(expand5((c16 >> 5) & 31)) << 8) | expand5((c16 >> 10) & 31);
                            ta = (c16 & 0x8000) ? 31 : 0;
                            break;
                        }
                        }
                    }
                    else {                                                  // chemin lent : compresse ou texture a cheval sur deux fentes
                        uint8_t tr, tg, tb;
                        sampleTexture(poly, s, t, tr, tg, tb, ta);
                        trgb = (uint32_t(tr) << 16) | (uint32_t(tg) << 8) | tb;
                    }
                    if (ta == 0) continue;                                    // texel transparent
                    const float tr = float((trgb >> 16) & 255), tg = float((trgb >> 8) & 255), tb = float(trgb & 255);
                    if (mode == 1) {                                          // decalque : le texel recouvre la couleur du sommet
                        const float f = float(ta) / 31.0f;
                        r = tr * f + r * (1.0f - f); g = tg * f + g * (1.0f - f); bl = tb * f + bl * (1.0f - f);
                    }
                    else {                                                  // modulation
                        r = tr * r * (1.0f / 255.0f); g = tg * g * (1.0f / 255.0f); bl = tb * bl * (1.0f / 255.0f);
                    }
                    alpha = (polyAlpha == 31) ? ta : polyAlpha * ta / 31;
                    if (alpha == 0) continue;
                }
                if (mode == 2 && (disp3dcnt & 2)) { r += hr; g += hg; bl += hb; }
                if ((disp3dcnt & 4) && alpha <= int(dispRegs[0x10] & 31)) continue;   // test alpha
                const uint32_t ir = uint32_t(std::min(255.0f, r + 0.5f));
                const uint32_t ig = uint32_t(std::min(255.0f, g + 0.5f));
                const uint32_t ib = uint32_t(std::min(255.0f, bl + 0.5f));
                if (alpha >= 31) {
                    depth_[idx] = sz;
                    idBuf_[idx] = curId_;
                    if (shadowKind == 0) {
                        below_[idx] = color_[idx];
                        cov_[idx] = aaPoly ? covN : uint8_t(31);
                        polyId_[idx] = uint8_t(polyIdNum);
                        pixFlags_[idx] = uint8_t(1 | (fogOn ? 2 : 0));
                    }
                    color_[idx] = (31u << 24) | (ir << 16) | (ig << 8) | ib;
                }
                else {                                                      // translucide : melange avec ce qui est deja dessine
                    const uint32_t dst = color_[idx];
                    const uint32_t dstA = dst >> 24;
                    if (dstA) {
                        // la couleur se melange ; l'opacite resultante est la plus grande des deux
                        const float f = float(alpha) / 31.0f;
                        const uint32_t orr = uint32_t(ir * f + ((dst >> 16) & 255) * (1.0f - f) + 0.5f);
                        const uint32_t og = uint32_t(ig * f + ((dst >> 8) & 255) * (1.0f - f) + 0.5f);
                        const uint32_t ob = uint32_t(ib * f + (dst & 255) * (1.0f - f) + 0.5f);
                        const uint32_t outA = uint32_t(alpha) > dstA ? uint32_t(alpha) : dstA;
                        color_[idx] = (outA << 24) | (orr << 16) | (og << 8) | ob;
                    }
                    else {                                                  // rien dessous : le pixel garde son opacite
                        color_[idx] = (uint32_t(alpha) << 24) | (ir << 16) | (ig << 8) | ib;
                    }
                    if (writeDepthTranslucent) depth_[idx] = sz;
                    if (!fogOn) pixFlags_[idx] = uint8_t(pixFlags_[idx] & ~2u);   // translucide sans brouillard : il s'arrete la
                }
            }
        }
    }
}