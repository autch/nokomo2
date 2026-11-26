#include <windows.h>
#include "kpi.h"
#include "kpi_impl.h"
#include "kpi_message.h" //2026/11/22 追加(kpi_CreateMessenger をここに移した)
#include "kpi_workfolder.h" //2026/11/23 追加(kpi_CreateWorkFolder)

class KbKpiNullConfig : public KbKpiUnknownImpl<IKpiConfig>
{//常にデフォルト値を返す IKpiConfig
 //本体からの kpi_CreateConfig が失敗したときにこのクラスのインスタンスを返す
public:
    void   WINAPI SetInt(const wchar_t *cszSection, const wchar_t *cszKey, INT64  nValue){}
    INT64  WINAPI GetInt(const wchar_t *cszSection, const wchar_t *cszKey, INT64  nDefault){return nDefault;}
    void   WINAPI SetFloat(const wchar_t *cszSection, const wchar_t *cszKey, double dValue){}
    double WINAPI GetFloat(const wchar_t *cszSection, const wchar_t *cszKey, double dDefault){return dDefault;}
    void   WINAPI SetStr(const wchar_t *cszSection, const wchar_t *cszKey, const wchar_t *cszValue){}
    DWORD  WINAPI GetStr(const wchar_t *cszSection, const wchar_t *cszKey, wchar_t *pszValue,
                         DWORD dwSize, const wchar_t *cszDefault){
        // ・バッファが足りない場合は途中で切らずに空文字列にする(IKpiConfig::GetStr の仕様)
        //   (以前は lstrcpynW で途中まで切った文字列を返していた)
        // ・cszDefault が NULL の場合は空文字列として扱う(以前は pszValue が終端されなかった)
        // ・dwSize が sizeof(wchar_t) 未満の場合は pszValue に書き込まない
        if(!cszDefault){
            cszDefault = L"";
        }
        const DWORD dwLen = (DWORD)lstrlenW(cszDefault);
        const DWORD dwRet = (dwLen+1)*sizeof(wchar_t);
        //pszValue が NULL なら書き込まない(本体の IKpiConfig::GetStr と同じ)
        if(pszValue && dwSize >= sizeof(wchar_t)){
            if(dwRet <= dwSize){
                for(DWORD i = 0; i <= dwLen; i++){//終端の 0 も含めてコピー
                    pszValue[i] = cszDefault[i];
                }
            }
            else{
                pszValue[0] = 0;
            }
        }
        return dwRet;
    }
    //binary
    void   WINAPI SetBin(const wchar_t *cszSection, const wchar_t *cszKey, const BYTE *pBuffer, DWORD dwSize){};
    DWORD  WINAPI GetBin(const wchar_t *cszSection, const wchar_t *cszKey, BYTE *pBuffer, DWORD dwSize){
        //pBuffer が NULL なら書き込まない(本体の IKpiConfig::GetBin と同じ)
        if(pBuffer){ZeroMemory(pBuffer, dwSize);}
        return 0;}
};
///////////////////////////////////////////////////////////////////////////////
//IKpiUnkProvider のヘルパー関数(実際に使うのはプラグイン側)
//IKpiUnkProvider に初めからないのは、機能を追加する度に
//派生クラスを作らないといけなくなるのを回避するため
///////////////////////////////////////////////////////////////////////////////
DWORD WINAPI kpi_CreateConfig(IKpiUnkProvider *pProvider,
                              const GUID *pGUID,
                              DWORD *pdwPlatform,
                              IKpiConfig **ppConfig)
{
    if(pdwPlatform){
    //pProvider が仕様変更等により IID_IKpiConfig に対応しない
    //場合、ppvObj 以外の未知の引数は値が変更されないため、
    //変更が期待される引数に対して不定な値にならないように
    //予め何らかの値を代入しておく
        *pdwPlatform = (DWORD)-1;//未対応の場合は -1 を返す
    }
    //ppConfig が NULL でも落ちないようにする
    // kpi.h の仕様「設定項目が特になく、pdwPlatform だけが目的の場合は(ppConfig に) NULL を渡しても良い」
    // (以前は確かめずに *ppConfig に書き込んでいた)
    // ppConfig が NULL の場合は NULL のまま CreateInstance に渡し(本体は *pdwPlatform だけを返す)、
    // 常にデフォルト値を返す IKpiConfig も作らない
    // v3.17beta5 より前の本体は ppvObj が NULL だと何もしないので、*pdwPlatform は -1 のまま
    if(ppConfig){
        *ppConfig = NULL;
    }
    DWORD dwRet =pProvider->CreateInstance(IID_IKpiConfig,
                                     (void*)pGUID,
                                     (void*)pdwPlatform,
                                     NULL, NULL,
                                     (void**)ppConfig);
    if(!ppConfig){
        return dwRet;
    }
    if(*ppConfig){
        return dwRet;
    }
    //プラグインの呼び出し側(通常は KbMedia Player 本体)が IKpiConfig に未対応
    //常にデフォルト値を返す IKpiConfig を返す
    *ppConfig = new KbKpiNullConfig;
    if(pdwPlatform){
        *pdwPlatform = (DWORD)-1;
    }
    return 1;
}
///////////////////////////////////////////////////////////////////////////////
DWORD WINAPI kpi_CreateConfig(IKpiUnknown *pUnknown, //kpi_CreateInstance の第3引数
                              const GUID *pGUID,
                              DWORD *pdwPlatform,
                              IKpiConfig **ppConfig)
{//IKpiUnknown(kpi_CreateInstance の第3引数)から直接取得
 //呼び出し時は pUnknown に kpi_CreateInstance の第3引数をそのまま渡すこと
    IKpiUnkProvider *pProvider = NULL;
    //pUnknown が NULL でも落ちないようにする(IKpiUnkProvider を得られない場合と同じ扱い)
    if(pUnknown && pUnknown->QueryInterface(IID_IKpiUnkProvider, (void**)&pProvider) == S_OK){
        DWORD dwRet = kpi_CreateConfig(pProvider, pGUID, pdwPlatform, ppConfig);
        pProvider->Release();//pProvider が不要なら ppConfig を解放後でなくても解放可能
        return dwRet;
    }
    //プラグインの呼び出し側(通常は KbMedia Player 本体)が IKpiConfig に未対応
    //常にデフォルト値を返す IKpiConfig を返す
    //ppConfig が NULL の場合は作らない(上の kpi_CreateConfig と同じ)
    if(ppConfig){
        *ppConfig = new KbKpiNullConfig;
    }
    if(pdwPlatform){
        *pdwPlatform = (DWORD)-1;
    }
    //1 を返す(上の kpi_CreateConfig で本体が IKpiConfig を返さなかった場合と同じにする)
    return 1;
}
///////////////////////////////////////////////////////////////////////////////
DWORD WINAPI kpi_CreateMessenger(IKpiUnknown *pUnknown, //kpi_CreateInstance の第3引数
                                 IKpiMessenger **ppMessenger)
{//IKpiMessenger を取得する。*ppMessenger が NULL なら、本体がこの口に対応していない(kpi_message.h)
 //(2026/11/22 kpi_message.h の inline 関数からここに移した。中身は変えていない)
    if(!ppMessenger){
        return 0;
    }
    *ppMessenger = NULL;
    if(!pUnknown){
        return 0;
    }
    IKpiUnkProvider *pProvider = NULL;
    if(pUnknown->QueryInterface(IID_IKpiUnkProvider, (void**)&pProvider) != S_OK || !pProvider){
        return 0;
    }
    DWORD dwRet = pProvider->CreateInstance(IID_IKpiMessenger,
                                            NULL, NULL, NULL, NULL,
                                            (void**)ppMessenger);
    pProvider->Release();//pProvider は *ppMessenger の解放前に解放して良い
    return dwRet;
}
///////////////////////////////////////////////////////////////////////////////
DWORD WINAPI kpi_CreateWorkFolder(IKpiUnkProvider *pProvider,
                                  DWORD dwFlags,
                                  IKpiWorkFolder **ppWorkFolder)
{//プラグイン用の作業フォルダを作る(kpi_workfolder.h。2026/11/23 追加)
 //失敗したら 0 を返し、*ppWorkFolder は NULL(本体がこの口に対応していない場合を含む)
    if(!ppWorkFolder){
        return 0;
    }
    *ppWorkFolder = NULL;
    if(!pProvider){
        return 0;
    }
    pProvider->CreateInstance(IID_IKpiWorkFolder,
                              (void*)(DWORD_PTR)dwFlags, NULL, NULL, NULL,
                              (void**)ppWorkFolder);
    //戻り値ではなく *ppWorkFolder で判断する(IKpiUnkProvider::CreateInstance の説明を参照)
    return *ppWorkFolder ? 1 : 0;
}
///////////////////////////////////////////////////////////////////////////////
DWORD WINAPI kpi_CreateWorkFolder(IKpiUnknown *pUnknown, //kpi_CreateInstance の第3引数
                                  DWORD dwFlags,
                                  IKpiWorkFolder **ppWorkFolder)
{
    if(!ppWorkFolder){
        return 0;
    }
    *ppWorkFolder = NULL;
    if(!pUnknown){
        return 0;
    }
    IKpiUnkProvider *pProvider = NULL;
    if(pUnknown->QueryInterface(IID_IKpiUnkProvider, (void**)&pProvider) != S_OK || !pProvider){
        return 0;
    }
    DWORD dwRet = kpi_CreateWorkFolder(pProvider, dwFlags, ppWorkFolder);
    pProvider->Release();//pProvider は *ppWorkFolder の解放前に解放して良い
    return dwRet;
}
/*
///////////////////////////////////////////////////////////////////////////////
DWORD WINAPI kpi_CreateDecoder(IKpiUnkProvider *pProvider,
                               const KPI_MEDIAINFO *cpRequest,//再生周波数等の要求値が含まれた KPI_MEDIAINFO
                               IKpiFile     *pFile,   //音楽データ
                               IKpiFolder   *pFolder, //音楽データがあるフォルダ
                               IKpiDecoder **ppDecoder)
{
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
    if(pUnknown->QueryInterface(IID_IKpiUnkProvider, (void**)&pProvider) == S_OK){
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
    return fnCreateInstance(IID_IKpiDecoderModule, (void**)ppModule, pUnknown);
}
///////////////////////////////////////////////////////////////////////////////
*/
