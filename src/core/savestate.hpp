#pragma once
// Sauvegardes d'etat : tout l'etat de la console (memoire, registres, processeurs, 2D, 3D, son, horloge...) dans un
// fichier, avec une miniature de l'ecran. Une sauvegarde ne se recharge que pour le meme jeu.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

struct System;
class Bus9;
class ArmCpu;

// Ecriture : tout est copie octet par octet (les tailles des tableaux variables sont notees avant).
class StateWriter {
public:
    static constexpr bool loading = false;
    std::vector<uint8_t> data;
    template <class T> void io(T& v) {
        static_assert(std::is_trivially_copyable<T>::value, "type non copiable");
        raw(&v, sizeof v);
    }
    template <class T> void io(std::vector<T>& v) {
        static_assert(std::is_trivially_copyable<T>::value, "type non copiable");
        uint64_t n = v.size();
        raw(&n, sizeof n);
        if (n) raw(v.data(), size_t(n) * sizeof(T));
    }
    void raw(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + n);
    }
};

// Lecture : "dry" = verification seulement (on parcourt le fichier sans rien modifier). On ne recharge que si la
// verification a consomme exactement tout le fichier : un fichier abime ou d'une autre version ne casse rien.
class StateReader {
public:
    static constexpr bool loading = true;
    StateReader(const uint8_t* p, size_t n, bool dryRun) : p_(p), n_(n), dry_(dryRun) {}
    bool ok() const { return ok_; }
    bool finished() const { return ok_ && pos_ == n_; }
    template <class T> void io(T& v) {
        static_assert(std::is_trivially_copyable<T>::value, "type non copiable");
        if (!take(sizeof v)) return;
        if (!dry_) std::memcpy(&v, p_ + pos_ - sizeof v, sizeof v);
    }
    template <class T> void io(std::vector<T>& v) {
        static_assert(std::is_trivially_copyable<T>::value, "type non copiable");
        uint64_t n = 0;
        if (!take(sizeof n)) return;
        std::memcpy(&n, p_ + pos_ - sizeof n, sizeof n);
        if (n > (n_ - pos_) / (sizeof(T) ? sizeof(T) : 1)) { ok_ = false; return; }
        const size_t bytes = size_t(n) * sizeof(T);
        if (!take(bytes)) return;
        if (!dry_) {
            v.resize(size_t(n));                       // meme taille : la memoire ne bouge pas (pointeurs gardes)
            if (bytes) std::memcpy(v.data(), p_ + pos_ - bytes, bytes);
        }
    }
private:
    bool take(size_t k) {
        if (!ok_ || n_ - pos_ < k) { ok_ = false; return false; }
        pos_ += k;
        return true;
    }
    const uint8_t* p_;
    size_t n_, pos_ = 0;
    bool dry_, ok_ = true;
};

struct SaveStateInfo {
    bool exists = false;
    int64_t time = 0;                         // date de la sauvegarde (secondes depuis 1970)
    std::vector<uint32_t> thumb;              // image des deux ecrans, 256 x 384, 0x00RRGGBB
};

struct SaveStateTarget {                      // ce qui est sauvegarde
    System* sys = nullptr;
    Bus9* bus9 = nullptr;
    ArmCpu* cpu9 = nullptr;
    ArmCpu* cpu7 = nullptr;
    std::string gameCode;                     // une sauvegarde ne se recharge que pour le meme jeu
    uint16_t headerCrc = 0;
};

bool saveStateFile(const std::filesystem::path& file, const SaveStateTarget& t, const std::vector<uint32_t>& screen,
                   std::string& error);
bool loadStateFile(const std::filesystem::path& file, const SaveStateTarget& t, std::string& error);
bool readStateInfo(const std::filesystem::path& file, SaveStateInfo& info);
