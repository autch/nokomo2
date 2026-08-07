#include "module.hpp"

#include "kmp_adapter.hpp"
#include "strconv.hpp"

namespace nokomo::detail {

namespace {

struct RawModule {
    HMODULE h = nullptr;
    IKpiDecoderModule* mod = nullptr;
    bool legacy = false; // 旧仕様(kmp_pi.h)をアダプタ経由で使用
};

// LoadLibrary → kpi_CreateInstance(IID_IKpiDecoderModule)。
// 失敗時は reason を設定して {nullptr, nullptr} を返す。
// ctx.loadMutex を保持した状態で呼ぶこと(currentPluginKey の整合のため)。
RawModule loadRaw(const std::wstring& path, HostContext& ctx, std::wstring* reason)
{
    RawModule r;
    // 依存 DLL の検索先: プラグイン自身のフォルダ + アプリのフォルダ + System32
    // + AddDllDirectory で追加されたフォルダ(HostOptions::dllSearchDirs)
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                               LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!h) {
        // 古い検索順(カレントディレクトリ含む)でのフォールバック
        h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!h) {
        DWORD e = GetLastError();
        *reason = (e == ERROR_BAD_EXE_FORMAT)
                      ? L"アーキテクチャ不一致(x86/x64)"
                      : L"LoadLibrary 失敗 (error " + std::to_wstring(e) + L")";
        return r;
    }
    auto fn = (pfn_kpiCreateInstance)(void*)GetProcAddress(h, "kpi_CreateInstance");
    if (!fn) {
        // 旧仕様(kmp_pi.h)プラグイン: アダプタ経由で新仕様として扱う
        auto legacyFn = (pfnGetKMPModule)(void*)GetProcAddress(h, SZ_KMP_GETMODULE);
        if (legacyFn) {
            KMPMODULE* km = legacyFn(); // ロード中 1 回だけ呼ぶ(仕様)
            if (!km || km->dwVersion != KMPMODULE_VERSION) {
                // 仕様: バージョン不一致は Init/Deinit を呼ばずに直ちにアンロード
                *reason = L"未対応の旧仕様モジュールバージョン";
                FreeLibrary(h);
                return r;
            }
            auto tagFn = (pfnGetTagInfo)(void*)GetProcAddress(h, SZ_KMP_GETTAGINFO);
            r.h = h;
            r.mod = createKmpAdapterModule(km, tagFn);
            r.legacy = true;
            return r;
        }
        *reason = L"kpi_CreateInstance / kmp_GetTestModule がエクスポートされていない";
        FreeLibrary(h);
        return r;
    }
    ctx.currentPluginKey = stemOf(path);
    void* pv = nullptr;
    HRESULT hr = fn(IID_IKpiDecoderModule, &pv, ctx.root.get());
    ctx.currentPluginKey.clear();
    if (FAILED(hr) || !pv) {
        *reason = L"デコーダプラグインではない(IID_IKpiDecoderModule 非対応)";
        FreeLibrary(h);
        return r;
    }
    r.h = h;
    r.mod = (IKpiDecoderModule*)pv;
    return r;
}

uint32_t slotLimitFrom(DWORD multipleInstance)
{
    if (multipleInstance == KPI_MULTINST_INFINITE) return UINT32_MAX;
    if (multipleInstance == KPI_MULTINST_UNIQUE) return 1;
    // KPI_MULTINST_ZERO(=1) は「常に別プロセス」だが、このホスト自体が
    // KbMedia Player とは別プロセスなので同時 1 個として扱う。
    // KPI_MULTINST_ZERO + N は同時 N 個(KPI_MULTINST_ONE(=2) は 1 個)。
    if (multipleInstance <= KPI_MULTINST_ZERO) return 1;
    return multipleInstance - KPI_MULTINST_ZERO;
}

} // namespace

PluginInfo probePlugin(const std::wstring& path, HostContext& ctx)
{
    PluginInfo info;
    info.path = path;

    std::lock_guard<std::mutex> loadLock(ctx.loadMutex);
    std::wstring reason;
    RawModule r = loadRaw(path, ctx, &reason);
    if (!r.mod) {
        info.valid = false;
        info.invalidReason = std::move(reason);
        return info;
    }

    info.isLegacy = r.legacy;
    const KPI_DECODER_MODULEINFO* mi = nullptr;
    r.mod->GetModuleInfo(&mi);
    if (!mi) {
        info.valid = false;
        info.invalidReason = L"GetModuleInfo が情報を返さない";
    } else if (mi->dwModuleVersion != KPI_DECODER_MODULE_VERSION) {
        info.valid = false;
        info.invalidReason = L"未対応のモジュールバージョン(" +
                             std::to_wstring(mi->dwModuleVersion) + L")";
    } else {
        // *mi の寿命は次のメソッド呼び出しまでなのでディープコピーする
        info.valid = true;
        info.moduleVersion = mi->dwModuleVersion;
        info.pluginVersion = mi->dwPluginVersion;
        info.multipleInstance = mi->dwMultipleInstance;
        info.supportTagInfo = mi->dwSupportTagInfo;
        info.supportConfig = mi->dwSupportConfig;
        info.guid = mi->guid;
        info.description = mi->cszDescription ? mi->cszDescription : L"";
        info.copyright = mi->cszCopyright ? mi->cszCopyright : L"";
        info.supportExts = mi->cszSupportExts ? toLower(mi->cszSupportExts) : L"";
        info.multiSongExts = mi->cszMultiSongExts ? toLower(mi->cszMultiSongExts) : L"";
        if (info.supportExts.empty()) {
            info.valid = false;
            info.invalidReason = L"対応拡張子がない";
        }
    }

    // 情報だけ取得したら即アンロード(KbMedia Player と同じ動作。仕様上想定済み)
    r.mod->Release();
    FreeLibrary(r.h);
    return info;
}

LoadedModule::~LoadedModule()
{
    if (mod_) mod_->Release();
    if (h_) FreeLibrary(h_);
}

void LoadedModule::acquireSlot()
{
    if (maxSlots_ == UINT32_MAX) return;
    std::unique_lock<std::mutex> lock(slotMutex_);
    slotCv_.wait(lock, [this] { return activeSlots_ < maxSlots_; });
    ++activeSlots_;
}

void LoadedModule::releaseSlot()
{
    if (maxSlots_ == UINT32_MAX) return;
    {
        std::lock_guard<std::mutex> lock(slotMutex_);
        if (activeSlots_ > 0) --activeSlots_;
    }
    slotCv_.notify_one();
}

std::shared_ptr<LoadedModule> ModuleManager::acquire(const PluginInfo& info,
                                                     HostContext& ctx,
                                                     std::wstring* errOut)
{
    std::lock_guard<std::mutex> lock(m_);

    auto touchResident = [this](const std::wstring& path,
                                const std::shared_ptr<LoadedModule>& sp) {
        for (auto rit = resident_.begin(); rit != resident_.end(); ++rit) {
            if (rit->first == path) { resident_.erase(rit); break; }
        }
        resident_.emplace_front(path, sp);
        while (resident_.size() > residentCap_) resident_.pop_back();
    };

    auto it = live_.find(info.path);
    if (it != live_.end()) {
        if (auto sp = it->second.lock()) {
            touchResident(info.path, sp);
            return sp;
        }
        live_.erase(it);
    }

    std::wstring reason;
    RawModule r;
    {
        std::lock_guard<std::mutex> loadLock(ctx.loadMutex);
        r = loadRaw(info.path, ctx, &reason);
    }
    if (!r.mod) {
        if (errOut) *errOut = reason;
        return nullptr;
    }

    std::shared_ptr<LoadedModule> sp(new LoadedModule());
    sp->h_ = r.h;
    sp->mod_ = r.mod;
    sp->maxSlots_ = slotLimitFrom(info.multipleInstance);
    live_[info.path] = sp;
    touchResident(info.path, sp);
    return sp;
}

} // namespace nokomo::detail
