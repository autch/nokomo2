#include <windows.h>
#include "kpi_decoder.h"

///////////////////////////////////////////////////////////////////////////////
//プラグイン側で使うためのヘルパー関数
//(実際に使うのはプラグイン側)
///////////////////////////////////////////////////////////////////////////////
DWORD WINAPI kpi_CreateDecoder(IKpiUnkProvider *pProvider,
                               const KPI_MEDIAINFO *cpRequest,//再生周波数等の要求値が含まれた KPI_MEDIAINFO
                               IKpiFile     *pFile,   //音楽データ
                               IKpiFolder   *pFolder, //音楽データがあるフォルダ
                               IKpiDecoder **ppDecoder)
{
    //先に NULL にしておく(ほかのヘルパー(kpi_CreateConfig/kpi_CreateLocalFile 等)と同じ)
    *ppDecoder = NULL;
    return pProvider->CreateInstance(IID_IKpiDecoder,
                                     (void*)cpRequest, pFile, pFolder,
                                     NULL,
                                     (void**)ppDecoder);
}
///////////////////////////////////////////////////////////////////////////////
DWORD WINAPI kpi_CreateDecoder(IKpiUnknown *pUnknown,  //kpi_CreateInstance の第3引数から直接取得
                               const KPI_MEDIAINFO *cpRequest,//再生周波数等の要求値が含まれた KPI_MEDIAINFO
                               IKpiFile     *pFile,    //音楽データ
                               IKpiFolder   *pFolder,  //音楽データがあるフォルダ
                               IKpiDecoder **ppDecoder)
{//IKpiUnknown(kpi_CreateInstance の第3引数)から直接取得
 //呼び出し時は pUnknown に kpi_CreateInstance の第3引数をそのまま渡すこと
    IKpiUnkProvider *pProvider = NULL;
    //pUnknown が NULL でも落ちないようにする(IKpiUnkProvider を得られない場合と同じ扱い)
    if(pUnknown && pUnknown->QueryInterface(IID_IKpiUnkProvider, (void**)&pProvider) == S_OK){
        DWORD dwRet = kpi_CreateDecoder(pProvider,
                                        cpRequest, pFile, pFolder, ppDecoder);
        pProvider->Release();//pProvider が不要なら ppDecoder を解放後でなくても解放可能
        return dwRet;
    }
    *ppDecoder = NULL;
    return 0;
}
///////////////////////////////////////////////////////////////////////////////
//プラグインがエクスポートする関数を使う際のヘルパー関数
//(実際に使うのは本体側)
///////////////////////////////////////////////////////////////////////////////
HRESULT WINAPI kpi_CreateDecoderModule(pfn_kpiCreateInstance fnCreateInstance,
                                       IKpiDecoderModule **ppModule,
                                       IKpiUnknown *pUnknown)
{
    //先に NULL にしておく。プラグインが失敗時に *ppvObject を書き換えなくても、
    // 呼び出し側の初期値が残らないようにする
    *ppModule = NULL;
    return fnCreateInstance(IID_IKpiDecoderModule, (void**)ppModule, pUnknown);
}
///////////////////////////////////////////////////////////////////////////////

