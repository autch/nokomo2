#include "session.hpp"

#include <vector>

#include "pcm_convert.hpp"
#include "strconv.hpp"

namespace nokomo::detail {

struct SessionImpl {
    PluginInfo info;
    HostOptions opt;
    DecodeRequest req;

    // 破棄順が重要: メンバは宣言の逆順で破棄される。
    // decoder(最後に宣言) → folder → file → slot → module の順に解放され、
    // デコーダ解放時点でファイルとモジュール(DLL)がまだ生きていることを保証する。
    std::shared_ptr<LoadedModule> module;
    LoadedModule::SlotGuard slot{nullptr};
    ComPtr<IKpiFile> file;
    ComPtr<IKpiFolder> folder;
    ComPtr<IKpiDecoder> decoder;

    uint32_t trackCount = 0;
    MediaInfo mi;
    TrackTags tags;

    bool selected = false;
    bool ended = true;
    uint64_t renderPos = 0;              // Render で取得済みのフレーム数(打ち切り適用後)
    std::vector<uint8_t> chunk;          // Render 用バッファ(dwUnitSample 対応)
    size_t chunkOff = 0, chunkLen = 0;   // バイト単位

    bool selectTrack(uint32_t number);
    size_t read(void* dst, size_t frames);
    void resolveTotals();
};

///////////////////////////////////////////////////////////////////////////////

void SessionImpl::resolveTotals()
{
    mi.totalFrames = kUnknown64;
    mi.fadeFrames = 0;
    const uint32_t rate = mi.sampleRate;
    if (!rate) return;

    if (mi.loop100ns == 0) {
        // 非ループ曲: Render の自然終了に任せる(qwFadeOut/dwLoopCount は無視: 仕様)
        return;
    }

    const uint64_t hostFade100 =
        (req.fadeMs != kUnknown64 ? req.fadeMs : opt.defaultFadeMs) * 10000;
    const uint64_t effFade100 =
        (mi.fadeOut100ns == kUnknown64) ? hostFade100 : mi.fadeOut100ns;
    const uint64_t len100 =
        (mi.length100ns == kUnknown64) ? opt.defaultLengthMs * 10000 : mi.length100ns;

    uint64_t total100;
    if (mi.loop100ns == kUnknown64) {
        // ループ時間不明(無限ループの可能性): 1ループ分+フェードで打ち切り
        total100 = len100 + effFade100;
    } else {
        uint32_t loops = mi.loopCount ? mi.loopCount
                                      : (req.loopCount ? req.loopCount : opt.defaultLoopCount);
        if (loops == 0) loops = 1;
        total100 = len100 + (uint64_t)(loops - 1) * mi.loop100ns + effFade100;
    }

    // kpi_decoder.h 記載の手順: 100ns → サンプル(切り捨て)
    mi.totalFrames = total100 * rate / (1000 * 10000);
    const uint64_t beforeFade = (total100 - effFade100) * rate / (1000 * 10000);
    mi.fadeFrames = mi.totalFrames - beforeFade;
}

bool SessionImpl::selectTrack(uint32_t number)
{
    selected = false;
    ended = true;
    if (!decoder || number == 0) return false;

    ComPtr<TagSink> sink;
    sink.attach(new TagSink);
    const KPI_MEDIAINFO* pmi = nullptr;
    DWORD got = decoder->Select(number, &pmi, sink.get(), KPI_TAGGET_FLAG_ALL);
    if (got == 0 || !pmi) return false;

    // *pmi の寿命は次のメソッド呼び出しまで。直ちにコピーする。
    mi = MediaInfo{};
    mi.trackNumber = pmi->dwNumber;
    mi.trackCount = pmi->dwCount;
    mi.sampleRate = pmi->dwSampleRate;
    mi.bitsPerSample = pmi->nBitsPerSample;
    mi.channels = pmi->dwChannels;
    mi.formatType = pmi->dwFormatType;
    mi.length100ns = pmi->qwLength;
    mi.loop100ns = pmi->qwLoop;
    mi.fadeOut100ns = pmi->qwFadeOut;
    mi.loopCount = pmi->dwLoopCount;
    mi.unitSample = pmi->dwUnitSample;
    mi.seekableFlags = pmi->dwSeekableFlags;

    sink->harvest(&tags);

    if (mi.sampleRate == 0 || mi.channels == 0 || mi.bytesPerSample() == 0) return false;
    // 新仕様の dwUnitSample は 16384 以下だが、旧仕様アダプタ経由では
    // dwUnitRender 由来でこれを超え得るため、明らかな異常値のみ弾く
    if (mi.unitSample > (1u << 22)) return false;

    resolveTotals();

    // 仕様: 選曲直後の再生位置は未定義。本体は Render 前に必ず Seek を呼ぶ。
    decoder->Seek(0, KPI_MEDIAINFO::SEEK_FLAGS_SAMPLE);

    renderPos = 0;
    chunkOff = chunkLen = 0;
    ended = false;
    selected = true;
    return true;
}

size_t SessionImpl::read(void* dst, size_t frames)
{
    if (!selected) return 0;
    const uint32_t bpf = mi.bytesPerFrame();
    uint8_t* out = (uint8_t*)dst;
    size_t served = 0;

    while (served < frames) {
        if (chunkOff < chunkLen) {
            size_t availFrames = (chunkLen - chunkOff) / bpf;
            size_t n = frames - served < availFrames ? frames - served : availFrames;
            memcpy(out + served * bpf, chunk.data() + chunkOff, n * bpf);
            chunkOff += n * bpf;
            served += n;
            continue;
        }
        if (ended) break;

        const uint32_t unit = mi.unitSample ? mi.unitSample : 4096;
        chunk.resize((size_t)unit * bpf);
        DWORD got = decoder->Render(chunk.data(), unit);
        if (got > unit) got = unit; // 不正な戻り値への保険
        const bool natural = (got < unit);

        // ループ曲の打ち切りとフェードアウト(ホストの責務)
        if (mi.totalFrames != kUnknown64) {
            const uint64_t start = renderPos;
            if (start >= mi.totalFrames) {
                got = 0;
            } else if (start + got > mi.totalFrames) {
                got = (DWORD)(mi.totalFrames - start);
            }
            if (got > 0 && mi.fadeFrames > 0) {
                const uint64_t fadeStart = mi.totalFrames - mi.fadeFrames;
                if (start + got > fadeStart) {
                    const uint64_t first = start > fadeStart ? start : fadeStart;
                    const size_t offFrames = (size_t)(first - start);
                    const size_t count = (size_t)(start + got - first);
                    const double step = -1.0 / (double)mi.fadeFrames;
                    const double gain0 = (double)(mi.totalFrames - first) / (double)mi.fadeFrames;
                    applyGainFrames(chunk.data() + offFrames * bpf, mi.bitsPerSample,
                                    mi.channels, count, gain0, step);
                }
            }
            if (start + got >= mi.totalFrames) ended = true;
        }
        if (natural) ended = true;

        renderPos += got;
        chunkOff = 0;
        chunkLen = (size_t)got * bpf;
        if (got == 0 && ended) break;
    }
    return served;
}

} // namespace nokomo::detail

///////////////////////////////////////////////////////////////////////////////
// DecodeSession(公開クラス)
///////////////////////////////////////////////////////////////////////////////

namespace nokomo {

DecodeSession::DecodeSession(std::unique_ptr<detail::SessionImpl> impl)
    : impl_(std::move(impl)) {}

DecodeSession::~DecodeSession() = default;

const PluginInfo& DecodeSession::plugin() const { return impl_->info; }
uint32_t DecodeSession::trackCount() const { return impl_->trackCount; }
bool DecodeSession::selectTrack(uint32_t number) { return impl_->selectTrack(number); }
const MediaInfo& DecodeSession::mediaInfo() const { return impl_->mi; }
const TrackTags& DecodeSession::tags() const { return impl_->tags; }
size_t DecodeSession::read(void* buffer, size_t frames) { return impl_->read(buffer, frames); }
bool DecodeSession::eof() const
{
    return impl_->ended && impl_->chunkOff >= impl_->chunkLen;
}

} // namespace nokomo

///////////////////////////////////////////////////////////////////////////////

namespace nokomo::detail {

std::unique_ptr<DecodeSession> createDecodeSession(std::shared_ptr<LoadedModule> module,
                                                   const PluginInfo& info,
                                                   const std::wstring& filePath,
                                                   const DecodeRequest& req,
                                                   const HostOptions& opt,
                                                   std::wstring* errOut)
{
    auto impl = std::make_unique<SessionImpl>();
    impl->info = info;
    impl->opt = opt;
    impl->req = req;
    impl->module = module;
    impl->slot = LoadedModule::SlotGuard(module); // 同時インスタンス数制限

    impl->file.attach(LocalKpiFile::open(filePath));
    if (!impl->file) {
        if (errOut) *errOut = L"ファイルを開けない: " + filePath;
        return nullptr;
    }
    IKpiFolder* folder = DirectoryFolder::open(parentDirOf(filePath));
    if (!folder) folder = new KbKpiNullFolder; // 何もしない IKpiFolder(SDK 提供)
    impl->folder.attach(folder);

    KPI_MEDIAINFO reqInfo;
    kpi_InitMediaInfo(&reqInfo);
    reqInfo.dwSampleRate = req.sampleRate;
    reqInfo.nBitsPerSample = req.bitsPerSample;
    reqInfo.dwLoopCount = req.loopCount;
    reqInfo.qwFadeOut = (req.fadeMs == kUnknown64) ? (UINT64)-1 : req.fadeMs * 10000;

    DWORD count;
    {
        // モジュールのメソッドは排他的に呼び出す(仕様)
        std::lock_guard<std::mutex> lock(module->callLock());
        count = module->module()->Open(&reqInfo, impl->file.get(), impl->folder.get(),
                                       impl->decoder.put());
    }
    if (count == 0 || !impl->decoder) {
        if (errOut) *errOut = L"プラグインがファイルを開けない: " + info.description;
        return nullptr;
    }
    impl->trackCount = count;
    return std::make_unique<DecodeSession>(std::move(impl));
}

} // namespace nokomo::detail
