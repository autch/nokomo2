#pragma once
#ifndef kpi_workfolderH
#define kpi_workfolderH

#include "kpi.h"

///////////////////////////////////////////////////////////////////////////////
/*プラグイン用の作業フォルダ(2026/11 追加)

  プラグインが自分で作るファイル(変換した結果・外部のプログラムや DLL にファイル名で
  渡すためのコピーなど)の置き場所を、本体から借りるための口。
  プラグインが自分で %TEMP% の下にフォルダを作り、使い終わったら消す、という管理を
  しなくて済む。どの種類のプラグインでも使える。

  再生するファイルそのものを実ファイルとして使いたいだけなら、この口ではなく
  IKpiFile::GetRealFileW/A を使うこと(一時ファイルの作成と削除は本体が行う)。
  アーカイブプラグイン・変換プラグインは、実ファイルが必要な場合はこの口で作業フォルダを借りる
  (2026/11/25 に IKpiUnArcModule::Open と IKpiConverterModule::Open の cszOutputPath を廃止した。
   kpi_unarc.h・kpi_converter.h を参照)。

  取得の仕方：
    kpi_CreateInstance の第3引数 pUnknown を kpi_CreateWorkFolder(下のヘルパー関数)に渡す。
    kpi_CreateInstance から戻った後に(Open の中などで)作りたい場合は、pUnknown から
    QueryInterface で IKpiUnkProvider を取得して持っておき(モジュールの解放時に Release する)、
    必要になったときに kpi_CreateWorkFolder(IKpiUnkProvider 版)に渡す。
    モジュールが生きている間はいつ呼んでも良い(kbrunkpi.exe 経由で動いている場合も同じ)。

  得られるもの：
    ・呼ぶたびに、新しい空のフォルダが 1 つ作られる。同時にいくつ作っても良い。
      他のプラグイン・他のインスタンスと共有されない。
    ・GetPath で得たフォルダの中に、ファイルやフォルダを自由に作って良い。
      リンク(ジャンクション・シンボリックリンク等)は作らないこと。
    ・最後の Release で、本体がフォルダを中身ごと削除する。
      プラグインは、Release の前に中のファイルを閉じておくこと(子プロセスに使わせて
      いる場合は、子プロセスの終了を待ってから Release する)。中のファイルをプラグインが
      自分で削除する必要はない(削除しても良い)。
      使用中のファイルが残っていて削除できなかった分は、本体の終了時に削除される。
    ・プラグインは、遅くともモジュールの解放時までに Release すること。

  失敗する場合(*ppWorkFolder が NULL)：
    ・この口に対応していない本体(v3.17beta7 より前)や、本体以外のアプリから使われた場合。
    ・本体が作業フォルダを作れなかった場合(%TEMP% が使えない等)。
    ・KPI_WORKFOLDER_FLAG_ANSI を指定したが、作業フォルダのパスが ANSI で表せない場合
      (本体は ANSI で表せる別の場所を探さない)。
    作業フォルダが必須のプラグインは、得られなければ Open を失敗させること。
*/
///////////////////////////////////////////////////////////////////////////////
class __declspec(uuid("{41ED7B0A-CFCB-4093-9259-64559C3A6D6D}")) IKpiWorkFolder;
#define IID_IKpiWorkFolder __uuidof(IKpiWorkFolder)
///////////////////////////////////////////////////////////////////////////////
enum{
    //kpi_CreateWorkFolder の dwFlags
    KPI_WORKFOLDER_FLAG_ANSI = 0x01,//作業フォルダのパスが ANSI で表せることを求める(表せなければ失敗)
        //ファイル名を ANSI でしか受け取れないライブラリ等に、中のファイルを渡す場合に指定する。
        //成功した場合、GetPath の結果は WideCharToMultiByte(CP_ACP) で欠けずに変換できる。
        //(中に作るファイルの名前を ANSI で表せるものにするのはプラグインの役目)
    //上記以外のビットは予約(0 にすること。未定義のビットを指定すると失敗する)
};
///////////////////////////////////////////////////////////////////////////////
class IKpiWorkFolder : public IKpiUnknown
{//本体が実装、プラグイン側は使用するのみ
public:
    virtual const wchar_t* WINAPI GetPath(void)=0;
/*
    const wchar_t* WINAPI GetPath(void);
      作業フォルダのフルパスを返す(末尾に \ が付く。例: "C:\...\Temp\Kbmplay\ABCDEFGH\")。
      ・戻り値は NULL にも空文字列にもならない。フォルダは作成済みで、中は空。
      ・戻り値のポインタは、このインスタンスの最後の Release まで有効。
      ・どのスレッドから呼んでも良い。
      ・パスの長さは MAX_PATH 未満だが、どれだけ短いかは決まっていない。
        中に作るファイルのフルパスが MAX_PATH に収まるかは、プラグインが確かめること。
*/
};
///////////////////////////////////////////////////////////////////////////////
//ヘルパー関数(定義は kpi.cpp。プラグインは kpi.cpp をプロジェクトに追加すること)
//IKpiWorkFolder を作る。成功したら 0 以外を返し、*ppWorkFolder にインスタンスを入れる。
//失敗したら 0 を返し、*ppWorkFolder に NULL を入れる
DWORD WINAPI kpi_CreateWorkFolder(IKpiUnkProvider *pProvider,
                                  DWORD dwFlags,//KPI_WORKFOLDER_FLAG_XXX の組み合わせ(通常は 0)
                                  IKpiWorkFolder **ppWorkFolder);
DWORD WINAPI kpi_CreateWorkFolder(IKpiUnknown *pUnknown, //kpi_CreateInstance の第3引数から直接作る
                                  DWORD dwFlags,
                                  IKpiWorkFolder **ppWorkFolder);
///////////////////////////////////////////////////////////////////////////////
#endif
