このディレクトリは Kobarin 氏の KbMedia Player プラグイン SDK
(kpisdk.7z、2017/03/06 版)のコピーです。原本は KbMedia Player 公式サイト
から入手出来ます。

ビルドのため文字コードを CP932(Shift_JIS) から UTF-8 に変換していますが、
内容は無変更です(MSVC は /utf-8 でビルドします。CP932 のままだと非日本語
ロケールの MSVC や clang で正しく処理出来ないため)。

SDK を更新する場合は新しい kpisdk.7z を展開した上で、このディレクトリで

  for f in <展開先>/*; do iconv -f CP932 -t UTF-8 "$f" > "$(basename $f)"; done

として再変換してください。
