#include "doctest/doctest.h"
#include "core/utf8_file.h"
#include <fstream>
using namespace suji;
TEST_CASE("utf8 no bom") {
  std::string p = "test_utf8_tmp.txt";
  REQUIRE(write_utf8_no_bom(p, u8"中文ABC\n第二行\n"));
  std::ifstream in(p, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)), {});
  // 不以 BOM(EF BB BF)开头
  CHECK_FALSE((data.size() >= 3 && (unsigned char)data[0]==0xEF && (unsigned char)data[1]==0xBB && (unsigned char)data[2]==0xBF));
  CHECK(data.rfind(u8"中文ABC", 0) == 0);
}
