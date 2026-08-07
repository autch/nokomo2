# nokomo2

KbMedia Player 現行プラグイン仕様(KPI)の**ホスト側**(プラグインを使う側)C++ 実装。
デコーダプラグイン(.kpi)をロードして PCM デコード・タグ列挙を行うライブラリ
`nokomo2` と、WAV 一括変換 CLI ツール `nkmconv` からなる。

旧仕様(kmp_pi.h)のみ対応の Delphi 製「のこもDecoder」の後継。
現在のスコープは**デコーダプラグインのみ**(アーカイブ/演奏/デバイスは対象外)。
現行仕様(kpi_CreateInstance)と旧仕様(kmp_GetTestModule)の両方に対応する。

## 設計の要点

KbMedia Player 本体と同じ動作方針をとる:

- **プラグイン情報のキャッシュと遅延ロード**
  走査時は DLL をロードして `GetModuleInfo` だけ呼び、対応拡張子等のスナップ
  ショットを取って即アンロードする(SDK が明示的に想定している動作)。
  取得した情報は DLL のサイズ+更新日時をキーにキャッシュファイルへ永続化し、
  次回以降は変更がない限り DLL をロードしない。
  使用時にロードした DLL は LRU で数本(`HostOptions::maxResidentModules`)常駐させる。
- **タグ専用オープン**
  タグ列挙(`PluginRegistry::scanTags`)は `Open(cpRequest=NULL)` +
  `Select(…, ppMediaInfo=NULL, 最小限のフラグ)` のタグ取得専用プロトコルを使う。
  `KPI_DECODER_MODULEINFO::TAG_TITLE` が立っていないプラグインにはタグ専用
  オープンを行わない(仕様)。複数曲ファイルは `Select(0)` で全体情報も取る。

  なお KbMedia Player 本体では、大量のファイルを列挙する際に「どのファイルの
  タグをどう取得するか」の判断(拡張子マッチ+同一拡張子の別フォーマットを
  見分けるごく軽いフォーマットチェック)と一般的なタグ形式の解析を本体側に
  実装しており、1ファイルごとにプラグインを呼び出してはいない。プラグイン
  仕様はこのすみわけの上に成り立っている。nokomo2 でも同じ整理とし、
  `scanTags` は「プラグインに問い合わせる側のパス」に徹する。大量列挙時の
  トリアージと一般形式の高速なタグ読みは nokomo2 を使うアプリの責務であり、
  その判断材料として DLL をロードせずに引ける `findByExtension()` と
  `PluginInfo::supportTagInfo` を提供する。
- **ループ曲の終端処理はホストの責務**
  `KPI_MEDIAINFO` の qwLength/qwLoop/qwFadeOut/dwLoopCount から総フレーム数を
  解決し、`DecodeSession::read` が打ち切りとフェードアウトを適用する。
- **同時インスタンス数制限**
  `dwMultipleInstance` を尊重する。kbrunkpi.exe 相当の別プロセス化は行わず、
  上限到達時はブロックして待つ(KPI_MULTINST_ZERO は同時1個として扱う)。
- **旧仕様プラグインはアダプタで対応**
  kmp_GetTestModule のみをエクスポートする旧仕様プラグインは、KMPMODULE を
  IKpiDecoderModule/IKpiDecoder にラップして新仕様として扱う。
  Render(バイト)/SetPosition(ミリ秒・曲番号*1000 で選曲)/dwLength=曲数*1000/
  dwReserved1(無限ループ)等の旧プロトコルはアダプタ内で変換する。
  旧仕様の Open は ANSI ファイル名を要求するため、パスが ANSI で表現出来ない
  場合は IKpiFile::GetRealFileA が仕様どおりテンポラリへコピーを作成する。
  kmp_GetTestTagInfo をエクスポートするプラグインはタグ列挙にも使用する。

## 構成

ビルドに必要なものはすべて src/ 以下にあり、git の管理対象も src/(とこの
README、.gitignore)のみ。KbMedia Player 本体や SDK・サンプルソース等の
配布物は公式サイトから入手する(このリポジトリには含めない)。

```
src/
  CMakeLists.txt
  include/nokomo/nokomo.hpp  公開 API(PluginRegistry / DecodeSession / FileTags)
  kpisdk/                    KPI SDK のコピー(UTF-8 変換のみ、内容は無変更。
                             原本は公式配布の kpisdk.7z(CP932))
  lib/host_objects.*         ホスト実装: IKpiFile(ローカルファイル+mmap GetBuffer),
                             IKpiFolder(実ディレクトリ), IKpiTagInfo, IKpiConfig(INI),
                             IKpiUnkProvider
  lib/module.*               DLL ライフサイクル(probe / LRU 常駐 / インスタンス数制限)
  lib/registry.cpp           走査・キャッシュ・拡張子マッチング・scanTags/openDecode
  lib/decode.cpp             DecodeSession(選曲・Render ループ・打ち切り・フェード)
  lib/kmp_adapter.*          旧仕様(kmp_pi.h)アダプタ(KMPMODULE を IKpiDecoderModule/
                             IKpiDecoder にラップ。以降のコードは新旧を区別しない)
  lib/wav_writer.*           RIFF WAVE / RF64 出力(int=PCM, float=IEEE_FLOAT+fact)
  lib/pcm_convert.hpp        サンプル形式変換(8/16/24/32int, 32/64float)
  lib/kpi_mingw_uuid.h       MinGW/clang クロス検証用 __uuidof シム(MSVC では無効)
  tools/nkmconv.cpp          CLI ツール
```

## ビルド

本番: CMake + MSVC(VS2022)。リポジトリルートから:

```
cmake -S src -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

x86 プラグインを使う場合は `-A Win32` でビルドした x86 版ホストが必要
(プロセス内ロードのためアーキテクチャが一致している必要がある)。

開発時の検証は Linux 上で clang + MinGW ヘッダによるクロスコンパイルが可能:

```
cd src
clang++ --target=x86_64-w64-mingw32 -fms-extensions -std=c++17 \
  -DUNICODE -D_UNICODE -DNOMINMAX -Iinclude -Ikpisdk -Ilib -c lib/*.cpp tools/nkmconv.cpp
clang++ --target=x86_64-w64-mingw32 -municode -static -o nkmconv.exe *.o
```

Wine で実プラグイン(KbMedia Player v3.16β7 配布物の x64 版 Plugins 一式)に
対する動作確認済み:
kbflac のタグ取得・ビットパーフェクトデコード、kbvgm のループ曲変換
(ループ回数・フェード適用)、約30プラグインの probe とキャッシュ。

## nkmconv の使い方

```
nkmconv -P <Pluginsフォルダ> [オプション] <入力ファイル...>
  -o <dir>        出力先(既定: 入力と同じ場所)
  -b <bits>       出力ビット数 8/16/24/32、float は -32/-64(既定: ネイティブ)
  -r <Hz>         再生周波数の要求値
  -t <n>          曲番号(既定: 全曲。複数曲は 入力名_NN.wav で出力)
  --loop <n> --fade <ms> --default-length <ms>
  --list          プラグイン一覧
  --tags          タグ情報のみ表示
  --dll-dir <dir> 依存 DLL(kbzlib.dll 等)の検索ディレクトリ
  --no-cache      キャッシュ無効
  --rf64          常に RF64 で出力
```

### 依存 DLL の検索ディレクトリと所有権

KbMedia Player の配布形態では、プラグインの依存 DLL(kbzlib.dll 等)は本体 exe
と同じフォルダに置かれる。nokomo2 を使うアプリでこれを解決するのが
`HostOptions::dllSearchDirs` で、内部では AddDllDirectory +
`LOAD_LIBRARY_SEARCH_USER_DIRS` を使う。

AddDllDirectory は**プロセスグローバルな状態**のため、登録の所有権はアプリが持つ:

- `DllSearchDir::cookie == nullptr` の項目は `PluginRegistry` が構築時に登録し、
  取得した Cookie を `dllDirCookies()` で返す。**Registry が解除することはない**。
  解除するかどうか・いつ解除するかはアプリの判断で、
  `nokomo::removeDllSearchDirectory(cookie)` を呼ぶ。
- `cookie != nullptr` の項目はアプリが登録済みとみなし、Registry は何もしない
  (アプリは `nokomo::addDllSearchDirectory(dir)` で自分で登録出来る)。

Add/RemoveDllDirectory は Win8 未満に存在しないため、ラッパ関数は動的解決して
おり非対応 OS では nullptr / false を返す(直接インポートすると古い環境で exe の
ロード自体が失敗するので、必ずラッパを使うこと)。

### 大きなファイルと RF64

出力は EBU Tech 3306 の定石に従い、`RIFF` + ダミー `JUNK` チャンクで書き始めて
close 時に 4GiB を超えていたら `RF64` + `ds64` へ自動昇格する。
4GiB 未満のファイルは従来どおりの純粋な WAV となり互換性を失わない
(`JUNK` チャンクが 1 個入るだけで、標準的なリーダはこれを無視する)。
ハイレゾ長時間録音などで RIFF の 32bit サイズ制限は問題にならない。

キャッシュは `%APPDATA%\nokomo2\plugins.cache`、プラグイン設定は
`%APPDATA%\nokomo2\config\*.ini` に保存される。

## ライブラリ API の例

```cpp
nokomo::PluginRegistry reg({});
reg.loadCache();
reg.scanDirectory(L"C:\\kbmp\\Plugins");
reg.saveCache();

// タグ列挙(エクスプローラ風表示用: 高速・DLL は必要時のみロード)
if (auto tags = reg.scanTags(path)) { /* tags->tracks[i].find(L"Title") */ }

// WAV 変換
auto s = reg.openDecode(path, {});
for (uint32_t n = 1; n <= s->trackCount(); ++n) {
    s->selectTrack(n);
    const auto& mi = s->mediaInfo(); // sampleRate/bits/channels/totalFrames
    std::vector<uint8_t> buf(4096 * mi.bytesPerFrame());
    while (size_t got = s->read(buf.data(), 4096)) { /* write */ }
}
```

## FLAC 出力対応の検討メモ(ライセンス確認済み)

libFLAC / libFLAC++ は Xiph.org の BSD 系ライセンス(3条項BSD相当、
COPYING.Xiph)であり、静的/動的リンクとも配布形態を問わず利用可能。
条件は著作権表示と免責文言をドキュメント等に同梱することのみ。
GPL が適用されるのは `flac`/`metaflac` コマンドラインツール(と一部プラグイン)
だけで、ライブラリには及ばない(FLAC ソース同梱の README に明記)。
libogg も同様の BSD 系。したがって nokomo2 への FLAC 出力組み込みは
ライセンス上の障害なし。なお FLAC は整数 PCM(4〜32bit)のみで float は
格納出来ないため、float 出力の可逆保存が必要なら WavPack(32bit float 対応)
が候補になる。

## 既知の制限 / 今後

- `IKpiTagInfo::GetTagInfo`(本体の ID3v2/APE 等解析ルーチンの提供)は
  `(DWORD)-1`(未対応)を返す。タグ形式の解析はプラグインホストの責務では
  なくホスト(アプリ)側の判断で持つものと整理し、nokomo2 では意図的に
  対象外とする。このためタグ解析を本体に委ねるプラグイン
  (kbmad、kpid2p の DSF 等)ではプラグイン経由でのタグ取得が出来ない
- 旧仕様プラグインの kmp_Config(設定ダイアログ)は呼び出さない
- 旧仕様の複数曲ファイルは曲ごとの長さ・タグを取得出来ない(旧仕様自体の制限)
- アーカイブ/演奏/デバイスプラグインは対象外(仕様も未確定のため)
- `kpi_CreateDecoder`(プラグインからプラグインを使う)は未提供
  (本体も kbrunkpi 経由では未実装)
- シーク API(`DecodeSession::seek`)は未実装
- キャッシュに存在しなくなった DLL のエントリが残る(実害なし)

## SDK について

`src/kpisdk/` は Kobarin 氏の KbMedia Player プラグイン SDK(kpisdk.7z、
2017/03/06版)をビルド用に UTF-8 変換したコピー(内容は無変更。詳細は
src/kpisdk/README-nokomo2.txt)。CP932 の原本は KbMedia Player 公式サイト
から入手出来る。SDK ドキュメントによりプラグインを使用するアプリケーションの
作成・配布は許可されている(詳細はプラグインソースコード一式の readme.txt)。
