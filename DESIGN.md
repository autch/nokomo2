# nokomo2 設計文書

設計上の決定とその理由をまとめる。使い方・ビルド方法は [README](README.md) を参照。

## 使命と役割分担

nokomo2 の使命は「KbMedia Player のプラグインシステムのホスト側を実装し、
アプリケーションに API として開放する」ことである。KbMedia Player 本体が
担っている機能のうち、プラグインの呼び出し規約に属さないもの(タグ形式の解析、
大量列挙時のトリアージ等)は nokomo2 の対象外とし、nokomo2 を使うアプリの
責務とする。

## プラグイン情報のキャッシュと遅延ロード

KbMedia Player 本体と同じ動作方針をとる。

- 走査時は DLL をロードして `GetModuleInfo` だけ呼び、対応拡張子等のスナップ
  ショットを取って即アンロードする(SDK が明示的に想定している動作)。
- 取得した情報は DLL のサイズ+更新日時をキーにキャッシュファイルへ永続化し、
  次回以降は変更がない限り DLL をロードしない。
- キャッシュのキーはパス正規化(小文字化+`lexically_normal`)した文字列。
  同じ DLL がパス表記の揺れで重複登録されるのを防ぐ。読み込み時に相対パスの
  旧エントリは破棄し、保存時に実在しない DLL のエントリは持ち越さない。
- 使用時にロードした DLL は LRU で数本(`HostOptions::maxResidentModules`)
  常駐させる。

## タグ専用オープンとタグ取得の責務分担

- タグ列挙(`PluginRegistry::scanTags`)は `Open(cpRequest=NULL)` +
  `Select(…, ppMediaInfo=NULL, 最小限のフラグ)` のタグ取得専用プロトコルを使う。
  `KPI_DECODER_MODULEINFO::TAG_TITLE` が立っていないプラグインにはタグ専用
  オープンを行わない(仕様)。ただし 1 ファイル複数曲の拡張子(cszMultiSongExts)
  は、曲数を知る手段が Open の戻り値しかないため dwSupportTagInfo に関わらず
  開く(SDK 2026/11/25版で明記。本体の動作も同じ)。複数曲ファイルは
  `Select(0)` で全体情報も取る。
- Select の成否は「戻り値 == 要求した曲番号」で判定する。`Select(0)` の失敗
  (0xFFFFFFFF 等)は本体と同じくファイルを開けなかったものとみなし、次の候補
  プラグインへフォールバックする(失敗にも 0 を返す古いプラグインは成功・
  タグなしの扱いになる)。曲番号は Open が返した曲数を超えて渡さない(ホストの
  保証事項)。
- KbMedia Player 本体では、大量のファイルを列挙する際に「どのファイルのタグを
  どう取得するか」の判断(拡張子マッチ+同一拡張子の別フォーマットを見分ける
  ごく軽いフォーマットチェック)と一般的なタグ形式の解析を本体側に実装して
  おり、1ファイルごとにプラグインを呼び出してはいない。プラグイン仕様はこの
  すみわけの上に成り立っている。nokomo2 でも同じ整理とし、`scanTags` は
  「プラグインに問い合わせる側のパス」に徹する。大量列挙時のトリアージと
  一般形式の高速なタグ読みは nokomo2 を使うアプリの責務であり、その判断材料
  として DLL をロードせずに引ける `findByExtension()` と
  `PluginInfo::supportTagInfo` を提供する。
- 同様の理由で `IKpiTagInfo::GetTagInfo`(本体の ID3v2/APE 等解析ルーチンの
  提供)は**意図的に対象外**とし、`(DWORD)-1`(未対応)を返す。タグ形式の
  解析はプラグインホストの責務ではなくアプリ側の判断で持つものと整理した。
  このためタグ解析を本体に委ねるプラグイン(kbmad、kpid2p の DSF 等)では
  プラグイン経由でのタグ取得が出来ない。対応するとしても最後の最後。

## 候補プラグインのフォールバック

同一拡張子に複数のプラグインが対応する場合、優先度順(プラグインバージョン
降順→更新日時降順)に試行し、失敗したら次の候補へフォールバックする
(KbMedia Player の優先度規則と同じ)。試行過程は `OpenAttempt` として
`openDecode`/`scanTags` の出力引数で取得でき、アプリ側で可視化出来る。

## ループ曲の終端処理

ループ曲(`qwLoop != 0`)の演奏終了処理はホストの責務(仕様)。
`KPI_MEDIAINFO` の qwLength/qwLoop/qwFadeOut/dwLoopCount から総フレーム数を
kpi_decoder.h 記載の手順で解決し、`DecodeSession::read` が打ち切りと
フェードアウト(リニア)を適用する。長さ不明・無限ループ曲は
`HostOptions::defaultLengthMs`/`defaultFadeMs` で打ち切る。

## 同時インスタンス数制限

`dwMultipleInstance` を尊重する。kbrunkpi.exe 相当の別プロセス化は行わず、
上限到達時はブロックして待つ。KPI_MULTINST_ZERO(常に別プロセス)は、
このホスト自体が KbMedia Player とは別プロセスであることから同時1個として
扱う。KPI_MULTINST_UNIQUE のプロセス横断の一意性は保証しない(プロセス内 1 個)。

## 旧仕様(kmp_pi.h)プラグインへの対応

アダプタ方式をとる。`kmp_GetTestModule` のみをエクスポートする旧仕様
プラグインは、KMPMODULE を IKpiDecoderModule/IKpiDecoder にラップして
新仕様として扱い、レジストリ以降のコードは新旧を区別しない。

- プロトコル変換: Render(バイト⇔サンプル)、SetPosition(ミリ秒⇔サンプル、
  曲番号*1000 で選曲⇔Select)、dwLength=曲数*1000⇔曲数、
  dwReserved1(無限ループ)⇔qwLoop=-1(ホストが打ち切り+フェード)、
  dwReentrant⇔dwMultipleInstance。
- `kmp_GetTestTagInfo` をエクスポートするプラグインは TAG_TITLE 相当とみなし
  タグ列挙にも使用する(IKmpTagInfo→IKpiTagInfo ブリッジ)。
- 旧仕様の Open は ANSI ファイル名を要求する。`IKpiFile::GetRealFileA` は
  1) 無損失 ANSI 変換(変換→逆変換の往復一致で判定。WC_NO_BEST_FIT_CHARS は
  ACP=UTF-8 環境でフラグ自体がエラーになるため使わない)、
  2) 8.3 短縮名(コピー不要で同一フォルダの参照ファイルも解決出来る)、
  3) テンポラリコピー(仕様記載のフォールバック。Release 時に削除)
  の順で ANSI 名を用意する。
- **ホストはファイルの OS ハンドルを保持しない**(遅延オープン)。旧仕様
  プラグインは実ファイル名を共有モード 0(排他)で開くことがあり
  (auPiECE 等)、ホストがハンドルを保持していると共有違反で Open が失敗する。
  KbMedia Player の旧仕様経路はファイル名を渡すだけでハンドルを持たないため、
  それと同じ条件にする。`LocalKpiFile` は最初の Read/Seek/GetBuffer まで
  ハンドルを開かず、GetSize も未オープン時は属性 API で取得する。
- 旧仕様の kmp_Config(設定ダイアログ)は呼び出さない。
- 旧仕様の複数曲ファイルは曲ごとの長さ・タグを取得出来ない(旧仕様自体の制限)。

## 依存 DLL の検索ディレクトリと所有権

KbMedia Player の配布形態では、プラグインの依存 DLL(kbzlib.dll 等)は本体
exe と同じフォルダに置かれる。nokomo2 を使うアプリでこれを解決するのが
`HostOptions::dllSearchDirs` で、内部では AddDllDirectory +
`LOAD_LIBRARY_SEARCH_USER_DIRS` を使う(プラグイン自身のフォルダとアプリの
フォルダは常に検索される)。

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

## WAV / RF64 出力

出力は EBU Tech 3306 の定石に従い、`RIFF` + ダミー `JUNK` チャンク
(ds64 と同サイズ)で書き始めて、close 時に 4GiB を超えていたら `RF64` +
`ds64` へ昇格する。4GiB 未満のファイルは従来どおりの純粋な WAV となり
互換性を失わない(`JUNK` チャンクが 1 個入るだけで、標準的なリーダはこれを
無視する)。ハイレゾ長時間録音などで RIFF の 32bit サイズ制限は問題にならない。
int は WAVE_FORMAT_PCM、float(-32/-64)は WAVE_FORMAT_IEEE_FLOAT + fact。
3ch 以上、またはスピーカー配置(`KPI_MEDIAINFO::dwSpeakerConfig`)が
チャンネル数の標準配置と異なる場合は WAVE_FORMAT_EXTENSIBLE とし、
dwChannelMask に配置を入れる(1/2ch の標準配置は従来どおりで出力は変わらない)。
dwSpeakerConfig は本体と同じく、立っているビット数が dwChannels と合わない値や
定義のないビット(0x20000 より上)を含む値を 0(標準配置)として扱う。
dwChannels は 1〜8(SDK 2026/11/18版)で、9ch 以上の曲は Select 失敗とする。

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

## ホストが提供するオブジェクトの安全側の約束事

SDK 2026/10/31版(KbMedia Player 3.17beta3)で、未定義動作や不測の事故を
減らす方向に約束事が見直された(インターフェースは無変更)。ホスト側の
対応は以下のとおり。

- **文字列取得のバッファ不足**: `IKpiConfig::GetStr`、`IKpiFile::GetFileName`、
  `IKpiFolder::GetFolderName/EnumFiles` は、バッファが足りない場合に途切れた
  文字列ではなく空文字列を入れて必要サイズを返す。途切れたパスが想定外の場所を
  指す事故を避けるため(GetStr は旧仕様では切り捨てだった)。
- **設定のセクション名・キー名**: 大文字と小文字を区別しない。比較は
  `CompareStringOrdinal(..., TRUE)`(ロケール非依存の Unicode 序数比較)で行い、
  ASCII 以外の文字も畳み込む。INI 上の綴りは最初に登録されたものを保持し、
  読み込み時に綴り違いの重複キーがあれば後の値を採用する。
- **規約違反の名前**: SDK は名前の前後の空白・制御文字・キー名の `= [ ]` を
  禁止したが、違反時のホスト動作は規定していない。nokomo2 は拒否せず受け入れる
  (INI 保存時にエスケープするため注入や読み戻しの破綻は起きず、拒否すると
  旧来のプラグインの設定が黙って消えるため)。
- **`IKpiFile::Read` の短い読み込み**: 戻り値が dwSize 未満の場合(EOF・
  エラー・中断を含む)、残りのバッファを 0 で埋める。本体(KbMedia Player)と
  同じ挙動にする多重防御で、プラグインがこれを前提にしてはならない点は SDK の
  とおり。プラグインが提供する IKpiFile(アーカイブ/変換プラグイン)を読む
  経路は nokomo2 には無い。

SDK 2026/11/25版(KbMedia Player 3.17beta7)で追加・明記された事項への対応:

- **IKpiConfig の保存上限**: SetStr は 16KB(終端を含む)、SetBin は 1MB。
  超える場合は本体と同じく何もしない(保存しない)。
- **IKpiMessenger**(利用者へのお知らせ): `HostOptions::onPluginMessage` が
  設定されている場合だけ提供する(未設定なら「対応していない本体」と同じく
  NULL を返す)。通知元プラグインは kpi_CreateInstance 中の取得(仕様上の取得
  時期)でのみ特定出来、それ以外の時期・スレッドの取得では空になる。本文の
  改行・タブ以外の制御文字は空白にし、長さは 4096 文字で切る。ハンドラは
  プラグインのスレッドから同期的に呼ぶ(本体は写して即座に戻るが、nokomo2 は
  キューを持たないのでハンドラ側がすぐ戻る前提)。
- **IKpiWorkFolder**(プラグイン用の作業フォルダ): `HostOptions::workFolderRoot`
  (既定 `%TEMP%\nokomo2`)の下に呼ぶたびに新しい空フォルダを作る。
  KPI_WORKFOLDER_FLAG_ANSI は親パスが CP_ACP で無損失に表せなければ失敗、
  未定義ビットも失敗。最後の Release で中身ごと削除する(リンクは辿らず
  リンク自体だけ消す)。使用中で消せなかった分は PluginRegistry の破棄時
  (全モジュール解放後)に再試行する。

## 仕様解釈の典拠

仕様の解釈で迷った場合は、現行 SDK(src/kpisdk/ の kpi*.h 2026/11/25版と
kmp_pi.h 2016/10/03版)を正とする。旧仕様の再入要件などは時期により変遷しており、
過去のホスト実装や個々のプラグインの挙動は実例・参考であって典拠ではない。

## 既知の制限 / 今後

- ホスト側タグ解析(`GetTagInfo`)は対象外(前述)
- アーカイブ/演奏/デバイスプラグインは対象外(仕様も未確定のため)
- `kpi_CreateDecoder`(プラグインからプラグインを使う)は未提供。
  理由と対応に必要なことは次節
- IKpiVideoWindow(映像・ウィンドウ)は対象外(WAV 変換には不要)
- シーク API(`DecodeSession::seek`)は未実装

### kpi_CreateDecoder を提供していない理由

本体は v3.17beta7 時点で kbrunkpi 経由以外では実装済みで、動きは
kpisdk_for_ai_decoder.md 6-14 に明記されている。仕様上の不明点は
もうないが、nokomo2 の作り(複数セッションの並列実行、別プロセス
実行なし)では次の障壁を解決してからでないと提供出来ないため、未対応
としている。HostRoot::CreateInstance は IID_IKpiDecoder に 0 を返す
(呼んだプラグインからは「失敗」に見え、仕様上許される結果)。

- **入れ子のロックによるデッドロック**: `IKpiDecoderModule::Open` は
  そのモジュールの `callLock` を持ったまま呼ぶため、Open の中から
  kpi_CreateDecoder が呼ばれると、別モジュールの `callLock` を入れ子で
  取ることになる。
  - 同一スレッド上の循環(A→A、A→B→A)は仕様でも失敗とされているので、
    スレッドごとの入れ子スタックで検出して失敗にすればよい
  - 別スレッドで「A が B を使う」と「B が A を使う」が同時に起きると
    ABBA デッドロックになる。これは仕様上の循環ではなく正当な呼び出し。
    KbMedia Player 本体はモジュールのメソッドをメインスレッドで呼ぶので
    起きないが、nokomo2 では起きうる。取る順序の固定、タイムアウト付きの
    取得で失敗させる、などの方針を決める必要がある
- **インスタンス数の上限**: `acquireSlot()` は空きを待つため、入れ子の
  中で待つと同様に固まりうる。本体は上限超過時に kbrunkpi 経由の
  デコーダを返すが、nokomo2 には別プロセス実行がない。入れ子のときは
  待たずに失敗にする(本体も KPI_MULTINST_UNIQUE の使用中は失敗)
- **返すデコーダのプロキシ**: 使われる側の LoadedModule とスロットを
  デコーダの寿命まで保持し、Open・Select・Seek・Render の間は入れ子
  スタックに積んでおく必要がある。また cpRequest の nBitsPerSample が
  0 でなければ本体がそのビット数に変換して返す決まりなので、Render で
  バッファ長の換算と pcm_convert による変換を行う
- **HostRoot から registry への参照**: 使うプラグインの選択(拡張子で
  優先度最上位の 1 つだけ。Open が失敗してもほかは試さない)に
  registry の候補一覧が要るが、今の HostContext からはたどれない

次の点は本体と揃えられないが、割り切りで済む。

- .mid を本体の MIDI 設定に従って処理してから渡す動きは再現しない
- 使われる側が旧仕様プラグインで、呼んだプラグインの渡した IKpiFile が
  実ファイル名(GetRealFileA)を返さない場合は失敗する

実例は本体同梱の kbbms(BMS)。キー音の .wav(無ければ同名の
.mp3 / .ogg)をホストから受け取った IKpiFolder で開き、nBitsPerSample=16
を要求して 16bit 以外が返ると読み込みを諦めるので、ビット数の変換は
必須。kbbms は IKpiVideoWindow にも対応しているが、仕様上この対応は
任意で、CreateVideoWindow を呼ばないホストでは映像のない曲として演奏
されるため、kpi_CreateDecoder さえ提供すれば音声の変換は出来る見込み。
