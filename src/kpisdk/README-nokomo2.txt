このディレクトリは Kobarin 氏の KbMedia Player プラグイン SDK
(2026/10/31 版、KbMedia Player 3.17beta3 付属)のコピーです。原本は
KbMedia Player 公式サイトから入手出来ます。

ファイルは原本のまま無変更です(この README-nokomo2.txt のみ追加)。
SDK は 2026/10/31 版から文字コードが UTF-8(BOM 付き)になったため、
変換せずにそのまま置いています(MSVC は /utf-8 でビルドします)。

SDK を更新する場合は新しい SDK のファイルをこのディレクトリにそのまま
上書きコピーしてください。原本が CP932 だった 2017/03/06 版以前の SDK
を使う場合は

  for f in <展開先>/*; do iconv -f CP932 -t UTF-8 "$f" > "$(basename $f)"; done

として UTF-8 に変換する必要があります。

kpi_converter.h/.cpp(変換プラグイン)は nokomo2 では使用していません
(SDK 一式として同梱しているのみ)。
