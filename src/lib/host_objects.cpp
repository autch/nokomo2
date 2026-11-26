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
    // 存在チェックのみ行い、ハンドルはまだ開かない(遅延オープン。理由はヘッダ参照)
    DWORD attr = GetFileAttributesW(full);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return nullptr;
    return new LocalKpiFile(full);
}

LocalKpiFile::LocalKpiFile(std::wstring fullPath)
    : fullPath_(std::move(fullPath))
{
    fileName_ = fileNameOf(fullPath_);
}

HANDLE LocalKpiFile::ensureHandle()
{
    std::lock_guard<std::mutex> lock(m_);
    if (!openTried_) {
        openTried_ = true;
        h_ = CreateFileW(fullPath_.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    return h_;
}

LocalKpiFile::~LocalKpiFile()
{
    if (view_) UnmapViewOfFile((LPVOID)view_);
    if (mapping_) CloseHandle(mapping_);
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    if (!tempCopyPath_.empty()) DeleteFileW(tempCopyPath_.c_str());
    if (!tempCopyDir_.empty()) RemoveDirectoryW(tempCopyDir_.c_str());
}

// 損失なく ANSI へ変換出来る場合のみ true。
// WC_NO_BEST_FIT_CHARS は ACP=UTF-8(Win10 のベータ設定や一部の Wine)で
// ERROR_INVALID_FLAGS になり全パスが変換不能扱いになるため使わず、
// 「変換して逆変換した結果が一致するか」で無損失を判定する
// (CreateFileA が同じファイルを開けることの判定として十分)。
static bool toAnsiStrict(const std::wstring& s, std::string* out)
{
    int need = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return false;
    out->resize((size_t)need);
    WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, out->data(), need, nullptr, nullptr);
    out->resize(strlen(out->c_str()));
    int wn = MultiByteToWideChar(CP_ACP, 0, out->c_str(), -1, nullptr, 0);
    if (wn <= 0) return false;
    std::wstring back((size_t)wn, L'\0');
    MultiByteToWideChar(CP_ACP, 0, out->c_str(), -1, back.data(), wn);
    back.resize(wcslen(back.c_str()));
    return back == s;
}

DWORD WINAPI LocalKpiFile::Read(void* pBuffer, DWORD dwSize)
{
    if (!pBuffer || dwSize == 0) return 0;
    DWORD read = 0;
    if (!aborted_.load(std::memory_order_relaxed)) {
        HANDLE h = ensureHandle();
        if (h != INVALID_HANDLE_VALUE && !ReadFile(h, pBuffer, dwSize, &read, nullptr)) {
            read = 0;
        }
    }
    // dwSize 未満しか読めなかった場合は残りを 0 で埋める(SDK 2026/10/31版)。
    // プラグインはこれを前提にしてはならないが、未初期化領域の読み出しによる
    // 不定動作を防ぐ多重防御として本体(KbMedia Player)と同じ挙動にする
    if (read < dwSize) memset((BYTE*)pBuffer + read, 0, dwSize - read);
    return read;
}

UINT64 WINAPI LocalKpiFile::Seek(INT64 i64Pos, DWORD dwOrigin)
{
    if (aborted_.load(std::memory_order_relaxed)) return KPI_FILE_EOF;
    HANDLE h = ensureHandle();
    if (h == INVALID_HANDLE_VALUE) return KPI_FILE_EOF;
    LARGE_INTEGER pos, out;
    pos.QuadPart = i64Pos;
    // FILE_BEGIN/CURRENT/END は KPI 側と同じ値
    if (!SetFilePointerEx(h, pos, &out, dwOrigin)) return KPI_FILE_EOF;
    return (UINT64)out.QuadPart;
}

UINT64 WINAPI LocalKpiFile::GetSize(void)
{
    if (aborted_.load(std::memory_order_relaxed)) return KPI_FILE_EOF;
    {
        // ハンドルを開かずに取得出来るならそうする(排他オープン中のプラグインと
        // 競合しないため)
        std::lock_guard<std::mutex> lock(m_);
        if (!openTried_) {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(fullPath_.c_str(), GetFileExInfoStandard, &fad)) {
                return ((UINT64)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            }
            return KPI_FILE_EOF;
        }
    }
    if (h_ == INVALID_HANDLE_VALUE) return KPI_FILE_EOF;
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
        wchar_t shortBuf[MAX_PATH * 4];
        if (toAnsiStrict(fullPath_, &ansiPath_)) {
            ansiState_ = 1;
        } else if (DWORD sn = GetShortPathNameW(fullPath_.c_str(), shortBuf,
                                                (DWORD)(sizeof(shortBuf) / sizeof(shortBuf[0])));
                   sn > 0 && sn < sizeof(shortBuf) / sizeof(shortBuf[0]) &&
                   toAnsiStrict(shortBuf, &ansiPath_)) {
            // 8.3 短縮名なら ANSI で表現出来ることが多い(KbMP と同様の手法)。
            // コピーが不要で、同一フォルダの参照ファイルもそのまま解決出来る。
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
        // GetBuffer は実ハンドルが必要(ここで初めて開くことがある)
        if (!openTried_) {
            openTried_ = true;
            h_ = CreateFileW(fullPath_.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (h_ == INVALID_HANDLE_VALUE) return FALSE;
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
    // 上限は 16KB(終端の 0 を含む。8191 文字)。超える場合は何もしない(kpi.h 2026/11/13版)
    if (v && (wcslen(v) + 1) * sizeof(wchar_t) > 16 * 1024) return;
    setValue(sec, key, v ? v : L"");
}

DWORD WINAPI MemoryConfig::GetStr(const wchar_t* sec, const wchar_t* key, wchar_t* pszValue,
                                  DWORD dwSize, const wchar_t* def)
{
    if (!pszValue || dwSize < sizeof(wchar_t)) return 0;
    std::wstring v;
    if (!findValue(sec, key, &v)) v = def ? def : L"";
    DWORD needed = (DWORD)((v.size() + 1) * sizeof(wchar_t));
    // サイズ不足時は切り捨てずに空文字列を入れて必要サイズを返す(SDK 2026/10/31版)。
    // 途切れたパスが想定外の場所を指す事故を避けるため
    if (dwSize < needed) {
        pszValue[0] = 0;
        return needed;
    }
    memcpy(pszValue, v.c_str(), needed);
    return needed;
}

void WINAPI MemoryConfig::SetBin(const wchar_t* sec, const wchar_t* key, const BYTE* p, DWORD size)
{
    if (!p) return;
    // 上限は 1MB。超える場合は何もしない(kpi.h 2026/11/13版)
    if (size > 1024 * 1024) return;
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
// HostMessenger
///////////////////////////////////////////////////////////////////////////////

void WINAPI HostMessenger::Notify(DWORD /*dwFlags*/, const wchar_t* cszFileName,
                                  const wchar_t* cszText)
{
    if (!cszText || !*cszText || !handler_ || !*handler_) return;
    // 1 件の長さに上限を設け、改行・タブ以外の制御文字は空白にする(kpi_message.h:
    // 本体はそのようにする。ファイルのタグ等をそのまま渡して良いことになっている)
    constexpr size_t kMaxText = 4096;
    constexpr size_t kMaxName = 1024;
    auto sanitize = [](const wchar_t* src, size_t maxLen, bool allowNewline) {
        std::wstring out;
        for (const wchar_t* p = src; *p && out.size() < maxLen; ++p) {
            wchar_t c = *p;
            bool keep = allowNewline && (c == L'\r' || c == L'\n' || c == L'\t');
            if (!keep && (c < 0x20 || c == 0x7F)) c = L' ';
            out += c;
        }
        return out;
    };
    PluginMessage msg;
    msg.pluginPath = pluginPath_;
    if (cszFileName) msg.fileName = sanitize(cszFileName, kMaxName, false);
    msg.text = sanitize(cszText, kMaxText, true);
    try {
        (*handler_)(msg);
    } catch (...) {
        // 例外をプラグイン(C++ 例外の境界を越えられない)へ伝播させない
    }
}

///////////////////////////////////////////////////////////////////////////////
// HostWorkFolder / WorkFolderJanitor
///////////////////////////////////////////////////////////////////////////////

// dir(末尾セパレータなし)を中身ごと削除する。リンク(ジャンクション等)は辿らず
// リンク自体だけを消す。dir が既に無ければ成功
static bool removeTree(const std::wstring& dir)
{
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND ||
                                                GetLastError() == ERROR_PATH_NOT_FOUND;
    if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd;
        HANDLE hf = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                     FindExSearchNameMatch, nullptr, 0);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                std::wstring child = dir + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                    SetFileAttributesW(child.c_str(),
                                       fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                        RemoveDirectoryW(child.c_str()); // リンク自体のみ
                    } else {
                        removeTree(child);
                    }
                } else {
                    DeleteFileW(child.c_str());
                }
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
    }
    return RemoveDirectoryW(dir.c_str()) != FALSE;
}

WorkFolderJanitor::~WorkFolderJanitor()
{
    // 全モジュール解放後。使用中で消せなかった作業フォルダをもう一度消す
    for (const auto& dir : pending_) removeTree(dir);
}

void WorkFolderJanitor::remove(const std::wstring& dir)
{
    if (removeTree(dir)) return;
    std::lock_guard<std::mutex> lock(m_);
    pending_.push_back(dir);
}

HostWorkFolder* HostWorkFolder::create(std::shared_ptr<WorkFolderJanitor> janitor,
                                       const std::wstring& rootIn, DWORD dwFlags)
{
    // 未定義のビットを指定すると失敗する(kpi_workfolder.h)
    if (dwFlags & ~(DWORD)KPI_WORKFOLDER_FLAG_ANSI) return nullptr;

    std::wstring root = rootIn;
    if (root.empty()) {
        wchar_t tmp[MAX_PATH + 1];
        DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
        if (n == 0 || n > MAX_PATH) return nullptr;
        root.assign(tmp, n);
        if (!root.empty() && root.back() != L'\\' && root.back() != L'/') root += L'\\';
        root += L"nokomo2";
    }
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(root.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    if (n == 0 || n >= sizeof(full) / sizeof(full[0])) return nullptr;
    root.assign(full, n);
    while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    std::error_code ec;
    fs::create_directories(fs::path(root), ec);

    if (dwFlags & KPI_WORKFOLDER_FLAG_ANSI) {
        // GetPath の結果が CP_ACP で欠けずに変換出来ること。表せなければ失敗
        // (本体と同じく ANSI で表せる別の場所は探さない)
        std::string ansi;
        if (!toAnsiStrict(root, &ansi)) return nullptr;
    }

    static std::atomic<uint32_t> counter{0};
    for (int attempt = 0; attempt < 64; ++attempt) {
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        uint32_t v = (uint32_t)qpc.QuadPart * 2654435761u ^ GetCurrentProcessId() * 40503u ^
                     (counter.fetch_add(1) << 20);
        wchar_t name[16];
        swprintf(name, 16, L"%08X", v);
        std::wstring dir = root;
        if (dir.back() != L'\\') dir += L'\\';
        dir += name;
        // 中に作るファイルの余地も考え、フォルダ自体は MAX_PATH 未満にする
        if (dir.size() + 1 >= MAX_PATH) return nullptr;
        if (CreateDirectoryW(dir.c_str(), nullptr)) {
            return new HostWorkFolder(std::move(janitor), dir + L"\\");
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return nullptr;
    }
    return nullptr;
}

HostWorkFolder::~HostWorkFolder()
{
    std::wstring dir = path_;
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    janitor_->remove(dir);
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
            // 中間ディレクトリ(%APPDATA%\nokomo2 等)ごと作成する
            // (CreateDirectoryW は親が存在しないと失敗し、設定が保存されなくなる)
            std::error_code ec;
            fs::create_directories(fs::path(options.configDir), ec);
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
    if (IsEqualGUID(riid, IID_IKpiMessenger)) {
        // 受け取り先がなければ「この口に対応していない」(*ppvObj=NULL)
        if (!ctx_->messageHandler) return 0;
        // 通知元プラグインは kpi_CreateInstance 中の取得(仕様上の取得時期)でのみ特定出来る
        std::wstring pluginPath;
        if (ctx_->loadingThread.load() == GetCurrentThreadId()) {
            pluginPath = ctx_->currentPluginPath;
        }
        *ppvObj = static_cast<IKpiMessenger*>(
            new HostMessenger(ctx_->messageHandler, std::move(pluginPath)));
        return 1;
    }
    if (IsEqualGUID(riid, IID_IKpiWorkFolder)) {
        // pvParam1 = (DWORD_PTR)dwFlags(KPI_WORKFOLDER_FLAG_XXX)
        HostWorkFolder* wf = HostWorkFolder::create(ctx_->workFolders, ctx_->options.workFolderRoot,
                                                    (DWORD)(DWORD_PTR)pvParam1);
        if (!wf) return 0;
        *ppvObj = static_cast<IKpiWorkFolder*>(wf);
        return 1;
    }
    // 未知の IID: 仕様どおり何もしないで 0 を返す(*ppvObj は NULL のまま)
    return 0;
}

} // namespace nokomo::detail
