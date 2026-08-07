#include "kmp_adapter.hpp"

#include "strconv.hpp"

namespace nokomo::detail {

namespace {

///////////////////////////////////////////////////////////////////////////////
// IKmpTagInfo → IKpiTagInfo ブリッジ
// 旧仕様の SetValueX(サイズ引数なし、NUL 終端)を新仕様へ中継する。
// SetValue(NULL, NULL)(本体タグ取得の抑止)もそのまま伝わる。
class KmpTagBridge final : public IKmpTagInfo {
public:
    explicit KmpTagBridge(IKpiTagInfo* sink) : sink_(sink) {}
    void WINAPI aSetValueA(const char* n, const char* v) override
    {
        sink_->aSetValueA(n, -1, v, -1);
    }
    void WINAPI aSetValueW(const char* n, const wchar_t* v) override
    {
        sink_->aSetValueW(n, -1, v, -1);
    }
    void WINAPI aSetValueU8(const char* n, const char* v) override
    {
        sink_->aSetValueU8(n, -1, v, -1);
    }
private:
    IKpiTagInfo* sink_;
};

///////////////////////////////////////////////////////////////////////////////
class KmpAdapterDecoder final : public KbKpiDecoderImpl {
public:
    KmpAdapterDecoder(IKpiDecoderModule* owner, KMPMODULE* mod, pfnGetTagInfo tagFn,
                      HKMP handle, const SOUNDINFO& si, DWORD count,
                      std::string ansiPath, IKpiFile* file)
        : owner_(owner), mod_(mod), tagFn_(tagFn), h_(handle), si_(si),
          count_(count), ansiPath_(std::move(ansiPath)), file_(file)
    {
        owner_->AddRef();
        file_->AddRef(); // 実ファイル名を使用中は IKpiFile を解放してはいけない(仕様)
        multiSong_ = (si_.dwReserved2 != 0);
        INT32 bits = (INT32)si_.dwBitsPerSample; // 負値 = float
        bytesPerFrame_ = (DWORD)((bits < 0 ? -bits : bits) / 8) * si_.dwChannels;
    }

    DWORD WINAPI Select(DWORD dwNumber, const KPI_MEDIAINFO** ppMediaInfo,
                        IKpiTagInfo* pTagInfo, DWORD /*dwTagGetFlags*/) override
    {
        if (dwNumber > count_) {
            if (ppMediaInfo) *ppMediaInfo = nullptr;
            return 0;
        }
        if (dwNumber >= 1) {
            if (multiSong_ && dwNumber != currentSong_) {
                mod_->SetPosition(h_, dwNumber * 1000); // 旧仕様の選曲プロトコル
                currentSong_ = dwNumber;
            }
        }
        // 旧仕様のタグはファイル単位(kmp_GetTestTagInfo)。曲番号によらず同じ。
        if (pTagInfo && tagFn_) {
            KmpTagBridge bridge(pTagInfo);
            tagFn_(ansiPath_.c_str(), &bridge);
        }
        if (ppMediaInfo) {
            if (dwNumber == 0) {
                *ppMediaInfo = nullptr; // 全体情報要求時にメディア情報はない
            } else {
                fillMediaInfo(dwNumber);
                *ppMediaInfo = &mi_;
            }
        }
        return dwNumber == 0 ? count_ : dwNumber;
    }

    DWORD WINAPI Render(BYTE* pBuffer, DWORD dwSizeSample) override
    {
        DWORD got = mod_->Render(h_, pBuffer, dwSizeSample * bytesPerFrame_);
        return got / bytesPerFrame_;
    }

    UINT64 WINAPI Seek(UINT64 qwPosSample, DWORD /*dwFlag*/) override
    {
        if (multiSong_ && qwPosSample > 0) {
            // 旧仕様の複数曲では SetPosition が選曲を兼ねるためシーク不可。
            // 誤って曲が切り替わらないよう先頭に戻すだけにする。
            mod_->SetPosition(h_, 0);
            return 0;
        }
        UINT64 ms = qwPosSample * 1000 / si_.dwSamplesPerSec;
        DWORD ret = mod_->SetPosition(h_, (DWORD)ms);
        return (UINT64)ret * si_.dwSamplesPerSec / 1000;
    }

    DWORD WINAPI UpdateConfig(void*) override { return 0; }

private:
    ~KmpAdapterDecoder() override
    {
        mod_->Close(h_);
        file_->Release(); // 実ファイルを閉じた後に解放
        owner_->Release();
    }

    void fillMediaInfo(DWORD number)
    {
        kpi_InitMediaInfo(&mi_);
        mi_.dwNumber = number;
        mi_.dwCount = count_;
        mi_.dwFormatType = KPI_MEDIAINFO::FORMAT_PCM;
        mi_.dwSampleRate = si_.dwSamplesPerSec;
        mi_.nBitsPerSample = (INT32)si_.dwBitsPerSample; // 負値(float)もそのまま通る
        mi_.dwChannels = si_.dwChannels;
        if (multiSong_ || si_.dwLength == 0xFFFFFFFF) {
            mi_.qwLength = (UINT64)-1; // 旧仕様の複数曲は曲ごとの長さを取得出来ない
        } else {
            mi_.qwLength = kpi_MsTo100ns(si_.dwLength);
        }
        // dwReserved1=1: Render を繰り返しても終了しない曲。
        // ホストが長さ(または既定長)+フェードで打ち切る = qwLoop 不明として扱う
        mi_.qwLoop = si_.dwReserved1 ? (UINT64)-1 : 0;
        mi_.qwFadeOut = (UINT64)-1; // ホスト設定に従う
        mi_.dwUnitSample = bytesPerFrame_ ? si_.dwUnitRender / bytesPerFrame_ : 0;
        // 旧仕様の dwSeekable はミリ秒精度(歌詞同期に足る)を意味する
        mi_.dwSeekableFlags = (si_.dwSeekable && !multiSong_)
            ? (KPI_MEDIAINFO::SEEK_FLAGS_ACCURATE | KPI_MEDIAINFO::SEEK_FLAGS_ROUGH)
            : 0;
    }

    IKpiDecoderModule* owner_;
    KMPMODULE* mod_;
    pfnGetTagInfo tagFn_;
    HKMP h_;
    SOUNDINFO si_;
    DWORD count_;
    std::string ansiPath_;
    IKpiFile* file_;
    bool multiSong_ = false;
    DWORD bytesPerFrame_ = 0;
    DWORD currentSong_ = 1; // 旧仕様: SetPosition 未呼び出し時は 1 曲目
    KPI_MEDIAINFO mi_{};
};

///////////////////////////////////////////////////////////////////////////////
class KmpAdapterModule final : public KbKpiDecoderModuleImpl {
public:
    KmpAdapterModule(KMPMODULE* mod, pfnGetTagInfo tagFn) : mod_(mod), tagFn_(tagFn)
    {
        desc_ = mod_->pszDescription ? fromAnsi(mod_->pszDescription) : L"";
        copyright_ = mod_->pszCopyright ? fromAnsi(mod_->pszCopyright) : L"";
        if (mod_->ppszSupportExts) {
            for (const char** p = mod_->ppszSupportExts; *p; ++p) {
                if (!exts_.empty()) exts_ += L'/';
                exts_ += toLower(fromAnsi(*p));
            }
        }

        ZeroMemory(&info_, sizeof(info_));
        info_.cb = sizeof(info_);
        info_.dwModuleVersion = KPI_DECODER_MODULE_VERSION; // 新仕様として振る舞う
        info_.dwPluginVersion = mod_->dwPluginVersion;
        info_.dwMultipleInstance = multiInstanceFrom(mod_->dwReentrant);
        info_.cszDescription = desc_.c_str();
        info_.cszCopyright = copyright_.c_str();
        info_.cszSupportExts = exts_.c_str();
        info_.cszMultiSongExts = L""; // 旧仕様はモジュール情報から判別不可(Open 時に判明)
        // kmp_GetTestTagInfo があればデコードせずにタグ取得可能(タグ専用オープン可)
        info_.dwSupportTagInfo = tagFn_ ? KPI_DECODER_MODULEINFO::TAG_TITLE : 0;

        if (mod_->Init) mod_->Init();
        inited_ = true;
    }

    void WINAPI GetModuleInfo(const KPI_DECODER_MODULEINFO** ppInfo) override
    {
        *ppInfo = &info_;
    }

    DWORD WINAPI Open(const KPI_MEDIAINFO* cpRequest, IKpiFile* pFile,
                      IKpiFolder* /*pFolder*/, IKpiDecoder** ppDecoder) override
    {
        *ppDecoder = nullptr;
        // 旧仕様は ANSI の実ファイル名からしか開けない
        const char* ansi = nullptr;
        if (!pFile || !pFile->GetRealFileA(&ansi) || !ansi) return 0;

        SOUNDINFO si;
        ZeroMemory(&si, sizeof(si));
        if (cpRequest) {
            si.dwSamplesPerSec = cpRequest->dwSampleRate;
            si.dwBitsPerSample = (DWORD)cpRequest->nBitsPerSample;
            si.dwChannels = cpRequest->dwChannels;
        }
        HKMP h = mod_->Open(ansi, &si);
        if (!h) return 0;
        // 旧仕様: 適切な情報が入っていない場合は直ちに Close する(本体と同じ)
        if (si.dwSamplesPerSec == 0 || si.dwChannels == 0 ||
            ((INT32)si.dwBitsPerSample > 0 ? si.dwBitsPerSample % 8 : (-(INT32)si.dwBitsPerSample) % 8) != 0 ||
            si.dwBitsPerSample == 0) {
            mod_->Close(h);
            return 0;
        }
        DWORD count = 1;
        if (si.dwReserved2) {
            count = si.dwLength / 1000; // 曲数を 1000 倍した値(仕様)
            if (count == 0) {
                mod_->Close(h);
                return 0;
            }
        }
        *ppDecoder = new KmpAdapterDecoder(this, mod_, tagFn_, h, si, count, ansi, pFile);
        return count;
    }

    BOOL WINAPI EnumConfig(IKpiConfigEnumerator*) override { return FALSE; }
    DWORD WINAPI ApplyConfig(const wchar_t*, const wchar_t*, INT64, double,
                             const wchar_t*) override { return KPI_CFGRET_OK; }

private:
    ~KmpAdapterModule() override
    {
        if (inited_ && mod_->Deinit) mod_->Deinit();
    }

    static DWORD multiInstanceFrom(DWORD dwReentrant)
    {
        if (dwReentrant == 1) return KPI_MULTINST_INFINITE;
        if (dwReentrant == 0xFFFFFFFF) return KPI_MULTINST_ZERO; // ホストでは同時1個扱い
        return KPI_MULTINST_ONE; // dwReentrant == 0
    }

    KMPMODULE* mod_;
    pfnGetTagInfo tagFn_;
    bool inited_ = false;
    std::wstring desc_, copyright_, exts_;
    KPI_DECODER_MODULEINFO info_{};
};

} // namespace

IKpiDecoderModule* createKmpAdapterModule(KMPMODULE* mod, pfnGetTagInfo tagFn)
{
    return new KmpAdapterModule(mod, tagFn);
}

} // namespace nokomo::detail
