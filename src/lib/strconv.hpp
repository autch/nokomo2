#pragma once
#include <windows.h>
#include <string>

// 文字コード変換とテキストユーティリティ。
// KPI のタグ通知は ANSI/UTF-16/UTF-8 の 3 系統があり、
// サイズ引数は「バッファサイズ(バイト単位)」または -1(NUL終端)で渡される。

namespace nokomo::detail {

// lenBytes < 0 なら NUL 終端文字列とみなす
std::wstring fromAnsi(const char* s, int lenBytes = -1);
std::wstring fromUtf8(const char* s, int lenBytes = -1);
std::wstring fromWide(const wchar_t* s, int lenBytes = -1);

std::string toUtf8(const std::wstring& s);
std::wstring utf8ToWide(const std::string& s);

std::wstring toLower(std::wstring s);

// 1行テキストへの埋め込み用エスケープ(\\, \n, \r, \t)
std::string escapeText(const std::wstring& s);
std::wstring unescapeText(const std::string& s);

std::string base64Encode(const unsigned char* data, size_t size);
std::string base64Encode(const std::string& data);
bool base64Decode(const std::wstring& text, std::string* out);

// GUID <-> "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
std::wstring guidToString(const GUID& g);
bool guidFromString(const std::wstring& s, GUID* out);

// KPI の GetXXXName 系共通仕様:
//   必要バッファサイズ(バイト、終端NUL含む)を返す。
//   バッファ不足時は空文字列を格納。格納すべき文字列がない場合は空文字列+0。
DWORD copyStringOut(wchar_t* pszName, DWORD dwSizeBytes, const std::wstring& s);

// パスユーティリティ
std::wstring extensionOf(const std::wstring& path); // 小文字、'.'含む。なければ空
std::wstring fileNameOf(const std::wstring& path);
std::wstring parentDirOf(const std::wstring& path);
std::wstring stemOf(const std::wstring& path);      // ファイル名から拡張子を除いたもの

} // namespace nokomo::detail
