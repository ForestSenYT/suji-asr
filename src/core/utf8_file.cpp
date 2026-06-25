#include "core/utf8_file.h"
#include <fstream>
namespace suji {
bool write_utf8_no_bom(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc); // 二进制 = 不做 \n->\r\n,不写 BOM
  if (!out) return false;
  out.write(content.data(), (std::streamsize)content.size());
  return (bool)out;
}
}
