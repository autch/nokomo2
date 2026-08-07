#pragma once
// プラグイン DLL のライフサイクル管理。
//
// KbMedia Player と同様に、
//  ・情報取得(probe)は GetModuleInfo だけ呼んで即アンロード
//  ・使用時にロードし、未使用になっても LRU で数本は常駐させる
// という遅延ロード方式をとる。
// kpi_CreateInstance はロードごとに 1 回だけ呼ぶ(仕様)。

#include <windows.h>

#include "kpi.h"
#include "kpi_decoder.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "host_objects.hpp"
#include "nokomo/nokomo.hpp"

namespace nokomo::detail {

// DLL をロードして KPI_DECODER_MODULEINFO のスナップショットを取り、即アンロードする。
// 失敗しても PluginInfo(valid=false, invalidReason 設定済み)を返す。
// path のファイルサイズ・更新日時は呼び出し側で設定すること。
PluginInfo probePlugin(const std::wstring& path, HostContext& ctx);

class LoadedModule {
public:
    ~LoadedModule();

    IKpiDecoderModule* module() const { return mod_; }
    // IKpiDecoderModule のメソッド呼び出しはこのロックで直列化する
    // (本体はモジュールのメソッドを排他的に呼ぶことを保証している)
    std::mutex& callLock() { return callLock_; }

    // dwMultipleInstance に基づく同時インスタンス数制限。
    // kbrunkpi 相当の別プロセス化は行わず、上限到達時はブロックして待つ。
    class SlotGuard {
    public:
        explicit SlotGuard(std::shared_ptr<LoadedModule> m) : m_(std::move(m)) {
            if (m_) m_->acquireSlot();
        }
        ~SlotGuard() { release(); }
        SlotGuard(SlotGuard&& o) noexcept : m_(std::move(o.m_)) {}
        SlotGuard& operator=(SlotGuard&& o) noexcept {
            if (this != &o) { release(); m_ = std::move(o.m_); }
            return *this;
        }
        SlotGuard(const SlotGuard&) = delete;
        SlotGuard& operator=(const SlotGuard&) = delete;
        void release() { if (m_) { m_->releaseSlot(); m_.reset(); } }
    private:
        std::shared_ptr<LoadedModule> m_;
    };

private:
    friend class ModuleManager;
    LoadedModule() = default;
    void acquireSlot();
    void releaseSlot();

    HMODULE h_ = nullptr;
    IKpiDecoderModule* mod_ = nullptr;
    std::mutex callLock_;
    uint32_t maxSlots_ = 0;
    std::mutex slotMutex_;
    std::condition_variable slotCv_;
    uint32_t activeSlots_ = 0;
};

class ModuleManager {
public:
    explicit ModuleManager(size_t residentCap) : residentCap_(residentCap) {}

    // ロード済みなら共有、なければロード。失敗時 nullptr(errOut に理由)。
    std::shared_ptr<LoadedModule> acquire(const PluginInfo& info, HostContext& ctx,
                                          std::wstring* errOut);

private:
    std::mutex m_;
    std::map<std::wstring, std::weak_ptr<LoadedModule>> live_;
    // 未使用でもロードしたままにする分(LRU)。shared_ptr を保持することで
    // FreeLibrary を遅延させる。
    std::deque<std::pair<std::wstring, std::shared_ptr<LoadedModule>>> resident_;
    size_t residentCap_;
};

} // namespace nokomo::detail
