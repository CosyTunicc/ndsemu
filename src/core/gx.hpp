#pragma once
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

// Moteur 3D de la console (etape 1) : commandes de la file de geometrie, piles de matrices, transformation
// des sommets, decoupage contre les plans proche et lointain, triangles a couleur par sommet avec tampon de
// profondeur. Pas encore : textures, eclairage, transparence, contour, brouillard, elimination des faces.
//
// Fonctionnement : le jeu ecrit des commandes (file GXFIFO a 0x04000400, empaquetees quatre par mot, ou ports
// directs a 0x04000400 + 4 * commande). Elles s'executent aussitot. SWAP_BUFFERS fige la liste de polygones ;
// au debut du V-Blank suivant (onVBlank), la liste est dessinee dans l'image 3D que le moteur 2D affiche en
// fond 0.
struct GxVtx {
    int32_t x = 0, y = 0, z = 0, w = 0x1000;   // coordonnees apres projection (virgule fixe 20.12)
    uint8_t r = 0, g = 0, b = 0;
    int32_t s = 0, t = 0;                      // coordonnees de texture (virgule fixe 12.4, en texels)
};

struct GxPoly {
    int n = 0;
    GxVtx v[10];
    uint32_t attr = 0;                          // POLYGON_ATTR au moment du BEGIN_VTXS
    uint32_t texParam = 0;                      // TEXIMAGE_PARAM et PLTT_BASE a la fin du polygone
    uint32_t texPal = 0;
    int8_t facing = 0;                          // -1 : sommets dans le sens inverse des aiguilles (face avant) ; +1 : dos ; 0 : degenere
};

// Memoire de textures telle que le systeme la voit au moment du dessin.
struct GxTexMem {
    const uint8_t* tex[4] = {};                 // 4 fentes de 128 Ko (banques A a D en mode texture)
    const uint8_t* pal[6] = {};                 // 6 fentes de 16 Ko (banques E, F, G en mode palette de textures)
};

class Gx {
public:
    Gx();
    ~Gx();
    Gx(const Gx&) = delete;
    Gx& operator=(const Gx&) = delete;
    template <class Ar> void serialize(Ar& a);   // sauvegarde d'etat (savestate.cpp)

    void writeFifo(uint32_t word);                    // GXFIFO : commandes empaquetees
    void writePort(uint32_t cmd, uint32_t param);     // port direct 0x04000400 + 4 * cmd
    void onVBlank(const GxTexMem& mem);               // si un echange de tampons est demande : dessine la liste

    uint32_t clearColor = 0;                          // CLEAR_COLOR (0x04000350) : couleur 15 bits, bit 15 brouillard, alpha bits 16-20, ID bits 24-29
    // Registres d'affichage 3D de 0x04000330 a 0x040003BF (ecriture seule) : couleurs des contours (0x330), seuil du
    // test alpha (0x340), couleur (0x358) et decalage (0x35C) du brouillard, table du brouillard (0x360), table toon (0x380)
    uint8_t dispRegs[0x90] = {};
    uint16_t disp3dcnt = 0;                           // DISP3DCNT (0x04000060), recopie au debut de chaque image
    uint32_t clearDepth = 0x7FFF;                     // CLEAR_DEPTH (0x04000354)
    bool cullEnabled = true;                          // elimination des faces selon les bits 6 (dos) et 7 (face) de POLYGON_ATTR
    bool cullFlip = false;                            // inverse le sens "face avant" (pour verifier la convention)

    // Image 3D prete a etre affichee : 256 x 192 pixels, octet du haut = opacite 0..31 (0 = transparent), puis 0xRRGGBB.
    // Le moteur 2D s'en sert pour melanger la 3D avec la couche situee dessous (voir BLDCNT dans video.cpp).
    const uint32_t* frame() const { return deferred ? front_.data() : color_.data(); }

    // Dessin differe (comme la console : une image 3D est affichee pendant l'image suivante) et sur un autre coeur.
    // A choisir avant le premier V-Blank. finish() : attend le dessin en cours et rend visible la derniere image.
    bool deferred = false, async = false;
    void finish();

    // Statistiques pour le diagnostic
    uint64_t commandCount = 0;
    uint64_t swapCount = 0;
    uint64_t polygonsDrawn = 0;        // dans la derniere image dessinee
    uint64_t polygonsSkipped = 0;      // polygones d'ombre (masques et ombres), dessines avec le tampon de pochoir
    uint64_t texturedPolygons = 0;     // polygones texturés dessines dans la derniere image

    // Diagnostic de la derniere image dessinee
    struct Stats {
        int polys = 0, lit = 0, translucent = 0, shadow = 0;
        int fmt[8] = {}, mode[4] = {}, texgen[4] = {};
        int texMissing = 0, palMissing = 0, infoMissing = 0, slowPath = 0;
        int offscreen = 0, dropped = 0;               // rejetes car hors champ / perdus (liste pleine)
        int renderFlags[4] = {};                       // bits 6-7 de POLYGON_ATTR : rien, dos, face, les deux
        int culled = 0;                                // faces ecartees (non stockees)
        int winNeg = 0, winPos = 0;                    // pixels gagnants : polygones d'aire negative (sens inverse) / positive
    } stats;
    void printStats(FILE* f) const;                   // resume
    void dumpPolygons(FILE* f) const;                 // liste complete des polygones de la derniere image
    uint64_t unknownCommands = 0;
    uint64_t renderNs = 0;                            // temps passe a dessiner les images 3D (cumul, en ns)

    // Tests de la geometrie (BOX_TEST, POS_TEST, VEC_TEST). Les commandes s'executent aussitot, donc le resultat
    // est toujours pret : le jeu le lit dans GXSTAT bit 1 (boite visible) et dans les registres de resultat.
    bool boxTestResult = false;                       // GXSTAT bit 1 : tout ou partie de la boite est dans le champ
    int32_t posResult[4] = {};                        // POS_RESULT : x, y, z, w apres transformation (20.12)
    int16_t vecResult[3] = {};                        // VEC_RESULT : vecteur transforme par la matrice des vecteurs
    void readClipMatrix(int32_t* out16);              // CLIPMTX_RESULT : projection x position
    void readVecMatrix(int32_t* out9) const;          // VECMTX_RESULT : matrice des vecteurs 3 x 3

private:
    void exec(uint32_t cmd, const uint32_t* p);
    void nextCommand();
    void submitVertex();
    void transformPoint(int32_t x, int32_t y, int32_t z, int32_t out[4]);
    void emitPolygon(const GxVtx* v, int n);
    void updateClip();
    void rasterize(const GxPoly& poly);
    uint8_t texByte(uint32_t addr) const;
    uint16_t palHalf(uint32_t addr) const;
    bool sampleTexture(const GxPoly& poly, int s, int t, uint8_t& r, uint8_t& g, uint8_t& b, int& a) const;

    // Texture preparee pour le dessin rapide (une par polygone, palette reutilisee d'un polygone au suivant)
    struct TexFast {
        uint32_t fmt = 0;
        int W = 0, H = 0;
        bool repS = false, repT = false, flipS = false, flipT = false, clear0 = false;
        const uint8_t* data = nullptr;                   // donnees contigues dans une fente, sinon nullptr (chemin lent)
        uint32_t pal32[256];                              // couleurs 0xRRGGBB deja converties
        uint32_t palKey = 0xFFFFFFFFu;                    // (format, PLTT_BASE) de la palette en memoire
    };
    void prepareTexture(const GxPoly& poly, TexFast& tf) const;

    // file de commandes
    uint32_t packed_ = 0;
    int packedPos_ = 4;
    bool inParams_ = false;
    uint32_t cmd_ = 0;
    int need_ = 0, have_ = 0;
    uint32_t params_[32] = {};
    uint32_t portBuf_[0x80][32] = {};
    int portHave_[0x80] = {};

    // matrices (4 x 4, par lignes, virgule fixe 20.12)
    int mtxMode_ = 0;
    int32_t proj_[16], pos_[16], vec_[16], tex_[16];
    int32_t projStack_[16];
    int32_t posStack_[32][16], vecStack_[32][16];
    int posSp_ = 0;
    int32_t clip_[16];
    bool clipDirty_ = true;

    // etat des sommets
    int32_t vx_ = 0, vy_ = 0, vz_ = 0;
    uint8_t cr_ = 255, cg_ = 255, cb_ = 255;       // couleur courante
    int32_t texS_ = 0, texT_ = 0;                  // coordonnee de texture courante (apres transformation)
    uint32_t texParam_ = 0, texPal_ = 0;           // TEXIMAGE_PARAM, PLTT_BASE
    GxTexMem texMem_;
    TexFast texFast_;
    uint32_t diffuse_ = 0x7FFF;                    // couleur de matiere (DIF_AMB)
    // eclairage
    int32_t lightDir_[4][3] = {};                  // directions des lumieres (virgule fixe 20.12, deja transformees)
    uint16_t lightColor_[4] = { 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF };
    uint16_t ambient_ = 0, specular_ = 0, emission_ = 0;      // diffuse_ ci-dessous
    void applyLighting(int32_t nx, int32_t ny, int32_t nz);
    uint32_t polyAttrNext_ = 0, polyAttr_ = 0;
    uint32_t viewport_ = 0xBFFF0000u >> 0;         // x1 = 0, y1 = 0, x2 = 255, y2 = 191
    int vtxType_ = 0;
    int vtxCount_ = 0;
    GxVtx asm_[4];                                 // sommets en cours d'assemblage

    // listes de polygones : en construction, puis figee par SWAP_BUFFERS
    std::vector<GxPoly> building_, pending_, lastDrawn_;
    bool swapPending_ = false;
    int rejectedOffscreen_ = 0, droppedPolys_ = 0;       // depuis le dernier echange de tampons
    int pendingRejected_ = 0, pendingDropped_ = 0;       // ceux de la liste figee

    // image 3D
    std::vector<uint32_t> color_;
    std::vector<float> depth_;
    std::vector<uint8_t> stencil_;                     // ombres : pixels marques par les polygones masque (ID 0)
    std::vector<uint8_t> polyId_;                      // ID (0..63) du polygone opaque visible a chaque pixel
    std::vector<uint8_t> pixFlags_;                    // bit 0 : polygone opaque (contours), bit 1 : brouillard
    std::vector<uint32_t> below_;                      // anticrenelage : ce qu'il y avait sous le bord d'un polygone
    std::vector<uint8_t> cov_;                         // anticrenelage : part du pixel couverte par le polygone (0..31)
    void edgeAndFog();                                 // contours, brouillard puis anticrenelage, sur l'image terminee
    uint16_t reg16(uint32_t off) const { return uint16_t(rRegs_[off] | (rRegs_[off + 1] << 8)); }
    void renderFrame();                                // dessine rList_ avec l'etat copie au V-Blank (r...)
    void workerLoop();
    // etat copie au V-Blank pour le dessin
    std::vector<GxPoly> rList_;
    uint8_t rRegs_[0x90] = {};
    uint16_t r3dcnt_ = 0;
    uint32_t rClearColor_ = 0, rClearDepth_ = 0x7FFF, rViewport_ = 0xBFFF0000u;
    int rRejected_ = 0, rDropped_ = 0, rCulled_ = 0;
    bool pendingWbuf_ = false, rWbuf_ = false;         // profondeur W (SWAP_BUFFERS bit 1)
    static constexpr float kWScale = 1.0f / 16777215.0f;  // W (20.12, 24 bits) -> profondeur 0..1
    std::vector<uint8_t> texCopy_[4], palCopy_[6];
    std::vector<uint32_t> front_ = std::vector<uint32_t>(256 * 192, 0);   // image visible (mode differe)
    std::thread thread_;
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    bool jobBusy_ = false, quit_ = false, publishPending_ = false;
    bool lastWasMask_ = false;                         // le polygone d'ombre precedent etait un masque
    std::vector<uint16_t> idBuf_;                      // polygone visible a chaque pixel (diagnostic)
    uint16_t curId_ = 0;
    int culledPolys_ = 0, pendingCulled_ = 0;
};