#include "core/utf8_file.h"
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
namespace {
// Convert a UTF-8 path to UTF-16 so the file is created with the correct Unicode
// name. A narrow std::ofstream path is interpreted in the system ANSI codepage on
// Windows, which mangles non-ASCII (e.g. Chinese) filenames (测试视频 -> 娴嬭瘯瑙嗛).
static std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
} // namespace
#endif
namespace suji {
bool write_utf8_no_bom(const std::string& path, const std::string& content) {
#ifdef _WIN32
  // Wide path -> correct Unicode filename on disk (binary = no \n->\r\n, no BOM).
  std::ofstream out(utf8_to_wide(path).c_str(), std::ios::binary | std::ios::trunc);
#else
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
#endif
  if (!out) return false;
  out.write(content.data(), (std::streamsize)content.size());
  return (bool)out;
}
}
