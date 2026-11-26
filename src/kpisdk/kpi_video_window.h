#pragma once
#ifndef kpi_video_windowH
#define kpi_video_windowH

#include "kpi.h"

#pragma pack(push, 1) //構造体のアラインメント
///////////////////////////////////////////////////////////////////////////////
/*デコーダプラグインの映像・ウィンドウ・再生位置(2026/11 追加。対応は任意)

  デコーダプラグイン(IKpiDecoder)が、音に合わせて映像やウィンドウを出すための口。
  対応する場合は、IKpiDecoder::QueryInterface で IID_IKpiVideoWindow に答える。
  対応しないプラグインは何もしなくて良い(今までどおり)。

  出来ること：
    ・本体が作った親ウィンドウの中に、映像を描く子ウィンドウを作る(動画・BGA など)。
      位置・大きさ・全画面・常に手前に表示・表示/非表示・曲の切り替わりは本体が行う。
    ・自分のウィンドウ(外部のアプリのウィンドウを含む)の表示・位置などを、本体の指示に従わせる。
    ・本体の再生位置・再生状態を知る(音はデコードより遅れて鳴るので、映像を音に合わせるのに使う)。
  使うものだけに対応すれば良い(使わない機能は GetCaps で返さなければ、本体は呼ばない)。

  本体の呼び方：
    Open → (QueryInterface) → Select → GetCaps → (SetPlayStatus) → (CreateVideoWindow) → Seek → Render …
    ・QueryInterface は、対応しているかを確かめるだけの目的で呼ばれることがある。
      答えただけでは何も始めないこと(ウィンドウを出すのは CreateVideoWindow が呼ばれてから)。
    ・GetCaps・SetPlayStatus・CreateVideoWindow は、Select と同じスレッドから、Select の後・
      その曲の最初の Seek の前に呼ばれる。Seek・Render とは同時に呼ばれない。
    ・SetVisible・SetTopMost・GetWindowPosition・SetWindowPosition は、CreateVideoWindow が
      成功した後に呼ばれる。Seek・Render とは別のスレッドから、Seek・Render の最中にも呼ばれる。
      すぐに戻ること。Seek・Render の終わりを待ってはいけない
      (自分のウィンドウのスレッドに PostMessage する、ShowWindowAsync・SWP_ASYNCWINDOWPOS を使うなど)。
      Select・GetCaps・SetPlayStatus・CreateVideoWindow とは同時に呼ばれない。
    ・タグの取得などで、演奏しないために開いたときは、GetCaps 以降は呼ばれない。
      CreateVideoWindow が呼ばれなかったら、ウィンドウを表示しないこと。
      (この口に対応していない本体や、本体以外のアプリから使われたときも同じ)
    ・本体は、次の曲を先に開いておくことがある(ギャップレス再生)。CreateVideoWindow が呼ばれても、
      SetVisible(TRUE) が呼ばれるか、本体が子ウィンドウを表示するまでは、まだ前の曲が鳴っている。

  kbrunkpi.exe 経由で動いているときも同じに使える(プラグインは意識しなくて良い)。
  ウィンドウの指示は、Seek・Render の合間に呼ばれる(同時には呼ばれない)。
*/
///////////////////////////////////////////////////////////////////////////////
class __declspec(uuid("{A6DBD3F5-D5C0-495D-AD8A-7A97A6B00ABF}")) IKpiVideoWindow;
#define IID_IKpiVideoWindow __uuidof(IKpiVideoWindow)
///////////////////////////////////////////////////////////////////////////////
enum{//IKpiVideoWindow::GetCaps の戻り値(組み合わせ)
     //(VIDEO_PARENT・WINDOW_MOVE・WINDOW_SIZE は、演奏プラグインの KPI_PLAYER_CAPS_XXX と同じ値・同じ意味)
    KPI_VIDEOWINDOW_CAPS_VIDEO_PARENT = 0x08, //本体が作った親ウィンドウの中に映像を描ける(CreateVideoWindow に親ウィンドウを渡して良い)
    KPI_VIDEOWINDOW_CAPS_WINDOW_MOVE  = 0x10, //SetWindowPosition の nLeft/nTop に従える(自分のウィンドウの位置を変えられる)
    KPI_VIDEOWINDOW_CAPS_WINDOW_SIZE  = 0x20, //SetWindowPosition の nVideoWidth/nVideoHeight に従える(大きさを変えられる)
    KPI_VIDEOWINDOW_CAPS_WINDOW       = 0x100,//自分のウィンドウを使える(CreateVideoWindow に NULL を渡して良い)
    KPI_VIDEOWINDOW_CAPS_PLAY_STATUS  = 0x200,//本体の再生位置・再生状態が要る(SetPlayStatus を呼んでほしい)
};
///////////////////////////////////////////////////////////////////////////////
struct KPI_PLAY_STATUS
{//本体の再生位置・再生状態(本体が書き、プラグインは読むだけ)
    DWORD cb;            //=sizeof(KPI_PLAY_STATUS)
    DWORD dwPosition;    //再生位置(ミリ秒。今鳴っている音の位置)
    DWORD dwStatus;      //再生状態(KPI_STATUS_STOP/PLAYING/PAUSE。kpi_player.h と同じ値：0/1/2)
    DWORD dwReserved[5]; //予約(0)
/*
    ・本体が別のスレッドから書き換えるので、volatile として読むこと(SetPlayStatus の引数の型のまま使えば良い)。
    ・dwPosition は、再生中は数ミリ秒〜数十ミリ秒ごとに更新される。細かい時刻が要る場合は、
      値が変わった時刻を自分で覚えて補うこと。シーク・ループ・曲の頭で、前より小さい値になることがある。
    ・dwStatus は、ギャップレス再生で先に開かれた曲では、音が切り替わる前から PLAYING になっていることがある。
      表示して良いかの判断には使わないこと(表示は SetVisible か、本体が行う)。
    ・曲が鳴り終わった後は、最後の値のまま更新されなくなる。
*/
};
///////////////////////////////////////////////////////////////////////////////
class IKpiVideoWindow : public IKpiUnknown
{//プラグインが実装、本体が使用する(IKpiDecoder::QueryInterface で取得)
public:
    virtual DWORD WINAPI GetCaps(void)=0;
    virtual void  WINAPI SetPlayStatus(const volatile KPI_PLAY_STATUS *pStatus)=0;
    virtual BOOL  WINAPI CreateVideoWindow(HWND hParent, HWND *phChild)=0;
    virtual void  WINAPI SetVisible(BOOL bVisible)=0;
    virtual void  WINAPI SetTopMost(BOOL bTopMost)=0;
    virtual void  WINAPI GetWindowPosition(int *pLeft, int *pTop,
                                           int *pVideoWidth, int *pVideoHeight)=0;
    virtual void  WINAPI SetWindowPosition(int nLeft, int nTop,
                                           int nVideoWidth, int nVideoHeight)=0;
/*
    IKpiPlayer(kpi_player.h)の同じ名前のメソッドとは、引数・戻り値の意味を揃えてある。
    違うのは、呼ばれるスレッド(上の「本体の呼び方」)と、最初の表示
    (演奏プラグインは Play の bVisible、こちらは SetVisible(TRUE) まで表示しない)。

    DWORD WINAPI GetCaps(void);
      Select で選んだ曲で使う機能を KPI_VIDEOWINDOW_CAPS_XXX の組み合わせで返す。
      本体は Select の後に呼ぶ。次に Select を呼ぶまで、同じ値を返すこと。曲ごとに変えて良い。
      映像もウィンドウもない曲では、VIDEO_PARENT・WINDOW を返さないこと(0 で良い)。
      映像を出す場合は、KPI_MEDIAINFO の dwVideoWidth/dwVideoHeight も設定する(今までどおり)。
      VIDEO_PARENT と WINDOW の両方を返しても良い(本体が選ぶ。今の本体は親ウィンドウを先に試す)。

    void WINAPI SetPlayStatus(const volatile KPI_PLAY_STATUS *pStatus);
      本体の再生位置・再生状態の場所を受け取る。
      本体は、GetCaps が KPI_VIDEOWINDOW_CAPS_PLAY_STATUS を返した場合だけ、1 つのインスタンスに 1 度だけ呼ぶ
      (1 ファイル複数曲では、このフラグが初めて返った Select の後)。
      pStatus の指す先は、IKpiDecoder を解放する(Release で破棄する)まで有効。プラグインは読むだけ。
      解放した後は読まないこと(自分のスレッドから読む場合は、解放までにスレッドを止めること)。

    BOOL WINAPI CreateVideoWindow(HWND hParent, HWND *phChild);
      「これから映像(ウィンドウ)の表示を始めるので準備して」の合図。
      本体は、GetCaps が VIDEO_PARENT か WINDOW を返した場合だけ、1 つのインスタンスに 1 度だけ呼ぶ
      (1 ファイル複数曲では、これらのフラグが初めて返った Select の後。下の呼び直しを除く)。
      成功したら TRUE、失敗したら FALSE を返す。*phChild には、どの場合も値を入れること(なければ NULL)。

      hParent が NULL でない場合(GetCaps が VIDEO_PARENT を返したときだけ)：
        本体が作った映像の親ウィンドウ hParent の中に、映像を描く子ウィンドウを作って *phChild に返す。
        ・子ウィンドウは hParent の子(WS_CHILD)で、隠して作り、EnableWindow(FALSE) で無効にしておく
          (マウスの操作は親ウィンドウが受け取る。移動・全画面・右クリックのメニューなどは本体が行う)
        ・表示・位置・大きさ・全画面・常に手前に表示・タイトルは本体が親ウィンドウで行う。
          子ウィンドウの大きさは親ウィンドウが合わせる(親ウィンドウのクライアント領域いっぱい)ので、
          プラグインは WM_SIZE などで映像の描画を合わせる(縦横比を保つ場合の余白もプラグインが描く)
        ・タイトルを決めたい場合は、子ウィンドウにウィンドウ名を付ける(SetWindowText など)。本体は、表示している
          子ウィンドウに名前があれば、それをそのまま親ウィンドウのタイトルにする。付け方はプラグインの自由で、
          本体の付け方に合わせなくて良い。後から変えても良い(少し遅れて反映される)。長すぎる分は切られる。
          名前がなければ(空なら)、本体がタイトルを決める(今の本体は「Video - ファイル名 -」)
        ・本体は SetVisible・SetTopMost・GetWindowPosition・SetWindowPosition を呼ばない
        ・子ウィンドウは、プラグインが自分で作ったスレッドで作り、そのスレッドはメッセージを処理し続けること。
          CreateVideoWindow を呼んだスレッドで作ってはいけない
          (親ウィンドウは別のスレッドのウィンドウなので、子ウィンドウのスレッドが止まると親ウィンドウの操作も止まる)
        ・子ウィンドウだけをすぐに作って返し、時間のかかる準備(画像の読み込みなど)は後から続けること
          (その間は黒い画面で良い)。子ウィンドウが出来るのを待つ時間には上限を付けること
        ・子ウィンドウは IKpiDecoder を解放するまでに消すこと。親ウィンドウは消さないこと
        ・子ウィンドウを作れなかった場合は、*phChild を NULL にして FALSE を返す。
          GetCaps が WINDOW も返していれば、本体は hParent を NULL にしてもう一度呼ぶ
        本体の約束：親ウィンドウは、IKpiDecoder を解放するまで消さず、作り直さず、子ウィンドウを別の親に
        付け替えない。子ウィンドウの中身を途中で作り直したい場合は、返した子ウィンドウを入れ物にして、
        その中に作った子(孫)を作り直すこと(本体から見た子ウィンドウは解放まで同じにする)。
        kbrunkpi.exe 経由のときは、hParent は kbrunkpi.exe のプロセスの中に作られた親ウィンドウになる
        (本体の親ウィンドウと同じに扱って良い)。

      hParent が NULL の場合(GetCaps が WINDOW を返したときだけ)：
        自分のウィンドウ(外部のアプリのウィンドウを含む)を、隠したまま準備する。*phChild は NULL にする。
        以後の表示・常に手前に表示・位置は、本体が SetVisible・SetTopMost・SetWindowPosition で指示する。
        トップレベルのウィンドウでないと動かないものや、別のプロセスのウィンドウは、こちらで実装する。
        準備に時間がかかる場合は、始めるだけで戻って良い(その間の指示は覚えておき、出来てから従う)。
        利用者が閉じるボタンなどでウィンドウを閉じたときは、出来るだけ隠すだけにして、次に SetVisible(TRUE) が
        呼ばれたらまた表示すること(本体の親ウィンドウと同じ動き)。本体は閉じられたことを知らないので、次の曲・
        停止の後の再生・表示の設定を変えたときなどに、今までどおり SetVisible(TRUE) を呼ぶ。
        外部のアプリが閉じられて終了してしまう場合は、次の SetVisible(TRUE) で起動し直すか、何もしなくて良い
        (デコード(音)は止めないこと)。

    void WINAPI SetVisible(BOOL bVisible);
      自分のウィンドウの表示・非表示を変える。デコードの状態は変えない。
      本体は、その曲の音が鳴り始めるときに TRUE で、鳴り終わったときや「停止時に隠す」の設定で止めたとき、
      「Video」の表示モードが映像を出さない設定になったときに FALSE で呼ぶ。
      SetVisible(TRUE) が呼ばれるまでは表示しないこと。
      対応出来ない場合は何もしなくて良い(本体は表示の状態を取得しない)。

    void WINAPI SetTopMost(BOOL bTopMost);
      自分のウィンドウを常に手前に表示するか。対応出来ない場合や、プラグインの都合で従えない場合は、
      何もしなくて良い(本体は状態を取得しない)。

    void WINAPI GetWindowPosition(int *pLeft, int *pTop, int *pVideoWidth, int *pVideoHeight);
    void WINAPI SetWindowPosition(int nLeft, int nTop, int nVideoWidth, int nVideoHeight);
      IKpiPlayer の同じ名前のメソッドと同じ(kpi_player.h)。
      nLeft/nTop はウィンドウの左上の位置(スクリーン座標)、nVideoWidth/nVideoHeight は映像を表示する
      領域(クライアント領域)の大きさ。GetWindowPosition は実際の位置と大きさを返す。
      GetCaps で、従えるものだけ WINDOW_MOVE(位置)・WINDOW_SIZE(大きさ)を返す。従えない方の値は無視して良い。
      本体は、動画のウィンドウと同じ位置・大きさの設定を使う(フラグのある方だけ。フラグのない方は覚えない)。
      本体と同じ位置にしたくない場合は、どちらのフラグも返さず、プラグインが自分で位置を管理すれば良い。
      GetWindowPosition は、少し前の値を返しても良い(本体は、利用者が動かした位置を覚えるために時々呼ぶ)。
*/
};
///////////////////////////////////////////////////////////////////////////////
#pragma pack(pop) //構造体のアラインメント
#endif //kpi_video_windowH
