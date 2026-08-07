#include "strconv.hpp"

#include <cwctype>
#include <cstdio>

namespace nokomo::detail {

static std::wstring fromMultiByte(UINT codePage, const char* s, int lenBytes)
{
    if (!s) return {};
    if (lenBytes < 0) lenBytes = (int)strlen(s);
    if (lenBytes == 0) return {};
    int need = MultiByteToWideChar(codePage, 0, s, lenBytes, nullptr, 0);
    if (need <= 0) return {};
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(codePage, 0, s, lenBytes, out.data(), need);
    // 明示長で渡された場合に混入し得る終端 NUL 以降を捨てる
    size_t z = out.find(L'\0');
    if (z != std::wstring::npos) out.resize(z);
    return out;
}

std::wstring fromAnsi(const char* s, int lenBytes) { return fromMultiByte(CP_ACP, s, lenBytes); }
std::wstring fromUtf8(const char* s, int lenBytes) { return fromMultiByte(CP_UTF8, s, lenBytes); }

std::wstring fromWide(const wchar_t* s, int lenBytes)
{
    if (!s) return {};
    std::wstring out;
    if (lenBytes < 0) {
        out.assign(s);
    } else {
        out.assign(s, s + (size_t)lenBytes / sizeof(wchar_t));
        size_t z = out.find(L'\0');
        if (z != std::wstring::npos) out.resize(z);
    }
    return out;
}

std::string toUtf8(const std::wstring& s)
{
    if (s.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s) { return fromUtf8(s.c_str(), (int)s.size()); }

std::wstring toLower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::string escapeText(const std::wstring& s)
{
    std::string u8 = toUtf8(s);
    std::string out;
    out.reserve(u8.size());
    for (char c : u8) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::wstring unescapeText(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[++i]) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return utf8ToWide(out);
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const unsigned char* data, size_t size)
{
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < size) v |= data[i + 1] << 8;
        if (i + 2 < size) v |= data[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += (i + 1 < size) ? kB64[(v >> 6) & 63] : '=';
        out += (i + 2 < size) ? kB64[v & 63] : '=';
    }
    return out;
}

std::string base64Encode(const std::string& data)
{
    return base64Encode((const unsigned char*)data.data(), data.size());
}

static int b64Value(wchar_t c)
{
    if (c >= L'A' && c <= L'Z') return (int)(c - L'A');
    if (c >= L'a' && c <= L'z') return (int)(c - L'a') + 26;
    if (c >= L'0' && c <= L'9') return (int)(c - L'0') + 52;
    if (c == L'+') return 62;
    if (c == L'/') return 63;
    return -1;
}

bool base64Decode(const std::wstring& text, std::string* out)
{
    out->clear();
    unsigned acc = 0;
    int bits = 0;
    for (wchar_t c : text) {
        if (c == L'=' || c == L'\r' || c == L'\n') continue;
        int v = b64Value(c);
        if (v < 0) return false;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back((char)((acc >> bits) & 0xFF));
        }
    }
    return true;
}

std::wstring guidToString(const GUID& g)
{
    wchar_t buf[64];
    swprintf(buf, 64, L"{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
             g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

bool guidFromString(const std::wstring& s, GUID* out)
{
    unsigned long d1;
    unsigned d2, d3, b[8];
    if (swscanf(s.c_str(), L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                &d1, &d2, &d3,
                &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11) {
        return false;
    }
    out->Data1 = d1;
    out->Data2 = (unsigned short)d2;
    out->Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; ++i) out->Data4[i] = (unsigned char)b[i];
    return true;
}

DWORD copyStringOut(wchar_t* pszName, DWORD dwSizeBytes, const std::wstring& s)
{
    if (!pszName || dwSizeBytes < sizeof(wchar_t)) return 0;
    if (s.empty()) {
        pszName[0] = 0;
        return 0;
    }
    DWORD needed = (DWORD)((s.size() + 1) * sizeof(wchar_t));
    if (dwSizeBytes < needed) {
        pszName[0] = 0; // 仕様: 不足時は途切れた文字列ではなく空文字列
        return needed;
    }
    memcpy(pszName, s.c_str(), needed);
    return needed;
}

static size_t lastSep(const std::wstring& path)
{
    return path.find_last_of(L"\\/");
}

std::wstring extensionOf(const std::wstring& path)
{
    size_t sep = lastSep(path);
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    if (sep != std::wstring::npos && dot < sep) return {};
    return toLower(path.substr(dot));
}

std::wstring fileNameOf(const std::wstring& path)
{
    size_t sep = lastSep(path);
    return (sep == std::wstring::npos) ? path : path.substr(sep + 1);
}

std::wstring parentDirOf(const std::wstring& path)
{
    size_t sep = lastSep(path);
    if (sep == std::wstring::npos) return L".";
    if (sep == 2 && path[1] == L':') return path.substr(0, 3); // "C:\"
    return path.substr(0, sep);
}

std::wstring stemOf(const std::wstring& path)
{
    std::wstring name = fileNameOf(path);
    size_t dot = name.find_last_of(L'.');
    return (dot == std::wstring::npos || dot == 0) ? name : name.substr(0, dot);
}

} // namespace nokomo::detail
