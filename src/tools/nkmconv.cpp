// nkmconv: KbMedia Player デコーダプラグイン(.kpi)による WAV 一括変換ツール
//
// 使い方:
//   nkmconv [オプション] <入力ファイル...>
//
// オプション:
//   -P, --plugins <dir>   プラグイン検索ディレクトリ(複数指定可)
//                         省略時は exe と同じ場所の Plugins フォルダ
//   -o, --out <dir>       出力先ディレクトリ(省略時は入力と同じ場所)
//   -r, --rate <Hz>       再生周波数の要求値(0=プラグイン任せ)
//   -b, --bits <n>        出力ビット数 8/16/24/32、float は -32/-64、0=ネイティブ
//   -t, --track <n>       変換する曲番号(1ベース)。省略時は全曲
//   --loop <n>            ループ曲のループ回数
//   --fade <ms>           フェードアウト時間(ミリ秒)
//   --default-length <ms> 長さ不明曲のデフォルト長
//   --list                認識しているプラグインを一覧表示して終了
//   --tags                変換せずタグ情報を表示(タグ専用オープンの動作確認)
//   --no-cache            プラグイン情報キャッシュを使わない

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nokomo/nokomo.hpp"
#include "pcm_convert.hpp"
#include "strconv.hpp"
#include "wav_writer.hpp"

using namespace nokomo;
namespace nd = nokomo::detail;

namespace {

std::wstring exeDir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (n == 0) return L".";
    return nd::parentDirOf(std::wstring(buf, n));
}

std::wstring appDataDir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return exeDir();
    return std::wstring(buf, n) + L"\\nokomo2";
}

void printTags(const wchar_t* indent, const TrackTags& tags)
{
    for (const auto& kv : tags.values) {
        fwprintf(stdout, L"%ls%ls = %ls\n", indent, kv.first.c_str(), kv.second.c_str());
    }
    for (const auto& pic : tags.pictures) {
        fwprintf(stdout, L"%ls[画像] %ls %ux%u (%u bytes)\n", indent,
                 pic.mimeType.c_str(), pic.width, pic.height, (unsigned)pic.data.size());
    }
}

std::wstring formatTime(uint64_t frames, uint32_t rate)
{
    if (frames == kUnknown64 || rate == 0) return L"??:??";
    uint64_t sec = frames / rate;
    wchar_t buf[32];
    swprintf(buf, 32, L"%llu:%02llu", sec / 60, sec % 60);
    return buf;
}

struct Options {
    std::vector<std::wstring> pluginDirs;
    std::vector<std::wstring> dllDirs;
    std::wstring outDir;
    DecodeRequest req;
    int32_t outBits = 0; // 0=ネイティブ
    uint32_t track = 0;  // 0=全曲
    bool listOnly = false;
    bool tagsOnly = false;
    bool noCache = false;
    bool forceRf64 = false;
    uint64_t defaultLengthMs = 0;
    std::vector<std::wstring> files;
};

bool convertOne(PluginRegistry& reg, const Options& opt, const std::wstring& input)
{
    std::wstring err;
    auto session = reg.openDecode(input, opt.req, &err);
    if (!session) {
        fwprintf(stderr, L"エラー: %ls: %ls\n", input.c_str(), err.c_str());
        return false;
    }
    fwprintf(stdout, L"%ls (%ls)\n", input.c_str(), session->plugin().description.c_str());

    const uint32_t count = session->trackCount();
    uint32_t first = opt.track ? opt.track : 1;
    uint32_t last = opt.track ? opt.track : count;
    if (first > count) {
        fwprintf(stderr, L"エラー: 曲番号 %u は範囲外(全 %u 曲)\n", first, count);
        return false;
    }

    bool allOk = true;
    for (uint32_t n = first; n <= last; ++n) {
        if (!session->selectTrack(n)) {
            fwprintf(stderr, L"  曲 %u: Select 失敗\n", n);
            allOk = false;
            continue;
        }
        const MediaInfo& mi = session->mediaInfo();
        if (mi.formatType != 0 /*KPI_MEDIAINFO::FORMAT_PCM*/) {
            fwprintf(stderr, L"  曲 %u: PCM 以外の形式(DoP等)は未対応\n", n);
            allOk = false;
            continue;
        }

        // 出力ファイル名
        std::wstring dir = opt.outDir.empty() ? nd::parentDirOf(input) : opt.outDir;
        std::wstring stem = nd::stemOf(input);
        wchar_t suffix[16] = L"";
        if (count > 1) swprintf(suffix, 16, L"_%02u", n);
        std::wstring outPath = dir + L"\\" + stem + suffix + L".wav";

        const int32_t outBits = opt.outBits ? opt.outBits : mi.bitsPerSample;
        nd::WavWriter wav;
        if (!wav.open(outPath, mi.sampleRate, mi.channels, outBits, opt.forceRf64)) {
            fwprintf(stderr, L"  曲 %u: 出力ファイルを開けない: %ls\n", n, outPath.c_str());
            allOk = false;
            continue;
        }

        const std::wstring* title = session->tags().find(L"Title");
        // 打ち切り長が未定(自然終了)でも、曲の長さが分かっていれば表示に使う
        uint64_t displayFrames = mi.totalFrames;
        if (displayFrames == kUnknown64 && mi.length100ns != kUnknown64 && mi.length100ns > 0) {
            displayFrames = mi.length100ns * mi.sampleRate / (1000 * 10000);
        }
        fwprintf(stdout, L"  曲 %u/%u%ls%ls [%uHz %dbit %uch %ls] -> %ls\n",
                 n, count,
                 title ? L": " : L"", title ? title->c_str() : L"",
                 mi.sampleRate, mi.bitsPerSample, mi.channels,
                 formatTime(displayFrames, mi.sampleRate).c_str(),
                 outPath.c_str());

        const size_t chunkFrames = 65536;
        std::vector<uint8_t> buf(chunkFrames * mi.bytesPerFrame());
        std::vector<uint8_t> conv;
        const bool needConvert = (outBits != mi.bitsPerSample);
        if (needConvert) {
            conv.resize(chunkFrames * nd::bytesPerSampleOf(outBits) * mi.channels);
        }

        uint64_t done = 0;
        int lastPercent = -1;
        for (;;) {
            size_t got = session->read(buf.data(), chunkFrames);
            if (got == 0) break;
            const void* out = buf.data();
            if (needConvert) {
                nd::convertSamples(conv.data(), outBits, buf.data(), mi.bitsPerSample,
                                   got * mi.channels);
                out = conv.data();
            }
            if (wav.write(out, got) < got) {
                fwprintf(stderr, L"  警告: 書き込み失敗(ディスクフル?)のため打ち切り\n");
                allOk = false;
                break;
            }
            done += got;
            if (mi.totalFrames != kUnknown64 && mi.totalFrames > 0) {
                int percent = (int)(done * 100 / mi.totalFrames);
                if (percent != lastPercent) {
                    fwprintf(stdout, L"\r  %3d%%", percent);
                    fflush(stdout);
                    lastPercent = percent;
                }
            }
        }
        wav.close();
        if (lastPercent >= 0) fwprintf(stdout, L"\r        \r");
        fwprintf(stdout, L"  完了: %llu フレーム%ls\n", (unsigned long long)wav.framesWritten(),
                 wav.wroteRf64() ? L" (RF64)" : L"");
    }
    return allOk;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);

    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](const wchar_t* what) -> std::wstring {
            if (i + 1 >= argc) {
                fwprintf(stderr, L"エラー: %ls に引数が必要\n", what);
                exit(2);
            }
            return argv[++i];
        };
        if (a == L"-P" || a == L"--plugins") opt.pluginDirs.push_back(next(a.c_str()));
        else if (a == L"--dll-dir") opt.dllDirs.push_back(next(a.c_str()));
        else if (a == L"-o" || a == L"--out") opt.outDir = next(a.c_str());
        else if (a == L"-r" || a == L"--rate") opt.req.sampleRate = (uint32_t)_wtoi(next(a.c_str()).c_str());
        else if (a == L"-b" || a == L"--bits") opt.outBits = _wtoi(next(a.c_str()).c_str());
        else if (a == L"-t" || a == L"--track") opt.track = (uint32_t)_wtoi(next(a.c_str()).c_str());
        else if (a == L"--loop") opt.req.loopCount = (uint32_t)_wtoi(next(a.c_str()).c_str());
        else if (a == L"--fade") opt.req.fadeMs = (uint64_t)_wtoi64(next(a.c_str()).c_str());
        else if (a == L"--default-length") opt.defaultLengthMs = (uint64_t)_wtoi64(next(a.c_str()).c_str());
        else if (a == L"--list") opt.listOnly = true;
        else if (a == L"--tags") opt.tagsOnly = true;
        else if (a == L"--no-cache") opt.noCache = true;
        else if (a == L"--rf64") opt.forceRf64 = true;
        else if (a == L"-h" || a == L"--help") {
            fwprintf(stdout,
                L"nkmconv - KbMedia Player デコーダプラグインによる WAV 変換\n"
                L"使い方: nkmconv [オプション] <入力ファイル...>\n"
                L"  -P, --plugins <dir>   プラグイン検索ディレクトリ(複数可)\n"
                L"  --dll-dir <dir>       プラグインの依存 DLL の検索ディレクトリ(複数可)\n"
                L"  -o, --out <dir>       出力先ディレクトリ\n"
                L"  -r, --rate <Hz>       再生周波数の要求値\n"
                L"  -b, --bits <n>        出力ビット数(8/16/24/32/-32/-64, 0=ネイティブ)\n"
                L"  -t, --track <n>       変換する曲番号(既定: 全曲)\n"
                L"  --loop <n>            ループ回数\n"
                L"  --fade <ms>           フェードアウト時間\n"
                L"  --default-length <ms> 長さ不明曲のデフォルト長\n"
                L"  --list                プラグイン一覧表示\n"
                L"  --tags                タグ情報のみ表示\n"
                L"  --no-cache            プラグイン情報キャッシュを使わない\n"
                L"  --rf64                常に RF64 で出力(既定: 4GiB 超過時に自動昇格)\n");
            return 0;
        }
        else if (!a.empty() && a[0] == L'-') {
            fwprintf(stderr, L"エラー: 不明なオプション %ls\n", a.c_str());
            return 2;
        }
        else opt.files.push_back(a);
    }

    if (opt.pluginDirs.empty()) {
        std::wstring def = exeDir() + L"\\Plugins";
        if (GetFileAttributesW(def.c_str()) != INVALID_FILE_ATTRIBUTES) {
            opt.pluginDirs.push_back(def);
        }
    }
    if (opt.pluginDirs.empty()) {
        fwprintf(stderr, L"エラー: プラグインディレクトリが見つからない(-P で指定)\n");
        return 2;
    }

    HostOptions hostOpt;
    std::wstring appData = appDataDir();
    if (!opt.noCache) hostOpt.cacheFilePath = appData + L"\\plugins.cache";
    hostOpt.configDir = appData + L"\\config";
    // cookie 未指定(nullptr) → 登録は PluginRegistry に任せる。
    // CLI はプロセス終了まで使うので解除(removeDllSearchDirectory)は行わない。
    for (const auto& dir : opt.dllDirs) hostOpt.dllSearchDirs.push_back({dir, nullptr});
    if (opt.defaultLengthMs) hostOpt.defaultLengthMs = opt.defaultLengthMs;

    PluginRegistry reg(hostOpt);
    reg.loadCache();
    for (const auto& dir : opt.pluginDirs) reg.scanDirectory(dir);
    reg.saveCache();

    if (opt.listOnly) {
        for (const PluginInfo* p : reg.plugins()) {
            if (p->valid) {
                fwprintf(stdout, L"%ls\n  %ls%ls\n  対応拡張子: %ls%ls%ls\n",
                         p->path.c_str(), p->description.c_str(),
                         p->isLegacy ? L" [旧仕様]" : L"",
                         p->supportExts.c_str(),
                         p->multiSongExts.empty() ? L"" : L" (複数曲: ",
                         p->multiSongExts.empty() ? L"" : (p->multiSongExts + L")").c_str());
            } else {
                fwprintf(stdout, L"%ls\n  無効: %ls\n", p->path.c_str(), p->invalidReason.c_str());
            }
        }
        return 0;
    }

    if (opt.files.empty()) {
        fwprintf(stderr, L"エラー: 入力ファイルが指定されていない(-h でヘルプ)\n");
        return 2;
    }

    int failures = 0;
    for (const auto& file : opt.files) {
        if (opt.tagsOnly) {
            auto tags = reg.scanTags(file, TagFieldAll);
            if (!tags) {
                fwprintf(stderr, L"エラー: %ls: タグ取得不可\n", file.c_str());
                ++failures;
                continue;
            }
            fwprintf(stdout, L"%ls (%u 曲)\n", file.c_str(), tags->trackCount);
            if (!tags->album.values.empty()) {
                fwprintf(stdout, L"  [全体]\n");
                printTags(L"    ", tags->album);
            }
            for (uint32_t i = 0; i < tags->tracks.size(); ++i) {
                if (tags->tracks[i].values.empty() && tags->trackCount == 1) continue;
                fwprintf(stdout, L"  [曲 %u]\n", i + 1);
                printTags(L"    ", tags->tracks[i]);
            }
        } else {
            if (!convertOne(reg, opt, file)) ++failures;
        }
    }
    return failures ? 1 : 0;
}
