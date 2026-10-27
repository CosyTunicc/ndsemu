#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Bibliotheque de jeux : dossiers surveilles, jeux trouves dedans, reglages. Aucune dependance a l'interface :
// ce fichier ne fait que lire le disque et garder l'etat (chemins en UTF-8).

// Niveau de compatibilite d'un jeu (choisi par l'utilisateur, garde dans le fichier de reglages).
enum class Compat { Unknown, Perfect, Playable, InGame, Menus, Broken };
const char* compatLabel(Compat c);
int compatCount();

struct GameEntry {
    std::string path;                 // chemin complet du fichier .nds
    std::string fileName;             // nom du fichier seul
    std::string title;                // titre lu dans la banniere (1re ligne), sinon le nom du fichier
    std::string subtitle;             // lignes suivantes de la banniere (souvent l'editeur)
    std::string gameCode;             // AMCP, A2DP...
    uint16_t headerCrc = 0;           // CRC de l'en-tete : distingue deux versions d'un meme jeu
    uint64_t size = 0;                // taille du fichier en octets
    int64_t mtime = 0;                // date de modification (pour ne relire la banniere que si le fichier change)
    bool hasSave = false;             // une sauvegarde .sav existe a cote
    int states = 0;                   // sauvegardes d'etat (etats/<nom>.etat1..4 a cote du programme)
    bool hasIcon = false;
    uint32_t icon[32 * 32] = {};      // icone 32 x 32, format 0xAABBGGRR (directement utilisable comme texture RGBA)
};

struct GameFolder {
    std::string path;                 // dossier ("" pour le groupe des fichiers ajoutes un par un)
    bool open = true;                 // deplie dans la liste
    bool watched = true;              // re-analyse automatique
    bool missing = false;             // le dossier n'existe plus
    std::vector<GameEntry> games;
};

// Temps de jeu d'un jeu (fichier ndsemu_temps_de_jeu.ini, ecrit par la fenetre de jeu)
struct PlayStat { int64_t last = 0, seconds = 0; };      // derniere partie (secondes depuis 1970), duree totale

class Library {
public:
    std::vector<GameFolder> folders;              // dans l'ordre d'ajout ; le groupe "fichiers ajoutes" a un chemin vide
    std::map<std::string, Compat> compat;         // code du jeu -> compatibilite
    std::string configPath;                       // fichier de reglages (a cote du programme)

    // Reglages de l'interface et du lancement, gardes dans le fichier
    bool gridView = false;
    int sortColumn = 0;                           // 0 nom, 1 compatibilite, 2 extras, 3 type, 4 taille
    bool sortAscending = true;
    bool sound = true;                            // sinon --sans-son
    bool report = false;                          // --rapport (rapport ecrit dans diag.txt)
    bool showRecent = true;                       // section "Recemment joues" en haut de la liste
    bool checkUpdates = true;                     // rechercher une nouvelle version au demarrage
    std::string skipVersion;                      // version que l'utilisateur a choisi d'ignorer

    void load();
    void save() const;

    bool addFolder(const std::string& path);      // false si deja present
    void addFile(const std::string& path);        // .nds ajoute seul (groupe "fichiers ajoutes")
    void removeFolder(size_t index);
    void rescan(size_t index);                    // relit le dossier (les bannieres deja lues sont gardees)
    void rescanAll();
    // Appelee souvent : re-analyse les dossiers surveilles toutes les quelques secondes. Renvoie true si quelque
    // chose a change (l'interface recree alors les textures des nouvelles icones).
    bool poll(double nowSeconds);

    Compat compatOf(const GameEntry& g) const;
    void setCompat(const GameEntry& g, Compat c);

    // Lit l'en-tete et la banniere d'un fichier .nds. false si ce n'est pas une ROM DS lisible.
    static bool readRomInfo(const std::string& path, GameEntry& out);

    // Temps de jeu : cle "CODE-CRC" (le meme jeu garde son temps s'il change de dossier ou de nom)
    std::map<std::string, PlayStat> play;
    const PlayStat* playOf(const GameEntry& g) const;
    void loadPlayStats();
    static std::string gameKey(const std::string& code, uint16_t headerCrc);
    // Ajoute du temps de jeu (appele par la fenetre de jeu, toutes les minutes et a la fermeture)
    static bool addPlayTime(const std::string& statsFile, const std::string& key, int64_t seconds, int64_t when);
    std::string playStatsPath() const;

private:
    double lastPoll_ = -1e9;
    int64_t playStamp_ = -1;
    void scanInto(GameFolder& f);
    int countStates(const std::string& romPath) const;
};

// "64,0 Mo", "512 Ko"...
std::string formatSize(uint64_t bytes);
