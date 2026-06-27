// Qwen3 hotwords file -> comma-separated CSV conversion.
//
// asr.cpp's static load_hotwords_csv() turns a one-term-per-line hotwords file
// into the inline "foo,bar,baz" string that Qwen3's `hotwords` config field wants
// (per sherpa-onnx c-api.h: "Optional comma-separated hotwords (UTF-8, ASCII ',')").
// That helper is static (file-local), so per the codebase convention (see
// test_cli_numeric_args.cpp) we MIRROR its logic here and test the BEHAVIOR against
// real temp files: '#' comments + blank lines skipped, CR/whitespace trimmed,
// terms joined with ',', empty input -> "".
#include "doctest/doctest.h"
#include <string>
#include <fstream>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

// Mirror of asr.cpp's load_hotwords_csv — kept in sync by review.
static std::string load_hotwords_csv(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::string line, out;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back()=='\r' || line.back()==' ' || line.back()=='\t')) line.pop_back();
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) continue;
    if (line[b] == '#') continue;
    std::string term = line.substr(b);
    if (!out.empty()) out += ',';
    out += term;
  }
  return out;
}

// Write `content` to a unique temp file; caller removes it.
static std::string write_temp(const std::string& content) {
  fs::path p = fs::temp_directory_path() /
               ("suji_hotwords_" + std::to_string((unsigned long long)::clock()) + ".txt");
  std::ofstream o(p, std::ios::binary);
  o << content;
  o.close();
  return p.string();
}

TEST_CASE("hotwords csv: joins terms with comma, skips comments and blanks") {
  std::string path = write_temp(
    "# a comment line\n"
    "\n"
    "奥斯特罗姆\n"
    "威廉姆森\n"
    "  \n"                    // whitespace-only -> skipped
    "# another comment\n"
    "GOVERNANCE\n");
  CHECK(load_hotwords_csv(path) == "奥斯特罗姆,威廉姆森,GOVERNANCE");
  fs::remove(path);
}

TEST_CASE("hotwords csv: strips CRLF and trailing/leading whitespace per term") {
  std::string path = write_temp("  foo  \r\nbar\r\n\tbaz\t\r\n");
  CHECK(load_hotwords_csv(path) == "foo,bar,baz");
  fs::remove(path);
}

TEST_CASE("hotwords csv: empty / comments-only file yields empty string") {
  std::string p1 = write_temp("");
  CHECK(load_hotwords_csv(p1).empty());
  fs::remove(p1);

  std::string p2 = write_temp("# only a comment\n\n   \n");
  CHECK(load_hotwords_csv(p2).empty());
  fs::remove(p2);
}

TEST_CASE("hotwords csv: missing file yields empty string (no throw)") {
  CHECK(load_hotwords_csv("/no/such/hotwords/file.txt").empty());
}

TEST_CASE("hotwords csv: single term has no trailing comma") {
  std::string path = write_temp("奥斯特罗姆\n");
  CHECK(load_hotwords_csv(path) == "奥斯特罗姆");
  fs::remove(path);
}
