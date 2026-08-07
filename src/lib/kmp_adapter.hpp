#pragma once
// 旧プラグイン仕様(kmp_pi.h / kmp_GetTestModule)のアダプタ。
// KMPMODULE を IKpiDecoderModule / IKpiDecoder にラップし、レジストリ以降の
// コードが新旧仕様を区別せずに扱えるようにする。
//
// 新旧仕様の対応関係:
//   Open(ANSIファイル名)       ← IKpiFile::GetRealFileA(必要ならテンポラリコピー)
//   Render(バイト単位)         ← Render(サンプル単位)を変換
//   SetPosition(ミリ秒)        ← Seek(サンプル単位)を変換
//   SetPosition(曲番号*1000)   ← Select(複数曲時の選曲)
//   dwLength=曲数*1000         ← Open の戻り値(曲数)
//   dwReserved1=1(無限ループ)  ← KPI_MEDIAINFO::qwLoop=-1(ホストが打ち切り+フェード)
//   dwReentrant                ← KPI_DECODER_MODULEINFO::dwMultipleInstance
//   kmp_GetTestTagInfo         ← IKpiTagInfo へブリッジ(TAG_TITLE 相当)

#include <windows.h>
#include <tchar.h>

#include "kpi.h"
#include "kpi_decoder.h"
#include "kpi_impl.h"
#include "kmp_pi.h"

#include "kpi_mingw_uuid.h" // MinGW/clang クロスビルド検証用(MSVC では無効)

#include <string>

namespace nokomo::detail {

// KMPMODULE をラップした IKpiDecoderModule を作成する。
// kmp_GetTestModule は本体仕様どおりロード中 1 回だけ呼び出すこと(呼び出し済みの
// KMPMODULE* を渡す)。KMPMODULE と関数ポインタは FreeLibrary まで有効であること。
// dwVersion の検証は呼び出し側で行うこと(不一致なら Init を呼ばずにアンロード)。
// 戻り値は参照カウント 1。KMPMODULE::Init は作成時、Deinit は破棄時に呼ばれる。
IKpiDecoderModule* createKmpAdapterModule(KMPMODULE* mod, pfnGetTagInfo tagFn);

} // namespace nokomo::detail
