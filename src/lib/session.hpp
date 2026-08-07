#pragma once
// DecodeSession の内部生成 API(registry.cpp から使用)

#include <memory>
#include <string>

#include "module.hpp"
#include "nokomo/nokomo.hpp"

namespace nokomo::detail {

// ファイルを開いて IKpiDecoderModule::Open まで行う。
// 失敗時は nullptr(errOut に理由)。
std::unique_ptr<DecodeSession> createDecodeSession(std::shared_ptr<LoadedModule> module,
                                                   const PluginInfo& info,
                                                   const std::wstring& filePath,
                                                   const DecodeRequest& req,
                                                   const HostOptions& opt,
                                                   std::wstring* errOut);

} // namespace nokomo::detail
