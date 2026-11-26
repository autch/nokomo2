このディレクトリは Kobarin 氏の KbMedia Player プラグイン SDK
(2026/11/25 版、KbMedia Player 3.17beta7 付属)のコピーです。原本は
KbMedia Player 公式サイトから入手出来ます。

ファイルは原本のまま無変更です(この README-nokomo2.txt のみ追加)。
SDK は 2026/10/31 版から文字コードが UTF-8(BOM 付き)になったため、
変換せずにそのまま置いています(MSVC は /utf-8 でビルドします)。

SDK を更新する場合は新しい SDK のファイルをこのディレクトリにそのまま
上書きコピーしてください。原本が CP932 だった 2017/03/06 版以前の SDK
を使う場合は

  for f in <展開先>/*; do iconv -f CP932 -t UTF-8 "$f" > "$(basename $f)"; done

として UTF-8 に変換する必要があります。

なお 2026/11/25 版の kpi.h 冒頭の版表記は「2026/11/18版(本体Ver 3.17beta6 用)」
のままですが、更新履歴の最新は 2026/11/25(v3.17beta7)です。

kpi_converter.h/.cpp(変換プラグイン)・kpi_unarc.h/.cpp・kpi_player.h/.cpp・
kpi_device.h/.cpp・kpi_video_window.h は nokomo2 では使用していません
(SDK 一式として同梱しているのみ)。kpisdk_for_ai*.md は SDK 付属の AI 向け補足
(ヘッダに書かれていない前提・本体の動きの細部)で、これも原本のままです。
