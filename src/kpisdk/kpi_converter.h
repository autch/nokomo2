#pragma once
#ifndef kpi_converterH
#define kpi_converterH

#include "kpi.h"
#pragma pack(push, 1) //構造体のアラインメント

#define KPI_CONVERTER_MODULE_VERSION 1 //仕様確定時に 100 とする予定
//2026/11/25 KPI_CONVERTER_MODULE_VERSION を 0 から 1 に変更(v3.17beta7)
//  ・IKpiConverterModule::Open の作業用フォルダ(cszOutputPath)を廃止した。
//    作業用フォルダが必要なプラグインは IKpiWorkFolder(kpi_workfolder.h)で本体から借りる。
//  ・引数の数が変わるので(x86 では古い組み合わせで呼ぶとスタックが壊れる)、IID_IKpiConverter と
//    IID_IKpiConverterModule を変更した。古い本体は新しいプラグインを、新しい本体は古いプラグインを
//    読み込まない(kpi_CreateInstance が E_NOINTERFACE を返す)。
//  修正前: class __declspec(uuid("{7CDB88B3-A7F2-4414-97F5-BF165EFFB7E7}")) IKpiConverter;
//  修正前: class __declspec(uuid("{7FB5494E-9030-4019-9C36-6DCE041DAEEF}")) IKpiConverterModule;
class __declspec(uuid("{5295D84F-CC11-47C8-9A4E-9D2F8ECF453F}")) IKpiConverter;
class __declspec(uuid("{42905C89-70AA-4805-99E2-8AC4F8467F4F}")) IKpiConverterModule;

#define IID_IKpiConverter       __uuidof(IKpiConverter)
#define IID_IKpiConverterModule __uuidof(IKpiConverterModule)

struct KPI_CONVERTER_MODULEINFO
{
    enum{
        TAG_TITLE  = 0x01,   //タイトル取得対応(Media エクスプローラでの表示用情報)
        TAG_EXTRA  = 0x01<<1,//詳細(変換用に開いたときだけ取得可能な情報)対応(今の本体は使っていない)
    };
    DWORD cb;                //=sizeof(KPI_CONVERTER_MODULEINFO)
    DWORD dwModuleVersion;   //=KPI_CONVERTER_MODULE_VERSION
    DWORD dwPluginVersion;   //プラグインのバージョン
    DWORD dwMultipleInstance;//KPI_MULTINST_INFINITE/ZERO/ONE/UNIQUE
    GUID  guid;              //プラグイン識別子
    const wchar_t *cszDescription; //表示名
    const wchar_t *cszCopyright;   //著作権(複数名いる場合は改行文字可)
    const wchar_t *cszSupportExts; //対応拡張子('/' で区切る)(例：".rcp/.g36")(拡張子も含める)
    const wchar_t *cszToExts;      //変換先拡張子(".mid" など)(/ で区切って複数指定は不可)
    void  *pvReserved1;            //予約(must be NULL)
    void  *pvReserved2;            //予約(must be NULL)
    DWORD dwSupportTagInfo;        //タグ取得対応(TAG_TITLE/EXTRA の組み合わせ)
    DWORD dwSupportConfig;         //IKpiConfig による設定の読み書きに未対応なら 0
    DWORD dwReserved[4];           //must be 0
    //cszToExts 以外はほぼ KPI_DECODER_MODULEINFO と同じ
    //cszToExts は 1 つだけ。/ で区切って複数に対応するかは未定。
    //(2026/11/25 修正。以前は「今のところ .mid のみ対応」としていたが、消し忘れだった)
    //・本体は、変換結果の IKpiFile の GetFileName の拡張子で、変換結果をどう開くかを決める。
    //  .mid なら MIDI として扱い、それ以外はその拡張子のデコーダプラグインで開く
    //  (例えば .s98 に変換すれば、.s98 に対応したデコーダプラグインで再生される)。
    //  変換結果の名前は cszToExts の拡張子で終わるようにすること。
    //・変換は 1 段だけ。変換結果を、さらに別の変換プラグインに通すことはしない。
    //・変換プラグインが対応する拡張子のファイルは、1 ファイル複数曲として扱われない
    //  (同じ拡張子をデコーダプラグインが 1 ファイル複数曲として名乗っていても)。
    //  変換元が 1 ファイル複数曲の形式(例えば NSF の曲番号を選んで MIDI にする)は、今の仕様では扱えない
    //  (そういう形式を想定していなかったため。必要になったら口を足す)。
};

class IKpiConverter : public IKpiUnknown
{
public:
    virtual BOOL WINAPI Convert(IKpiFile  **ppDestFile, //変換先
                                IKpiTagInfo *pTagInfo,
                                DWORD dwTagGetFlags)=0;
    virtual DWORD  WINAPI UpdateConfig(void *pvReserved)=0; //設定が変更されたら呼ばれる(下を参照)
    //
    //BOOL WINAPI Convert(IKpiFile  **ppDestFile, IKpiTagInfo *pTagInfo, DWORD dwTagGetFlags);
    //変換実行(変換元ファイルは IKpiConverterModule::Open で渡される IKpiFile)
    //ppDestFile == NULL の場合はタグ取得のみで変換はしない(速度優先)
    //変換に実ファイルが必要な場合は、IKpiWorkFolder(kpi_workfolder.h)で本体から作業フォルダを
    //借りて、その中にテンポラリを作成する(IKpiConverterModule::Open の説明を参照)
    //(2026/11/25 修正。以前は IKpiConverterModule::Open の cszOutputPath 以下に作成していた)
    //*ppDestFile の実装が面倒な場合、kpi_unarc.h/kpi_unarc.cpp をプロジェクトに追加して
    //kpi_CreateLocalFile/kpi_CreateMemoryFile を呼ぶことが出来る
    //kpi_CreateLocalFile で渡す実ファイル、kpi_CreateMemoryFile で渡すメモリバッファは
    //IKpiConverter::Release() が呼ばれるまで使えるようにすること
    //(本体側で実ファイルやメモリバッファのコピーはされない)
    //本体は *ppDestFile を Release してから IKpiConverter を Release する(2026/11/25 明記)
    //
    //本体の呼び方(2026/11/25 明記)
    //・タグ取得(Media エクスプローラの一覧など)：TAG_TITLE がなければ呼ばれない。あれば
    //  IKpiConverterModule::Open → Convert(NULL, pTagInfo, ...) → Release。
    //  ・Convert(NULL) の戻り値は使われない(タグが取れなくても開けるファイルとして扱われる)。
    //  ・Open の失敗は「開けないファイル」として扱われる(書庫の中のファイルを演奏リストに加えない等)。
    //  ・一覧を出すたびに呼ばれるので、Convert(NULL) では変換しないこと(外部のプログラムを動かさない等)。
    //・再生：IKpiConverterModule::Open → Convert(&pFile, pTagInfo, KPI_TAGGET_FLAG_ALL)。
    //  ・Open・Convert が失敗したとき、*ppDestFile を返さなかったとき、変換結果が発音のない SMF
    //    だったときは、本体は変換結果を使わず、変換元のファイルのまま同じ拡張子のデコーダ/演奏
    //    プラグインで開く(そういうプラグインがなければ再生失敗)。
    //  ・再生のたびに変換される(本体は変換結果を覚えておかない)。Convert は有限の時間で戻ること。
    //    (変換結果がとても大きくなる形式は、上限を決めて超えたら失敗を返す)
    //・UpdateConfig：設定が変更された時点では呼ばれない。そのインスタンスで次に Convert が呼ばれる
    //  直前に呼ばれる(変換し終わったインスタンスには、事実上呼ばれない)。
};

class IKpiConverterModule : public IKpiUnknown
{
public:
    virtual void  WINAPI GetModuleInfo(const KPI_CONVERTER_MODULEINFO **ppInfo)=0;
    virtual BOOL  WINAPI Open(IKpiFile   *pFile,  //変換元ファイル
                              IKpiFolder *pFolder,//変換元ファイルがあるフォルダ
                              IKpiConverter **ppConverter)=0;
    //2026/11/25 修正(作業用フォルダ cszOutputPath を廃止。IKpiWorkFolder で借りる。KPI_CONVERTER_MODULE_VERSION の説明を参照)
    virtual BOOL  WINAPI EnumConfig(IKpiConfigEnumerator *pEnumerator)=0;
    virtual DWORD WINAPI ApplyConfig(const wchar_t *cszSection,//セクション名
                                     const wchar_t *cszKey,    //キー名
                                     INT64  nValue, //変更後の値(BOOL/INT の場合)
                                     double dValue, //変更後の値(FLOAT の場合)
                                     const wchar_t *cszValue)=0;//変更後の値(文字列)
    //BOOL WINAPI Open(IKpiFile *pFile, IKpiFolder *pFolder, IKpiConverter **ppConverter);
    //pFile を変換可能なら *ppConverter に IKpiConverter のインスタンスを設定して TRUE を返す
    //変換に必要なプログラム(コンバータソフト等)がない等、変換出来ない場合は FALSE を返す
    //変換に実ファイル(変換の結果に限らず、変換の過程で一時的に必要になるものを含む)が必要な場合は、
    //IKpiWorkFolder(kpi_workfolder.h)で本体から作業フォルダを借りて、その中に作成すること
    //・作業フォルダは IKpiConverter ごとに借りて、IKpiConverter の Release まで持っておく
    //  (変換結果の実ファイルは IKpiConverter が Release されるまで存在していなければならないため)
    //  タグ取得だけ(Convert の ppDestFile が NULL)のときは借りなくて済むよう、
    //  実際に変換するときに借りることを推奨
    //・作業フォルダの中のファイルは、最後の Release で本体が中身ごと消すので、プラグイン側で
    //  削除する必要はない(削除しても良い)。子プロセスに使わせている場合は、その終了を待ってから
    //  Release すること
    //・外部のプログラムや DLL が Unicode のパスに対応していない場合は KPI_WORKFOLDER_FLAG_ANSI を付けて借りる
    //・借りられなかった場合(ANSI で表せない場所など)は、実ファイルが必要な変換だけを失敗させる
    //  (タグの取得やメモリ上で済む変換は続けて良い)
    //・作業フォルダの中に、変換元のデータに書かれた名前でファイルを作る場合は、名前にパスの区切り
    //  (\ / :)や .. が含まれていないことを確かめること(作業フォルダの外に書き込まないため)
    //(2026/11/25 修正。以前は第3引数の作業用フォルダ cszOutputPath 以下に作成していた)
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
