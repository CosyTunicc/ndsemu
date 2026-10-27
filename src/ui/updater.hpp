#pragma once
// Mises a jour : ndsemu demande a GitHub la derniere version publiee (GitHub Releases), la compare a la sienne,
// puis, si l'utilisateur accepte, telecharge l'archive .zip de la version, remplace ses fichiers et se relance.
// Cette partie ne depend d'aucun systeme ; le telechargement et l'installation sont faits par la classe derivee
// (updater_win32.cpp).
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Version du programme et depot GitHub (definis par CMakeLists.txt)
#ifndef NDSEMU_VERSION
#define NDSEMU_VERSION "0.0.0"
#endif
#ifndef NDSEMU_GITHUB_REPO
#define NDSEMU_GITHUB_REPO "CosyTunicc/ndsemu"
#endif

struct ReleaseInfo {
    std::string version;        // "1.2.0" (etiquette sans le "v")
    std::string tag;            // "v1.2.0"
    std::string name;           // titre de la version
    std::string notes;          // nouveautes (texte de la version sur GitHub, en Markdown)
    std::string page;           // page de la version
    std::string zipName, zipUrl;
    uint64_t zipSize = 0;
};

// <0 si a est plus ancienne que b, 0 si egales, >0 si plus recente ("v1.10" > "1.9", "1.2" == "1.2.0")
int compareVersions(const std::string& a, const std::string& b);
// Lit la reponse de l'API GitHub (/releases/latest) ; choisit l'archive .zip pour Windows (pas celle de la PS5)
bool parseRelease(const std::string& json, ReleaseInfo& out, std::string& error);
// Nouveautes : le Markdown de GitHub rendu en texte simple (titres, listes, gras)
std::string notesToText(const std::string& markdown);

class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Installing, Installed, Failed };

    Updater(std::string currentVersion, std::string repo);
    virtual ~Updater();

    void check(bool manual);           // en arriere-plan ; manual : l'utilisateur l'a demande (on affiche aussi "a jour")
    void install();                    // telecharge et installe la version trouvee, en arriere-plan
    virtual void restart() = 0;        // relance le programme (apres l'installation)
    virtual void openPage(const std::string& url) = 0;

    State state() const { return state_.load(); }
    bool manual() const { return manual_.load(); }
    float progress() const { return progress_.load(); }          // 0..1 pendant le telechargement
    std::string error() const;
    ReleaseInfo release() const;
    const std::string& currentVersion() const { return current_; }
    const std::string& repo() const { return repo_; }

protected:
    // Telecharge une adresse https ; onProgress(recu, total) (total = 0 si inconnu). false + error si echec.
    virtual bool httpGet(const std::string& url, std::string& body, std::string& error,
                         const std::function<void(uint64_t, uint64_t)>& onProgress) = 0;
    // Installe les fichiers de l'archive telechargee a la place de ceux du programme
    virtual bool installZip(const std::string& zipData, std::string& error) = 0;
    void stopWorker();                 // a appeler dans le destructeur de la classe derivee
    std::atomic<bool> cancel_{ false };

private:
    void run(bool doInstall);
    void fail(const std::string& e);
    std::string current_, repo_;
    std::atomic<State> state_{ State::Idle };
    std::atomic<bool> manual_{ false };
    std::atomic<float> progress_{ 0.0f };
    mutable std::mutex mutex_;
    std::string error_;
    ReleaseInfo release_;
    std::thread worker_;
};

#ifdef _WIN32
#include <memory>
// Windows : telechargement par WinHTTP, archive ouverte par tar.exe (fourni avec Windows 10 et 11), fichiers
// remplaces a cote de l'exe (l'ancien exe, encore ouvert, est renomme en .old puis efface au lancement suivant).
std::unique_ptr<Updater> createWin32Updater(void* hwnd);
void removeOldUpdateFiles();          // efface les .old laisses par une mise a jour (au demarrage de la bibliotheque)
void relaunchAfterUpdate();           // relance l'exe si "Redemarrer" a ete demande (a la fermeture de la bibliotheque)
#endif
