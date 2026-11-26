#pragma once
#ifndef kpi_deviceH
#define kpi_deviceH

#include "kpi.h"
#include "kpi_decoder.h" //KPI_MEDIAINFO::FORMAT_PCM/FORMAT_DOP

#pragma pack(push, 1) //構造体のアラインメント

///////////////////////////////////////////////////////////////////////////////
/*デバイスプラグイン
  DirectSound や waveOut/ASIO/WASAPI 等のデバイスに PCM を出力。
  出力先をファイルとすれば、WAVE ファイル出力プラグインや、他形式への
  コンバータも作成可能。(本体がきちんと対応しないと実用性が低くなりそうだが)

  デバイスプラグインがエクスポートする関数
  typedef HRESULT (WINAPI *pfn_kpiCreateInstance)(REFIID riid,    //IKpiXXXModule の IID
                                                void **ppvObject, //IKpiXXXModule の戻り値
                                                IKpiUnknown *pUnknown);
  HRESULT WINAPI kpi_CreateInstance(REFIID riid, void **ppvObject, IKpiUnknown *pUnknown);
  riid に IID_IKpiOutDeviceModule が渡される。
  必要であれば pUnknown から IKpiConfig 等を取得すること。
  kpi_CreateConfig 等のヘルパー関数を呼ぶ場合は kpi.cpp もプロジェクトに追加する。

  *ppvObject にインスタンスを渡す場合は、適切な型にキャストすること。
  例えば、

    if(IsEqualIID(riid, IID_IKpiOutDeviceModule)){
        CKpiXXXOutDeviceModule *pModule = new CKpiXXXOutDeviceModule;
        // *ppvObject = pModule; //NG
        *ppvObject = (IKpiOutDeviceModule*)pModule; //OK
    }

  のようにすること。CKpiXXXOutDeviceModule が複数のクラスから派生している場合、
  適切なポインタが渡されない可能性がある。
*/
///////////////////////////////////////////////////////////////////////////////
#define KPI_OUTDEVICE_MODULE_VERSION 9 //仕様確定時に 100 とする予定

class __declspec(uuid("{59AFA38B-17A0-4DDC-952A-3AED1901226D}")) IKpiOutDevice;
class __declspec(uuid("{01CCD9F7-A698-4305-B2D1-8E98D0736471}")) IKpiOutDeviceModule;
class __declspec(uuid("{42C2C87A-96FA-4DE6-8E23-A539F33B6DC5}")) IKpiDeviceEnumerator;

#define IID_IKpiOutDevice        __uuidof(IKpiOutDevice)
#define IID_IKpiOutDeviceModule  __uuidof(IKpiOutDeviceModule)
#define IID_IKpiDeviceEnumerator __uuidof(IKpiDeviceEnumerator)

///////////////////////////////////////////////////////////////////////////////
class IKpiOutDevice : public IKpiUnknown
{
public:
    virtual BOOL   WINAPI Output(BYTE *pOutput, int nSizeSample)=0;
    virtual void   WINAPI Wait(void)=0;
    virtual UINT64 WINAPI GetPosition(void)=0;
    virtual void   WINAPI Stop(void)=0;
    virtual void   WINAPI Pause(void)=0;
    virtual void   WINAPI Restart(void)=0;
    virtual BOOL   WINAPI SetVolume(int nVolume)=0;
    virtual BOOL   WINAPI SetRate(int nRate)=0;
    virtual DWORD  WINAPI UpdateConfig(void *pvReserved)=0;
/*
    BOOL WINAPI Output(BYTE *pOutput, int nSizeSample);
      デバイスに PCM を出力

      IKpiOutDeviceModule::Open によるインスタンス作成時、内部で
      nBufferSizeSample の 2 倍以上のバッファを確保しておくこと。
      nSizeSample はサンプル単位で通常は nBufferSizeSample と等しい。
      nSizeSample < nBufferSizeSample の場合はデコード終了。
      その場合、nSizeSample を超える部分のバッファには無音が入っている。
      (2026/11/25 明記)
      ・nSizeSample が 0 のこともある(曲の長さがバッファの区切りにちょうど合ったときなど)。
      ・本体は曲の切り替わりでデバイスを開き直さないことがある(次の曲に途切れずに続けるとき)。
        その場合、曲の境目でも nSizeSample は nBufferSizeSample のまま続き、少ない数が来るのは
        続けずに終わるとき(停止する・開き直す前)だけ。

      出力先デバイスは Open 時の cszDeviceName / cszId に応じたデバイス
      pOutput のバッファサイズは Open 時の nBufferSizeSample * (abs(nBps)/8) * nChannels に等しい。
      事前に Wait を呼び出して Output が可能になるまで待ってから呼ばれることが
      保証される。

      成功時は TRUE を返す
      デバイスに何等かのエラーが発生して失敗した場合は FALSE を返す
      FALSE を返した場合、本体は直ちに演奏を停止し、以後は Output を呼び出すことはない

      pOutput の値
      ・float・double(KPI_DEVICE_FORMAT::nBitsPerSample が -32・-64)のときは、NaN・±∞ は含まれない
        (本体が 0 にしてから渡す。デバイスプラグインは NaN・±∞ を確かめなくてよい)。
        値は -1.0〜1.0 の範囲に収めずに渡す(デコーダが返した値や、音量・エコー・リプレイゲインなどにより
        1.0 を超えることがある)。範囲に収めるべきかはデバイスによって変わるため。
          ・OS に float のまま渡す場合(DirectSound・waveOut など)は、OS がその後で音量を掛けるので、
            収めずに渡せば 0dBFS を超える分が残る(収めると音量を下げても 1.0 で切られた波形のまま鳴る)。
          ・ファイルに書き出す場合(SUPPORT_FLAG_DISKWRITE)も、0dBFS を超える分を残せる。
          ・自分で整数に直す場合や、機器に直接渡す場合(ASIO・WASAPI の排他モードなど)は、
            デバイスプラグインが範囲を確かめること。
        (2026/11/17 修正。v3.17beta6 の開発中に一時 -1.0〜1.0 に収めて渡す仕様にしたが、上の理由で取りやめた。
         v3.17beta5 以前の本体も収めずに渡している)
      ・整数(8・16・24・32)のときは、本体が範囲に収めてから整数に直す。
      ・FORMAT_DOP のときは、本体はデータを加工しない(DoP のデータはもともと 1.0 未満)。
    void WINAPI Wait(void);
      次の Output が可能になるまで待つ。

      Wait の呼び出しスレッドとは異なるスレッドから下記のメソッドが呼ばれることがあり、
      排他制御もされない。
      GetPosition/Stop/Pause/Restart/SetVolume/SetRate

      Output は必ず Wait の後に、Wait とは排他的に呼ばれる(同じスレッドから呼ばれるとは限らない)

      Output が可能になったら速やかにリターンすること。
      待ってる間に Stop が呼ばれた場合は速やかにリターンすること。その場合、
      次回 Output が呼ばれたら再生を開始しなければならない。

      Output と Wait の呼び出しはいつも 1 対 1 に対応しているとは限らない。
      Output の呼び出し前に 2 回以上 Wait が呼ばれることがある。
      WaitForSingle/MultipleObject(s) で AutoReset なイベントオブジェクトを指定する場合などは
      注意すること。（2度目の呼び出しでデッドロックにならないように）

      ただし、Wait を 1 度も呼ばずに Output が呼ばれることは決してない。

    UINT64 WINAPI GetPosition(void);
      デバイスの再生位置をサンプル単位で返す。
      Stop が呼ばれたら再生位置を 0 に戻すこと。

      Output が 1 回も呼ばれていない段階でこのメソッドが呼ばれることはない。

    void WINAPI Stop(void);
      再生を停止する。Wait の処理中に呼ばれた場合、Wait は速やかにリターンしなければならない。
      Stop の後に Output が呼ばれたらその時点で再生を再開すること。
      Pause 中に Stop が呼ばれることもあることに注意。

    void WINAPI Pause(void);
      一時停止。Restart が呼ばれるまで再生を一時停止する。
      Pause 中に Stop が呼ばれたら再生を停止すること。
      Pause 中に Output が呼ばれることはない。
      Pause が多重に呼ばれることはない。（内部で参照カウントを数える必要はない）
      Pause 中に Stop が呼ばれることもあることに注意。

    void WINAPI Restart(void);
      再開。Pause の後にしか呼ばれない。

    BOOL WINAPI SetVolume(int nVolume);
      音量制御。単位は %
      増幅は無効(したがって 0 <= nVolume <= 100)
      未対応の場合は FALSE を返す。(本体によってソフトウェア的に処理される)
      (2026/11/25 明記)
      ・本体は SUPPORT_FLAG_VOLUME を立てたデバイスにだけ呼ぶ(v3.17beta7 以降。それより前の本体はフラグに
        関わらず呼んでいた)。立てていないデバイスには、本体がソフトウェア的に音量を掛ける。
        ただし、ほかのアプリや古い本体からは呼ばれることがあるので、対応しない場合も FALSE を返すこと。
      ・フラグを立てたデバイスでも、FALSE を返せば本体がソフトウェア的に処理する。
      ・音量が 100 のときは SetVolume(100) を呼ぶ。

    BOOL WINAPI SetRate(int nRate);//サンプリングレートを変換
      サンプリングレートを変換。単位は %
      nRate = 200 : 2 倍(200%)にする
      nRate = 100 : 変更しない
      nRate =  50 : 半分(50%) にする
      未対応の場合は FALSE を返す。
      (2026/11/25 明記)
      ・本体は SUPPORT_FLAG_RATE を立てたデバイスにだけ呼ぶ(v3.17beta7 以降。それより前の本体はフラグに
        関わらず呼んでいた)。ほかのアプリや古い本体からは呼ばれることがあるので、対応しない場合も
        FALSE を返すこと。
      ・本体が呼ぶのは、速度の変更を本体の速度変更の処理(音程を変えずに速度を変える DLL)で
        できないときだけ。その場合、速度(50〜300%)を 50〜200 に収めて渡す(200 を超える分は 200)。
      ・本体は戻り値を見ない。FALSE を返しても本体は代わりの処理をしないので、その場合は速度が
        変わらない。フラグを立てていないデバイスでも、速度変更の DLL が使えなければ速度は変わらない。

    DWORD WINAPI UpdateConfig(void *pvReserved);
      設定が変更されたら本体によって呼ばれる。
      pvReserved には NULL が渡される。
      0 を返すこと。
      (2026/11/25 明記)設定が変更された時点では呼ばれない。次の Output の直前に、Output を呼ぶのと
      同じスレッドから呼ばれる。本体はその時点でデバイスを開き直さないので、その場で反映できない
      設定は次の Open のときに反映する。

    各種メソッドはいつも同一スレッドから呼ばれるとは限らないが、Wait 以外の
    メソッドは本体によって排他的に呼ばれることが保証される。
*/
};
///////////////////////////////////////////////////////////////////////////////
class IKpiDeviceEnumerator : public IKpiUnknown
{//デバイス名を列挙する
 //Enumerate を繰り返し呼び出すこと
public:
    virtual void WINAPI Enumerate(const wchar_t *cszDeviceName, const wchar_t *cszId)=0;
};
///////////////////////////////////////////////////////////////////////////////
struct KPI_DEVICE_FORMAT
{
    DWORD cb;//=sizeof(KPI_DEVICE_FORMAT)
    DWORD dwFormatType;//KPI_MEDIAINFO::FORMAT_PCM or FORMAT_DOP
    DWORD dwSampleRate;   //再生周波数
    INT32 nBitsPerSample; //ビット数(負のときはfloat)
    DWORD dwChannels;     //チャネル数(SUPPORT_FLAG_MULTICHANNEL を立てていなければ 1 か 2、立てていれば 1〜8)
    DWORD dwSpeakerConfig;//スピーカー配置(WAVEFORMATEXTENSIBLE::dwChannelMask と同じ値)
                          //0 はチャネル数で決まる標準の配置(KPI_MEDIAINFO::dwSpeakerConfig の説明を参照)
                          //0 でないときは、立っているビットの数が dwChannels と一致することを本体が保証する
                          //SUPPORT_FLAG_MULTICHANNEL を立てていないデバイスには常に 0
    DWORD dwReserved[4];  //予約(常に0)
};
///////////////////////////////////////////////////////////////////////////////
struct KPI_DEVICE_MODULEINFO
{
    enum{
        SUPPORT_FLAG_VOLUME = 0x01,   //音量変更(SetVolume)に対応
        SUPPORT_FLAG_RATE = 0x01 << 1,//再生レート変換(SetRate)に対応
        SUPPORT_FLAG_BITPERFECT = 0x01 << 2,//ビットパーフェクト(WASAPI/ASIO)
        SUPPORT_FLAG_DISKWRITE = 0x01 << 3,//ファイル出力
        SUPPORT_FLAG_MULTICHANNEL = 0x01 << 4,//マルチチャンネル(3ch 以上)に対応
            //このフラグを立てたデバイスにだけ、本体は 3ch 以上(8ch まで)で Open を呼ぶ
            //立てていないデバイスには 2ch 以下で Open を呼ぶ(多チャンネルの曲は本体でまとめる)
            //KPI_DEVICE_FORMAT::dwSpeakerConfig の説明も参照
    };
    DWORD cb;                //=sizeof(KPI_DEVICE_MODULEINFO)
    DWORD dwModuleVersion;   //=KPI_OUTDEVICE_MODULE_VERSION
    DWORD dwPluginVersion;   //プラグインのバージョン
    DWORD dwMultipleInstance;//KPI_MULTINST_INFINITE/ZERO/ONE/UNIQUE
    GUID  guid;              //プラグイン識別子
    const wchar_t *cszDescription; //表示名
    const wchar_t *cszCopyright;   //著作権(複数名いる場合は改行文字可)
    const wchar_t *cszTypeName;    //デバイスの種類名(DirectSound/waveOut/WASAPI/ASIO etc...)
    DWORD dwSupportConfig;  //IKpiConfig による設定の読み書きに未対応なら 0
    DWORD dwSupportFlags;   //SUPPORT_FLAG_XXX の組み合わせ
                            //(2026/11/25 明記)今の本体が見ているのは VOLUME・RATE(立てていなければ SetVolume・
                            //SetRate を呼ばない。v3.17beta7 以降)・DISKWRITE・MULTICHANNEL。BITPERFECT は見ていない
    DWORD dwReserved[4];    //must be 0
};
///////////////////////////////////////////////////////////////////////////////
class IKpiOutDeviceModule : public IKpiUnknown
{//IKpiOutDevice のインスタンス作成とデバイス名の列挙を行う。
public:
    virtual void  WINAPI GetModuleInfo(const KPI_DEVICE_MODULEINFO **ppInfo)=0;
    virtual BOOL  WINAPI Open(const wchar_t *cszDeviceName, //デバイス名
                              const wchar_t *cszId,         //識別文字列
                              const KPI_DEVICE_FORMAT *cpFormat, //再生周波数等
                              int nBufferSizeSample,        //再生バッファサイズ（サンプル単位）
                              IKpiOutDevice **ppOutDevice)=0;
    virtual void  WINAPI EnumDevices(IKpiDeviceEnumerator *pEnumerator)=0;//デバイス名の列挙
    virtual BOOL  WINAPI EnumConfig(IKpiConfigEnumerator *pEnumerator)=0;
    virtual DWORD WINAPI ApplyConfig(const wchar_t *cszSection, //セクション名
                                     const wchar_t *cszKey,     //キー名
                                     INT64  nValue,             //変更後の値(INT)
                                     double dValue,             //変更後の値(FLOAT)
                                     const wchar_t *cszValue)=0;//変更後の値(STR)
/*
    void  WINAPI GetModuleInfo(const KPI_DEVICE_MODULEINFO **ppInfo);
      KPI_DEVICE_MODULEINFO を *ppInfo に設定して返す。
      IKpiDecoderModule::GetModuleInfo(kpi_decoder.h) とほぼ同じため、そちらも
      合わせて参照すること。

    BOOL WINAPI Open(const wchar_t *cszDeviceName, //デバイス名
                     const wchar_t *cszId, //識別文字列
                     const KPI_DEVICE_FORMAT *cpFormat, //再生周波数等
                     int nBufferSizeSample, //再生バッファサイズ（サンプル単位）
                     IKpiOutDevice **ppOutDevice);
      デバイスを開く
      成功したら IKpiOutDevice のインスタンスを *ppOutDevice に設定して TRUE を返す。
      失敗したら *ppOutDevice を NULL に設定して FALSE を返す
      cszDeviceName が空文字列(cszDeviceName = "")のとき(NULL は渡されない)は
      Enumerate 時に一番最初に見つかるデバイスと見なすこと
      (DirectSound・waveOut の既定のデバイス(プライマリ サウンド ドライバー・サウンド マッパー)を想定したもの。
       今の本体は空のデバイス名を渡さないが、ほかのアプリから渡されることがあるので、どのプラグインも
       失敗させずに最初のデバイスとして扱うこと。2026/11/23 追記)

    void WINAPI EnumDevices(IKpiDeviceEnumerator *pEnumerator);//デバイス名の列挙
      デバイス名を列挙する。
      IKpiDeviceEnumerator::Enumerate を繰り返し呼び出す。
      呼び出し時は常に最新情報を列挙すること。

    BOOL WINAPI EnumConfig(IKpiConfigEnumerator *pEnumerator);
      設定項目を列挙する。
      本体が列挙された設定項目を元に設定画面を自動生成する。
      設定項目がない場合は FALSE を返す、ある場合は TRUE を返す。
      IKpiDecoderModule::EnumConfig(kpi_decoder.h) とほぼ同じため、そちらも
      合わせて参照すること。

      KPI_CFG_INFO::cszSection = デバイス名-識別子 となっている場合は
      EnumDevices で列挙されたデバイス用のセクションと見なす。
      (設定画面を生成時に必要に応じて自動で選択状態にする)

    DWORD WINAPI ApplyConfig(const wchar_t *cszSection,//セクション名
                             const wchar_t *cszKey,    //キー名
                             INT64  nValue, //変更後の値(BOOL/INT の場合)
                             double dValue, //変更後の値(FLOAT の場合)
                             const wchar_t *cszValue)=0;//変更後の値(文字列)
      cszSection と cszKey に該当する設定の変更を「適用」する直前に呼ばれる。
      呼ばれた時点ではまだ設定は変更されていない。
      IKpiDecoderModule::ApplyConfig(kpi_decoder.h) とほぼ同じため、そちらも
      合わせて参照すること。

    EnumDevices と Open は複数のスレッドから呼ばれる可能性があるが、排他的に
    呼ばれることが保証される。
    (2026/11/25 明記)今の本体は、EnumDevices と Open を MTA のスレッドから呼ぶ(再生のときは演奏スレッド、
    設定画面の「最適な設定を調べる」と EnumDevices は本体が作る MTA のスレッド。v3.17beta6 以前の本体は、
    この 2 つをメインスレッド(STA)から呼んでいた)。COM の扱いは kpi.h の「スレッドと COM について」を参照。Open の処理時は内部的に EnumDevices に相当する
    処理を行う可能性が高いと思われるが、本体側ではこれらのメソッドは必ず排他
    的に呼び出すので EnumDevices の排他制御はほぼ不要と思われる。

    IKpiOutDeviceModule のインスタンス作成と破棄はプラグインがロードされている間
    一度だけ行なわれる。

    必要な初期化はインスタンス作成時、後始末はインスタンス破棄時に行うこと。

    本体はプラグインをロード後 Open まで行わず、最低限の情報だけ取得後にアンロード
    することがある。例えば起動時にプラグインを初めて認識した場合は GetModuleInfo
    だけ呼び出し後にアンロードする。設定画面の表示時、プラグインを使用中でなければ

    GetModuleInfo/EnumConfig/ApplyConfig

    しか呼ばず、設定画面を閉じたところでアンロードする。

    もしプラグインの初期化に時間がかかる場合は Open 時に初期化を1度だけ実行するなど、
    プラグイン側で独自に工夫する。その際は未初期化のまま IKpiOutDeviceModule を破棄
    されたり、どのメソッドが最初に呼ばれても動作に問題が起こらないように注意すること。
*/
};
///////////////////////////////////////////////////////////////////////////////
//プラグインがエクスポートする関数
//typedef HRESULT (WINAPI *pfn_kpiCreateInstance)(REFIID riid,      //IKpiXXXModule の IID
//                                                void **ppvObject, //IKpiXXXModule の戻り値
//                                                IKpiUnknown *pUnknown);
//HRESULT WINAPI kpi_CreateInstance(REFIID riid, void **ppvObject, IKpiUnknown *pUnknown);
//IKpiDecoderModule と同じ(kpi.h を参照)
//riid に IID_IKpiOutDeviceModule が渡される
//必要であれば pUnknown から IKpiConfig 等を取得すること
//kpi_CreateConfig 等のヘルパー関数を呼ぶ場合は kpi.cpp もプロジェクトに追加する
///////////////////////////////////////////////////////////////////////////////
extern HRESULT WINAPI kpi_CreateOutDeviceModule(pfn_kpiCreateInstance fnCreateInstance,
                                                IKpiOutDeviceModule **ppModule,
                                                IKpiUnknown *pUnknown);
//↑プラグイン使用側用のヘルパー関数(プラグイン作成には不要)
//プラグイン使用者(通常は KbMedia Player)が呼び出す場合は kpi_device.cpp も
//プロジェクトに追加する
///////////////////////////////////////////////////////////////////////////////
#pragma pack(pop) //構造体のアラインメント
#endif
