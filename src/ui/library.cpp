#include "ui/library.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

fs::path toPath(const std::string& utf8) { return fs::u8path(utf8); }
std::string toUtf8(const fs::path& p) { return p.u8string(); }

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool isNds(const fs::path& p) { return lower(toUtf8(p.extension())) == ".nds"; }

// UTF-16 (petit-boutiste) -> UTF-8, jusqu'au premier 0
std::string utf16ToUtf8(const uint8_t* p, size_t maxChars) {
    std::string out;
    for (size_t i = 0; i < maxChars; i++) {
        uint32_t c = uint32_t(p[i * 2] | (p[i * 2 + 1] << 8));
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < maxChars) {       // paire de substitution
            const uint32_t lo = uint32_t(p[i * 2 + 2] | (p[i * 2 + 3] << 8));
            if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
        else {
            out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63));
            out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63));
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\r' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\r' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

int64_t fileTime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : int64_t(t.time_since_epoch().count());
}

bool hasSaveFile(const fs::path& rom) {
    std::error_code ec;
    fs::path sav = rom;
    sav.replace_extension(".sav");
    return fs::exists(sav, ec);
}

// Compatibilite deja verifiee avec ndsemu (3 premieres lettres du code : toutes les regions)
Compat defaultCompat(const std::string& code) {
    const std::string c3 = code.substr(0, 3);
    if (c3 == "AMC") return Compat::Playable;      // Mario Kart DS
    if (c3 == "A2D") return Compat::Playable;      // New Super Mario Bros.
    return Compat::Unknown;
}

}  // namespace

const char* compatLabel(Compat c) {
    switch (c) {
        case Compat::Perfect:  return "Parfait";
        case Compat::Playable: return "Jouable";
        case Compat::InGame:   return "En jeu";
        case Compat::Menus:    return "Menus";
        case Compat::Broken:   return "Ne d\xC3\xA9marre pas";
        default:               return "Non test\xC3\xA9";
    }
}
int compatCount() { return 6; }

std::string formatSize(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f Go", double(bytes) / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024) std::snprintf(buf, sizeof buf, "%.1f Mo", double(bytes) / (1024.0 * 1024));
    else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%llu Ko", (unsigned long long)(bytes / 1024));
    else std::snprintf(buf, sizeof buf, "%llu o", (unsigned long long)bytes);
    std::string s = buf;
    for (char& c : s) if (c == '.') c = ',';                    // virgule decimale
    return s;
}

// ---- lecture d'une ROM : en-tete (0x200 octets) puis banniere (icone + titres) ---------------------------
bool Library::readRomInfo(const std::string& path, GameEntry& g) {
    const fs::path p = toPath(path);
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    uint8_t hdr[0x200] = {};
    in.read(reinterpret_cast<char*>(hdr), sizeof hdr);
    if (in.gcount() < 0x170) return false;

    g.path = path;
    g.fileName = toUtf8(p.filename());
    g.gameCode.assign(reinterpret_cast<const char*>(hdr + 0x0C), 4);
    g.headerCrc = uint16_t(hdr[0x15E] | (hdr[0x15F] << 8));
    for (char& c : g.gameCode) if (c < 0x20 || c > 0x7E) c = '?';
    std::error_code ec;
    g.size = uint64_t(fs::file_size(p, ec));
    g.mtime = fileTime(p);
    g.hasSave = hasSaveFile(p);
    g.hasIcon = false;
    g.title.clear();
    g.subtitle.clear();

    const uint32_t bannerOfs = uint32_t(hdr[0x68] | (hdr[0x69] << 8) | (hdr[0x6A] << 16) | (uint32_t(hdr[0x6B]) << 24));
    if (bannerOfs >= 0x200 && bannerOfs < g.size) {
        uint8_t ban[0x840] = {};
        in.clear();
        in.seekg(std::streamoff(bannerOfs));
        in.read(reinterpret_cast<char*>(ban), sizeof ban);
        if (in.gcount() >= 0x540) {
            // Icone : 4 x 4 tuiles de 8 x 8 points (4 bits), palette de 16 couleurs, la couleur 0 est transparente
            uint32_t pal[16];
            for (int i = 0; i < 16; i++) {
                const uint16_t c = uint16_t(ban[0x220 + i * 2] | (ban[0x221 + i * 2] << 8));
                const uint32_t r = c & 31, gg = (c >> 5) & 31, b = (c >> 10) & 31;
                pal[i] = 0xFF000000u | (((b << 3) | (b >> 2)) << 16) | (((gg << 3) | (gg >> 2)) << 8) | ((r << 3) | (r >> 2));
            }
            pal[0] = 0;
            for (int ty = 0; ty < 4; ty++)
                for (int tx = 0; tx < 4; tx++)
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++) {
                            const uint8_t b = ban[0x20 + (ty * 4 + tx) * 32 + y * 4 + x / 2];
                            const int idx = (x & 1) ? (b >> 4) : (b & 15);
                            g.icon[(ty * 8 + y) * 32 + tx * 8 + x] = pal[idx];
                        }
            g.hasIcon = true;
            // Titres : japonais, anglais, francais, allemand... (128 caracteres UTF-16 chacun). On prend le francais.
            std::string t = utf16ToUtf8(ban + 0x240 + 2 * 0x100, 128);
            if (trim(t).empty()) t = utf16ToUtf8(ban + 0x240 + 1 * 0x100, 128);
            // 2 ou 3 lignes : titre (sur 1 ou 2 lignes) puis editeur sur la derniere
            std::vector<std::string> lines;
            std::istringstream ls(t);
            for (std::string l; std::getline(ls, l);) if (!trim(l).empty()) lines.push_back(trim(l));
            if (lines.size() >= 2) { g.subtitle = lines.back(); lines.pop_back(); }
            for (const std::string& l : lines) g.title += (g.title.empty() ? "" : " ") + l;
        }
    }
    if (g.title.empty()) {                                         // pas de banniere : titre de l'en-tete, ou nom du fichier
        std::string t(reinterpret_cast<const char*>(hdr), 12);
        t = trim(t.substr(0, t.find('\0')));
        g.title = t.empty() ? toUtf8(p.stem()) : t;
    }
    return true;
}

// ---- dossiers ---------------------------------------------------------------------------------------------
void Library::scanInto(GameFolder& f) {
    std::map<std::string, GameEntry> old;                          // deja lus : on garde la banniere si rien n'a change
    for (GameEntry& g : f.games) old[g.path] = g;
    std::vector<std::string> files;

    if (f.path.empty()) {                                          // fichiers ajoutes un par un
        for (const auto& kv : old) files.push_back(kv.first);
    } else {
        std::error_code ec;
        const fs::path dir = toPath(f.path);
        f.missing = !fs::is_directory(dir, ec);
        if (!f.missing) {
            fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
                std::error_code e2;
                if (it->is_regular_file(e2) && isNds(it->path())) files.push_back(toUtf8(it->path()));
            }
        }
    }

    std::vector<GameEntry> fresh;
    for (const std::string& path : files) {
        const fs::path p = toPath(path);
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) continue;
        auto o = old.find(path);
        if (o != old.end() && o->second.mtime == fileTime(p) && o->second.size == uint64_t(fs::file_size(p, ec))) {
            o->second.hasSave = hasSaveFile(p);
            o->second.states = countStates(path);
            fresh.push_back(o->second);
            continue;
        }
        GameEntry g;
        if (readRomInfo(path, g)) { g.states = countStates(path); fresh.push_back(g); }
    }
    f.games.swap(fresh);
}

int Library::countStates(const std::string& romPath) const {
    if (configPath.empty()) return 0;
    const fs::path dir = toPath(configPath).parent_path() / "etats";
    const fs::path stem = toPath(romPath).stem();
    int n = 0;
    std::error_code ec;
    for (int i = 1; i <= 4; i++) if (fs::exists(dir / fs::path(stem).concat(".etat" + std::to_string(i)), ec)) n++;
    return n;
}

bool Library::addFolder(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    const std::string norm = toUtf8(fs::weakly_canonical(toPath(path), ec));
    const std::string key = ec ? path : norm;
    for (const GameFolder& f : folders) if (!f.path.empty() && lower(f.path) == lower(key)) return false;
    GameFolder f;
    f.path = key;
    scanInto(f);
    // le groupe des fichiers seuls reste en dernier
    auto pos = std::find_if(folders.begin(), folders.end(), [](const GameFolder& g) { return g.path.empty(); });
    folders.insert(pos, std::move(f));
    save();
    return true;
}

void Library::addFile(const std::string& path) {
    GameEntry g;
    if (!readRomInfo(path, g)) return;
    for (const GameFolder& f : folders)                            // deja dans un dossier de la bibliotheque
        for (const GameEntry& e : f.games) if (lower(e.path) == lower(path)) return;
    auto it = std::find_if(folders.begin(), folders.end(), [](const GameFolder& f) { return f.path.empty(); });
    if (it == folders.end()) { folders.push_back(GameFolder{}); it = folders.end() - 1; it->watched = false; }
    it->games.push_back(g);
    save();
}

void Library::removeFolder(size_t index) {
    if (index >= folders.size()) return;
    folders.erase(folders.begin() + std::ptrdiff_t(index));
    save();
}

void Library::rescan(size_t index) { if (index < folders.size()) scanInto(folders[index]); }
void Library::rescanAll() { for (GameFolder& f : folders) scanInto(f); }

bool Library::poll(double now) {
    if (now - lastPoll_ < 3.0) return false;
    lastPoll_ = now;
    bool changed = false;
    if (fileTime(toPath(playStatsPath())) != playStamp_) { loadPlayStats(); changed = true; }
    for (GameFolder& f : folders) {
        if (!f.watched || f.path.empty()) continue;
        std::vector<std::pair<std::string, int64_t>> before;
        for (const GameEntry& g : f.games) before.push_back({ g.path, g.mtime + (g.hasSave ? 1 : 0) + g.states * 2 });
        const bool wasMissing = f.missing;
        scanInto(f);
        std::vector<std::pair<std::string, int64_t>> after;
        for (const GameEntry& g : f.games) after.push_back({ g.path, g.mtime + (g.hasSave ? 1 : 0) + g.states * 2 });
        if (before != after || wasMissing != f.missing) changed = true;
    }
    return changed;
}

// ---- temps de jeu -----------------------------------------------------------------------------------------
std::string Library::gameKey(const std::string& code, uint16_t crc) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "-%04X", crc);
    return code + buf;
}

std::string Library::playStatsPath() const {
    if (configPath.empty()) return "ndsemu_temps_de_jeu.ini";
    return toUtf8(toPath(configPath).parent_path() / "ndsemu_temps_de_jeu.ini");
}

const PlayStat* Library::playOf(const GameEntry& g) const {
    auto it = play.find(gameKey(g.gameCode, g.headerCrc));
    return it != play.end() ? &it->second : nullptr;
}

namespace {
// lignes "CODE-CRC=derniere duree"
std::map<std::string, PlayStat> readPlay(const fs::path& f) {
    std::map<std::string, PlayStat> m;
    std::ifstream in(f);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        PlayStat p;
        std::istringstream ss(line.substr(eq + 1));
        long long a = 0, b = 0;
        ss >> a >> b;
        p.last = a; p.seconds = b;
        m[line.substr(0, eq)] = p;
    }
    return m;
}
}

void Library::loadPlayStats() {
    const fs::path f = toPath(playStatsPath());
    playStamp_ = fileTime(f);
    play = readPlay(f);
}

bool Library::addPlayTime(const std::string& statsFile, const std::string& key, int64_t seconds, int64_t when) {
    const fs::path f = toPath(statsFile);
    std::map<std::string, PlayStat> m = readPlay(f);           // relu juste avant : un autre jeu ouvert a pu ecrire
    PlayStat& p = m[key];
    p.seconds += seconds;
    p.last = when;
    const fs::path tmp = fs::path(f).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << "# Temps de jeu de ndsemu (ecrit automatiquement) : code-crc=derniere_partie secondes\n";
        for (const auto& kv : m) out << kv.first << "=" << kv.second.last << " " << kv.second.seconds << "\n";
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, f, ec);
    if (ec) { fs::remove(f, ec); fs::rename(tmp, f, ec); }
    return !ec;
}

Compat Library::compatOf(const GameEntry& g) const {
    auto it = compat.find(g.gameCode);
    return it != compat.end() ? it->second : defaultCompat(g.gameCode);
}

void Library::setCompat(const GameEntry& g, Compat c) {
    compat[g.gameCode] = c;
    save();
}

// ---- reglages : fichier texte "cle=valeur", une ligne par dossier ----------------------------------------
void Library::load() {
    loadPlayStats();
    std::ifstream in(toPath(configPath));
    if (!in) return;
    std::string line;
    std::vector<std::string> looseFiles;
    while (std::getline(in, line)) {
        line = trim(line);
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "vue") gridView = (v == "grille");
        else if (k == "tri") { std::istringstream ss(v); int a = 0, b = 1; ss >> a >> b; sortColumn = std::clamp(a, 0, 4); sortAscending = b != 0; }
        else if (k == "son") sound = v != "0";
        else if (k == "rapport") report = v == "1";
        else if (k == "recents") showRecent = v != "0";
        else if (k == "mises_a_jour") checkUpdates = v != "0";
        else if (k == "version_ignoree") skipVersion = v;
        else if (k == "dossier" && v.size() > 4) {                 // ouvert|surveille|chemin
            GameFolder f;
            f.open = v[0] != '0';
            f.watched = v[2] != '0';
            f.path = v.substr(4);
            folders.push_back(std::move(f));
        }
        else if (k == "fichier") looseFiles.push_back(v);
        else if (k == "compat") {
            std::istringstream ss(v); std::string code; int c = 0; ss >> code >> c;
            if (code.size() == 4 && c >= 0 && c < compatCount()) compat[code] = Compat(c);
        }
    }
    rescanAll();
    for (const std::string& f : looseFiles) addFile(f);
}

void Library::save() const {
    if (configPath.empty()) return;
    std::ofstream out(toPath(configPath), std::ios::trunc);
    if (!out) return;
    out << "# Bibliotheque de ndsemu (ecrit automatiquement)\n";
    out << "vue=" << (gridView ? "grille" : "liste") << "\n";
    out << "tri=" << sortColumn << " " << (sortAscending ? 1 : 0) << "\n";
    out << "son=" << (sound ? 1 : 0) << "\n";
    out << "rapport=" << (report ? 1 : 0) << "\n";
    out << "recents=" << (showRecent ? 1 : 0) << "\n";
    out << "mises_a_jour=" << (checkUpdates ? 1 : 0) << "\n";
    if (!skipVersion.empty()) out << "version_ignoree=" << skipVersion << "\n";
    for (const GameFolder& f : folders) {
        if (f.path.empty()) { for (const GameEntry& g : f.games) out << "fichier=" << g.path << "\n"; continue; }
        out << "dossier=" << (f.open ? 1 : 0) << "|" << (f.watched ? 1 : 0) << "|" << f.path << "\n";
    }
    for (const auto& kv : compat) out << "compat=" << kv.first << " " << int(kv.second) << "\n";
}
