#include "doctest/doctest.h"
#include "core/utf8_file.h"
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace suji;

// Regression: a UTF-8 path with Chinese characters must create a file whose
// on-disk name is the correct Unicode name, not an ANSI-mangled one
// (测试视频 -> 娴嬭瘯瑙嗛). A narrow std::ofstream path mangled it.
TEST_CASE("write_utf8_no_bom writes a correctly-named Unicode (Chinese) file") {
  std::string path = u8"测试文件_utf8name.txt";
  REQUIRE(write_utf8_no_bom(path, "x"));
#ifdef _WIN32
  int n = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), w.data(), n);
  CHECK(GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES);
  DeleteFileW(w.c_str());
#endif
}

TEST_CASE("utf8 no bom") {
  std::string p = "test_utf8_tmp.txt";
  REQUIRE(write_utf8_no_bom(p, u8"中文ABC\n第二行\n"));
  std::ifstream in(p, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)), {});
  // 不以 BOM(EF BB BF)开头
  CHECK_FALSE((data.size() >= 3 && (unsigned char)data[0]==0xEF && (unsigned char)data[1]==0xBB && (unsigned char)data[2]==0xBF));
  CHECK(data.rfind(u8"中文ABC", 0) == 0);
}
