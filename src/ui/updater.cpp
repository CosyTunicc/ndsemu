#include "ui/updater.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <vector>

// ---- versions ------------------------------------------------------------------------------------------------
namespace {
std::vector<long> versionParts(const std::string& v) {
    std::vector<long> parts;
    size_t i = 0;
    while (i < v.size() && !std::isdigit(static_cast<unsigned char>(v[i]))) i++;     // "v1.2" -> "1.2"
    while (i < v.size()) {
        if (!std::isdigit(static_cast<unsigned char>(v[i]))) break;                   // "1.2-beta" : on s'arrete
        long n = 0;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) n = n * 10 + (v[i++] - '0');
        parts.push_back(n);
        if (i < v.size() && v[i] == '.') i++; else break;
    }
    while (!parts.empty() && parts.back() == 0) parts.pop_back();
    return parts;
}
}

int compareVersions(const std::string& a, const std::string& b) {
    const std::vector<long> pa = versionParts(a), pb = versionParts(b);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); i++) {
        const long x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// ---- JSON (juste ce qu'il faut pour la reponse de GitHub) ------------------------------------------------------
namespace {
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;
    const Json* get(const char* key) const {
        for (const auto& f : fields) if (f.first == key) return &f.second;
        return nullptr;
    }
    std::string str(const char* key) const { const Json* j = get(key); return j && j->type == String ? j->s : std::string(); }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& t) : t_(t) {}
    bool parse(Json& out) { skip(); if (!value(out, 0)) return false; skip(); return p_ == t_.size(); }
private:
    const std::string& t_;
    size_t p_ = 0;
    void skip() { while (p_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[p_]))) p_++; }
    bool lit(const char* w) {
        const size_t n = std::char_traits<char>::length(w);
        if (t_.compare(p_, n, w) != 0) return false;
        p_ += n;
        return true;
    }
    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += char(c);
        else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
        else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 63)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
    }
    bool hex4(uint32_t& v) {
        if (p_ + 4 > t_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        if (p_ >= t_.size() || t_[p_] != '"') return false;
        p_++;
        while (p_ < t_.size()) {
            const char c = t_[p_++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (p_ >= t_.size()) return false;
            const char e = t_[p_++];
            switch (e) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u': {
                uint32_t c1 = 0;
                if (!hex4(c1)) return false;
                if (c1 >= 0xD800 && c1 < 0xDC00 && p_ + 6 <= t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
                    p_ += 2;
                    uint32_t c2 = 0;
                    if (!hex4(c2)) return false;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                }
                utf8(o, c1);
                break;
            }
            default: o += e; break;                                         // \" \\ \/
            }
        }
        return false;
    }
    bool value(Json& v, int depth) {
        if (depth > 64 || p_ >= t_.size()) return false;
        const char c = t_[p_];
        if (c == '"') { v.type = Json::String; return string(v.s); }
        if (c == '{') {
            v.type = Json::Object;
            p_++; skip();
            if (p_ < t_.size() && t_[p_] == '}') { p_++; return true; }
            for (;;) {
                skip();
                std::pair<std::string, Json> f;
                if (!string(f.first)) return false;
                skip();
                if (p_ >= t_.size() || t_[p_++] != ':') return false;
                skip();
                if (!value(f.second, depth + 1)) return false;
                v.fields.push_back(std::move(f));
                skip();
                if (p_ >= t_.size()) return false;
                if (t_[p_] == ',') { p_++; continue; }
                if (t_[p_] == '}') { p_++; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = Json::Array;
            p_++; skip();
            if (p_ < t_.size() && t_[p_] == ']') { p_++; return true; }
            for (;;) {
                skip();
                Json item;
                if (!value(item, depth + 1)) return false;
                v.items.push_back(std::move(item));
                skip();
                if (p_ >= t_.size()) return false;
                if (t_[p_] == ',') { p_++; continue; }
                if (t_[p_] == ']') { p_++; return true; }
                return false;
            }
        }
        if (lit("true")) { v.type = Json::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Json::Bool; return true; }
        if (lit("null")) return true;
        const char* start = t_.c_str() + p_;
        char* end = nullptr;
        v.n = std::strtod(start, &end);
        if (end == start) return false;
        v.type = Json::Number;
        p_ += size_t(end - start);
        return true;
    }
};

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

bool parseRelease(const std::string& json, ReleaseInfo& out, std::string& error) {
    Json root;
    if (!JsonParser(json).parse(root) || root.type != Json::Object) { error = "r\xC3\xA9ponse de GitHub illisible"; return false; }
    out = ReleaseInfo();
    out.tag = root.str("tag_name");
    if (out.tag.empty()) {
        const std::string msg = root.str("message");
        error = msg.empty() ? "aucune version trouv\xC3\xA9" "e" : "GitHub : " + msg;
        return false;
    }
    out.version = out.tag;
    while (!out.version.empty() && !std::isdigit(static_cast<unsigned char>(out.version[0]))) out.version.erase(0, 1);
    out.name = root.str("name");
    out.notes = root.str("body");
    out.page = root.str("html_url");
    // archive : un .zip qui n'est pas celui de la PS5 ; on prefere un nom qui contient "windows" ou "win"
    int best = -1;
    if (const Json* assets = root.get("assets"); assets && assets->type == Json::Array) {
        for (const Json& a : assets->items) {
            const std::string name = a.str("name"), low = lower(name);
            if (low.size() < 4 || low.compare(low.size() - 4, 4, ".zip") != 0 || low.find("ps5") != std::string::npos) continue;
            const int score = low.find("win") != std::string::npos ? 2 : 1;
            if (score <= best) continue;
            best = score;
            out.zipName = name;
            out.zipUrl = a.str("browser_download_url");
            const Json* sz = a.get("size");
            out.zipSize = sz && sz->type == Json::Number ? uint64_t(sz->n) : 0;
        }
    }
    return true;
}

std::string notesToText(const std::string& md) {
    std::string out, line;
    size_t i = 0;
    while (i <= md.size()) {
        const size_t e = md.find('\n', i);
        line = md.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? md.size() + 1 : e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t k = 0;
        while (k < line.size() && line[k] == ' ') k++;
        std::string indent(k, ' ');
        std::string rest = line.substr(k);
        if (!rest.empty() && rest[0] == '#') {                            // titre
            size_t h = 0;
            while (h < rest.size() && rest[h] == '#') h++;
            while (h < rest.size() && rest[h] == ' ') h++;
            rest = rest.substr(h);
        } else if (rest.size() >= 2 && (rest[0] == '-' || rest[0] == '*' || rest[0] == '+') && rest[1] == ' ') {
            rest = "\xE2\x80\xA2 " + rest.substr(2);                       // puce
        }
        std::string clean;                                                // **gras**, `code`
        for (size_t c = 0; c < rest.size(); c++) {
            if (rest[c] == '`') continue;
            if (rest[c] == '*' && c + 1 < rest.size() && rest[c + 1] == '*') { c++; continue; }
            clean += rest[c];
        }
        out += indent + clean + "\n";
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// ---- Updater ---------------------------------------------------------------------------------------------------
Updater::Updater(std::string currentVersion, std::string repo) : current_(std::move(currentVersion)), repo_(std::move(repo)) {}

Updater::~Updater() { stopWorker(); }

void Updater::stopWorker() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

std::string Updater::error() const { std::lock_guard<std::mutex> l(mutex_); return error_; }
ReleaseInfo Updater::release() const { std::lock_guard<std::mutex> l(mutex_); return release_; }

void Updater::fail(const std::string& e) {
    { std::lock_guard<std::mutex> l(mutex_); error_ = e; }
    state_ = State::Failed;
}

void Updater::check(bool manual) {
    const State s = state_.load();
    if (s == State::Checking || s == State::Downloading || s == State::Installing || s == State::Installed) return;
    if (worker_.joinable()) worker_.join();
    manual_ = manual;
    state_ = State::Checking;
    worker_ = std::thread([this] { run(false); });
}

void Updater::install() {
    if (state_.load() != State::Available) return;
    if (worker_.joinable()) worker_.join();
    progress_ = 0;
    state_ = State::Downloading;
    worker_ = std::thread([this] { run(true); });
}

void Updater::run(bool doInstall) {
    if (!doInstall) {
        std::string body, err;
        if (!httpGet("https://api.github.com/repos/" + repo_ + "/releases/latest", body, err, nullptr)) { fail(err); return; }
        ReleaseInfo r;
        if (!parseRelease(body, r, err)) { fail(err); return; }
        { std::lock_guard<std::mutex> l(mutex_); release_ = r; error_.clear(); }
        state_ = compareVersions(r.version, current_) > 0 ? State::Available : State::UpToDate;
        return;
    }
    const ReleaseInfo r = release();
    if (r.zipUrl.empty()) { fail("la version " + r.version + " n'a pas d'archive .zip pour Windows"); return; }
    std::string zip, err;
    const bool ok = httpGet(r.zipUrl, zip, err, [this, &r](uint64_t got, uint64_t total) {
        if (!total) total = r.zipSize;
        if (total) progress_ = std::min(1.0f, float(double(got) / double(total)));
    });
    if (!ok) { fail(err); return; }
    if (r.zipSize && zip.size() != r.zipSize) { fail("t\xC3\xA9l\xC3\xA9" "chargement incomplet"); return; }
    progress_ = 1.0f;
    state_ = State::Installing;
    if (!installZip(zip, err)) { fail(err); return; }
    state_ = State::Installed;
}
