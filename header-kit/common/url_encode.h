// =============================================================================
// url_encode.h   (共用實作 — reflection 與 plugin 兩支 sample 都 include)
//
// 為什麼抽出來:header 格式約定(PROPOSAL §7)要求 sender / receiver 一致,
// 且兩支 sample 的輸出必須 byte-identical。把 encode 規則放在「唯一一份」實作,
// 避免兩支 sample 各寫一份而悄悄分歧。
//
// 規則(凍結,見 PROPOSAL §7 / CONTEXT C4):
//   - RFC 3986 unreserved (A-Z a-z 0-9 - _ . ~) 不編碼,其餘一律 %XX
//   - 空白編成 %20,不使用 '+'
//   - key 與 value 都用這支 encode(plugin 與 reflection 一致)
// =============================================================================

#pragma once
#include <string>

namespace headergen {

inline std::string UrlEncode(const std::string& in) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (unsigned char c : in) {
    const bool unreserved =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

}  // namespace headergen
