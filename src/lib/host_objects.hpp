#pragma once
// ホスト(プラグイン使用側)が実装する KPI インターフェース群。
// kpi.h のコメントにある「主に本体側で実装」のものに対応する。

#include <windows.h>

#include "kpi.h"
#include "kpi_decoder.h"
#include "kpi_impl.h"
#include "kpi_message.h"
#include "kpi_workfolder.h"

#include "kpi_mingw_uuid.h" // MinGW/clang クロスビルド検証用(MSVC では無効)

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "com_ptr.hpp"
#include "nokomo/nokomo.hpp"

namespace nokomo::detail {

///////////////////////////////////////////////////////////////////////////////
// IKpiFile のローカルファイル実装
//
// ホストが提供する IKpiFile::Seek は「要求位置ちょうどを返すか KPI_FILE_EOF」
// という(プラグイン提供側より厳しい)契約を満たす必要がある。
class LocalKpiFile final : public KbKpiFileImpl {
public:
    // 失敗時 nullptr。参照カウント 1 の状態で返す。
    static LocalKpiFile* open(const std::wstring& path);

    DWORD  WINAPI Read(void* pBuffer, DWORD dwSize) override;
    UINT64 WINAPI Seek(INT64 i64Pos, DWORD dwOrigin) override;
    UINT64 WINAPI GetSize(void) override;
    BOOL   WINAPI CreateClone(IKpiFile** ppFile) override;
    DWORD  WINAPI GetFileName(wchar_t* pszName, DWORD dwSize) override;
    BOOL   WINAPI GetRealFileW(const wchar_t** ppszFileNameW) override;
    BOOL   WINAPI GetRealFileA(const char** ppszFileNameA) override;
    BOOL   WINAPI GetBuffer(const BYTE** ppBuffer, size_t* pstSize) override;
    BOOL   WINAPI Abort(void) override;

private:
    explicit LocalKpiFile(std::wstring fullPath);
    ~LocalKpiFile() override;

    // OS ハンドルは最初の Read/Seek/GetBuffer まで開かない(遅延オープン)。
    // 旧仕様プラグイン等は GetRealFileA/W で得た実ファイル名を共有モード 0(排他)
    // で開くことがあり、ホストがハンドルを保持しているとその時点で共有違反になる。
    // KbMedia Player の旧仕様経路はファイル名を渡すだけでハンドルを持たないため、
    // それと同じ条件にする。
    HANDLE ensureHandle();

    HANDLE h_ = INVALID_HANDLE_VALUE;
    bool openTried_ = false;
    std::wstring fullPath_;
    std::wstring fileName_;
    std::atomic<bool> aborted_{false};
    // GetRealFileA 用(遅延生成)
    // パスが ANSI で表現出来ない場合は仕様どおりテンポラリにコピーを作成する
    std::string ansiPath_;
    int ansiState_ = 0; // 0=未試行 1=成功 2=失敗
    std::wstring tempCopyPath_; // 作成したテンポラリコピー(Release 時に削除)
    std::wstring tempCopyDir_;
    // GetBuffer 用(遅延生成、Release まで保持)
    HANDLE mapping_ = nullptr;
    const BYTE* view_ = nullptr;
    size_t viewSize_ = 0;
    bool mapTried_ = false;
    std::mutex m_;
};

///////////////////////////////////////////////////////////////////////////////
// IKpiFolder の実ディレクトリ実装
class DirectoryFolder final : public KbKpiFolderImpl {
public:
    // dir は実在するディレクトリのフルパス。失敗時 nullptr。
    static DirectoryFolder* open(const std::wstring& dir);

    DWORD WINAPI GetFolderName(wchar_t* pszName, DWORD dwSize) override;
    DWORD WINAPI EnumFiles(DWORD dwIndex, wchar_t* pszName, DWORD dwSize, DWORD dwLevel) override;
    BOOL  WINAPI OpenFile(const wchar_t* cszName, IKpiFile** ppFile) override;
    BOOL  WINAPI OpenFolder(const wchar_t* cszName, IKpiFolder** ppFolder) override;

private:
    explicit DirectoryFolder(std::wstring dir);

    std::wstring resolve(const std::wstring& name) const; // 相対/絶対 → フルパス

    std::wstring dir_; // 末尾セパレータなし(ドライブルート "C:\" を除く)
    std::mutex m_;
    std::map<DWORD, std::vector<std::wstring>> listCache_; // dwLevel → 列挙結果
};

///////////////////////////////////////////////////////////////////////////////
// IKpiTagInfo 実装: プラグインが通知するタグを収集する
//
// GetTagInfo(本体のタグ解析ルーチン呼び出し)は現状未対応で (DWORD)-1 を返す。
// KPI_TAGTYPE_NONE の通知(=ホスト側の自動タグ取得の抑止)だけは記録する。
class TagSink final : public KbKpiTagInfoImpl {
public:
    TagSink() = default;

    // 収集結果の取り出し(Select 呼び出し後にホストが使う)
    void harvest(TrackTags* out) const;
    // GetTagInfo が一度も呼ばれていなければ、本来ホストが Select 後に
    // 自前のタグ取得を行うべき(現状は未実装なので参考情報)
    bool pluginHandledTags() const { return getTagInfoCalled_; }

    DWORD WINAPI GetTagInfo(IKpiFile* pFile, IKpiFolder* pFolder,
                            DWORD dwType, DWORD dwTagGetFlags) override;
    DWORD WINAPI GetValue(const wchar_t* cszName, wchar_t* pszValue, int nSize) override;
    void  WINAPI SetOverwrite(BOOL bOverwrite) override;
    void  WINAPI SetPicture(DWORD dwType, const wchar_t* cszType,
                            const wchar_t* cszMIMEType, const wchar_t* cszDescription,
                            DWORD dwWidth, DWORD dwHeight,
                            const BYTE* pPictureData, DWORD dwSize) override;
    void WINAPI aSetValueA(const char* n, int ns, const char* v, int vs) override;
    void WINAPI aSetValueW(const char* n, int ns, const wchar_t* v, int vs) override;
    void WINAPI aSetValueU8(const char* n, int ns, const char* v, int vs) override;
    void WINAPI wSetValueA(const wchar_t* n, int ns, const char* v, int vs) override;
    void WINAPI wSetValueW(const wchar_t* n, int ns, const wchar_t* v, int vs) override;
    void WINAPI wSetValueU8(const wchar_t* n, int ns, const char* v, int vs) override;
    void WINAPI u8SetValueA(const char* n, int ns, const char* v, int vs) override;
    void WINAPI u8SetValueW(const char* n, int ns, const wchar_t* v, int vs) override;
    void WINAPI u8SetValueU8(const char* n, int ns, const char* v, int vs) override;

private:
    ~TagSink() override = default;

    void set(std::wstring name, std::wstring value);
    void setNull(); // xSetValue(NULL, NULL) 互換: ホストの自動タグ取得を抑止

    bool overwrite_ = false; // Select 直後の上書きモードは FALSE(仕様)
    bool getTagInfoCalled_ = false;
    std::vector<std::pair<std::wstring, std::wstring>> values_; // 通知順
    std::map<std::wstring, size_t> index_;                      // 小文字名 → values_ 添字
    std::vector<TagPicture> pictures_;
};

///////////////////////////////////////////////////////////////////////////////
// IKpiConfig 実装: セクション/キー → 文字列値。INI ファイルに永続化(任意)。
class MemoryConfig final : public KbKpiUnknownImpl<IKpiConfig> {
public:
    // iniPath が空ならメモリのみ
    explicit MemoryConfig(std::wstring iniPath);

    void   WINAPI SetInt(const wchar_t* sec, const wchar_t* key, INT64 v) override;
    INT64  WINAPI GetInt(const wchar_t* sec, const wchar_t* key, INT64 def) override;
    void   WINAPI SetFloat(const wchar_t* sec, const wchar_t* key, double v) override;
    double WINAPI GetFloat(const wchar_t* sec, const wchar_t* key, double def) override;
    void   WINAPI SetStr(const wchar_t* sec, const wchar_t* key, const wchar_t* v) override;
    DWORD  WINAPI GetStr(const wchar_t* sec, const wchar_t* key, wchar_t* pszValue,
                         DWORD dwSize, const wchar_t* def) override;
    void   WINAPI SetBin(const wchar_t* sec, const wchar_t* key, const BYTE* p, DWORD size) override;
    DWORD  WINAPI GetBin(const wchar_t* sec, const wchar_t* key, BYTE* p, DWORD size) override;

private:
    ~MemoryConfig() override;

    bool findValue(const wchar_t* sec, const wchar_t* key, std::wstring* out);
    void setValue(const wchar_t* sec, const wchar_t* key, std::wstring value);
    void load();
    void save();

    // セクション名・キー名は大文字と小文字を区別しない(SDK 2026/10/31版)。
    // ロケール非依存の Unicode 序数比較で大小を畳み込む
    struct NameLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const
        {
            return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(),
                                        TRUE) == CSTR_LESS_THAN;
        }
    };
    using KeyMap = std::map<std::wstring, std::wstring, NameLess>;

    std::mutex m_;
    std::map<std::wstring, KeyMap, NameLess> data_;
    std::wstring iniPath_;
    bool dirty_ = false;
};

///////////////////////////////////////////////////////////////////////////////
// IKpiMessenger 実装: プラグインからのお知らせを HostOptions::onPluginMessage へ渡す
class HostMessenger final : public KbKpiUnknownImpl<IKpiMessenger> {
public:
    HostMessenger(std::shared_ptr<const PluginMessageHandler> handler, std::wstring pluginPath)
        : handler_(std::move(handler)), pluginPath_(std::move(pluginPath)) {}

    void WINAPI Notify(DWORD dwFlags, const wchar_t* cszFileName, const wchar_t* cszText) override;

private:
    ~HostMessenger() override = default;
    // プラグインがモジュール解放まで保持するので、ハンドラは共有所有で持つ
    std::shared_ptr<const PluginMessageHandler> handler_;
    std::wstring pluginPath_;
};

///////////////////////////////////////////////////////////////////////////////
// IKpiWorkFolder 実装
//
// 最後の Release でフォルダを中身ごと削除する。使用中で消せなかった分は
// WorkFolderJanitor に預け、その破棄時(HostContext の破棄 = 全モジュール解放後)に
// もう一度削除を試みる。
class WorkFolderJanitor {
public:
    WorkFolderJanitor() = default;
    ~WorkFolderJanitor();
    WorkFolderJanitor(const WorkFolderJanitor&) = delete;
    WorkFolderJanitor& operator=(const WorkFolderJanitor&) = delete;

    // dir(末尾 \ なし)を中身ごと削除する。失敗したら後で再試行するため保持する
    void remove(const std::wstring& dir);

private:
    std::mutex m_;
    std::vector<std::wstring> pending_;
};

class HostWorkFolder final : public KbKpiUnknownImpl<IKpiWorkFolder> {
public:
    // root 直下に新しい空のフォルダを作る。失敗時 nullptr。参照カウント 1 で返す
    static HostWorkFolder* create(std::shared_ptr<WorkFolderJanitor> janitor,
                                  const std::wstring& root, DWORD dwFlags);

    const wchar_t* WINAPI GetPath(void) override { return path_.c_str(); }

private:
    HostWorkFolder(std::shared_ptr<WorkFolderJanitor> janitor, std::wstring path)
        : janitor_(std::move(janitor)), path_(std::move(path)) {}
    ~HostWorkFolder() override;

    std::shared_ptr<WorkFolderJanitor> janitor_;
    std::wstring path_; // 末尾 \ 付き
};

///////////////////////////////////////////////////////////////////////////////
// ホスト全体の状態。PluginRegistry が所有し、全プラグインで共有される。
class HostRoot;

struct HostContext {
    HostOptions options;
    ComPtr<HostRoot> root;          // kpi_CreateInstance に渡す IKpiUnknown

    // kpi_CreateInstance 呼び出しの直列化と、GUID_NULL 設定のキー解決に使う
    std::mutex loadMutex;
    std::wstring currentPluginKey;  // ロード中プラグインのファイル名 stem
    std::wstring currentPluginPath; // ロード中プラグインのフルパス
    // kpi_CreateInstance を呼んでいるスレッド(0=ロード中でない)。
    // ロード中以外のスレッドが currentPlugin* を読まないようにするため
    std::atomic<DWORD> loadingThread{0};

    // onPluginMessage の共有コピー(空なら IKpiMessenger を提供しない)
    std::shared_ptr<const PluginMessageHandler> messageHandler;
    std::shared_ptr<WorkFolderJanitor> workFolders = std::make_shared<WorkFolderJanitor>();

    std::mutex configMutex;
    std::map<std::wstring, ComPtr<MemoryConfig>> configs; // キー → 設定

    // GUID(または GUID_NULL ならロード中プラグイン名)に対応する設定を返す。
    // 戻り値は AddRef 済み。
    IKpiConfig* getConfig(const GUID* guid);
};

///////////////////////////////////////////////////////////////////////////////
// kpi_CreateInstance の第3引数として渡す IKpiUnknown 兼 IKpiUnkProvider。
// IID_IKpiConfig / IID_IKpiMessenger / IID_IKpiWorkFolder を処理する
// (未知の IID は *ppvObj=NULL, 戻り値 0)。
class HostRoot final : public KbKpiUnknownImpl<IKpiUnkProvider> {
public:
    explicit HostRoot(HostContext* ctx) : ctx_(ctx) {}

    DWORD WINAPI CreateInstance(REFIID riid, void* pvParam1, void* pvParam2,
                                void* pvParam3, void* pvParam4, void** ppvObj) override;

private:
    ~HostRoot() override = default;
    HostContext* ctx_;
};

} // namespace nokomo::detail
