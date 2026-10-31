#pragma once
#ifndef kpi_converterH
#define kpi_converterH

#include "kpi.h"
#pragma pack(push, 1) //構造体のアラインメント

#define KPI_CONVERTER_MODULE_VERSION 0 //仕様確定時に 100 とする予定
class __declspec(uuid("{7CDB88B3-A7F2-4414-97F5-BF165EFFB7E7}")) IKpiConverter;
class __declspec(uuid("{7FB5494E-9030-4019-9C36-6DCE041DAEEF}")) IKpiConverterModule;

#define IID_IKpiConverter       __uuidof(IKpiConverter)
#define IID_IKpiConverterModule __uuidof(IKpiConverterModule)

struct KPI_CONVERTER_MODULEINFO
{
    enum{
        TAG_TITLE  = 0x01,   //タイトル取得対応(Media クスプローラでの表示用情報)
        TAG_EXTRA  = 0x01<<1,//詳細(変換用に開いたときだけ取得可能な情報)対応
    };
    DWORD cb;                //=sizeof(KPI_CONVERTER_MODULEINFO)
    DWORD dwModuleVersion;   //=KPI_CONVERTER_MODULE_VERSION
    DWORD dwPluginVersion;   //プラグインのバージョン
    DWORD dwMultipleInstance;//KPI_MULTINST_INFINITE/ZERO/ONE/UNIQUE
    GUID  guid;              //プラグイン識別子
    const wchar_t *cszDescription; //表示名
    const wchar_t *cszCopyright;   //著作権(複数名いる場合は改行文字可)
    const wchar_t *cszSupportExts; //対応拡張子('/' で区切る)(例：".rcp/.g36")(拡張子も含める)
    const wchar_t *cszToExts;      //変換先拡張子(".mid")(今のところ .mid のみ対応)(/ で区切って複数指定は不可)
    void  *pvReserved1;            //予約(must be NULL)
    void  *pvReserved2;            //予約(must be NULL)
    DWORD dwSupportTagInfo;        //タグ取得対応(TAG_TITLE/EXTRA の組み合わせ)
    DWORD dwSupportConfig;         //IKpiConfig による設定の読み書きに未対応なら 0
    DWORD dwReserved[4];           //must be 0
    //cszToExts 以外はほぼ KPI_DECODER_MODULEINFO と同じ
    //cszToExts は今のところ .mid のみ対応。/ で区切って複数に対応するかは未定。
};

class IKpiConverter : public IKpiUnknown
{
public:
    virtual BOOL WINAPI Convert(IKpiFile  **ppDestFile, //変換先
                                IKpiTagInfo *pTagInfo,
                                DWORD dwTagGetFlags)=0;
    virtual DWORD  WINAPI UpdateConfig(void *pvReserved)=0; //設定が変更されたら呼ばれる
    //
    //BOOL WINAPI Convert(IKpiFile  **ppDestFile, IKpiTagInfo *pTagInfo, DWORD dwTagGetFlags);
    //変換実行(変換元ファイルは IKpiConverterModule::Open で渡される IKpiFile)
    //ppDestFile == NULL の場合はタグ取得のみで変換はしない(速度優先)
    //変換に実ファイルが必要な場合、IKpiConverterModule::Open の cszOutputPath 以下の
    //フォルダにテンポラリを作成すると、IKpiConverter->Release() の後に KbMedia Player 
    //によって自動的に削除される
    //cszOutputPath のフォルダは Convert の呼び出し前に本体によって作成される為、
    //既に存在していることを前提にして良い
    //Open 呼び出し時点で cszOutputPath のフォルダは空である
    //*ppDestFile の実装が面倒な場合、kpi_unarc.h/kpi_unarc.cpp をプロジェクトに追加して
    //kpi_CreateLocalFile/kpi_CreateMemoryFile を呼ぶことが出来る
    //kpi_CreateLocalFile で渡す実ファイル、kpi_CreateMemoryFile で渡すメモリバッファは
    //IKpiConverter::Release() が呼ばれるまで使えるようにすること
    //(本体側で実ファイルやメモリバッファのコピーはされない)
};

class IKpiConverterModule : public IKpiUnknown
{
public:
    virtual void  WINAPI GetModuleInfo(const KPI_CONVERTER_MODULEINFO **ppInfo)=0;
    virtual BOOL  WINAPI Open(IKpiFile   *pFile,  //変換元ファイル
                              IKpiFolder *pFolder,//変換元ファイルがあるフォルダ
                              const wchar_t *cszOutputPath,//出力先(or作業用)フォルダ(末尾の \ を含む)
                              IKpiConverter **ppConverter)=0;
    virtual BOOL  WINAPI EnumConfig(IKpiConfigEnumerator *pEnumerator)=0;
    virtual DWORD WINAPI ApplyConfig(const wchar_t *cszSection,//セクション名
                                     const wchar_t *cszKey,    //キー名
                                     INT64  nValue, //変更後の値(BOOL/INT の場合)
                                     double dValue, //変更後の値(FLOAT の場合)
                                     const wchar_t *cszValue)=0;//変更後の値(文字列)
    //BOOL WINAPI Open(IKpiFile *pFile, IKpiFolder *pFolder, IKpiTagInfo *pTagInfo, 
    //                 const wchar_t *cszOutputPath, IKpiConverter **ppConverter);
    //pFile を変換可能なら *ppConverter に IKpiConverter のインスタンスを設定して TRUE を返す
    //変換に必要なプログラム(コンバータソフト等)がない等、変換出来ない場合は FALSE を返す
    //変換に実ファイル(変換の結果に限らず、変換の過程で一時的に必要になるものを含む)が必要な場合、
    //cszOutputPath 以下にテンポラリを作成することを推奨
    //cszOutputPath 以下に作成されたファイルは KbMedia Player によって自動的に削除される
    //(*ppConverter を Release した後に削除)
    //Open 以外は IKpiDecoderModule 等とほぼ同じ
};

///////////////////////////////////////////////////////////////////////////////
//プラグインがエクスポートする関数
//typedef HRESULT (WINAPI *pfn_kpiCreateInstance)(REFIID riid,      //IKpiXXXModule の IID
//                                                void **ppvObject, //IKpiXXXModule の戻り値
//                                                IKpiUnknown *pUnknown);
//HRESULT WINAPI kpi_CreateInstance(REFIID riid, void **ppvObject, IKpiUnknown *pUnknown);
//IKpiDecoderModule と同じ(kpi.h を参照)
//riid に IID_IKpiConverterModule が渡される
//必要であれば pUnknown から IKpiConfig 等を取得すること
//kpi_CreateConfig 等のヘルパー関数を呼ぶ場合は kpi.cpp もプロジェクトに追加する

//プラグイン使用側用のヘルパー関数(プラグイン作成には不要)
//プラグイン使用者(通常は KbMedia Player)が呼び出す場合は kpi_convert.cpp も
//プロジェクトに追加する
///////////////////////////////////////////////////////////////////////////////
extern HRESULT WINAPI kpi_CreateConverterModule(pfn_kpiCreateInstance fnCreateInstance,
                                            IKpiConverterModule **ppModule, IKpiUnknown *pUnknown);
#pragma pack(pop) //構造体のアラインメント
#endif
