#include "host_objects.hpp"

#include <filesystem>

#include "strconv.hpp"

namespace fs = std::filesystem;

namespace nokomo::detail {

///////////////////////////////////////////////////////////////////////////////
// LocalKpiFile
///////////////////////////////////////////////////////////////////////////////

LocalKpiFile* LocalKpiFile::open(const std::wstring& path)
{
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(path.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    if (n == 0 || n >= sizeof(full) / sizeof(full[0])) return nullptr;
    HANDLE h = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    return new LocalKpiFile(h, full);
}

LocalKpiFile::LocalKpiFile(HANDLE h, std::wstring fullPath)
    : h_(h), fullPath_(std::move(fullPath))
{
    fileName_ = fileNameOf(fullPath_);
}

LocalKpiFile::~LocalKpiFile()
{
    if (view_) UnmapViewOfFile((LPVOID)view_);
    if (mapping_) CloseHandle(mapping_);
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    if (!tempCopyPath_.empty()) DeleteFileW(tempCopyPath_.c_str());
    if (!tempCopyDir_.empty()) RemoveDirectoryW(tempCopyDir_.c_str());
}

// 損失なく ANSI へ変換出来る場合のみ true
static bool toAnsiStrict(const std::wstring& s, std::string* out)
{
    BOOL usedDefault = FALSE;
    int need = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s.c_str(), -1,
                                   nullptr, 0, nullptr, &usedDefault);
    if (need <= 0 || usedDefault) return false;
    out->resize((size_t)need);
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s.c_str(), -1,
                        out->data(), need, nullptr, nullptr);
    out->resize(strlen(out->c_str()));
    return true;
}

DWORD WINAPI LocalKpiFile::Read(void* pBuffer, DWORD dwSize)
{
    if (aborted_.load(std::memory_order_relaxed)) return 0;
    DWORD read = 0;
    if (!ReadFile(h_, pBuffer, dwSize, &read, nullptr)) return 0;
    return read;
}

UINT64 WINAPI LocalKpiFile::Seek(INT64 i64Pos, DWORD dwOrigin)
{
    if (aborted_.load(std::memory_order_relaxed)) return KPI_FILE_EOF;
    LARGE_INTEGER pos, out;
    pos.QuadPart = i64Pos;
    // FILE_BEGIN/CURRENT/END は KPI 側と同じ値
    if (!SetFilePointerEx(h_, pos, &out, dwOrigin)) return KPI_FILE_EOF;
    return (UINT64)out.QuadPart;
}

UINT64 WINAPI LocalKpiFile::GetSize(void)
{
    if (aborted_.load(std::memory_order_relaxed)) return KPI_FILE_EOF;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h_, &sz)) return KPI_FILE_EOF;
    return (UINT64)sz.QuadPart;
}

BOOL WINAPI LocalKpiFile::CreateClone(IKpiFile** ppFile)
{
    *ppFile = nullptr;
    if (aborted_.load(std::memory_order_relaxed)) return FALSE;
    LocalKpiFile* clone = open(fullPath_); // クローンのファイルポインタは先頭(仕様)
    if (!clone) return FALSE;
    *ppFile = clone;
    return TRUE;
}

DWORD WINAPI LocalKpiFile::GetFileName(wchar_t* pszName, DWORD dwSize)
{
    return copyStringOut(pszName, dwSize, fileName_);
}

BOOL WINAPI LocalKpiFile::GetRealFileW(const wchar_t** ppszFileNameW)
{
    if (aborted_.load(std::memory_order_relaxed)) { *ppszFileNameW = nullptr; return FALSE; }
    *ppszFileNameW = fullPath_.c_str();
    return TRUE;
}

BOOL WINAPI LocalKpiFile::GetRealFileA(const char** ppszFileNameA)
{
    *ppszFileNameA = nullptr;
    if (aborted_.load(std::memory_order_relaxed)) return FALSE;
    std::lock_guard<std::mutex> lock(m_);
    if (ansiState_ == 0) {
        ansiState_ = 2;
        if (toAnsiStrict(fullPath_, &ansiPath_)) {
            ansiState_ = 1;
        } else {
            // 仕様: ANSI で表現出来ない場合はテンポラリにコピーを作成して
            // コピーのファイル名を返す。
            // GetTempPathA/GetTempFileNameA 経由なのでテンポラリ側は ANSI 安全。
            char tmpDir[MAX_PATH], uniq[MAX_PATH];
            if (GetTempPathA(MAX_PATH, tmpDir) &&
                GetTempFileNameA(tmpDir, "nkm", 0, uniq)) {
                DeleteFileA(uniq); // 予約に使ったファイルをディレクトリに差し替え
                if (CreateDirectoryA(uniq, nullptr)) {
                    // 仕様: フォルダ名だけが非 ANSI の場合、ファイル名部分は
                    // オリジナルと同じにする。ファイル名も非 ANSI なら拡張子だけ
                    // 維持してランダム名(拡張子も非 ANSI なら失敗)。
                    std::string nameA;
                    if (!toAnsiStrict(fileName_, &nameA)) {
                        std::string extA;
                        if (toAnsiStrict(extensionOf(fileName_), &extA)) {
                            nameA = "copy" + extA;
                        }
                    }
                    if (!nameA.empty()) {
                        std::string pathA = std::string(uniq) + "\\" + nameA;
                        std::wstring pathW = fromAnsi(pathA.c_str());
                        if (CopyFileW(fullPath_.c_str(), pathW.c_str(), FALSE)) {
                            tempCopyDir_ = fromAnsi(uniq);
                            tempCopyPath_ = std::move(pathW);
                            ansiPath_ = std::move(pathA);
                            ansiState_ = 1;
                        }
                    }
                    if (ansiState_ != 1) RemoveDirectoryA(uniq);
                }
            }
        }
    }
    if (ansiState_ != 1) return FALSE;
    *ppszFileNameA = ansiPath_.c_str();
    return TRUE;
}

BOOL WINAPI LocalKpiFile::GetBuffer(const BYTE** ppBuffer, size_t* pstSize)
{
    *ppBuffer = nullptr;
    *pstSize = 0;
    if (aborted_.load(std::memory_order_relaxed)) return FALSE;
    std::lock_guard<std::mutex> lock(m_);
    if (!mapTried_) {
        mapTried_ = true;
        UINT64 size = 0;
        LARGE_INTEGER sz;
        if (GetFileSizeEx(h_, &sz)) size = (UINT64)sz.QuadPart;
        if (size > 0 && size <= (UINT64)(size_t)-1) {
            mapping_ = CreateFileMappingW(h_, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (mapping_) {
                view_ = (const BYTE*)MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
                if (view_) {
                    viewSize_ = (size_t)size;
                } else {
                    CloseHandle(mapping_);
                    mapping_ = nullptr;
                }
            }
        }
    }
    if (!view_) return FALSE;
    *ppBuffer = view_;
    *pstSize = viewSize_;
    return TRUE;
}

BOOL WINAPI LocalKpiFile::Abort(void)
{
    aborted_.store(true, std::memory_order_relaxed);
    return TRUE;
}

///////////////////////////////////////////////////////////////////////////////
// DirectoryFolder
///////////////////////////////////////////////////////////////////////////////

DirectoryFolder* DirectoryFolder::open(const std::wstring& dir)
{
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(dir.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    if (n == 0 || n >= sizeof(full) / sizeof(full[0])) return nullptr;
    std::wstring path(full);
    // 末尾セパレータを除去(ドライブルート "C:\" は残す)
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return nullptr;
    return new DirectoryFolder(std::move(path));
}

DirectoryFolder::DirectoryFolder(std::wstring dir) : dir_(std::move(dir)) {}

DWORD WINAPI DirectoryFolder::GetFolderName(wchar_t* pszName, DWORD dwSize)
{
    std::wstring name;
    if (dir_.size() == 3 && dir_[1] == L':') {
        // ドライブルートは ':' を '$' に置換(仕様): "C:\" → "C$"
        name = dir_.substr(0, 2);
        name[1] = L'$';
    } else {
        name = fileNameOf(dir_);
    }
    return copyStringOut(pszName, dwSize, name);
}

static void walkDir(const fs::path& dir, const std::wstring& prefix,
                    DWORD depthLeft, bool unlimited, std::vector<std::wstring>* out)
{
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    for (const auto& e : it) {
        std::error_code ec2;
        std::wstring name = e.path().filename().wstring();
        if (e.is_directory(ec2)) {
            out->push_back(prefix + name + L"/");
            if (unlimited || depthLeft > 0) {
                walkDir(e.path(), prefix + name + L"/",
                        unlimited ? 0 : depthLeft - 1, unlimited, out);
            }
        } else {
            out->push_back(prefix + name);
        }
    }
}

DWORD WINAPI DirectoryFolder::EnumFiles(DWORD dwIndex, wchar_t* pszName, DWORD dwSize, DWORD dwLevel)
{
    std::lock_guard<std::mutex> lock(m_);
    auto it = listCache_.find(dwLevel);
    if (it == listCache_.end()) {
        std::vector<std::wstring> list;
        walkDir(fs::path(dir_), L"", dwLevel, dwLevel == (DWORD)-1, &list);
        it = listCache_.emplace(dwLevel, std::move(list)).first;
    }
    const auto& list = it->second;
    if (dwIndex >= list.size()) {
        if (pszName && dwSize >= sizeof(wchar_t)) pszName[0] = 0;
        return 0;
    }
    return copyStringOut(pszName, dwSize, list[dwIndex]);
}

std::wstring DirectoryFolder::resolve(const std::wstring& name) const
{
    if (name.find(L'>') != std::wstring::npos) return {}; // アーカイブは未対応
    std::wstring n = name;
    for (auto& c : n) if (c == L'/') c = L'\\';
    std::wstring combined;
    bool absolute = (n.size() >= 2 && (n[1] == L':' || (n[0] == L'\\' && n[1] == L'\\')));
    combined = absolute ? n : dir_ + L"\\" + n;
    wchar_t full[MAX_PATH * 4];
    DWORD r = GetFullPathNameW(combined.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    if (r == 0 || r >= sizeof(full) / sizeof(full[0])) return {};
    return full;
}

BOOL WINAPI DirectoryFolder::OpenFile(const wchar_t* cszName, IKpiFile** ppFile)
{
    *ppFile = nullptr;
    if (!cszName || !cszName[0]) return FALSE;
    std::wstring path = resolve(cszName);
    if (path.empty()) return FALSE;
    LocalKpiFile* f = LocalKpiFile::open(path);
    if (!f) return FALSE;
    *ppFile = f;
    return TRUE;
}

BOOL WINAPI DirectoryFolder::OpenFolder(const wchar_t* cszName, IKpiFolder** ppFolder)
{
    *ppFolder = nullptr;
    if (!cszName || !cszName[0]) return FALSE;
    std::wstring n = cszName;
    // 仕様: 末尾は \ / > のいずれかでなければならない
    wchar_t tail = n.back();
    if (tail == L'>') return FALSE; // アーカイブは未対応
    if (tail != L'\\' && tail != L'/') return FALSE;
    n.pop_back();
    if (n.empty()) return FALSE;
    std::wstring path = resolve(n);
    if (path.empty()) return FALSE;
    DirectoryFolder* f = DirectoryFolder::open(path);
    if (!f) return FALSE;
    *ppFolder = f;
    return TRUE;
}

///////////////////////////////////////////////////////////////////////////////
// TagSink
///////////////////////////////////////////////////////////////////////////////

void TagSink::harvest(TrackTags* out) const
{
    out->values = values_;
    out->pictures = pictures_;
}

DWORD WINAPI TagSink::GetTagInfo(IKpiFile*, IKpiFolder*, DWORD dwType, DWORD)
{
    getTagInfoCalled_ = true;
    if (dwType == KPI_TAGTYPE_NONE) return 1; // 仕様: NONE は常に 1
    return (DWORD)-1; // ホスト側タグ解析ルーチンは未実装
}

DWORD WINAPI TagSink::GetValue(const wchar_t* cszName, wchar_t* pszValue, int nSize)
{
    if (!pszValue || nSize < (int)sizeof(wchar_t)) return 0;
    pszValue[0] = 0;
    if (!cszName) return 0;
    auto it = index_.find(toLower(cszName));
    if (it == index_.end()) return 0;
    const std::wstring& v = values_[it->second].second;
    DWORD needed = (DWORD)((v.size() + 1) * sizeof(wchar_t));
    // GetValue はサイズ不足時「切り捨て」(GetXXXName 系の空文字列とは異なる)
    size_t maxChars = (size_t)nSize / sizeof(wchar_t) - 1;
    size_t n = v.size() < maxChars ? v.size() : maxChars;
    memcpy(pszValue, v.c_str(), n * sizeof(wchar_t));
    pszValue[n] = 0;
    return needed;
}

void WINAPI TagSink::SetOverwrite(BOOL bOverwrite) { overwrite_ = bOverwrite != FALSE; }

void WINAPI TagSink::SetPicture(DWORD dwType, const wchar_t* cszType,
                                const wchar_t* cszMIMEType, const wchar_t* cszDescription,
                                DWORD dwWidth, DWORD dwHeight,
                                const BYTE* pPictureData, DWORD dwSize)
{
    if (!pPictureData || dwSize == 0) return;
    TagPicture pic;
    pic.type = dwType;
    pic.typeName = cszType ? cszType : L"";
    pic.mimeType = cszMIMEType ? cszMIMEType : L"";
    pic.description = cszDescription ? cszDescription : L"";
    pic.width = dwWidth;
    pic.height = dwHeight;
    pic.data.assign(pPictureData, pPictureData + dwSize);
    for (auto& p : pictures_) {
        if (p.type == pic.type && p.description == pic.description) {
            if (overwrite_) p = std::move(pic);
            return;
        }
    }
    pictures_.push_back(std::move(pic));
}

void TagSink::set(std::wstring name, std::wstring value)
{
    if (name.empty()) return;
    std::wstring key = toLower(name);
    auto it = index_.find(key);
    if (it != index_.end()) {
        if (overwrite_) values_[it->second].second = std::move(value);
        return;
    }
    index_.emplace(std::move(key), values_.size());
    values_.emplace_back(std::move(name), std::move(value));
}

void TagSink::setNull()
{
    // 旧仕様互換: SetValue(NULL, NULL) は GetTagInfo(KPI_TAGTYPE_NONE) と同じ
    getTagInfoCalled_ = true;
}

void WINAPI TagSink::aSetValueA(const char* n, int ns, const char* v, int vs)
{
    if (!n && !v) { setNull(); return; }
    if (!n) return;
    set(fromAnsi(n, ns), v ? fromAnsi(v, vs) : L"");
}
void WINAPI TagSink::aSetValueW(const char* n, int ns, const wchar_t* v, int vs)
{
    if (!n && !v) { setNull(); return; }
    if (!n) return;
    set(fromAnsi(n, ns), v ? fromWide(v, vs) : L"");
}
void WINAPI TagSink::aSetValueU8(const char* n, int ns, const char* v, int vs)
{
    if (!n && !v) { setNull(); return; }
    if (!n) return;
    set(fromAnsi(n, ns), v ? fromUtf8(v, vs) : L"");
}
void WINAPI TagSink::wSetValueA(const wchar_t* n, int ns, const char* v, int vs)
{
    if (!n && !v) { setNull(); return; }
    if (!n) return;
    set(fromWide(n, ns), v ? fromAnsi(v, vs) : L"");
}
void WINAPI TagSink::wSetValueW(const wchar_t* n, int ns, const wchar_t* v, int vs)
{
    if (!n && !v) { setNull(); return; }
    if (!n) return;
    set(fromWide(n, ns), v ? fromWide(v, vs) : L"");
}
void WINAPI TagSink::wSetValueU8(const wchar_t* n, int ns, const char* v, int vs)
{
    if (!n && !v) { setNull(); return; }
    if (!n) return;
    set(fromWide(n, ns), v ? fromUtf8(v, vs) : L"");
}
void WINAPI TagSink::u8SetValueA(const char* n, int ns, const char* v, int vs)
{
    if (!n && !v) { setNull(); return; }
    if (!n) return;
    set(fromUtf8(n, ns), v ? fromAnsi(v, vs) : L"");
}
void WINAPI TagSink::u8SetValueW(const char* n, int ns, const wchar_t* v, int vs)
{
    if (!n && !v) { setNull(); return; }
    if (!n) return;
    set(fromUtf8(n, ns), v ? fromWide(v, vs) : L"");
}
void WINAPI TagSink::u8SetValueU8(const char* n, int ns, const char* v, int vs)
{
    if (!n && !v) { setNull(); return; }
    if (!n) return;
    set(fromUtf8(n, ns), v ? fromUtf8(v, vs) : L"");
}

///////////////////////////////////////////////////////////////////////////////
// MemoryConfig
///////////////////////////////////////////////////////////////////////////////

MemoryConfig::MemoryConfig(std::wstring iniPath) : iniPath_(std::move(iniPath))
{
    load();
}

MemoryConfig::~MemoryConfig()
{
    save();
}

bool MemoryConfig::findValue(const wchar_t* sec, const wchar_t* key, std::wstring* out)
{
    if (!sec || !key) return false;
    std::lock_guard<std::mutex> lock(m_);
    auto s = data_.find(sec);
    if (s == data_.end()) return false;
    auto k = s->second.find(key);
    if (k == s->second.end()) return false;
    *out = k->second;
    return true;
}

void MemoryConfig::setValue(const wchar_t* sec, const wchar_t* key, std::wstring value)
{
    if (!sec || !key || !sec[0] || !key[0]) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        data_[sec][key] = std::move(value);
        dirty_ = true;
    }
    save();
}

void WINAPI MemoryConfig::SetInt(const wchar_t* sec, const wchar_t* key, INT64 v)
{
    setValue(sec, key, std::to_wstring(v));
}

INT64 WINAPI MemoryConfig::GetInt(const wchar_t* sec, const wchar_t* key, INT64 def)
{
    std::wstring v;
    if (!findValue(sec, key, &v) || v.empty()) return def;
    wchar_t* end = nullptr;
    INT64 r = wcstoll(v.c_str(), &end, 10);
    if (end == v.c_str()) return def; // 数値化出来ない
    return r;
}

void WINAPI MemoryConfig::SetFloat(const wchar_t* sec, const wchar_t* key, double v)
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%.17g", v);
    setValue(sec, key, buf);
}

double WINAPI MemoryConfig::GetFloat(const wchar_t* sec, const wchar_t* key, double def)
{
    std::wstring v;
    if (!findValue(sec, key, &v) || v.empty()) return def;
    wchar_t* end = nullptr;
    double r = wcstod(v.c_str(), &end);
    if (end == v.c_str()) return def;
    return r;
}

void WINAPI MemoryConfig::SetStr(const wchar_t* sec, const wchar_t* key, const wchar_t* v)
{
    setValue(sec, key, v ? v : L"");
}

DWORD WINAPI MemoryConfig::GetStr(const wchar_t* sec, const wchar_t* key, wchar_t* pszValue,
                                  DWORD dwSize, const wchar_t* def)
{
    if (!pszValue || dwSize < sizeof(wchar_t)) return 0;
    std::wstring v;
    if (!findValue(sec, key, &v)) v = def ? def : L"";
    DWORD needed = (DWORD)((v.size() + 1) * sizeof(wchar_t));
    // サイズ不足時は切り捨て(仕様)
    size_t maxChars = (size_t)dwSize / sizeof(wchar_t) - 1;
    size_t n = v.size() < maxChars ? v.size() : maxChars;
    memcpy(pszValue, v.c_str(), n * sizeof(wchar_t));
    pszValue[n] = 0;
    return needed;
}

void WINAPI MemoryConfig::SetBin(const wchar_t* sec, const wchar_t* key, const BYTE* p, DWORD size)
{
    if (!p) return;
    setValue(sec, key, utf8ToWide(base64Encode(p, size)));
}

DWORD WINAPI MemoryConfig::GetBin(const wchar_t* sec, const wchar_t* key, BYTE* p, DWORD size)
{
    if (!p) return 0;
    ZeroMemory(p, size);
    std::wstring v;
    if (!findValue(sec, key, &v)) return 0;
    std::string bin;
    if (!base64Decode(v, &bin)) return 0;
    DWORD needed = (DWORD)bin.size();
    if (size >= needed) {
        // サイズが足りる場合のみ格納。余りはゼロクリア済み
        memcpy(p, bin.data(), bin.size());
    }
    // 不足時は仕様どおり dwSize 分ゼロクリアのまま必要サイズを返す
    return needed;
}

void MemoryConfig::load()
{
    if (iniPath_.empty()) return;
    HANDLE h = CreateFileW(iniPath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string text;
    if (sz.QuadPart > 0 && sz.QuadPart < 16 * 1024 * 1024) {
        text.resize((size_t)sz.QuadPart);
        DWORD read = 0;
        ReadFile(h, text.data(), (DWORD)text.size(), &read, nullptr);
        text.resize(read);
    }
    CloseHandle(h);

    std::wstring section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = unescapeText(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || section.empty()) continue;
        std::wstring key = unescapeText(line.substr(0, eq));
        std::wstring value = unescapeText(line.substr(eq + 1));
        if (!key.empty()) data_[section][key] = std::move(value);
    }
}

void MemoryConfig::save()
{
    std::lock_guard<std::mutex> lock(m_);
    if (iniPath_.empty() || !dirty_) return;
    std::string out = "# nokomo2 plugin config\n";
    for (const auto& s : data_) {
        out += "[" + escapeText(s.first) + "]\n";
        for (const auto& kv : s.second) {
            out += escapeText(kv.first) + "=" + escapeText(kv.second) + "\n";
        }
    }
    HANDLE h = CreateFileW(iniPath_.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(h);
    dirty_ = false;
}

///////////////////////////////////////////////////////////////////////////////
// HostContext / HostRoot
///////////////////////////////////////////////////////////////////////////////

IKpiConfig* HostContext::getConfig(const GUID* guid)
{
    std::wstring key;
    if (!guid || IsEqualGUID(*guid, GUID{})) {
        // GUID_NULL: プラグインの置き場所(ここではファイル名)で決定(仕様準拠の近似)
        key = currentPluginKey.empty() ? L"_default" : currentPluginKey;
    } else {
        key = guidToString(*guid);
    }
    std::lock_guard<std::mutex> lock(configMutex);
    auto it = configs.find(key);
    if (it == configs.end()) {
        std::wstring iniPath;
        if (!options.configDir.empty()) {
            CreateDirectoryW(options.configDir.c_str(), nullptr);
            iniPath = options.configDir + L"\\" + key + L".ini";
        }
        ComPtr<MemoryConfig> cfg;
        cfg.attach(new MemoryConfig(std::move(iniPath)));
        it = configs.emplace(std::move(key), std::move(cfg)).first;
    }
    it->second->AddRef();
    return it->second.get();
}

DWORD WINAPI HostRoot::CreateInstance(REFIID riid, void* pvParam1, void* pvParam2,
                                      void* /*pvParam3*/, void* /*pvParam4*/, void** ppvObj)
{
    if (!ppvObj) return 0;
    *ppvObj = nullptr;
    if (IsEqualGUID(riid, IID_IKpiConfig)) {
        // pvParam1 = GUID*(保存場所識別子), pvParam2 = DWORD*(プラットフォーム)
        DWORD* platform = (DWORD*)pvParam2;
        if (platform) *platform = 0; // 本体(=このホスト)が直接ロードしている
        *ppvObj = ctx_->getConfig((const GUID*)pvParam1);
        return 1;
    }
    // 未知の IID: 仕様どおり何もしないで 0 を返す(*ppvObj は NULL のまま)
    return 0;
}

} // namespace nokomo::detail
