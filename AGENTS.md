# AGENTS.md — 開発者・コーディングエージェント向けメモ

利用者向けの情報は [README.md](README.md)、設計上の決定は [DESIGN.md](DESIGN.md)
を参照。この文書はリポジトリの運用ルールと開発ワークフローをまとめる。

## リポジトリ運用

- git の管理対象は `src/` とトップレベルの README.md / DESIGN.md / AGENTS.md /
  .gitignore のみ。作業ツリーにはこのほかに KbMedia Player 本体の配布物、
  SDK の原本(CP932)、プラグインのサンプルソース等の参照資料を置くが、
  これらはバージョン管理しない(公式サイト等から入手するもの)
- **git 管理対象のファイルに、管理外のローカルディレクトリのパスを書かない**。
  クローンした第三者には存在せず意味不明になる。参照資料に言及するときは
  「KbMedia Player 公式配布の kpisdk.7z」のように入手元・配布物名で書く
- コミットメッセージは日本語

## ソース構成

```
src/
  CMakeLists.txt
  include/nokomo/nokomo.hpp  公開 API(PluginRegistry / DecodeSession / FileTags)
  kpisdk/                    KPI SDK のコピー(UTF-8 変換のみ、内容は無変更)
  lib/host_objects.*         ホスト実装: IKpiFile(遅延オープン+mmap GetBuffer),
                             IKpiFolder(実ディレクトリ), IKpiTagInfo, IKpiConfig(INI),
                             IKpiUnkProvider
  lib/module.*               DLL ライフサイクル(probe / LRU 常駐 / インスタンス数制限)
  lib/registry.cpp           走査・キャッシュ・拡張子マッチング・scanTags/openDecode
  lib/decode.cpp             DecodeSession(選曲・Render ループ・打ち切り・フェード)
  lib/kmp_adapter.*          旧仕様(kmp_pi.h)アダプタ(KMPMODULE を IKpiDecoderModule/
                             IKpiDecoder にラップ。以降のコードは新旧を区別しない)
  lib/wav_writer.*           RIFF WAVE / RF64 出力
  lib/pcm_convert.hpp        サンプル形式変換(8/16/24/32int, 32/64float)
  lib/kpi_mingw_uuid.h       MinGW/clang クロス検証用 __uuidof シム(MSVC では無効)
  tools/nkmconv.cpp          CLI ツール
```

## 仕様の典拠

- 仕様の解釈で迷った場合は、**現行 SDK(src/kpisdk/ の kpi*.h と kmp_pi.h
  2016/10/03版)を正**とする。旧仕様の再入要件などは時期により変遷しており、
  過去のホスト実装(のこもDecoder 等)や個々のプラグインの挙動は実例・参考で
  あって典拠ではない
- SDK ヘッダの原本は CP932。src/kpisdk/ には UTF-8 変換したコピーを置く
  (MSVC は /utf-8 でビルドするため。更新手順は src/kpisdk/README-nokomo2.txt)

## 開発時の検証ワークフロー(Linux)

MSVC 実機がなくても、clang + MinGW ヘッダでクロスコンパイルして Wine で
実プラグインに対する動作確認が出来る。

```
cd src
clang++ --target=x86_64-w64-mingw32 -fms-extensions -std=c++17 \
  -DUNICODE -D_UNICODE -DNOMINMAX -Iinclude -Ikpisdk -Ilib -c lib/*.cpp tools/nkmconv.cpp
clang++ --target=x86_64-w64-mingw32 -municode -static -o nkmconv.exe *.o
```

- `-fms-extensions` は SDK ヘッダの `__declspec(uuid())` / `__uuidof` に必要。
  MinGW ターゲットでは `__uuidof` が `__mingw_uuidof<T>()` になるため、
  lib/kpi_mingw_uuid.h(`__CRT_UUID_DECL`)が GUID 定義を供給する
  (MinGW gcc でも同様に動く)
- **公開ヘッダ(nokomo.hpp)の構造体レイアウトを変えたら必ず全ファイルを
  リビルドする**こと(差分ビルドで一部の .o が旧レイアウトのまま残ると
  実行時に破壊的に壊れる)
- 32bit の検証: i686 ツールチェーンが無くても
  `clang++ --target=i686-w64-mingw32 -fsyntax-only` +x86_64 用 MinGW ヘッダで
  構文検証出来る。x86 では WINAPI(stdcall)と thiscall が区別されるため、
  インターフェース実装の呼び出し規約漏れはここでコンパイルエラーとして出る
- Wine での動作確認は KbMedia Player 配布物の x64 版 Plugins 一式に対して行う
  (kbflac: タグ+ビットパーフェクト検証、kbvgm: ループ曲+依存 DLL(--dll-dir)、
  MFC 依存の一部プラグインは Wine では動かない)
- 旧仕様のプロトコル検証には MinGW でテストプラグイン(.kpi)を自作出来る
  (kmp_pi.h は C ABI なので g++ -shared で足りる。複数曲・排他オープン・
  タグ通知などの挙動を意図的に作り込んでホスト側の経路を叩く)

## 実装上の注意(ハマりどころ)

- KPI の参照カウント規約: メソッド引数で渡されたインターフェースは±0、
  QueryInterface/OpenFile 等で獲得したものは Release が 1 回多い。
  `GetRealFileW/A`・`GetBuffer` の結果をメソッド外で使うなら AddRef が必要
- `IKpiDecoderModule` のメソッドは排他的に呼ぶ(LoadedModule::callLock)。
  デコーダインスタンスは単一スレッド使用
- SessionImpl のメンバ宣言順は破棄順(逆順)に意味がある: decoder → folder →
  file → slot → module の順で解放されること
- `Select` の `*ppMediaInfo` と `IKpiTagInfo` の寿命は次のメソッド呼び出しまで。
  即コピーする
- 旧仕様プラグインは実ファイルを共有モード 0(排他)で開くことがある。
  ホストはファイルハンドルを保持しない(LocalKpiFile の遅延オープン)
- AddDllDirectory / RemoveDllDirectory は Win8 未満に存在しない。静的インポート
  すると古い環境で exe 自体が起動しなくなるため、必ず動的解決のラッパを使う
- `Date`/乱数など環境依存の API は使っていないが、`towlower` はロケール非依存の
  ASCII 折り畳みとして使っている(拡張子・パスキーの比較用)
