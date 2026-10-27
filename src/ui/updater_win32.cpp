// Mises a jour sous Windows : WinHTTP pour GitHub, tar.exe (ou PowerShell) pour ouvrir l'archive, puis
// remplacement des fichiers a cote de l'exe. Un exe en cours d'execution ne peut pas etre ecrase, mais il peut
// etre renomme : l'ancien devient ndsemu.exe.old (efface au lancement suivant) et le nouveau prend sa place.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <vector>
#include "ui/updater.hpp"

namespace fs = std::filesystem;

namespace {

std::atomic<bool> gRelaunch{ false };

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

fs::path exeFile() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return fs::path(std::wstring(buf, n));
}

std::wstring lowerW(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(std::towlower(c));
    return s;
}

// Lance une commande sans fenetre et attend sa fin ; renvoie son code de sortie (-1 si elle n'a pas pu demarrer)
int runHidden(const std::wstring& cmd, DWORD timeoutMs) {
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    CloseHandle(pi.hThread);
    DWORD code = DWORD(-1);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    return int(code);
}

// Fichiers de l'utilisateur, jamais remplaces meme s'ils se trouvaient dans l'archive
bool isUserFile(const fs::path& rel) {
    const std::wstring name = lowerW(rel.filename().wstring()), ext = lowerW(rel.extension().wstring());
    if (ext == L".ini" || ext == L".sav" || ext == L".nds" || name == L"firmware.bin") return true;
    for (const fs::path& part : rel) if (lowerW(part.wstring()) == L"etats") return true;
    return false;
}

class Win32Updater : public Updater {
public:
    explicit Win32Updater(HWND hwnd) : Updater(NDSEMU_VERSION, NDSEMU_GITHUB_REPO), hwnd_(hwnd) {}
    ~Win32Updater() override { stopWorker(); }

    void restart() override {
        gRelaunch = true;                       // la bibliotheque relance l'exe apres avoir enregistre ses reglages
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
    void openPage(const std::string& url) override {
        if (url.rfind("https://", 0) == 0) ShellExecuteW(hwnd_, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

protected:
    bool httpGet(const std::string& url, std::string& body, std::string& error,
                 const std::function<void(uint64_t, uint64_t)>& onProgress) override {
        body.clear();
        const std::wstring wurl = widen(url);
        wchar_t host[256] = {}, path[4096] = {}, extra[2048] = {};
        URL_COMPONENTS uc = {};
        uc.dwStructSize = sizeof uc;
        uc.lpszHostName = host; uc.dwHostNameLength = DWORD(std::size(host));
        uc.lpszUrlPath = path; uc.dwUrlPathLength = DWORD(std::size(path));
        uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = DWORD(std::size(extra));
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) { error = "adresse invalide"; return false; }
        const std::wstring agent = L"ndsemu/" + widen(NDSEMU_VERSION);
        HINTERNET ses = WinHttpOpen(agent.c_str(), 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (Windows 8.1+) */, nullptr, nullptr, 0);
        if (!ses) ses = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
        if (!ses) { error = "WinHTTP indisponible"; return false; }
        WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
        HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
        HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", (std::wstring(path) + extra).c_str(), nullptr,
                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
        auto close = [&]() { if (req) WinHttpCloseHandle(req); if (con) WinHttpCloseHandle(con); WinHttpCloseHandle(ses); };
        const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        const bool api = std::wstring(host) == L"api.github.com";
        if (!req || !WinHttpSendRequest(req, api ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, api ? DWORD(-1) : 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req, nullptr)) {
            error = "impossible de joindre GitHub : vérifie la connexion à Internet (erreur " + std::to_string(GetLastError()) + ")";
            close();
            return false;
        }
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            if (api && status == 404) error = "aucune version publiée sur GitHub (" + repo() + ")";
            else if (status == 403 || status == 429) error = "GitHub limite les demandes pour l'instant : réessaie dans une heure";
            else error = "GitHub a répondu " + std::to_string(status);
            close();
            return false;
        }
        uint64_t total = 0;
        wchar_t lenBuf[32] = {};
        DWORD lenSize = sizeof lenBuf;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenSize, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(lenBuf, nullptr, 10);
        if (total) body.reserve(size_t(total));
        std::vector<char> buf(64 * 1024);
        for (;;) {
            if (cancel_) { error = "annulé"; close(); return false; }
            DWORD got = 0;
            if (!WinHttpReadData(req, buf.data(), DWORD(buf.size()), &got)) {
                error = "téléchargement interrompu (erreur " + std::to_string(GetLastError()) + ")";
                close();
                return false;
            }
            if (got == 0) break;
            body.append(buf.data(), got);
            if (onProgress) onProgress(body.size(), total);
        }
        close();
        return true;
    }

    bool installZip(const std::string& zipData, std::string& error) override {
        std::error_code ec;
        const fs::path exe = exeFile(), dir = exe.parent_path();
        wchar_t tmpBuf[MAX_PATH + 1] = {};
        GetTempPathW(MAX_PATH, tmpBuf);
        const fs::path work = fs::path(tmpBuf) / L"ndsemu_mise_a_jour";
        fs::remove_all(work, ec);
        fs::create_directories(work / L"contenu", ec);
        const fs::path zip = work / L"ndsemu.zip", out = work / L"contenu";
        {
            std::ofstream f(zip, std::ios::binary | std::ios::trunc);
            f.write(zipData.data(), std::streamsize(zipData.size()));
            if (!f) { error = "impossible d'écrire l'archive dans le dossier temporaire"; return false; }
        }
        // ouverture de l'archive : tar.exe (Windows 10 1803 et plus), sinon PowerShell
        wchar_t sys[MAX_PATH] = {};
        GetSystemDirectoryW(sys, MAX_PATH);
        const fs::path tar = fs::path(sys) / L"tar.exe";
        int code = -1;
        if (fs::exists(tar, ec))
            code = runHidden(L"\"" + tar.wstring() + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + out.wstring() + L"\"", 120000);
        if (code != 0) {
            auto quote = [](std::wstring s) { std::wstring r; for (wchar_t c : s) { r += c; if (c == L'\'') r += c; } return L"'" + r + L"'"; };
            code = runHidden(L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath " +
                             quote(zip.wstring()) + L" -DestinationPath " + quote(out.wstring()) + L" -Force\"", 180000);
        }
        if (code != 0) { error = "impossible d'ouvrir l'archive téléchargée"; fs::remove_all(work, ec); return false; }

        // le dossier de la nouvelle version : celui qui contient ndsemu.exe (a la racine de l'archive ou dans un sous-dossier)
        fs::path root;
        int bestDepth = 99;
        for (fs::recursive_directory_iterator it(out, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && lowerW(it->path().filename().wstring()) == L"ndsemu.exe" && it.depth() < bestDepth) {
                bestDepth = it.depth();
                root = it->path().parent_path();
            }
        if (root.empty()) { error = "l'archive ne contient pas ndsemu.exe"; fs::remove_all(work, ec); return false; }
        {
            std::ifstream f(root / L"ndsemu.exe", std::ios::binary);
            char mz[2] = {};
            f.read(mz, 2);
            if (mz[0] != 'M' || mz[1] != 'Z' || fs::file_size(root / L"ndsemu.exe", ec) < 64 * 1024) {
                error = "le ndsemu.exe de l'archive n'est pas un programme valide";
                fs::remove_all(work, ec);
                return false;
            }
        }

        // remplacement : chaque fichier existant est d'abord renomme en .old (possible meme s'il est ouvert)
        struct Done { fs::path target, old; };
        std::vector<Done> done;
        auto rollback = [&]() {
            for (auto it = done.rbegin(); it != done.rend(); ++it) {
                DeleteFileW(it->target.c_str());
                if (!it->old.empty()) MoveFileExW(it->old.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        };
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const fs::path rel = fs::relative(it->path(), root, ec);
            if (rel.empty() || isUserFile(rel)) continue;
            const fs::path target = dir / rel;
            fs::create_directories(target.parent_path(), ec);
            Done d{ target, {} };
            if (fs::exists(target, ec)) {
                fs::path old = target;
                old += L".old";
                DeleteFileW(old.c_str());
                for (int n = 2; fs::exists(old, ec) && n < 20; n++) { old = target; old += L".old" + std::to_wstring(n); DeleteFileW(old.c_str()); }
                if (!MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    error = "impossible de remplacer " + rel.u8string() + " (erreur " + std::to_string(GetLastError()) + ")";
                    rollback();
                    fs::remove_all(work, ec);
                    return false;
                }
                d.old = old;
            }
            if (!CopyFileW(it->path().c_str(), target.c_str(), FALSE)) {
                const DWORD e = GetLastError();
                if (!d.old.empty()) MoveFileExW(d.old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
                error = "impossible de copier " + rel.u8string() + " (erreur " + std::to_string(e) + ")";
                rollback();
                fs::remove_all(work, ec);
                return false;
            }
            done.push_back(d);
        }
        fs::remove_all(work, ec);
        return true;
    }

private:
    HWND hwnd_;
};

}  // namespace

std::unique_ptr<Updater> createWin32Updater(void* hwnd) { return std::make_unique<Win32Updater>(static_cast<HWND>(hwnd)); }

void removeOldUpdateFiles() {
    std::error_code ec;
    const fs::path dir = exeFile().parent_path();
    std::vector<fs::path> olds;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it.depth() >= 2) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::wstring ext = lowerW(it->path().extension().wstring());
        if (ext.rfind(L".old", 0) != 0) continue;
        fs::path orig = it->path();
        orig.replace_extension();                                     // ndsemu.exe.old -> ndsemu.exe
        if (fs::exists(orig, ec)) olds.push_back(it->path());        // seulement les copies laissees par une mise a jour
    }
    for (const fs::path& p : olds) DeleteFileW(p.c_str());           // echoue sans gravite si un ancien jeu tourne encore
}

void relaunchAfterUpdate() {
    if (!gRelaunch) return;
    const fs::path exe = exeFile();
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
#endif
