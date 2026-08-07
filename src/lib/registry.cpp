#include "nokomo/nokomo.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "host_objects.hpp"
#include "module.hpp"
#include "session.hpp"
#include "strconv.hpp"

namespace fs = std::filesystem;

namespace nokomo {

using namespace detail;

///////////////////////////////////////////////////////////////////////////////
// PluginInfo / TrackTags
///////////////////////////////////////////////////////////////////////////////

static std::vector<std::wstring> splitExts(const std::wstring& exts)
{
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= exts.size()) {
        size_t sep = exts.find(L'/', pos);
        if (sep == std::wstring::npos) sep = exts.size();
        if (sep > pos) out.push_back(exts.substr(pos, sep - pos));
        pos = sep + 1;
    }
    return out;
}

std::vector<std::wstring> PluginInfo::extensionList() const
{
    return splitExts(supportExts);
}

bool PluginInfo::supportsExtension(const std::wstring& extLower) const
{
    if (extLower.empty()) return false;
    for (const auto& e : splitExts(supportExts)) {
        if (e == extLower) return true;
    }
    return false;
}

bool PluginInfo::isMultiSongExtension(const std::wstring& extLower) const
{
    // 仕様: cszSupportExts にも含まれていないと認識されない
    if (!supportsExtension(extLower)) return false;
    for (const auto& e : splitExts(multiSongExts)) {
        if (e == extLower) return true;
    }
    return false;
}

bool PluginInfo::canTagOnlyOpen() const
{
    return (supportTagInfo & KPI_DECODER_MODULEINFO::TAG_TITLE) != 0;
}

const std::wstring* TrackTags::find(const std::wstring& name) const
{
    std::wstring key = toLower(name);
    for (const auto& kv : values) {
        if (toLower(kv.first) == key) return &kv.second;
    }
    return nullptr;
}

///////////////////////////////////////////////////////////////////////////////
// PluginRegistry
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// AddDllDirectory / RemoveDllDirectory ラッパ
// Win8 未満(未更新の Win7)に存在しないため動的解決する
// (静的インポートにすると古い環境で exe のロード自体が失敗する)
///////////////////////////////////////////////////////////////////////////////

DllDirCookie addDllSearchDirectory(const std::wstring& dir)
{
    typedef void*(WINAPI * pfnAddDllDirectory)(PCWSTR);
    auto fn = (pfnAddDllDirectory)(void*)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "AddDllDirectory");
    if (!fn) return nullptr;
    return fn(dir.c_str()); // LOAD_LIBRARY_SEARCH_USER_DIRS で参照される
}

bool removeDllSearchDirectory(DllDirCookie cookie)
{
    if (!cookie) return false;
    typedef BOOL(WINAPI * pfnRemoveDllDirectory)(void*);
    auto fn = (pfnRemoveDllDirectory)(void*)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "RemoveDllDirectory");
    if (!fn) return false;
    return fn(cookie) != FALSE;
}

struct PluginRegistry::Impl {
    HostOptions opt;
    HostContext ctx;
    ModuleManager modules;
    mutable std::mutex m;
    std::map<std::wstring, PluginInfo> byPath;
    std::vector<DllDirCookie> dllDirCookies; // Registry 自身が登録した分(解除はアプリの責任)

    explicit Impl(HostOptions o)
        : opt(std::move(o)),
          modules(opt.maxResidentModules > 0 ? (size_t)opt.maxResidentModules : 0)
    {
        ctx.options = opt;
        ctx.root.attach(new HostRoot(&ctx));
        // DLL 検索ディレクトリの登録。プロセスグローバルな状態のため所有権はアプリ側:
        // cookie 指定済み(アプリが登録済み)の項目には触らず、未登録(nullptr)の項目
        // だけ登録して Cookie を返す。Registry が RemoveDllDirectory を呼ぶことはない。
        dllDirCookies.reserve(opt.dllSearchDirs.size());
        for (const auto& d : opt.dllSearchDirs) {
            dllDirCookies.push_back(d.cookie ? nullptr : addDllSearchDirectory(d.dir));
        }
    }

    // ロックを保持したまま候補をコピーして返す(走査と並行しても安全なように)
    std::vector<PluginInfo> candidatesFor(const std::wstring& extLower) const
    {
        std::vector<PluginInfo> out;
        std::lock_guard<std::mutex> lock(m);
        for (const auto& kv : byPath) {
            if (kv.second.valid && kv.second.supportsExtension(extLower)) {
                out.push_back(kv.second);
            }
        }
        std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) {
            if (a.pluginVersion != b.pluginVersion) return a.pluginVersion > b.pluginVersion;
            return a.mtime > b.mtime;
        });
        return out;
    }
};

PluginRegistry::PluginRegistry(HostOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

PluginRegistry::~PluginRegistry() = default;

const HostOptions& PluginRegistry::options() const { return impl_->opt; }

const std::vector<DllDirCookie>& PluginRegistry::dllDirCookies() const
{
    return impl_->dllDirCookies;
}

void PluginRegistry::scanDirectory(const std::wstring& dir, int maxDepth)
{
    std::error_code ec;
    // キャッシュのキーが実行時のカレントディレクトリに依存しないよう絶対パス化
    fs::path root = fs::absolute(fs::path(dir), ec);
    if (ec) root = fs::path(dir);
    fs::recursive_directory_iterator it(root,
                                        fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::directory_entry& e = *it;
        std::error_code ec2;
        if (e.is_directory(ec2)) {
            if (it.depth() >= maxDepth) it.disable_recursion_pending();
            continue;
        }
        std::wstring path = e.path().wstring();
        if (extensionOf(path) != L".kpi") continue;

        uint64_t size = e.file_size(ec2);
        if (ec2) continue;
        int64_t mtime = (int64_t)e.last_write_time(ec2).time_since_epoch().count();
        if (ec2) continue;

        {
            std::lock_guard<std::mutex> lock(impl_->m);
            auto found = impl_->byPath.find(path);
            if (found != impl_->byPath.end() &&
                found->second.fileSize == size && found->second.mtime == mtime) {
                continue; // キャッシュ有効: DLL をロードしない
            }
        }
        // DLL をロードして情報取得(GetModuleInfo のみ)→即アンロード
        PluginInfo info = probePlugin(path, impl_->ctx);
        info.fileSize = size;
        info.mtime = mtime;
        {
            std::lock_guard<std::mutex> lock(impl_->m);
            impl_->byPath[path] = std::move(info);
        }
    }
}

std::vector<const PluginInfo*> PluginRegistry::plugins() const
{
    std::vector<const PluginInfo*> out;
    std::lock_guard<std::mutex> lock(impl_->m);
    out.reserve(impl_->byPath.size());
    for (const auto& kv : impl_->byPath) out.push_back(&kv.second);
    return out;
}

std::vector<const PluginInfo*> PluginRegistry::findByExtension(const std::wstring& pathOrExt) const
{
    std::wstring ext = (!pathOrExt.empty() && pathOrExt[0] == L'.')
                           ? toLower(pathOrExt)
                           : extensionOf(pathOrExt);
    std::lock_guard<std::mutex> lock(impl_->m);
    std::vector<const PluginInfo*> out;
    for (const auto& kv : impl_->byPath) {
        if (kv.second.valid && kv.second.supportsExtension(ext)) out.push_back(&kv.second);
    }
    std::sort(out.begin(), out.end(), [](const PluginInfo* a, const PluginInfo* b) {
        if (a->pluginVersion != b->pluginVersion) return a->pluginVersion > b->pluginVersion;
        return a->mtime > b->mtime;
    });
    return out;
}

///////////////////////////////////////////////////////////////////////////////
// タグ列挙(タグ専用オープン)
///////////////////////////////////////////////////////////////////////////////

std::optional<FileTags> PluginRegistry::scanTags(const std::wstring& file, uint32_t tagFields)
{
    std::wstring ext = extensionOf(file);
    for (const PluginInfo& cand : impl_->candidatesFor(ext)) {
        // タグ専用オープン(Open(cpRequest=NULL))は TAG_TITLE 対応プラグインにのみ許される
        if (!cand.canTagOnlyOpen()) continue;

        std::wstring err;
        auto mod = impl_->modules.acquire(cand, impl_->ctx, &err);
        if (!mod) continue;
        LoadedModule::SlotGuard slot(mod);

        ComPtr<IKpiFile> f;
        f.attach(LocalKpiFile::open(file));
        if (!f) return std::nullopt;
        IKpiFolder* rawFolder = DirectoryFolder::open(parentDirOf(file));
        if (!rawFolder) rawFolder = new KbKpiNullFolder;
        ComPtr<IKpiFolder> folder;
        folder.attach(rawFolder);

        ComPtr<IKpiDecoder> dec;
        DWORD count;
        {
            std::lock_guard<std::mutex> lock(mod->callLock());
            // cpRequest=NULL: タグ取得専用オープン(プラグインは処理を最小限に抑える)
            count = mod->module()->Open(nullptr, f.get(), folder.get(), dec.put());
        }
        if (count == 0 || !dec) continue; // 次の候補プラグインへ

        FileTags out;
        out.pluginPath = cand.path;
        out.trackCount = count;

        if (count > 1) {
            // 複数曲: dwNumber=0 でファイル全体(アルバム相当)の情報を取得
            ComPtr<TagSink> sink;
            sink.attach(new TagSink);
            if (dec->Select(0, nullptr, sink.get(), tagFields)) {
                sink->harvest(&out.album);
            }
        }
        out.tracks.resize(count);
        for (DWORD i = 1; i <= count; ++i) {
            ComPtr<TagSink> sink;
            sink.attach(new TagSink);
            // ppMediaInfo=NULL: タグ情報のみ要求(仕様)
            if (dec->Select(i, nullptr, sink.get(), tagFields)) {
                sink->harvest(&out.tracks[i - 1]);
            }
        }
        return out;
    }
    return std::nullopt;
}

///////////////////////////////////////////////////////////////////////////////
// デコード用オープン
///////////////////////////////////////////////////////////////////////////////

std::unique_ptr<DecodeSession> PluginRegistry::openDecode(const std::wstring& file,
                                                          const DecodeRequest& req,
                                                          std::wstring* errOut)
{
    std::wstring ext = extensionOf(file);
    std::wstring lastErr = L"対応するプラグインがない: " + ext;
    for (const PluginInfo& cand : impl_->candidatesFor(ext)) {
        std::wstring err;
        auto mod = impl_->modules.acquire(cand, impl_->ctx, &err);
        if (!mod) {
            lastErr = cand.path + L": " + err;
            continue;
        }
        auto session = createDecodeSession(std::move(mod), cand, file, req, impl_->opt, &err);
        if (session) return session;
        lastErr = err;
    }
    if (errOut) *errOut = lastErr;
    return nullptr;
}

///////////////////////////////////////////////////////////////////////////////
// キャッシュの読み書き
///////////////////////////////////////////////////////////////////////////////

static const char kCacheHeader[] = "#nokomo2-plugin-cache 1";

bool PluginRegistry::saveCache() const
{
    if (impl_->opt.cacheFilePath.empty()) return false;
    std::string out = kCacheHeader;
    out += "\n";
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        for (const auto& kv : impl_->byPath) {
            const PluginInfo& p = kv.second;
            out += "[plugin]\n";
            out += "path=" + escapeText(p.path) + "\n";
            out += "size=" + std::to_string(p.fileSize) + "\n";
            out += "mtime=" + std::to_string(p.mtime) + "\n";
            out += "valid=" + std::string(p.valid ? "1" : "0") + "\n";
            if (!p.valid) out += "reason=" + escapeText(p.invalidReason) + "\n";
            out += "modver=" + std::to_string(p.moduleVersion) + "\n";
            out += "legacy=" + std::string(p.isLegacy ? "1" : "0") + "\n";
            out += "plugver=" + std::to_string(p.pluginVersion) + "\n";
            out += "multi=" + std::to_string(p.multipleInstance) + "\n";
            out += "taginfo=" + std::to_string(p.supportTagInfo) + "\n";
            out += "config=" + std::to_string(p.supportConfig) + "\n";
            out += "guid=" + escapeText(guidToString(p.guid)) + "\n";
            out += "desc=" + escapeText(p.description) + "\n";
            out += "copyright=" + escapeText(p.copyright) + "\n";
            out += "exts=" + escapeText(p.supportExts) + "\n";
            out += "mexts=" + escapeText(p.multiSongExts) + "\n";
        }
    }
    fs::path path(impl_->opt.cacheFilePath);
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(out.data(), (std::streamsize)out.size());
    return f.good();
}

bool PluginRegistry::loadCache()
{
    if (impl_->opt.cacheFilePath.empty()) return false;
    std::ifstream f(fs::path(impl_->opt.cacheFilePath), std::ios::binary);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    std::lock_guard<std::mutex> lock(impl_->m);
    PluginInfo cur;
    bool inPlugin = false;
    bool headerOk = false;
    auto commit = [&] {
        if (inPlugin && !cur.path.empty()) impl_->byPath[cur.path] = cur;
        cur = PluginInfo{};
    };
    size_t pos = 0;
    bool firstLine = true;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (firstLine) {
            firstLine = false;
            if (line != kCacheHeader) return false; // バージョン不一致は破棄
            headerOk = true;
            continue;
        }
        if (line.empty()) continue;
        if (line == "[plugin]") {
            commit();
            inPlugin = true;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || !inPlugin) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "path") cur.path = unescapeText(val);
        else if (key == "size") cur.fileSize = strtoull(val.c_str(), nullptr, 10);
        else if (key == "mtime") cur.mtime = strtoll(val.c_str(), nullptr, 10);
        else if (key == "valid") cur.valid = (val == "1");
        else if (key == "reason") cur.invalidReason = unescapeText(val);
        else if (key == "modver") cur.moduleVersion = (uint32_t)strtoul(val.c_str(), nullptr, 10);
        else if (key == "legacy") cur.isLegacy = (val == "1");
        else if (key == "plugver") cur.pluginVersion = (uint32_t)strtoul(val.c_str(), nullptr, 10);
        else if (key == "multi") cur.multipleInstance = (uint32_t)strtoul(val.c_str(), nullptr, 10);
        else if (key == "taginfo") cur.supportTagInfo = (uint32_t)strtoul(val.c_str(), nullptr, 10);
        else if (key == "config") cur.supportConfig = (uint32_t)strtoul(val.c_str(), nullptr, 10);
        else if (key == "guid") guidFromString(unescapeText(val), &cur.guid);
        else if (key == "desc") cur.description = unescapeText(val);
        else if (key == "copyright") cur.copyright = unescapeText(val);
        else if (key == "exts") cur.supportExts = unescapeText(val);
        else if (key == "mexts") cur.multiSongExts = unescapeText(val);
    }
    commit();
    return headerOk;
}

} // namespace nokomo
