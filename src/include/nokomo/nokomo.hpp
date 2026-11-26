#pragma once
// nokomo2: KbMedia Player 現行プラグイン仕様(KPI)のホスト側実装
//
// デコーダプラグイン(.kpi)をロードして PCM デコードとタグ列挙を行う。
// KbMedia Player 本体と同様に、
//   ・プラグイン情報(対応拡張子等)はキャッシュし、DLL は必要時のみロード
//   ・タグ列挙はタグ専用オープン(Open(cpRequest=NULL))で高速に行う
// という動作をする。
//
// スレッド安全性: PluginRegistry の各メソッドは複数スレッドから呼び出し可能。
// DecodeSession は 1 インスタンスにつき単一スレッドから使用すること。
// DecodeSession は生成元の PluginRegistry より長生きしてはならない。

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nokomo {

// KPI_TAGGET_FLAG_* と同値(kpi.h 参照)
enum : uint32_t {
    TagFieldTitle     = 0x001,
    TagFieldArtist    = 0x002,
    TagFieldAlbum     = 0x004,
    TagFieldGenre     = 0x008,
    TagFieldDate      = 0x010,
    TagFieldComment   = 0x020,
    TagFieldTrack     = 0x040,
    TagFieldAlbumArtist = 0x080,
    TagFieldCopyright = 0x100,
    TagFieldReplayGain = 0x200,
    TagFieldPicture   = 0x400,
    TagFieldGeneral   = 0x80000000u,
    TagFieldAll       = 0xFFFFFFFFu,
};

constexpr uint64_t kUnknown64 = ~0ull;

// KPI_DECODER_MODULEINFO のスナップショット + キャッシュ無効化キー。
// DLL をロードせずに拡張子マッチングや能力判定を行うために保持する。
struct PluginInfo {
    std::wstring path;                // DLL フルパス
    uint64_t fileSize = 0;            // キャッシュ無効化キー
    int64_t mtime = 0;                // キャッシュ無効化キー(更新日時)
    bool valid = false;               // false ならロード失敗/非対応 DLL
    std::wstring invalidReason;

    uint32_t moduleVersion = 0;       // KPI_DECODER_MODULE_VERSION(=200)のみ有効
    bool isLegacy = false;            // 旧仕様(kmp_pi.h)プラグイン(アダプタ経由で使用)
    uint32_t pluginVersion = 0;       // 拡張子競合時の優先度に使用
    uint32_t multipleInstance = 0;    // KPI_MULTINST_*
    uint32_t supportTagInfo = 0;      // TAG_TITLE/TAG_EXTRA/...
    uint32_t supportConfig = 0;
    GUID guid{};
    std::wstring description;
    std::wstring copyright;
    std::wstring supportExts;         // ".mp1/.mp2/.mp3" 形式
    std::wstring multiSongExts;

    std::vector<std::wstring> extensionList() const;
    bool supportsExtension(const std::wstring& extLower) const;
    bool isMultiSongExtension(const std::wstring& extLower) const;
    // 表示用の短い識別名: "kbflac.kpi v30" 形式(ファイル名 + dwPluginVersion)。
    // cszDescription は自由文で長いため、使用プラグインの提示にはこちらを推奨。
    std::wstring displayName() const;
    // タグ専用オープン(Open(cpRequest=NULL))を呼んで良いか。
    // KPI_DECODER_MODULEINFO::TAG_TITLE が立っているか、1ファイル複数曲の拡張子
    // (cszMultiSongExts)なら許される(後者は曲数を得るため、dwSupportTagInfo に
    // 関わらず開く。SDK 2026/11/25版で明記)
    bool canTagOnlyOpen(const std::wstring& extLower) const;
};

// AddDllDirectory が返す Cookie(PVOID)。SDK ヘッダのバージョン差を避けるため
// 独自の別名で公開する(値は RemoveDllDirectory にそのまま渡せる)。
using DllDirCookie = void*;

// プラグインから利用者へのお知らせ(IKpiMessenger::Notify)。
struct PluginMessage {
    std::wstring pluginPath;          // 通知したプラグイン(特定出来なければ空)
    std::wstring fileName;            // 関わるファイル名(特定のファイルに関わらなければ空)
    std::wstring text;                // 本文(改行・タブ以外の制御文字は空白に置換済み)
};
// プラグインの任意のスレッドから、プラグインが自分のロックを持ったまま呼ばれ得る。
// スレッドセーフにし、すぐに戻ること。PluginRegistry / DecodeSession を呼んではならない
// (モジュールの排他ロック保持中に呼ばれることがありデッドロックする)。
using PluginMessageHandler = std::function<void(const PluginMessage&)>;

// AddDllDirectory / RemoveDllDirectory の薄いラッパ。
// これらの API は Win8 未満(未更新の Win7)に存在しないため動的解決しており、
// 非対応 OS では nullptr / false を返す(アプリが直接インポートすると古い環境で
// exe のロード自体が失敗するので、解除にもこのラッパを使うこと)。
DllDirCookie addDllSearchDirectory(const std::wstring& dir);
bool removeDllSearchDirectory(DllDirCookie cookie);

struct HostOptions {
    std::wstring cacheFilePath;       // 空: キャッシュ永続化なし
    std::wstring configDir;           // 空: プラグイン設定を永続化しない(メモリのみ)
    int maxResidentModules = 4;       // 使用後もロードしたままにする DLL 数(LRU)
    uint64_t defaultLengthMs = 300000;// 長さ不明曲のデフォルト長
    uint64_t defaultFadeMs = 5000;    // フェードアウト時間のデフォルト
    uint32_t defaultLoopCount = 2;    // ループ曲のループ回数デフォルト

    // プラグインの依存 DLL(kbzlib.dll 等)の追加検索ディレクトリ。
    // KbMedia Player では本体 exe と同じ場所に依存 DLL が置かれるのに相当する。
    //
    // AddDllDirectory はプロセスグローバルな状態のため、登録の所有権はアプリが持つ:
    //  ・cookie == nullptr: PluginRegistry が構築時に AddDllDirectory を呼び、
    //    取得した Cookie を dllDirCookies() で返す。解除するかどうか・いつ解除
    //    するかはアプリの判断で、removeDllSearchDirectory() を呼ぶ。
    //    PluginRegistry が RemoveDllDirectory を呼ぶことはない。
    //  ・cookie != nullptr: アプリが登録済みとみなし、PluginRegistry は何もしない。
    struct DllSearchDir {
        std::wstring dir;
        DllDirCookie cookie = nullptr;
    };
    std::vector<DllSearchDir> dllSearchDirs;

    // IKpiMessenger(利用者へのお知らせ)の受け取り先。空ならプラグインに
    // IKpiMessenger を提供しない(プラグインは自前の方法で知らせるか何もしない)
    PluginMessageHandler onPluginMessage;

    // IKpiWorkFolder(プラグイン用の作業フォルダ)を作る親ディレクトリ。
    // 空: %TEMP%\nokomo2。作業フォルダはプラグインの最後の Release で中身ごと削除し、
    // 使用中で消せなかった分は PluginRegistry の破棄時(全モジュール解放後)に再試行する
    std::wstring workFolderRoot;
};

struct TagPicture {
    uint32_t type = 0;                // ID3v2 "APIC" 互換(3=Cover(front))
    std::wstring typeName;
    std::wstring mimeType;
    std::wstring description;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> data;
};

struct TrackTags {
    // 通知された順序を保持(名前は通知されたまま)
    std::vector<std::pair<std::wstring, std::wstring>> values;
    std::vector<TagPicture> pictures;
    // 名前は大文字小文字を区別しない。見つからなければ nullptr
    const std::wstring* find(const std::wstring& name) const;
};

struct FileTags {
    std::wstring pluginPath;          // 使用したプラグイン(フルパス)
    std::wstring pluginDisplay;       // 表示用(PluginInfo::displayName())
    uint32_t trackCount = 0;
    TrackTags album;                  // 複数曲ファイルの全体情報(Select(0)相当)
    std::vector<TrackTags> tracks;    // tracks[0] = 曲番号 1
};

// openDecode / scanTags の候補プラグイン試行記録。
// 同一拡張子に複数のプラグインが対応する場合、優先度順(バージョン降順→更新日時
// 降順)に試行し、失敗したら次の候補にフォールバックする。その過程の可視化用。
struct OpenAttempt {
    std::wstring pluginPath;
    std::wstring pluginDisplay;   // PluginInfo::displayName()
    bool used = false;            // このプラグインが採用された
    std::wstring reason;          // 不採用・失敗の理由(採用時は空)
};

struct DecodeRequest {
    uint32_t sampleRate = 0;          // 0 = プラグイン任せ
    int32_t bitsPerSample = -64;      // 負値 = float。0 = 要求しない
    uint32_t loopCount = 0;           // 0 = データ推奨値 → HostOptions 既定の順
    uint64_t fadeMs = kUnknown64;     // kUnknown64 = データ推奨値 → HostOptions 既定の順
};

// チャンネル数で決まる標準のスピーカー配置(KPI_MEDIAINFO::dwSpeakerConfig=0 の意味。
// WAVEFORMATEXTENSIBLE::dwChannelMask と同じ値)。1〜8ch 以外は 0
constexpr uint32_t defaultChannelMask(uint32_t channels)
{
    switch (channels) {
    case 1: return 0x4;   // FC
    case 2: return 0x3;   // FL FR
    case 3: return 0x7;   // FL FR FC
    case 4: return 0x33;  // FL FR BL BR
    case 5: return 0x37;  // FL FR FC BL BR
    case 6: return 0x3F;  // FL FR FC LFE BL BR
    case 7: return 0x70F; // FL FR FC LFE BC SL SR
    case 8: return 0x63F; // FL FR FC LFE BL BR SL SR
    default: return 0;
    }
}

// KPI_MEDIAINFO のコピー + ホストが解決した実効値
struct MediaInfo {
    uint32_t trackNumber = 0;
    uint32_t trackCount = 0;
    uint32_t sampleRate = 0;
    int32_t bitsPerSample = 0;        // 負値 = float
    uint32_t channels = 0;            // 1〜8
    uint32_t speakerConfig = 0;       // dwSpeakerConfig(0=標準配置。不正値は 0 に正規化済み)
    uint32_t formatType = 0;          // KPI_MEDIAINFO::FORMAT_PCM/DOP
    uint64_t length100ns = 0;         // 1ループ目の長さ(100ns)。kUnknown64=不明
    uint64_t loop100ns = 0;           // 2ループ目以降1ループの長さ。0=非ループ, kUnknown64=不明
    uint64_t fadeOut100ns = 0;
    uint32_t loopCount = 0;
    uint32_t unitSample = 0;
    uint32_t seekableFlags = 0;

    // ホスト解決値: read() はこの長さで打ち切り、末尾 fadeFrames をフェードアウトする
    uint64_t totalFrames = kUnknown64; // kUnknown64 = 自然終了まで再生
    uint64_t fadeFrames = 0;

    uint32_t bytesPerSample() const { return (uint32_t)(bitsPerSample < 0 ? -bitsPerSample : bitsPerSample) / 8; }
    uint32_t bytesPerFrame() const { return bytesPerSample() * channels; }
    // 実際のスピーカー配置(speakerConfig=0 なら標準配置)
    uint32_t channelMask() const { return speakerConfig ? speakerConfig : defaultChannelMask(channels); }
};

namespace detail { struct SessionImpl; }

class DecodeSession {
public:
    explicit DecodeSession(std::unique_ptr<detail::SessionImpl> impl);
    ~DecodeSession();
    DecodeSession(const DecodeSession&) = delete;
    DecodeSession& operator=(const DecodeSession&) = delete;

    const PluginInfo& plugin() const;
    uint32_t trackCount() const;

    // 曲番号は 1 ベース。成功すると mediaInfo()/tags() が更新される。
    bool selectTrack(uint32_t number);

    const MediaInfo& mediaInfo() const;
    const TrackTags& tags() const;

    // デコーダのネイティブ形式(mediaInfo() 参照)でインターリーブされたフレームを取得。
    // 戻り値 < frames で演奏終了。ループ曲の打ち切り・フェードアウトは適用済み。
    size_t read(void* buffer, size_t frames);
    bool eof() const;

private:
    std::unique_ptr<detail::SessionImpl> impl_;
};

class PluginRegistry {
public:
    explicit PluginRegistry(HostOptions options = {});
    ~PluginRegistry();
    PluginRegistry(const PluginRegistry&) = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

    // キャッシュの読み書き(cacheFilePath が空なら何もしない)
    bool loadCache();
    bool saveCache() const;

    // ディレクトリを走査して *.kpi を登録する。
    // キャッシュにあり、サイズ・更新日時が一致する DLL はロードしない。
    // maxDepth は KbMedia Player 互換で既定 3(dir 直下 + 3 階層下まで)。
    void scanDirectory(const std::wstring& dir, int maxDepth = 3);

    std::vector<const PluginInfo*> plugins() const;
    // pathOrExt の拡張子に対応する有効なプラグインを優先度順
    // (プラグインバージョン降順 → 更新日時降順)に返す
    std::vector<const PluginInfo*> findByExtension(const std::wstring& pathOrExt) const;

    // タグ列挙。タグ専用オープンを使い、対応プラグインだけをロードする。
    // tagFields には必要なフィールドだけを渡すと高速(既定: タイトル+アーティスト)。
    // attemptsOut に候補プラグインの試行過程(スキップ・失敗・採用)を返せる。
    std::optional<FileTags> scanTags(const std::wstring& file,
                                     uint32_t tagFields = TagFieldTitle | TagFieldArtist,
                                     std::vector<OpenAttempt>* attemptsOut = nullptr);

    // HostOptions::dllSearchDirs のうち、この PluginRegistry が構築時に
    // AddDllDirectory した項目の Cookie(並びは dllSearchDirs と対応)。
    // アプリ登録済み(cookie 指定あり)や登録失敗・API 非対応の項目は nullptr。
    // 解除はアプリの責任で removeDllSearchDirectory() を呼ぶ。
    const std::vector<DllDirCookie>& dllDirCookies() const;

    // デコード用オープン。失敗時は nullptr(errOut に理由)。
    // attemptsOut に候補プラグインの試行過程(失敗・フォールバック・採用)を返せる。
    std::unique_ptr<DecodeSession> openDecode(const std::wstring& file,
                                              const DecodeRequest& req = {},
                                              std::wstring* errOut = nullptr,
                                              std::vector<OpenAttempt>* attemptsOut = nullptr);

    const HostOptions& options() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nokomo
