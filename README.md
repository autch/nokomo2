# nokomo2

KbMedia Player のデコーダプラグイン(.kpi)を再生以外の用途から使うための
ライブラリと、それを使った WAV 一括変換ツール。

- **ライブラリ `nokomo2`** — プラグインのロード・タグ列挙・PCM デコードを行う
  ホスト側(プラグインを使う側)の C++ 実装。アプリケーションに組み込んで使う
- **CLI ツール `nkmconv`** — プラグインで開ける音楽ファイルを WAV/RF64 に
  一括変換するコマンドラインツール

現行プラグイン仕様(kpi_CreateInstance)と旧仕様(kmp_GetTestModule)の両方の
デコーダプラグインに対応する。アーカイブ/演奏/デバイスプラグインは対象外。

設計上の決定とその理由は [DESIGN.md](DESIGN.md) を参照。

## ビルド

CMake + MSVC(VS2022)。リポジトリルートから:

```
cmake -S src -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

x86 プラグインを使う場合は `-A Win32` でビルドした x86 版が必要
(プロセス内ロードのためプラグインとアーキテクチャが一致している必要がある)。

## nkmconv の使い方

```
nkmconv -P <Pluginsフォルダ> [オプション] <入力ファイル...>
  -o <dir>        出力先(既定: 入力と同じ場所)
  -b <bits>       出力ビット数 8/16/24/32、float は -32/-64(既定: ネイティブ)
  -r <Hz>         再生周波数の要求値
  -t <n>          曲番号(既定: 全曲。複数曲は 入力名_NN.wav で出力)
  --loop <n>      ループ曲のループ回数
  --fade <ms>     フェードアウト時間
  --default-length <ms>  長さ不明曲のデフォルト長
  --list          プラグイン一覧
  --tags          変換せずタグ情報を表示
  --dll-dir <dir> 依存 DLL(kbzlib.dll 等)の検索ディレクトリ
                  (通常は KbMedia Player 本体の x64/x86 フォルダを指定)
  --no-cache      プラグイン情報キャッシュを使わない
  --rf64          常に RF64 で出力(既定は 4GiB 超過時に自動昇格)
```

- `-P` には KbMedia Player の Plugins フォルダをそのまま指定出来る
  (作者別のサブフォルダ構成も本体と同じ規則で検索される)
- プラグイン情報は `%APPDATA%\nokomo2\plugins.cache` にキャッシュされ、
  2 回目以降の起動では変更のないプラグインをロードしない。
  プラグイン設定は `%APPDATA%\nokomo2\config\*.ini` に保存される
- 同一拡張子に複数のプラグインが対応する場合は優先度順に試行し、失敗したら
  次の候補にフォールバックする。不採用となった候補は
  「候補 xxx.kpi vNN → 理由」の形式で表示される
- 出力 WAV が 4GiB を超える場合は自動的に RF64 になる(それ未満は純粋な WAV)

## ライブラリ API の例

公開ヘッダは `src/include/nokomo/nokomo.hpp` のみ。

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

`PluginRegistry` はスレッドセーフ。`DecodeSession` は 1 インスタンスにつき
単一スレッドから使用し、生成元の `PluginRegistry` より長生きさせないこと。

## SDK・ライセンスについて

`src/kpisdk/` は Kobarin 氏の KbMedia Player プラグイン SDK(kpisdk.7z、
2017/03/06版)をビルド用に UTF-8 変換したコピー(内容は無変更。詳細は
src/kpisdk/README-nokomo2.txt)。CP932 の原本は KbMedia Player 公式サイト
から入手出来る。SDK ドキュメントによりプラグインを使用するアプリケーションの
作成・配布は許可されている(詳細はプラグインソースコード一式の readme.txt)。
