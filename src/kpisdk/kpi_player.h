#pragma once
#ifndef kpi_playerH
#define kpi_playerH

#include "kpi.h"
#include "kpi_decoder.h"
#pragma pack(push, 1) //構造体のアラインメント

///////////////////////////////////////////////////////////////////////////////
/*演奏プラグイン
  使用するライブラリやプログラムの構造上、デコーダプラグインに出来ない形式
  (PCM を取得出来ずに演奏の指示しか出来ない形式)のためのもの。
  音の出力までプラグインが行うため、本体の出力の設定や音の処理(イコライザ・
  リプレイゲイン等)、ギャップレス再生は効かない。
  PCM を取得出来る形式はデコーダプラグインにすること。動画もデコーダプラグインで
  扱える(本体が渡す親ウィンドウに映像を描く仕組みがある。kpi_video_window.h・kpisdk_for_ai_decoder.md の 6-15)。
  (2026/11 変更。以前は「動画を扱うためのもの」としていた)

  再生速度の変更等はプラグイン側で制御。
  動画のウィンドウは、プラグインが自分で作るか、本体が作った親ウィンドウの中に描くか
  (GetCaps の KPI_PLAYER_CAPS_VIDEO_PARENT と CreateVideoWindow)をプラグインが選ぶ。

  スレッドと COM については kpi.h の「スレッドと COM について」を参照。
  特に、各メソッドは同じスレッドから呼ばれるとは限らないので、ライブラリの
  オブジェクトをスレッドに結びつけて扱う必要がある場合(COM のオブジェクト・
  ウィンドウ等)は、プラグインが自分で作ったスレッドの中で作り・使い・消すこと。

  演奏プラグインがエクスポートする関数
  typedef HRESULT (WINAPI *pfn_kpiCreateInstance)(REFIID riid,    //IKpiXXXModule の IID
                                                void **ppvObject, //IKpiXXXModule の戻り値
                                                IKpiUnknown *pUnknown);
  HRESULT WINAPI kpi_CreateInstance(REFIID riid, void **ppvObject, IKpiUnknown *pUnknown);
  riid に IID_IKpiPlayerModule が渡される。
  必要であれば pUnknown から IKpiConfig 等を取得すること。
  kpi_CreateConfig 等のヘルパー関数を呼ぶ場合は kpi.cpp もプロジェクトに追加する。

  *ppvObject にインスタンスを渡す場合は、適切な型にキャストすること。
  例えば、

    if(IsEqualIID(riid, IID_IKpiPlayerModule)){
        CKpiXXXPlayerModule *pModule = new CKpiXXXPlayerModule;
        // *ppvObject = pModule; //NG
        *ppvObject = (IKpiPlayerModule*)pModule; //OK
    }

  のようにすること。CKpiXXXPlayerModule が複数のクラスから派生している場合、
  適切なポインタが渡されない可能性がある。
*/
///////////////////////////////////////////////////////////////////////////////
#define KPI_PLAYER_MODULE_VERSION 4 //仕様確定時に 100 とする予定
//(2026/11 に 3 から 4 に変更：IKpiPlayer に GetCaps・SetPitch・CreateVideoWindow を追加。
// 本体は KPI_PLAYER_MODULE_VERSION が一致しないプラグインを読み込まないので、
// 3 のプラグインは 4 の本体では使えない(4 のプラグインも 3 の本体では使えない))

class __declspec(uuid("{CD083F95-E9D2-4C8A-9FF0-A4003AB220F8}")) IKpiPlayer;
class __declspec(uuid("{20706B19-8A25-4DF6-890E-FF76AE7386C1}")) IKpiPlayerModule;

#define IID_IKpiPlayer       __uuidof(IKpiPlayer)
#define IID_IKpiPlayerModule __uuidof(IKpiPlayerModule)

///////////////////////////////////////////////////////////////////////////////
typedef KPI_DECODER_MODULEINFO KPI_PLAYER_MODULEINFO;
//KPI_PLAYER_MODULEINFO::dwModuleVersion == KPI_PLAYER_MODULE_VERSION
//としなければならないことを除けば KPI_DECODER_MODULEINFO と同じ
//
enum{//IKpiPlayer::GetStatus の戻り値
    KPI_STATUS_STOP = 0,    //停止中(最後まで演奏した場合も含む)
    KPI_STATUS_PLAYING = 1, //演奏中
    KPI_STATUS_PAUSE =   2, //一時停止中
};
#define KPI_STATUS_ERROR ((DWORD)-1) //エラーで止まった(2026/11 追加)
//GetStatus の戻り値は DWORD のため、KPI_STATUS_ERROR だけは DWORD の値として定義している
//(enum にすると、DWORD と比べたときにコンパイラによっては符号の警告が出るため)
//GetStatus の -1 はエラーの意味(qwLength 等の -1 の「不明」「本体の設定に従う」とは別)
//
//本体は KPI_STATUS_PLAYING/PAUSE 以外の値をすべて停止として扱う。
//KPI_STATUS_ERROR に対応していない本体では KPI_STATUS_STOP と同じ扱いになる。
//
//KPI_STATUS_STOP/PLAYING/PAUSE は、本体が映像の共有メモリ(KMP_STATUS)の dwStatus で
//デコーダプラグインに再生状態を知らせる値としても使う(その場合、本体は 0〜2 しか書かない)
///////////////////////////////////////////////////////////////////////////////
enum{//IKpiPlayer::GetCaps の戻り値(組み合わせ)(v4 で追加)
    KPI_PLAYER_CAPS_VOLUME = 0x01, //SetVolume で音量を変えられる
    KPI_PLAYER_CAPS_TEMPO  = 0x02, //SetTempo で速度を変えられる
    KPI_PLAYER_CAPS_PITCH  = 0x04, //SetPitch で音程を変えられる
    KPI_PLAYER_CAPS_VIDEO_PARENT = 0x08, //本体が作った親ウィンドウの中に映像を描ける(CreateVideoWindow)
    KPI_PLAYER_CAPS_WINDOW_MOVE  = 0x10, //SetWindowPosition の nLeft/nTop に従える(自分のウィンドウの位置を変えられる)
    KPI_PLAYER_CAPS_WINDOW_SIZE  = 0x20, //SetWindowPosition の nVideoWidth/nVideoHeight に従える(大きさを変えられる)
};
///////////////////////////////////////////////////////////////////////////////
class IKpiPlayer : public IKpiUnknown
{
public:
    virtual DWORD WINAPI Select(DWORD dwNumber,
                                const KPI_MEDIAINFO **ppInfo,
                                IKpiTagInfo *pTagInfo,
                                DWORD dwTagGetFlags)=0;
    virtual DWORD WINAPI GetCaps(void)=0;    //対応する機能(KPI_PLAYER_CAPS_XXX)(v4 で追加)
    virtual BOOL  WINAPI Play(DWORD dwStartPos, BOOL bVisible)=0;//dwStartPos の位置から演奏開始
    virtual BOOL  WINAPI Pause(void)=0;  //一時停止
    virtual BOOL  WINAPI Restart(void)=0;//再開
    virtual void  WINAPI Stop(void)=0;   //演奏停止
    virtual DWORD WINAPI GetPosition(void)=0;//再生位置取得(単位はミリ秒)
    virtual DWORD WINAPI GetStatus(void)=0;  //演奏状態(KPI_STATUS_STOP/PLAYING/PAUSE/ERROR)
    virtual void  WINAPI SetTempo(int nTempo)=0;
    virtual void  WINAPI SetPitch(int nPitch)=0;//音程(セント単位)(v4 で追加)
    virtual void  WINAPI SetVolume(int nVolume)=0;
    virtual void  WINAPI SetVisible(BOOL bVisible)=0;
    virtual void  WINAPI GetWindowPosition(int *pLeft, int *pTop,
                                           int *pVideoWidth, int *pVideoHeight)=0;
    virtual void  WINAPI SetWindowPosition(int nLeft, int nTop,
                                           int nVideoWidth, int nVideoHeight)=0;
    virtual void  WINAPI SetTopMost(BOOL bTopMost)=0;
    virtual void  WINAPI SetRepeat(BOOL bRepeat)=0;
    virtual DWORD WINAPI UpdateConfig(void *pvReserved)=0; //設定が変更されたら呼ばれる
    virtual BOOL  WINAPI CreateVideoWindow(HWND hParent, HWND *phChild)=0;//本体の親ウィンドウの中に映像の子ウィンドウを作る(v4 で追加)
/*
    範囲について：
      「今の本体は〜の範囲で呼ぶ」と書いてあるものは、今の本体の動作を示したもの。
      プラグインが対応出来る範囲を超えた値が渡された場合は、プラグイン側で
      適切に処理すること(対応出来る範囲の近い値に丸めるなど)。

    DWORD WINAPI Select(DWORD dwNumber, const KPI_MEDIAINFO **ppInfo,
                        IKpiTagInfo *pTagInfo, DWORD dwTagGetFlags);
      IKpiDecoder::Select(kpi_decoder.h) とほぼ同じため、そちらも合わせて参照すること。
      dwNumber に IKpiPlayerModule::Open が返した曲数より大きい値が
      渡されることはない。(渡した結果は未定義。プラグインは曲番号の範囲を確かめなくて良い)
      (IKpiDecoder::Select と同じ)
      戻り値も IKpiDecoder::Select と同じ。成功したら dwNumber と同じ値、失敗したら
      dwNumber と異なる値(dwNumber が 1 以上なら 0、0 なら 0xFFFFFFFF)を返す。(2026/11 明記)
      *ppInfo の KPI_MEDIAINFO は、dwFormatType を KPI_MEDIAINFO::FORMAT_PLAYER にする。
      (今の本体は dwFormatType を見ていない)
      映像がある場合は dwVideoWidth/dwVideoHeight を設定する(本体は各辺 16384 で頭打ちにする)。

    DWORD WINAPI GetCaps(void);
      Select で選んだ曲が対応する機能を KPI_PLAYER_CAPS_XXX の組み合わせで返す。(v4 で追加)
      本体は Select の後に呼ぶ。次に Select を呼ぶまで、同じ値を返すこと。
      曲ごとに変えて良い(例えばライブラリが音量の変更に対応しない形式の場合だけ外す)。
      KPI_PLAYER_CAPS_VOLUME がない場合、本体はフェードアウトせずに演奏を止める。
      (立っていると、本体はフェードアウトの間 SetVolume で音量を下げてから止める)
      KPI_PLAYER_CAPS_VOLUME/TEMPO/PITCH がない場合、本体は SetVolume/SetTempo/SetPitch を呼ばない。
      (2026/11/25 変更。以前の本体はフラグに関わらず呼んでいた。ほかのアプリからは呼ばれることが
       あるので、対応しない場合も呼ばれたら何もしないこと)
      本体は Select の前には SetVolume/SetTempo/SetPitch を呼ばない。Select の後、フラグがあれば
      今の速度・音程を SetTempo/SetPitch で渡す(音量は Play の前に SetVolume で渡す)。
      KPI_PLAYER_CAPS_VIDEO_PARENT がある場合だけ、本体は CreateVideoWindow を呼ぶ。
      KPI_PLAYER_CAPS_WINDOW_MOVE/SIZE は、自分のウィンドウを使う場合に、位置・大きさを
      本体が変えられるかを示す(SetWindowPosition の説明を参照)。CreateVideoWindow で
      子ウィンドウを返した場合は使われない。

    BOOL WINAPI Play(DWORD dwStartPos, BOOL bVisible);
      dwStartPos(ミリ秒)の位置から演奏を始める。
      演奏中・一時停止中に呼ばれた場合は、その位置から演奏し直す。
      (本体はシークの代わりにも使う)
      シークに対応しない場合は、今の位置より前が指定されたら先頭から演奏し直し、
      今の位置より後ろが指定されたら何もしない(そのまま演奏を続ける)。
      dwStartPos が曲の長さを超えている場合は、プラグイン側で適切に処理すること。
      bVisible は映像のウィンドウを表示するか。FALSE でも演奏は行う。
      (本体の「Video」の表示モードで決まる。映像のないファイルなら TRUE でもウィンドウを出さなくて良い)
      演奏を始められなかった場合は FALSE を返す。
      演奏が実際に始まる前に戻っても良い(その間の GetStatus は PLAYING を返して良い)。

    BOOL WINAPI Pause(void);
      一時停止する。一時停止した場合は TRUE を返す。
      停止中・一時停止中や、曲が終わった後など、一時停止出来ない場合は FALSE を返す。
      (本体は TRUE のとき、スペアナ用の音の取り込みを止める)

    BOOL WINAPI Restart(void);
      一時停止を解除して続きから演奏する。再開した場合は TRUE を返す。
      一時停止中でない場合は何もせずに FALSE を返す。

    void WINAPI Stop(void);
      演奏を止める。止めた後は Play で再び演奏出来ること。停止中に呼ばれても良い。
      停止中のウィンドウの表示・非表示は本体が SetVisible で指示するので、
      Stop の中では変えなくて良い。

    DWORD WINAPI GetPosition(void);
      再生位置(ミリ秒。曲の先頭から)を返す。
      本体は演奏中に短い間隔(今の本体は約 50ms ごと)で呼ぶので、時間のかかる処理はしないこと。
      停止中・曲が終わった後は 0 を返して良い。

    DWORD WINAPI GetStatus(void);
      KPI_STATUS_STOP/PLAYING/PAUSE/ERROR のどれかを返す。
      曲が最後まで演奏された場合は KPI_STATUS_STOP を返す。(本体は次の曲へ進む)
      ファイルやライブラリのエラーで演奏を続けられなくなった場合は KPI_STATUS_ERROR を返す。
      (本体は次の曲へ進む。単曲リピートでも同じ曲を開き直さずに演奏を止める)
      本体は短い間隔(今の本体は約 50ms ごと)で呼ぶので、時間のかかる処理はしないこと。

    void WINAPI SetTempo(int nTempo);
      再生速度を % で指定する(100 が等速)。今の本体は 50〜300 の範囲で呼ぶ。
      速度の変更で音程も一緒に変わって良い。
      (デコーダプラグインの場合は、本体が音程を保って速度を変える)
      速度を変えられない場合は何もせず、GetCaps で KPI_PLAYER_CAPS_TEMPO を返さないこと。
      (本体は KPI_PLAYER_CAPS_TEMPO を返した曲にだけ呼ぶ。GetCaps の説明を参照)

    void WINAPI SetPitch(int nPitch);
      音程をセント単位で指定する(0 が原音。100 で半音上がる)。(v4 で追加)
      今の本体は -1200〜1200 の範囲で呼ぶ。
      音程を変えられない場合は何もせず、GetCaps で KPI_PLAYER_CAPS_PITCH を返さないこと。
      (本体は KPI_PLAYER_CAPS_PITCH を返した曲にだけ呼ぶ。GetCaps の説明を参照)

    void WINAPI SetVolume(int nVolume);
      音量を 0〜100 で指定する(100 が元の音量、0 が無音)。
      本体はフェードイン・フェードアウトの間、約 200ms ごとに途中の値で呼ぶ。
      音量を変えられない場合は何もせず、GetCaps で KPI_PLAYER_CAPS_VOLUME を返さないこと。
      (本体は KPI_PLAYER_CAPS_VOLUME を返した曲にだけ呼ぶ。GetCaps の説明を参照)

    void WINAPI SetVisible(BOOL bVisible);
      映像のウィンドウの表示・非表示を変える。演奏の状態は変えない。
      停止中に呼ばれることもある(本体の「停止時に隠す」の設定のとき)。
      対応出来ない場合は何もしなくて良い(本体は表示の状態を取得しない)。(2026/11 追記)

    void WINAPI GetWindowPosition(int *pLeft, int *pTop, int *pVideoWidth, int *pVideoHeight);
    void WINAPI SetWindowPosition(int nLeft, int nTop, int nVideoWidth, int nVideoHeight);
      nLeft/nTop はウィンドウの左上の位置(スクリーン座標)。
      nVideoWidth/nVideoHeight は映像を表示する領域(クライアント領域)の大きさで、枠は含まない。
      (シークバーなど、プラグインが映像の外に表示するものも含まない)
      GetWindowPosition は、最大化・全画面表示のときは元に戻したときの位置と大きさを返す。
      GetWindowPosition は、従えない値も含めて、実際の位置と大きさを返すこと。
      位置・大きさを変えられない場合(外部のアプリのウィンドウを使う場合など)：
        ・GetCaps で、従えるものだけ KPI_PLAYER_CAPS_WINDOW_MOVE(位置)・
          KPI_PLAYER_CAPS_WINDOW_SIZE(大きさ)を返す。SetWindowPosition の従えない方の値は無視して良い。
        ・大きさを変えられない場合は、dwVideoWidth/dwVideoHeight にその大きさを入れる。
        ・本体は、フラグのない方の値を、次の曲に引き継ぐ位置・大きさとして覚えない。
          画面に収める計算は GetWindowPosition で得た実際の値で行い、大きさを変えられない場合は
          「動画サイズ」の指定を使えなくする。(2026/11 追加)
      今の本体は画面に収まるように直してから SetWindowPosition に渡すが、
      それでもデスクトップ画面外にはみ出す場合や対応出来ない大きさの場合は、
      プラグイン側で適切に処理すること。マルチモニタ環境も考慮に入れること。

    void WINAPI SetTopMost(BOOL bTopMost);
      映像のウィンドウを常に手前に表示するか。対応出来ない場合や、全画面表示の間など
      プラグインの都合で従えない場合は、何もしなくて良い(本体は状態を取得しない)。(2026/11 追記)

    void WINAPI SetRepeat(BOOL bRepeat);
      TRUE なら、曲の最後まで演奏したら先頭に戻って演奏を続ける(GetStatus は STOP にしない)。
      対応しなくても良い。(何もしなければ、曲の終わりで GetStatus が STOP を返し、
      本体が先頭から再生し直す)
      本体が WRD を再生する場合などは、本体がリピートするので FALSE が渡される。

    DWORD WINAPI UpdateConfig(void *pvReserved);
      設定が変わった後、次に GetPosition/GetStatus を呼ぶ前に呼ばれる。
      pvReserved は NULL。0 を返すこと(今の本体は戻り値を見ない)。
      演奏中に反映出来ない設定は、次の Open から反映すれば良い。

    BOOL WINAPI CreateVideoWindow(HWND hParent, HWND *phChild);
      本体が作った映像の親ウィンドウ hParent の中に、映像を描く子ウィンドウを作って *phChild に返す。(v4 で追加)
      (デコーダプラグインの IKpiVideoWindow::CreateVideoWindow(kpi_video_window.h)と同じ形・同じ意味)
      成功したら TRUE、失敗したら FALSE を返す。*phChild には、どの場合も値を入れること(なければ NULL)。
      本体は、GetCaps が KPI_PLAYER_CAPS_VIDEO_PARENT を返した場合だけ、Select の後、
      最初の Play の前に 1 度だけ、hParent に親ウィンドウを渡して呼ぶ。
      子ウィンドウを作れなかった場合は、*phChild を NULL にして FALSE を返す。その場合、本体は hParent を
      NULL にしてもう一度呼ぶ。
      hParent が NULL の場合は、「自分のウィンドウを使って」の意味。*phChild を NULL にして、自分の
      ウィンドウを使える場合は TRUE を返す(本体は SetVisible・GetWindowPosition・SetWindowPosition・
      SetTopMost を呼ぶ。最初の表示は今までどおり Play の bVisible)。
      KPI_PLAYER_CAPS_VIDEO_PARENT を返さないプラグインには、今の本体は呼ばない(今までどおり自分の
      ウィンドウを使うこと。実装は「*phChild を NULL にして、hParent が NULL なら TRUE、そうでなければ
      FALSE を返す」で良い)。親ウィンドウを用意できない本体(ほかのアプリ)は、NULL だけで呼ぶことがある。
      本体の約束：親ウィンドウは、IKpiPlayer を解放するまで消さず、作り直さず、子ウィンドウを別の親に
      付け替えない。
      子ウィンドウを返した場合：
        ・子ウィンドウは hParent の子(WS_CHILD)で、隠して作り、EnableWindow(FALSE) で無効にしておく
          (マウスの操作は親ウィンドウが受け取る。ドラッグで移動・ダブルクリックで全画面・右クリックのメニュー
           などは本体が行う)
        ・表示・位置・大きさ・全画面・常に手前に表示・タイトルは本体が親ウィンドウで行う。
          子ウィンドウの大きさは親ウィンドウが合わせる(親ウィンドウのクライアント領域いっぱい)ので、
          プラグインは WM_SIZE などで映像の描画を合わせる(縦横比を保つ場合の余白もプラグインが描く)
        ・タイトルを決めたい場合は、子ウィンドウにウィンドウ名を付ける(SetWindowText など)。本体は、表示している
          子ウィンドウに名前があれば、それをそのまま親ウィンドウのタイトルにする。付け方はプラグインの自由で、
          本体の付け方に合わせなくて良い。後から変えても良い(少し遅れて反映される)。長すぎる分は切られる。
          名前がなければ(空なら)、本体がタイトルを決める(今の本体は「Video - ファイル名 -」)
        ・本体は SetVisible・GetWindowPosition・SetWindowPosition・SetTopMost を呼ばない
          (Play の bVisible も使わなくて良い)
        ・子ウィンドウは、プラグインが自分で作ったスレッドで作り、そのスレッドはメッセージを処理し続けること
          (親ウィンドウは別のスレッドのウィンドウなので、子ウィンドウのスレッドが止まると親ウィンドウの操作も止まる)
        ・子ウィンドウは IKpiPlayer を解放する(Release で破棄する)までに消すこと。親ウィンドウは消さないこと
          (親ウィンドウは本体が曲をまたいで使う)
      kbrunkpi.exe 経由のときは、hParent は kbrunkpi.exe のプロセスの中に作られた親ウィンドウになる
      (本体の親ウィンドウと同じに扱って良い)。

    各メソッドは有限の時間で戻ること。本体は演奏プラグインの呼び出しに時間の上限を
    設けていないため、プラグインの中で待ち続けると本体も固まる。

    演奏プラグインは厳密に考えると意外と難しい…。

    Play() 呼び出し直後の GetStatus の結果がライブラリ次第で実際はまだ STOP
    だったりするなど。そういう場合は Play() 呼び出しから1～2秒程度は無条件に
    PLAYING と見なすなどして対処する。(本体側でそう解釈させるようにしているので
    実際は不要。今の本体は演奏を始めてから 1 秒間は STOP/ERROR を PLAYING とみなす)

    実際に演奏が始まるのが PostMessage によるメッセージを受け取った後だったり
    すると、

    Play();
    Stop();

    で演奏が止まらない場合もあったりする。昔の RealMedia 関係がそうなっていて
    とても扱いにくかった。(最近はどうだか知らないが)

    各種メソッドはいつも同一スレッドから呼ばれるとは限らないが、全てのメソッド
    は本体によって排他的に呼ばれることが保証される。
    (スレッドと COM の扱いは kpi.h の「スレッドと COM について」を参照)
*/
};
///////////////////////////////////////////////////////////////////////////////
class IKpiPlayerModule : public IKpiUnknown
{
public:
    virtual void  WINAPI GetModuleInfo(const KPI_PLAYER_MODULEINFO **ppInfo)=0;
    virtual DWORD WINAPI Open(const KPI_MEDIAINFO *cpRequest,
                              IKpiFile    *pFile,
                              IKpiFolder  *pFolder,
                              IKpiPlayer **ppPlayer)=0;
    virtual BOOL  WINAPI EnumConfig(IKpiConfigEnumerator *pEnumerator)=0;
    virtual DWORD WINAPI ApplyConfig(const wchar_t *cszSection,//セクション名
                                     const wchar_t *cszKey,    //キー名
                                     INT64  nValue, //変更後の値(BOOL/INT の場合)
                                     double dValue, //変更後の値(FLOAT の場合)
                                     const wchar_t *cszValue)=0;//変更後の値(文字列)
/*
    void  WINAPI GetModuleInfo(const KPI_PLAYER_MODULEINFO **ppInfo)=0;
      KPI_PLAYER_MODULEINFO を *ppInfo に設定して返す。
      IKpiDecoderModule::GetModuleInfo(kpi_decoder.h) とほぼ同じため、そちらも
      合わせて参照すること。

    DWORD WINAPI Open(const KPI_MEDIAINFO *cpRequest,//再生周波数等の要求値が含まれた KPI_MEDIAINFO
                              IKpiFile    *pFile,    //音楽データ
                              IKpiFolder  *pFolder,  //音楽データがあるフォルダ
                              IKpiPlayer **ppPlayer);
      pFile/pFolder から曲を開く。
      成功したら IKpiPlayer のインスタンスを *ppPlayer に設定し、含まれている
      曲の数を返す。(エラーの場合は *ppPlayer に NULL を設定して 0 を返す)

      IKpiDecoderModule::Open(kpi_decoder.h)と考え方はほぼ同じため、そちらも
      合わせて参照すること。

      cpRequest が NULL の場合はタグ取得用に開く。(2026/11 追記)
      (本体は KPI_PLAYER_MODULEINFO::dwSupportTagInfo に TAG_TITLE がある場合だけ呼ぶ。
       ただし cszMultiSongExts に含まれる拡張子のファイルは、曲の数を得るために
       TAG_TITLE がなくても呼ぶ。kpi_decoder.h の dwSupportTagInfo の説明を参照)
      その後は Select(曲番号, NULL, pTagInfo, dwTagGetFlags) だけが呼ばれ、
      演奏はされない。
      本体は演奏リストの情報集めなどでファイルごとに呼ぶので、演奏の準備
      (スレッド・ウィンドウ・デバイスの初期化等)はせず、軽く済ませること。
      本体はこの Open と Select の戻り値を、そのファイルを開けるか
      (アーカイブ内のファイルを演奏リストに加えるか等)の判断にも使う。
      開けないファイルの場合は 0 を返すこと。
      再生できるファイルなら、タグを取得できなくても(TAG_TITLE がなくても)成功を返すこと。(2026/11/23 追記)
      その場合、Select はタグを渡さずに曲番号を返せば良い。
      本体が呼ばない場合でも、ほかのアプリやこれからの本体が呼ぶことがあるので、cpRequest が NULL
      というだけで 0 を返してはいけない。
      再生できるかを軽く確かめられない形式(外部の部品を読み込まないと分からない等)は、
      部品が使えるかなど最低限の確認で成功を返して良い(実際に開けるかは再生のために開いたときに分かる)。

    BOOL WINAPI EnumConfig(IKpiConfigEnumerator *pEnumerator);
      設定項目を列挙する。
      本体が列挙された設定項目を元に設定画面を自動生成する。
      設定項目がない場合は FALSE を返す、ある場合は TRUE を返す。
      IKpiDecoderModule::EnumConfig(kpi_decoder.h) とほぼ同じため、そちらも
      合わせて参照すること。

    DWORD WINAPI ApplyConfig(const wchar_t *cszSection,//セクション名
                             const wchar_t *cszKey,    //キー名
                             INT64  nValue, //変更後の値(BOOL/INT の場合)
                             double dValue, //変更後の値(FLOAT の場合)
                             const wchar_t *cszValue)=0;//変更後の値(文字列)
      cszSection と cszKey に該当する設定の変更を「適用」する直前に呼ばれる。
      呼ばれた時点ではまだ設定は変更されていない。
      IKpiDecoderModule::ApplyConfig(kpi_decoder.h) とほぼ同じため、そちらも
      合わせて参照すること。

    IKpiPlayerModule のインスタンス作成と破棄はプラグインがロードされている間
    一度だけ行なわれる。

    必要な初期化はインスタンス作成時、後始末はインスタンス破棄時に行うこと。

    本体はプラグインをロード後 Open まで行わず、最低限の情報だけ取得後にアンロード
    することがある。例えば起動時にプラグインを初めて認識した場合は GetModuleInfo
    だけ呼び出し後にアンロードする。設定画面の表示時、プラグインを使用中でなければ

    GetModuleInfo/EnumConfig/ApplyConfig

    しか呼ばず、設定画面を閉じたところでアンロードする。

    もしプラグインの初期化に時間がかかる場合は Open 時に初期化を1度だけ実行するなど、
    プラグイン側で独自に工夫すること。その際は未初期化のまま IKpiPlayerModule
    を破棄されたり、どのメソッドが最初に呼ばれても動作に問題が起こらないように
    注意すること。

*/
};
///////////////////////////////////////////////////////////////////////////////
extern HRESULT WINAPI kpi_CreatePlayerModule(pfn_kpiCreateInstance fnCreateInstance,
                                             IKpiPlayerModule **ppModule, IKpiUnknown *pUnknown);
//↑プラグイン使用側用のヘルパー関数(プラグイン作成には不要)
//プラグイン使用者(通常は KbMedia Player)が呼び出す場合は kpi_player.cpp も
//プロジェクトに追加する
///////////////////////////////////////////////////////////////////////////////
#pragma pack(pop) //構造体のアラインメント
#endif

