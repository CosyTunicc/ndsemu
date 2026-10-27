#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

class IBus;

// Processeur de son de la console (cote ARM7) : registres, 16 canaux, mixage, capture.
//
// 16 canaux, a 0x04000400 + 0x10 x canal :
//   +0 SOUNDxCNT  volume (0-6), diviseur (8-9), maintien (15), panoramique (16-22), rapport cyclique (24-26),
//                 repetition (27-28 : 0 manuel, 1 boucle, 2 une fois), format (29-30 : PCM8, PCM16, ADPCM,
//                 onde carree / bruit), bit 31 = en cours
//   +4 SOUNDxSAD  adresse de l'echantillon          (ecriture seule)
//   +8 SOUNDxTMR  minuterie : un echantillon tous les (0x10000 - TMR) x 2 cycles de 33,5 MHz   (ecriture seule)
//   +A SOUNDxPNT  debut de la boucle, en mots       (ecriture seule)
//   +C SOUNDxLEN  longueur apres la boucle, en mots (ecriture seule)
// Puis SOUNDCNT (0x500), SOUNDBIAS (0x504), capture : SNDCAPxCNT (0x508, 0x509), SNDCAPxDAD / LEN (0x510...).
//
// Fonctionnement : tous les 1024 cycles (environ 32 728 Hz), chaque canal actif avance au rythme de sa
// minuterie, puis les canaux sont melanges en un echantillon stereo 16 bits, ajoute au tampon de sortie.
// Le temps du son suit celui de la console : run() est appele par System::advance et a chaque acces aux
// registres. Un son "une fois" s'arrete (bit 31 a 0) a la fin de ses donnees.
class Spu {
public:
    static constexpr uint32_t kCyclesPerSample = 1024;             // 33 513 982 Hz / 1024 = 32 728 Hz
    static constexpr int kSampleRate = 32728;

    struct Channel {
        uint32_t cnt = 0, sad = 0, len = 0;
        uint16_t tmr = 0, pnt = 0;
        // etat de lecture
        uint32_t accum = 0;                    // cycles accumules vers le prochain echantillon
        uint32_t pos = 0;                      // echantillon courant (ou prochain demi-octet en ADPCM)
        int32_t cur = 0;                       // valeur courante, 16 bits signes
        int adpcmIndex = 0;                    // ADPCM : indice dans la table des pas
        int32_t loopCur = 0;                   // ADPCM : etat sauvegarde au point de boucle
        int loopIndex = 0;
        bool loopSaved = false;
        uint32_t psgStep = 0;                  // onde carree : position 0..7
        uint16_t lfsr = 0x7FFF;                // bruit : registre a decalage
        int32_t out = 0;                       // derniere sortie (apres volume), pour la capture
    };
    Channel ch[16];
    template <class Ar> void serialize(Ar& a);   // sauvegarde d'etat (savestate.cpp)

    uint16_t soundCnt = 0;                     // SOUNDCNT : volume general (0-6), sorties (8-13), bit 15 = actif
    uint16_t soundBias = 0x200;                // SOUNDBIAS : niveau du silence (10 bits)
    uint8_t capCnt[2] = {};                    // SNDCAPxCNT : bit 7 = capture en cours
    uint32_t capDad[2] = {};
    uint16_t capLen[2] = {};

    IBus* bus = nullptr;                       // memoire vue par l'ARM7 (donnees des sons, capture)
    const uint16_t* power = nullptr;           // POWCNT2 : bit 0 = son allume (nullptr : toujours allume)

    // Journal (--journal-son) : departs, arrets et fins des canaux, avec l'heure de la console. nullptr = coupe.
    FILE* journal = nullptr;
    void note(uint64_t now, const char* fmt, ...);

    // Diagnostic (affiche dans le rapport) : le jeu fait-il vraiment du son ?
    uint64_t channelStarts = 0;                // nombre de departs de canaux
    uint64_t framesMixed = 0;                  // echantillons stereo produits
    uint64_t framesAudible = 0;                // dont non nuls
    int32_t peak = 0;                          // plus forte valeur produite
    uint64_t framesMuted = 0;                  // produits pendant que SOUNDCNT ou POWCNT2 coupait le son

    // Echantillons stereo produits (L, R, L, R...), en attente d'etre joues par la plateforme.
    std::vector<int16_t> output;
    size_t maxBuffered = kSampleRate / 2;      // en echantillons stereo : au-dela d'environ 1 s, les plus vieux sont jetes
    // Retire jusqu'a maxFrames echantillons stereo du tampon ; renvoie le nombre copie.
    size_t takeSamples(int16_t* dst, size_t maxFrames);

    // addr alignee sur 4, dans 0x04000400..0x0400051F ; now = cycles ecoules (horloge de 33,5 MHz)
    uint32_t read32(uint32_t addr, uint64_t now);
    void write32(uint32_t addr, uint32_t value, uint32_t mask, uint64_t now);

    // Produit les echantillons dus jusqu'a "now". Peu couteux si aucun n'est du.
    void run(uint64_t now) { if (now >= nextSample) runSlow(now); }

    // Nombre d'echantillons d'un son, d'apres son format et sa longueur (en mots)
    static uint64_t sampleCount(uint32_t format, uint32_t words);

private:
    uint64_t nextSample = kCyclesPerSample;    // cycle du prochain echantillon de sortie
    uint64_t journalLines = 0;
    struct Capture { uint32_t accum = 0, pos = 0; } cap[2];
    int32_t lastMixL = 0, lastMixR = 0;         // derniere sortie du mixeur (pour la capture)

    void runSlow(uint64_t now);
    void mixOne();                              // un echantillon de sortie
    void startChannel(int i);
    void stepChannel(int i);                    // la minuterie du canal deborde : echantillon suivant
    int32_t fetch(const Channel& c, uint32_t pos) const;
    void decodeAdpcm(Channel& c);
    void runCapture(int i);
};
