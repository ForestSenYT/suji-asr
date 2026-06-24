# Phase 1 — suji_core + suji_cli 单文件端到端 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把**一个**讲课音/视频文件,从输入到 SRT/VTT/JSON/Markdown 文档**正确**跑通(单流即可,暂不追求吞吐),作为后续批量/GPU/GUI 的地基。

**Architecture:** 一个 C++ 库 `suji_core`(封装 ffmpeg 解码 → Silero VAD 切段 → FireRedASR2-CTC 单流解码 → CT 标点 → 段落重建 → SRT/VTT/JSON/MD 写出),与一个 `suji_cli` 控制台程序驱动它。纯逻辑单元(时间格式化、段落重建、写出器)用 doctest 做 TDD;涉及模型/ffmpeg 的集成单元用 bundled `test_wavs` 做 fixture 测试。

**Tech Stack:** C++17 · CMake ≥ 3.20 · MSVC (VS2022) · sherpa-onnx v1.13.3 C API(预编译,`vendor/`)· ffmpeg.exe 子进程 · doctest(单头,vendored)。

## Global Constraints

- **平台**:Windows x64,MSVC(VS2022)。构建 `cmake -G "Visual Studio 17 2022" -A x64`。
- **C++ 标准**:C++17。
- **依赖复用**:链接 `vendor/sherpa-onnx-v1.13.3-cuda-12.x-cudnn-9.x-win-x64-cuda` 的 `lib/sherpa-onnx-c-api.lib` + `include/`;**不从源码编译 sherpa-onnx**。
- **C API 以真实头文件为准**(已读 `include/sherpa-onnx/c-api/c-api.h`,签名见下方"已核实 C API 参考")。配置结构体一律 `memset(&c, 0, sizeof(c))` 后再赋值。结果里的 `timestamps` 指针**可能为 NULL**,读前必判空。
- **输出编码**:全部 **UTF-8 无 BOM**(尤其 SRT;禁止 GBK)。文件换行用 `\n`。
- **模型/资产路径**(已就位,Phase 0):
  - ASR:`models/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/model.int8.onnx` + `tokens.txt`
  - VAD:`models/silero_vad.onnx`
  - 标点:`models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx`
  - ffmpeg:`vendor/ffmpeg-master-latest-win64-lgpl/bin/ffmpeg.exe`
  - 测试 wav:`models/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/*.wav`
- **运行期 DLL**:构建后把 sherpa `bin/*.dll`(`sherpa-onnx-c-api.dll`、`onnxruntime.dll` 等)拷到可执行同目录;GPU 测试时把 `vendor/cuda-redist/extract/nvidia/*/bin` 加入 PATH。
- **音频契约**:ASR/VAD 一律 16000 Hz、单声道、float32(范围 [-1,1])。VAD `SpeechSegment.start` 单位是**采样点**;全局时间 = `start/16000 + token_本地时间`。
- **DRY / YAGNI / TDD / 频繁提交**。Phase 1 **不**做:批量 `DecodeMultipleOfflineStreams`(Phase 2)、GPU 自适应(Phase 2)、多文件编排(Phase 3)、GUI(Phase 5)。Phase 1 默认 `provider=cpu`(GPU 单流也能跑,但吞吐不在本阶段目标)。

## 已核实 C API 参考(verbatim,来自 `c-api.h`)

```c
typedef struct SherpaOnnxFeatureConfig { int32_t sample_rate; int32_t feature_dim; } SherpaOnnxFeatureConfig;
typedef struct SherpaOnnxOfflineFireRedAsrCtcModelConfig { const char *model; } SherpaOnnxOfflineFireRedAsrCtcModelConfig;
// SherpaOnnxOfflineModelConfig 关键字段: const char *tokens; int32_t num_threads; int32_t debug;
//   const char *provider; const char *modeling_unit; ... SherpaOnnxOfflineFireRedAsrCtcModelConfig fire_red_asr_ctc;
// SherpaOnnxOfflineRecognizerConfig: SherpaOnnxFeatureConfig feat_config; SherpaOnnxOfflineModelConfig model_config;
//   ... const char *decoding_method; const char *rule_fsts; const char *rule_fars; ...
const SherpaOnnxOfflineRecognizer *SherpaOnnxCreateOfflineRecognizer(const SherpaOnnxOfflineRecognizerConfig *config);
void SherpaOnnxDestroyOfflineRecognizer(const SherpaOnnxOfflineRecognizer *recognizer);
const SherpaOnnxOfflineStream *SherpaOnnxCreateOfflineStream(const SherpaOnnxOfflineRecognizer *recognizer);
void SherpaOnnxDestroyOfflineStream(const SherpaOnnxOfflineStream *stream);
void SherpaOnnxAcceptWaveformOffline(const SherpaOnnxOfflineStream *stream, int32_t sample_rate, const float *samples, int32_t n);
void SherpaOnnxDecodeOfflineStream(const SherpaOnnxOfflineRecognizer *recognizer, const SherpaOnnxOfflineStream *stream);
void SherpaOnnxDecodeMultipleOfflineStreams(const SherpaOnnxOfflineRecognizer *recognizer, const SherpaOnnxOfflineStream **streams, int32_t n); // Phase 2
const SherpaOnnxOfflineRecognizerResult *SherpaOnnxGetOfflineStreamResult(const SherpaOnnxOfflineStream *stream);
void SherpaOnnxDestroyOfflineRecognizerResult(const SherpaOnnxOfflineRecognizerResult *r);
// Result: const char *text; float *timestamps; int32_t count; const char *tokens; const char *const *tokens_arr; const char *json; ...
//   timestamps 可能为 NULL;count 是 token 数;tokens_arr[i] 与 timestamps[i] 平行。

typedef struct SherpaOnnxSileroVadModelConfig { const char *model; float threshold; float min_silence_duration;
  float min_speech_duration; int32_t window_size; float max_speech_duration; } SherpaOnnxSileroVadModelConfig;
typedef struct SherpaOnnxVadModelConfig { SherpaOnnxSileroVadModelConfig silero_vad; int32_t sample_rate;
  int32_t num_threads; const char *provider; int32_t debug; SherpaOnnxTenVadModelConfig ten_vad; } SherpaOnnxVadModelConfig;
typedef struct SherpaOnnxSpeechSegment { int32_t start; float *samples; int32_t n; } SherpaOnnxSpeechSegment; // start = 采样点
const SherpaOnnxVoiceActivityDetector *SherpaOnnxCreateVoiceActivityDetector(const SherpaOnnxVadModelConfig *config, float buffer_size_in_seconds);
void SherpaOnnxDestroyVoiceActivityDetector(const SherpaOnnxVoiceActivityDetector *p);
void SherpaOnnxVoiceActivityDetectorAcceptWaveform(const SherpaOnnxVoiceActivityDetector *p, const float *samples, int32_t n);
int32_t SherpaOnnxVoiceActivityDetectorEmpty(const SherpaOnnxVoiceActivityDetector *p);
const SherpaOnnxSpeechSegment *SherpaOnnxVoiceActivityDetectorFront(const SherpaOnnxVoiceActivityDetector *p);
void SherpaOnnxVoiceActivityDetectorPop(const SherpaOnnxVoiceActivityDetector *p);
void SherpaOnnxVoiceActivityDetectorFlush(const SherpaOnnxVoiceActivityDetector *p); // 末尾冲刷出尾段
void SherpaOnnxDestroySpeechSegment(const SherpaOnnxSpeechSegment *p);

typedef struct SherpaOnnxOfflinePunctuationModelConfig { const char *ct_transformer; int32_t num_threads; int32_t debug; const char *provider; } SherpaOnnxOfflinePunctuationModelConfig;
typedef struct SherpaOnnxOfflinePunctuationConfig { SherpaOnnxOfflinePunctuationModelConfig model; } SherpaOnnxOfflinePunctuationConfig;
const SherpaOnnxOfflinePunctuation *SherpaOnnxCreateOfflinePunctuation(const SherpaOnnxOfflinePunctuationConfig *config);
void SherpaOnnxDestroyOfflinePunctuation(const SherpaOnnxOfflinePunctuation *punct);
const char *SherpaOfflinePunctuationAddPunct(const SherpaOnnxOfflinePunctuation *punct, const char *text); // 注意:无 "Onnx"
void SherpaOfflinePunctuationFreeText(const char *text);
```

---

## File Structure

```
CMakeLists.txt                  # 顶层:options、子目录、imported sherpa target
cmake/SujiPaths.cmake           # 定位 vendor SDK / ffmpeg / models(cache 变量,可覆盖)
third_party/doctest/doctest.h   # vendored 单头测试框架
src/core/
  types.h                       # AudioBuffer/Token/SpeechSeg/AsrResult/Segment/Transcript
  config.h                      # EngineConfig, Provider
  log.h log.cpp                 # 极简日志
  timestamp.h timestamp.cpp     # 秒 -> SRT/VTT 时间串;全局时间换算
  utf8_file.h utf8_file.cpp     # 写 UTF-8 无 BOM 文件
  segment_merge.h .cpp          # tokens+VAD段 -> 可读段落
  output/srt_writer.h .cpp
  output/vtt_writer.h .cpp
  output/json_writer.h .cpp
  output/md_writer.h .cpp
  media_decode.h .cpp           # ffmpeg 子进程 -> AudioBuffer
  vad.h .cpp                    # Silero VAD 包装 -> vector<SpeechSeg>
  asr.h .cpp                    # recognizer 包装 -> AsrResult(单流)
  punctuation.h .cpp            # CT 标点包装
  pipeline.h .cpp               # 串起来:文件 -> Transcript -> 写出
src/cli/main.cpp                # suji_cli:解析参数,调 pipeline
tests/
  test_main.cpp                 # #define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
  test_timestamp.cpp test_utf8_file.cpp test_segment_merge.cpp
  test_srt_writer.cpp test_vtt_writer.cpp test_json_writer.cpp test_md_writer.cpp
  integration/test_media_decode.cpp integration/test_vad.cpp
  integration/test_asr.cpp integration/test_punct.cpp integration/test_pipeline_e2e.cpp
```

**类型契约(贯穿全程,各 Task 的 Produces/Consumes 都引用此处)** — `src/core/types.h`:
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace suji {
struct AudioBuffer { std::vector<float> samples; int sample_rate = 16000; };
struct SpeechSeg   { int64_t start_sample = 0; std::vector<float> samples; }; // VAD 输出,start 为采样点
struct AsrResult   { std::string text; std::vector<std::string> tokens; std::vector<double> timestamps; }; // 单段本地时间(秒)
struct Token       { std::string text; double start = 0.0; };                 // 全局秒
struct Segment     { double start = 0.0; double end = 0.0; std::string text; std::vector<Token> tokens; };
struct Transcript  { std::vector<Segment> segments; std::string full_text; };
} // namespace suji
```

---

## Task 0: CMake 骨架 + doctest + 可构建空库

**Files:**
- Create: `CMakeLists.txt`, `cmake/SujiPaths.cmake`, `third_party/doctest/doctest.h`, `src/core/types.h`, `src/core/log.h`, `src/core/log.cpp`, `tests/test_main.cpp`, `tests/test_sanity.cpp`, `src/cli/main.cpp`

**Interfaces:**
- Produces: 可构建的 `suji_core`(静态库)、`suji_cli`(exe)、`suji_tests`(exe);CMake cache 变量 `SUJI_VENDOR_DIR`、`SUJI_MODELS_DIR`、`SUJI_FFMPEG`。

- [ ] **Step 1: 取 doctest 单头**（vendored,避免构建期联网）

Run:
```bash
curl.exe -L --fail -o third_party/doctest/doctest.h https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h
```
Expected: 文件存在,大小 ~200KB。（沙箱内用代理 curl;若失败改用 `Invoke-WebRequest`。）

- [ ] **Step 2: 写 `cmake/SujiPaths.cmake`**
```cmake
# 定位 Phase 0 已下载的 vendor SDK / ffmpeg / models。允许命令行 -D 覆盖。
set(SUJI_VENDOR_DIR "${CMAKE_SOURCE_DIR}/vendor/sherpa-onnx-v1.13.3-cuda-12.x-cudnn-9.x-win-x64-cuda" CACHE PATH "sherpa-onnx prebuilt SDK dir")
set(SUJI_MODELS_DIR "${CMAKE_SOURCE_DIR}/models" CACHE PATH "models dir")
set(SUJI_FFMPEG "${CMAKE_SOURCE_DIR}/vendor/ffmpeg-master-latest-win64-lgpl/bin/ffmpeg.exe" CACHE FILEPATH "ffmpeg.exe")

if(NOT EXISTS "${SUJI_VENDOR_DIR}/include/sherpa-onnx/c-api/c-api.h")
  message(FATAL_ERROR "sherpa-onnx SDK not found at ${SUJI_VENDOR_DIR} (run Phase 0 downloads)")
endif()

# Imported target: sherpa-onnx C API
add_library(sherpa_onnx_c SHARED IMPORTED GLOBAL)
set_target_properties(sherpa_onnx_c PROPERTIES
  IMPORTED_IMPLIB "${SUJI_VENDOR_DIR}/lib/sherpa-onnx-c-api.lib"
  IMPORTED_LOCATION "${SUJI_VENDOR_DIR}/bin/sherpa-onnx-c-api.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${SUJI_VENDOR_DIR}/include")

# 把 sherpa bin/*.dll 拷到目标 exe 同目录的函数
function(suji_copy_runtime_dlls target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${SUJI_VENDOR_DIR}/bin" "$<TARGET_FILE_DIR:${target}>"
    COMMENT "Copying sherpa-onnx runtime DLLs next to ${target}")
endfunction()
```

- [ ] **Step 3: 写顶层 `CMakeLists.txt`**
```cmake
cmake_minimum_required(VERSION 3.20)
project(suji LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8 /W4 /EHsc)   # /utf-8 关键:源码与执行字符集 UTF-8
endif()
include(cmake/SujiPaths.cmake)

# suji_core 库
file(GLOB_RECURSE SUJI_CORE_SRC CONFIGURE_DEPENDS src/core/*.cpp)
add_library(suji_core STATIC ${SUJI_CORE_SRC})
target_include_directories(suji_core PUBLIC src third_party)
target_link_libraries(suji_core PUBLIC sherpa_onnx_c)

# 把模型/ffmpeg 路径作为编译期默认(测试/CLI 用)
target_compile_definitions(suji_core PUBLIC
  SUJI_DEFAULT_MODELS_DIR="${SUJI_MODELS_DIR}"
  SUJI_DEFAULT_FFMPEG="${SUJI_FFMPEG}")

# CLI
add_executable(suji_cli src/cli/main.cpp)
target_link_libraries(suji_cli PRIVATE suji_core)
suji_copy_runtime_dlls(suji_cli)

# 测试
enable_testing()
file(GLOB_RECURSE SUJI_TEST_SRC CONFIGURE_DEPENDS tests/*.cpp)
add_executable(suji_tests ${SUJI_TEST_SRC})
target_link_libraries(suji_tests PRIVATE suji_core)
target_include_directories(suji_tests PRIVATE third_party src)
suji_copy_runtime_dlls(suji_tests)
add_test(NAME suji_tests COMMAND suji_tests)
```

- [ ] **Step 4: 写 `src/core/types.h`**（用上面"类型契约"全文）。

- [ ] **Step 5: 写 `src/core/log.h` / `log.cpp`**
```cpp
// log.h
#pragma once
#include <string>
namespace suji { void log_info(const std::string& m); void log_err(const std::string& m); }
```
```cpp
// log.cpp
#include "core/log.h"
#include <cstdio>
namespace suji {
void log_info(const std::string& m){ std::fprintf(stderr, "[INFO] %s\n", m.c_str()); }
void log_err (const std::string& m){ std::fprintf(stderr, "[ERR ] %s\n", m.c_str()); }
}
```

- [ ] **Step 6: 写 `tests/test_main.cpp` 与一个 sanity 测试**
```cpp
// tests/test_main.cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
```
```cpp
// tests/test_sanity.cpp
#include "doctest/doctest.h"
TEST_CASE("sanity") { CHECK(1 + 1 == 2); }
```

- [ ] **Step 7: 写最小 `src/cli/main.cpp`(占位)**
```cpp
#include <cstdio>
int main(int argc, char** argv) { std::puts("suji_cli placeholder"); return 0; }
```

- [ ] **Step 8: 配置 + 构建 + 跑测试**

Run:
```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build/Release/suji_tests.exe
```
Expected: 配置无 FATAL;编译通过;`suji_tests` 输出 `[doctest] ... assertions: 1 | 1 passed`。

- [ ] **Step 9: Commit**
```bash
git add CMakeLists.txt cmake/ third_party/ src/ tests/
git commit -m "build: CMake skeleton + doctest + suji_core/cli/tests build"
```

---

## Task 1: 时间格式化（纯逻辑,TDD）

**Files:** Create `src/core/timestamp.h`, `src/core/timestamp.cpp`, `tests/test_timestamp.cpp`

**Interfaces:**
- Produces: `std::string suji::format_srt_time(double sec);` // "HH:MM:SS,mmm";`std::string suji::format_vtt_time(double sec);` // "HH:MM:SS.mmm"

- [ ] **Step 1: 失败测试** `tests/test_timestamp.cpp`
```cpp
#include "doctest/doctest.h"
#include "core/timestamp.h"
using namespace suji;
TEST_CASE("srt time format") {
  CHECK(format_srt_time(0.0)        == "00:00:00,000");
  CHECK(format_srt_time(1.5)        == "00:00:01,500");
  CHECK(format_srt_time(61.25)      == "00:01:01,250");
  CHECK(format_srt_time(3661.007)   == "01:01:01,007");
}
TEST_CASE("vtt time format") {
  CHECK(format_vtt_time(61.25)      == "00:01:01.250");
}
```

- [ ] **Step 2: 跑测试确认失败**
Run: `cmake --build build --config Release` Expected: 链接错误 `format_srt_time` 未定义。

- [ ] **Step 3: 实现** `src/core/timestamp.h`
```cpp
#pragma once
#include <string>
namespace suji {
std::string format_srt_time(double seconds); // HH:MM:SS,mmm
std::string format_vtt_time(double seconds); // HH:MM:SS.mmm
}
```
`src/core/timestamp.cpp`
```cpp
#include "core/timestamp.h"
#include <cstdio>
#include <cmath>
namespace suji {
static std::string fmt(double seconds, char ms_sep) {
  if (seconds < 0) seconds = 0;
  long long total_ms = (long long)std::llround(seconds * 1000.0);
  int ms = (int)(total_ms % 1000); long long s = total_ms / 1000;
  int sec = (int)(s % 60); long long m = s / 60;
  int minute = (int)(m % 60); int hour = (int)(m / 60);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d%c%03d", hour, minute, sec, ms_sep, ms);
  return std::string(buf);
}
std::string format_srt_time(double s){ return fmt(s, ','); }
std::string format_vtt_time(double s){ return fmt(s, '.'); }
}
```

- [ ] **Step 4: 跑测试确认通过** Run: `cmake --build build --config Release && build/Release/suji_tests.exe` Expected: PASS。

- [ ] **Step 5: Commit** `git add src/core/timestamp.* tests/test_timestamp.cpp && git commit -m "feat: SRT/VTT time formatting"`

---

## Task 2: UTF-8 无 BOM 文件写出（TDD）

**Files:** Create `src/core/utf8_file.h`, `.cpp`, `tests/test_utf8_file.cpp`

**Interfaces:**
- Produces: `bool suji::write_utf8_no_bom(const std::string& path, const std::string& content);`（二进制写,不转换换行,不写 BOM,失败返回 false）

- [ ] **Step 1: 失败测试**
```cpp
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
```

- [ ] **Step 2: 确认失败** Run build → 链接错误。

- [ ] **Step 3: 实现**
`utf8_file.h`
```cpp
#pragma once
#include <string>
namespace suji { bool write_utf8_no_bom(const std::string& path, const std::string& content); }
```
`utf8_file.cpp`
```cpp
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
```

- [ ] **Step 4: 确认通过** Run tests. PASS.

- [ ] **Step 5: Commit** `git commit -am "feat: UTF-8 no-BOM file writer"`

---

## Task 3: 段落重建（纯逻辑,TDD）

把"全局 token 列表"按停顿(gap)与最大时长合并为可读段落。VAD 段已天然分割,但段内仍可能过长;并且要把碎段按时间间隔并成段落。本 Task 输入是**已带全局时间的 token 列表**(由 Task 6 pipeline 拼好),输出 `vector<Segment>`。

**Files:** Create `src/core/segment_merge.h`, `.cpp`, `tests/test_segment_merge.cpp`

**Interfaces:**
- Consumes: `std::vector<Token>`（全局秒,已排序）
- Produces: `std::vector<Segment> suji::merge_tokens(const std::vector<Token>& toks, double gap_sec, double max_dur_sec);`
  - 规则:相邻 token 间隔 > `gap_sec`,或当前段时长 ≥ `max_dur_sec` 时,**断段**。`Segment.text` = 段内 token 文本顺序拼接(原样,英文 token 自带前导空格)。`start`=首 token.start,`end`=末 token.start。

- [ ] **Step 1: 失败测试**
```cpp
#include "doctest/doctest.h"
#include "core/segment_merge.h"
using namespace suji;
static Token T(const char* s, double t){ Token x; x.text=s; x.start=t; return x; }
TEST_CASE("merge by gap") {
  std::vector<Token> toks = { T("你",0.0),T("好",0.3),T("世",2.0),T("界",2.2) }; // 0.3->2.0 gap=1.7
  auto segs = merge_tokens(toks, 1.0, 30.0);
  REQUIRE(segs.size() == 2);
  CHECK(segs[0].text == u8"你好");
  CHECK(segs[0].start == doctest::Approx(0.0));
  CHECK(segs[0].end   == doctest::Approx(0.3));
  CHECK(segs[1].text == u8"世界");
}
TEST_CASE("merge by max duration") {
  std::vector<Token> toks; for(int i=0;i<10;i++) toks.push_back(T("字", i*0.5)); // 连续 0.5s 间隔
  auto segs = merge_tokens(toks, 1.0, 2.0); // 2s 上限 -> 多段
  CHECK(segs.size() >= 2);
}
TEST_CASE("empty input") { CHECK(merge_tokens({},1.0,30.0).empty()); }
```

- [ ] **Step 2: 确认失败** build → 链接错误。

- [ ] **Step 3: 实现**
`segment_merge.h`
```cpp
#pragma once
#include "core/types.h"
#include <vector>
namespace suji { std::vector<Segment> merge_tokens(const std::vector<Token>& toks, double gap_sec, double max_dur_sec); }
```
`segment_merge.cpp`
```cpp
#include "core/segment_merge.h"
namespace suji {
std::vector<Segment> merge_tokens(const std::vector<Token>& toks, double gap_sec, double max_dur_sec) {
  std::vector<Segment> segs;
  if (toks.empty()) return segs;
  Segment cur; cur.start = toks[0].start; cur.end = toks[0].start;
  cur.tokens.push_back(toks[0]); cur.text = toks[0].text;
  for (size_t i = 1; i < toks.size(); ++i) {
    double gap = toks[i].start - toks[i-1].start;
    double dur = toks[i].start - cur.start;
    if (gap > gap_sec || dur >= max_dur_sec) {
      segs.push_back(cur);
      cur = Segment{}; cur.start = toks[i].start;
    }
    cur.tokens.push_back(toks[i]); cur.text += toks[i].text; cur.end = toks[i].start;
  }
  segs.push_back(cur);
  return segs;
}
}
```

- [ ] **Step 4: 确认通过** Run tests. PASS.
- [ ] **Step 5: Commit** `git commit -am "feat: paragraph merge by gap/duration"`

---

## Task 4: SRT 写出器（TDD）

**Files:** Create `src/core/output/srt_writer.h`, `.cpp`, `tests/test_srt_writer.cpp`

**Interfaces:**
- Consumes: `Transcript`
- Produces: `std::string suji::to_srt(const Transcript& t);`（标准 SRT:序号 / `start --> end` / 文本 / 空行）

- [ ] **Step 1: 失败测试**
```cpp
#include "doctest/doctest.h"
#include "core/output/srt_writer.h"
using namespace suji;
TEST_CASE("srt basic") {
  Transcript t; Segment s; s.start=1.0; s.end=2.5; s.text=u8"你好世界"; t.segments={s};
  std::string srt = to_srt(t);
  CHECK(srt == "1\n00:00:01,000 --> 00:00:02,500\n\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\n\n");
}
```

- [ ] **Step 2: 确认失败**.
- [ ] **Step 3: 实现**
`srt_writer.h`
```cpp
#pragma once
#include "core/types.h"
#include <string>
namespace suji { std::string to_srt(const Transcript& t); }
```
`srt_writer.cpp`
```cpp
#include "core/output/srt_writer.h"
#include "core/timestamp.h"
namespace suji {
std::string to_srt(const Transcript& t) {
  std::string out;
  int idx = 1;
  for (const auto& s : t.segments) {
    double end = (s.end > s.start) ? s.end : s.start + 2.0; // 防 0 时长
    out += std::to_string(idx++) + "\n";
    out += format_srt_time(s.start) + " --> " + format_srt_time(end) + "\n";
    out += s.text + "\n\n";
  }
  return out;
}
}
```

- [ ] **Step 4: 确认通过**. - [ ] **Step 5: Commit** `git commit -am "feat: SRT writer"`

---

## Task 5: VTT 写出器（TDD）

**Files:** Create `src/core/output/vtt_writer.h`, `.cpp`, `tests/test_vtt_writer.cpp`

**Interfaces:** Produces `std::string suji::to_vtt(const Transcript& t);`（首行 `WEBVTT\n\n`,时间用 `.` 毫秒分隔）

- [ ] **Step 1: 失败测试**
```cpp
#include "doctest/doctest.h"
#include "core/output/vtt_writer.h"
using namespace suji;
TEST_CASE("vtt basic") {
  Transcript t; Segment s; s.start=1.0; s.end=2.5; s.text=u8"abc"; t.segments={s};
  std::string vtt = to_vtt(t);
  CHECK(vtt.rfind("WEBVTT\n\n",0)==0);
  CHECK(vtt.find("00:00:01.000 --> 00:00:02.500\nabc\n\n") != std::string::npos);
}
```
- [ ] **Step 2: 确认失败**.
- [ ] **Step 3: 实现**
```cpp
// vtt_writer.h
#pragma once
#include "core/types.h"
#include <string>
namespace suji { std::string to_vtt(const Transcript& t); }
```
```cpp
// vtt_writer.cpp
#include "core/output/vtt_writer.h"
#include "core/timestamp.h"
namespace suji {
std::string to_vtt(const Transcript& t) {
  std::string out = "WEBVTT\n\n";
  for (const auto& s : t.segments) {
    double end = (s.end > s.start) ? s.end : s.start + 2.0;
    out += format_vtt_time(s.start) + " --> " + format_vtt_time(end) + "\n";
    out += s.text + "\n\n";
  }
  return out;
}
}
```
- [ ] **Step 4: 确认通过**. - [ ] **Step 5: Commit** `git commit -am "feat: VTT writer"`

---

## Task 6: JSON 写出器（TDD）

手写最小 JSON(含字级时间戳),避免引第三方库。需正确转义 `"` `\` 与控制符;中文按 UTF-8 原样输出(不转 \uXXXX)。

**Files:** Create `src/core/output/json_writer.h`, `.cpp`, `tests/test_json_writer.cpp`

**Interfaces:** Produces `std::string suji::to_json(const Transcript& t);`
- 结构:`{"full_text":"...","segments":[{"start":1.0,"end":2.5,"text":"...","tokens":[{"t":"你","start":1.0}, ...]}]}`

- [ ] **Step 1: 失败测试**
```cpp
#include "doctest/doctest.h"
#include "core/output/json_writer.h"
using namespace suji;
TEST_CASE("json escape + structure") {
  Transcript t; t.full_text=u8"他说\"hi\"";
  Segment s; s.start=1.0; s.end=2.0; s.text=u8"你"; Token tk; tk.text=u8"你"; tk.start=1.0; s.tokens={tk};
  t.segments={s};
  std::string j = to_json(t);
  CHECK(j.find("\\\"hi\\\"") != std::string::npos);          // 引号被转义
  CHECK(j.find("\"start\":1") != std::string::npos);
  CHECK(j.find("\xe4\xbd\xa0") != std::string::npos);        // 中文原样 UTF-8
}
```
- [ ] **Step 2: 确认失败**.
- [ ] **Step 3: 实现**
```cpp
// json_writer.h
#pragma once
#include "core/types.h"
#include <string>
namespace suji { std::string to_json(const Transcript& t); std::string json_escape(const std::string& s); }
```
```cpp
// json_writer.cpp
#include "core/output/json_writer.h"
#include <cstdio>
namespace suji {
std::string json_escape(const std::string& s){
  std::string o; o.reserve(s.size()+8);
  for(unsigned char c : s){
    switch(c){
      case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
      default:
        if(c < 0x20){ char b[8]; std::snprintf(b,sizeof(b),"\\u%04x",c); o += b; }
        else o += (char)c; // UTF-8 字节原样
    }
  }
  return o;
}
static std::string num(double d){ char b[32]; std::snprintf(b,sizeof(b),"%.3f",d); return b; }
std::string to_json(const Transcript& t){
  std::string o = "{\"full_text\":\"" + json_escape(t.full_text) + "\",\"segments\":[";
  for(size_t i=0;i<t.segments.size();++i){
    const auto& s = t.segments[i];
    o += "{\"start\":"+num(s.start)+",\"end\":"+num(s.end)+",\"text\":\""+json_escape(s.text)+"\",\"tokens\":[";
    for(size_t k=0;k<s.tokens.size();++k){
      o += "{\"t\":\""+json_escape(s.tokens[k].text)+"\",\"start\":"+num(s.tokens[k].start)+"}";
      if(k+1<s.tokens.size()) o += ",";
    }
    o += "]}"; if(i+1<t.segments.size()) o += ",";
  }
  o += "]}";
  return o;
}
}
```
- [ ] **Step 4: 确认通过**. - [ ] **Step 5: Commit** `git commit -am "feat: JSON writer with char timestamps"`

---

## Task 7: Markdown 写出器（TDD）

**Files:** Create `src/core/output/md_writer.h`, `.cpp`, `tests/test_md_writer.cpp`

**Interfaces:** Produces `std::string suji::to_markdown(const Transcript& t, const std::string& title);`
- 每段:`**[HH:MM:SS]** 文本`(时间锚点便于跳转复习),段间空行。顶部 `# title`。

- [ ] **Step 1: 失败测试**
```cpp
#include "doctest/doctest.h"
#include "core/output/md_writer.h"
using namespace suji;
TEST_CASE("md basic") {
  Transcript t; Segment s; s.start=61.0; s.text=u8"讲课内容"; t.segments={s};
  std::string md = to_markdown(t, u8"第一课");
  CHECK(md.rfind(u8"# 第一课",0)==0);
  CHECK(md.find(u8"**[00:01:01]** 讲课内容") != std::string::npos);
}
```
- [ ] **Step 2: 确认失败**.
- [ ] **Step 3: 实现**
```cpp
// md_writer.h
#pragma once
#include "core/types.h"
#include <string>
namespace suji { std::string to_markdown(const Transcript& t, const std::string& title); }
```
```cpp
// md_writer.cpp
#include "core/output/md_writer.h"
#include "core/timestamp.h"
namespace suji {
static std::string hhmmss(double sec){ std::string s = format_srt_time(sec); return s.substr(0,8); } // HH:MM:SS
std::string to_markdown(const Transcript& t, const std::string& title){
  std::string o = "# " + title + "\n\n";
  for(const auto& s : t.segments) o += "**[" + hhmmss(s.start) + "]** " + s.text + "\n\n";
  return o;
}
}
```
- [ ] **Step 4: 确认通过**. - [ ] **Step 5: Commit** `git commit -am "feat: Markdown writer with time anchors"`

---

## Task 8: EngineConfig + 默认值

**Files:** Create `src/core/config.h`

**Interfaces:** Produces `struct suji::EngineConfig`、`enum class suji::Provider`,被 vad/asr/punct/pipeline 消费。

- [ ] **Step 1: 写 `config.h`**（无独立测试,被后续 Task 测试覆盖）
```cpp
#pragma once
#include <string>
namespace suji {
enum class Provider { Cpu, Cuda };
inline const char* provider_str(Provider p){ return p==Provider::Cuda ? "cuda" : "cpu"; }
struct EngineConfig {
  std::string ffmpeg_path;     // ffmpeg.exe
  std::string asr_model;       // FireRedASR2-CTC model.int8.onnx
  std::string tokens;          // tokens.txt
  std::string vad_model;       // silero_vad.onnx
  std::string punct_model;     // CT punct model.int8.onnx
  std::string rule_fsts;       // 可选 ITN fst(空=关)
  Provider provider = Provider::Cpu;
  int num_threads = 4;         // CUDA 时置 1
  // VAD(默认值参考 c-api.h 示例;max_speech 取 20s 抑制超长段)
  float vad_threshold = 0.5f, vad_min_silence = 0.5f, vad_min_speech = 0.25f, vad_max_speech = 20.0f;
  int vad_window = 512;
  // 段落重建
  double merge_gap = 1.0, merge_max_dur = 30.0;
  // 输出开关
  bool out_srt=true, out_vtt=true, out_json=true, out_md=true;
};
}
```
- [ ] **Step 2: Commit** `git commit -am "feat: EngineConfig + defaults"`

---

## Task 9: 媒体解码（ffmpeg 子进程 → AudioBuffer）+ 集成测试

用 `ffmpeg.exe -i <in> -vn -ar 16000 -ac 1 -f f32le -` 从 stdout 管道读 float32 PCM,免临时文件。Windows 用 `_popen`("rb")。

**Files:** Create `src/core/media_decode.h`, `.cpp`, `tests/integration/test_media_decode.cpp`

**Interfaces:**
- Consumes: `EngineConfig.ffmpeg_path`
- Produces: `bool suji::decode_to_pcm(const std::string& ffmpeg, const std::string& input, AudioBuffer& out, std::string& err);`
  - 成功:`out.samples` 为 16k 单声道 f32,`out.sample_rate=16000`,返回 true;失败填 `err` 返回 false。

- [ ] **Step 1: 失败集成测试**（用 bundled wav;时长 0.wav≈10s → 期望 ~160k 采样,允许 ±10%）
```cpp
#include "doctest/doctest.h"
#include "core/media_decode.h"
#include <string>
using namespace suji;
static std::string ffmpeg(){ return SUJI_DEFAULT_FFMPEG; }
static std::string wav0(){ return std::string(SUJI_DEFAULT_MODELS_DIR) + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav"; }
TEST_CASE("decode wav to 16k mono pcm" * doctest::timeout(60)) {
  AudioBuffer ab; std::string err;
  REQUIRE_MESSAGE(decode_to_pcm(ffmpeg(), wav0(), ab, err), err);
  CHECK(ab.sample_rate == 16000);
  CHECK(ab.samples.size() > 120000);   // >7.5s
  CHECK(ab.samples.size() < 200000);   // <12.5s
}
TEST_CASE("decode missing file fails") {
  AudioBuffer ab; std::string err;
  CHECK_FALSE(decode_to_pcm(ffmpeg(), "no_such_file.wav", ab, err));
}
```

- [ ] **Step 2: 确认失败** build → 链接错误。
- [ ] **Step 3: 实现**
`media_decode.h`
```cpp
#pragma once
#include "core/types.h"
#include <string>
namespace suji { bool decode_to_pcm(const std::string& ffmpeg, const std::string& input, AudioBuffer& out, std::string& err); }
```
`media_decode.cpp`
```cpp
#include "core/media_decode.h"
#include <cstdio>
#include <vector>
namespace suji {
bool decode_to_pcm(const std::string& ffmpeg, const std::string& input, AudioBuffer& out, std::string& err) {
  // 路径含空格 -> 整条命令再包一层引号(cmd.exe 规则)
  std::string cmd = "\"\"" + ffmpeg + "\" -nostdin -loglevel error -i \"" + input +
                    "\" -vn -ar 16000 -ac 1 -f f32le -\"";
  FILE* pipe = _popen(cmd.c_str(), "rb");
  if (!pipe) { err = "failed to start ffmpeg"; return false; }
  out.samples.clear(); out.sample_rate = 16000;
  std::vector<float> buf(65536);
  size_t n;
  while ((n = std::fread(buf.data(), sizeof(float), buf.size(), pipe)) > 0)
    out.samples.insert(out.samples.end(), buf.begin(), buf.begin() + n);
  int rc = _pclose(pipe);
  if (out.samples.empty()) { err = "ffmpeg produced no audio (rc=" + std::to_string(rc) + ")"; return false; }
  return true;
}
}
```
> 注:`_popen` 在 Windows 需 `<cstdio>`;Release 构建可能需 `/D_CRT_SECURE_NO_WARNINGS`(已 `/W4`,不报错只警告)。失败检测以"无音频产出"为准,因 ffmpeg 经 cmd 包裹时 rc 不总可靠。

- [ ] **Step 4: 确认通过** Run: `build/Release/suji_tests.exe -tc="decode*"` Expected: PASS。
- [ ] **Step 5: Commit** `git commit -am "feat: ffmpeg subprocess decode to 16k mono f32 PCM"`

---

## Task 10: Silero VAD 包装 + 集成测试

**Files:** Create `src/core/vad.h`, `.cpp`, `tests/integration/test_vad.cpp`

**Interfaces:**
- Consumes: `EngineConfig`(vad_model + vad_* 参数)、`AudioBuffer`
- Produces: `class suji::Vad`(RAII):
  - `Vad(const EngineConfig& cfg);` `~Vad();` `bool ok() const;`
  - `std::vector<SpeechSeg> segment(const AudioBuffer& audio);` // 按 window 喂入,Flush 后收集所有段

- [ ] **Step 1: 失败集成测试**
```cpp
#include "doctest/doctest.h"
#include "core/vad.h"
#include "core/media_decode.h"
#include "core/config.h"
using namespace suji;
static EngineConfig cfg(){ EngineConfig c; c.ffmpeg_path=SUJI_DEFAULT_FFMPEG;
  c.vad_model=std::string(SUJI_DEFAULT_MODELS_DIR)+"/silero_vad.onnx"; return c; }
TEST_CASE("vad yields segments" * doctest::timeout(60)) {
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path,
    std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(cfg()); REQUIRE(vad.ok());
  auto segs = vad.segment(ab);
  REQUIRE(segs.size() >= 1);
  CHECK(segs[0].samples.size() > 0);
  CHECK(segs[0].start_sample >= 0);
}
```

- [ ] **Step 2: 确认失败**.
- [ ] **Step 3: 实现**
`vad.h`
```cpp
#pragma once
#include "core/types.h"
#include "core/config.h"
#include <vector>
struct SherpaOnnxVoiceActivityDetector;
namespace suji {
class Vad {
public:
  explicit Vad(const EngineConfig& cfg);
  ~Vad();
  Vad(const Vad&) = delete; Vad& operator=(const Vad&) = delete;
  bool ok() const { return vad_ != nullptr; }
  std::vector<SpeechSeg> segment(const AudioBuffer& audio);
private:
  const SherpaOnnxVoiceActivityDetector* vad_ = nullptr;
  int window_ = 512;
};
}
```
`vad.cpp`
```cpp
#include "core/vad.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <cstring>
namespace suji {
Vad::Vad(const EngineConfig& cfg) {
  window_ = cfg.vad_window;
  SherpaOnnxVadModelConfig c; std::memset(&c, 0, sizeof(c));
  c.silero_vad.model = cfg.vad_model.c_str();
  c.silero_vad.threshold = cfg.vad_threshold;
  c.silero_vad.min_silence_duration = cfg.vad_min_silence;
  c.silero_vad.min_speech_duration = cfg.vad_min_speech;
  c.silero_vad.max_speech_duration = cfg.vad_max_speech;
  c.silero_vad.window_size = cfg.vad_window;
  c.sample_rate = 16000; c.num_threads = 1; c.provider = "cpu"; c.debug = 0;
  vad_ = SherpaOnnxCreateVoiceActivityDetector(&c, 60.0f);
}
Vad::~Vad(){ if (vad_) SherpaOnnxDestroyVoiceActivityDetector(vad_); }
std::vector<SpeechSeg> Vad::segment(const AudioBuffer& audio) {
  std::vector<SpeechSeg> out;
  if (!vad_) return out;
  const float* p = audio.samples.data();
  int64_t total = (int64_t)audio.samples.size();
  for (int64_t i = 0; i + window_ <= total; i += window_) {
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, p + i, window_);
    while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
      const SherpaOnnxSpeechSegment* s = SherpaOnnxVoiceActivityDetectorFront(vad_);
      SpeechSeg seg; seg.start_sample = s->start; seg.samples.assign(s->samples, s->samples + s->n);
      out.push_back(std::move(seg));
      SherpaOnnxDestroySpeechSegment(s);
      SherpaOnnxVoiceActivityDetectorPop(vad_);
    }
  }
  SherpaOnnxVoiceActivityDetectorFlush(vad_); // 冲刷尾段
  while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
    const SherpaOnnxSpeechSegment* s = SherpaOnnxVoiceActivityDetectorFront(vad_);
    SpeechSeg seg; seg.start_sample = s->start; seg.samples.assign(s->samples, s->samples + s->n);
    out.push_back(std::move(seg));
    SherpaOnnxDestroySpeechSegment(s);
    SherpaOnnxVoiceActivityDetectorPop(vad_);
  }
  return out;
}
}
```

- [ ] **Step 4: 确认通过** Run: `build/Release/suji_tests.exe -tc="vad*"`. PASS。
- [ ] **Step 5: Commit** `git commit -am "feat: Silero VAD wrapper -> speech segments"`

---

## Task 11: ASR 包装（FireRedASR2-CTC 单流）+ 集成测试

**Files:** Create `src/core/asr.h`, `.cpp`, `tests/integration/test_asr.cpp`

**Interfaces:**
- Consumes: `EngineConfig`(asr_model/tokens/provider/num_threads/rule_fsts)、单段 `std::vector<float>`
- Produces: `class suji::Asr`(RAII,持有单个 recognizer):
  - `Asr(const EngineConfig& cfg);` `~Asr();` `bool ok() const;`
  - `AsrResult transcribe(const float* samples, int n);` // 单流:CreateStream→Accept→Decode→GetResult,本地时间戳

- [ ] **Step 1: 失败集成测试**（加载 740MB,放宽 timeout)
```cpp
#include "doctest/doctest.h"
#include "core/asr.h"
#include "core/media_decode.h"
#include "core/vad.h"
#include "core/config.h"
using namespace suji;
static EngineConfig cfg(){ EngineConfig c; std::string md=SUJI_DEFAULT_MODELS_DIR;
  c.ffmpeg_path=SUJI_DEFAULT_FFMPEG;
  c.asr_model=md+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/model.int8.onnx";
  c.tokens   =md+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/tokens.txt";
  c.vad_model=md+"/silero_vad.onnx"; c.provider=Provider::Cpu; c.num_threads=4; return c; }
TEST_CASE("asr transcribe a segment" * doctest::timeout(120)) {
  AudioBuffer ab; std::string err; auto c=cfg();
  REQUIRE(decode_to_pcm(c.ffmpeg_path, std::string(SUJI_DEFAULT_MODELS_DIR)+
    "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Asr asr(c); REQUIRE(asr.ok());
  auto r = asr.transcribe(ab.samples.data(), (int)ab.samples.size());
  CHECK_FALSE(r.text.empty());
  CHECK(r.tokens.size() == r.timestamps.size());
  CHECK(r.timestamps.size() >= 1);
}
```

- [ ] **Step 2: 确认失败**.
- [ ] **Step 3: 实现**
`asr.h`
```cpp
#pragma once
#include "core/types.h"
#include "core/config.h"
struct SherpaOnnxOfflineRecognizer;
namespace suji {
class Asr {
public:
  explicit Asr(const EngineConfig& cfg);
  ~Asr();
  Asr(const Asr&) = delete; Asr& operator=(const Asr&) = delete;
  bool ok() const { return rec_ != nullptr; }
  AsrResult transcribe(const float* samples, int n);
private:
  const SherpaOnnxOfflineRecognizer* rec_ = nullptr;
};
}
```
`asr.cpp`
```cpp
#include "core/asr.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <cstring>
namespace suji {
Asr::Asr(const EngineConfig& cfg) {
  SherpaOnnxOfflineRecognizerConfig c; std::memset(&c, 0, sizeof(c));
  c.feat_config.sample_rate = 16000;
  c.feat_config.feature_dim = 80;
  c.model_config.fire_red_asr_ctc.model = cfg.asr_model.c_str();
  c.model_config.tokens = cfg.tokens.c_str();
  c.model_config.num_threads = (cfg.provider == Provider::Cuda) ? 1 : cfg.num_threads;
  c.model_config.provider = provider_str(cfg.provider);
  c.model_config.debug = 0;
  c.decoding_method = "greedy_search";
  if (!cfg.rule_fsts.empty()) c.rule_fsts = cfg.rule_fsts.c_str(); // ITN 内置 FST(可选)
  rec_ = SherpaOnnxCreateOfflineRecognizer(&c);
}
Asr::~Asr(){ if (rec_) SherpaOnnxDestroyOfflineRecognizer(rec_); }
AsrResult Asr::transcribe(const float* samples, int n) {
  AsrResult out;
  if (!rec_) return out;
  const SherpaOnnxOfflineStream* st = SherpaOnnxCreateOfflineStream(rec_);
  SherpaOnnxAcceptWaveformOffline(st, 16000, samples, n);
  SherpaOnnxDecodeOfflineStream(rec_, st);
  const SherpaOnnxOfflineRecognizerResult* r = SherpaOnnxGetOfflineStreamResult(st);
  if (r) {
    if (r->text) out.text = r->text;
    int count = r->count;
    for (int i = 0; i < count; ++i) {
      out.tokens.push_back(r->tokens_arr && r->tokens_arr[i] ? r->tokens_arr[i] : "");
      out.timestamps.push_back(r->timestamps ? (double)r->timestamps[i] : 0.0); // 可能 NULL
    }
    SherpaOnnxDestroyOfflineRecognizerResult(r);
  }
  SherpaOnnxDestroyOfflineStream(st);
  return out;
}
}
```

- [ ] **Step 4: 确认通过** Run: `build/Release/suji_tests.exe -tc="asr*"`. PASS（约 5–25s,含模型加载）。
- [ ] **Step 5: Commit** `git commit -am "feat: FireRedASR2-CTC recognizer wrapper (single stream)"`

---

## Task 12: CT 标点包装 + 集成测试

**Files:** Create `src/core/punctuation.h`, `.cpp`, `tests/integration/test_punct.cpp`

**Interfaces:**
- Consumes: `EngineConfig.punct_model`、无标点文本
- Produces: `class suji::Punctuator`(RAII): `bool ok()`;`std::string add(const std::string& text);`(失败/未初始化时原样返回)

- [ ] **Step 1: 失败集成测试**
```cpp
#include "doctest/doctest.h"
#include "core/punctuation.h"
#include "core/config.h"
using namespace suji;
TEST_CASE("punct adds marks" * doctest::timeout(60)) {
  EngineConfig c; c.punct_model=std::string(SUJI_DEFAULT_MODELS_DIR)+
    "/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
  Punctuator p(c); REQUIRE(p.ok());
  std::string out = p.add(u8"今天天气怎么样明天呢");
  CHECK(out.size() >= std::string(u8"今天天气怎么样明天呢").size()); // 至少不更短(加了标点)
  CHECK(out != u8"今天天气怎么样明天呢");                              // 有变化
}
```

- [ ] **Step 2: 确认失败**.
- [ ] **Step 3: 实现**
`punctuation.h`
```cpp
#pragma once
#include "core/config.h"
#include <string>
struct SherpaOnnxOfflinePunctuation;
namespace suji {
class Punctuator {
public:
  explicit Punctuator(const EngineConfig& cfg);
  ~Punctuator();
  Punctuator(const Punctuator&) = delete; Punctuator& operator=(const Punctuator&) = delete;
  bool ok() const { return punct_ != nullptr; }
  std::string add(const std::string& text);
private:
  const SherpaOnnxOfflinePunctuation* punct_ = nullptr;
};
}
```
`punctuation.cpp`
```cpp
#include "core/punctuation.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <cstring>
namespace suji {
Punctuator::Punctuator(const EngineConfig& cfg) {
  if (cfg.punct_model.empty()) return;
  SherpaOnnxOfflinePunctuationConfig c; std::memset(&c, 0, sizeof(c));
  c.model.ct_transformer = cfg.punct_model.c_str();
  c.model.num_threads = 1; c.model.provider = "cpu"; c.model.debug = 0;
  punct_ = SherpaOnnxCreateOfflinePunctuation(&c);
}
Punctuator::~Punctuator(){ if (punct_) SherpaOnnxDestroyOfflinePunctuation(punct_); }
std::string Punctuator::add(const std::string& text) {
  if (!punct_ || text.empty()) return text;
  const char* res = SherpaOfflinePunctuationAddPunct(punct_, text.c_str()); // 注意函数名无 Onnx
  std::string out = res ? res : text;
  if (res) SherpaOfflinePunctuationFreeText(res);
  return out;
}
}
```

- [ ] **Step 4: 确认通过** Run: `build/Release/suji_tests.exe -tc="punct*"`. PASS。
- [ ] **Step 5: Commit** `git commit -am "feat: CT-Transformer punctuation wrapper"`

---

## Task 13: Pipeline（串联单文件端到端）+ e2e 集成测试

把各模块串起来:decode → VAD → 逐段 ASR(本地时间戳 + 段 start/16000 → 全局 token)→ 全局 token 列表 → 段落重建 → 每段文本过标点 → 组装 `Transcript`。

**Files:** Create `src/core/pipeline.h`, `.cpp`, `tests/integration/test_pipeline_e2e.cpp`

**Interfaces:**
- Consumes: `EngineConfig`、输入文件路径
- Produces: `bool suji::transcribe_file(const EngineConfig& cfg, const std::string& input, Transcript& out, std::string& err);`
  - 全局时间:`token.start = seg.start_sample/16000.0 + local_ts[i]`。先 merge_tokens 得段落,再对**每段** `text` 调标点(整篇一次也可,这里按段以保留时间对齐),`full_text` = 各段标点后文本拼接。

- [ ] **Step 1: 失败 e2e 测试**
```cpp
#include "doctest/doctest.h"
#include "core/pipeline.h"
#include "core/config.h"
using namespace suji;
static EngineConfig cfg(){ EngineConfig c; std::string md=SUJI_DEFAULT_MODELS_DIR;
  c.ffmpeg_path=SUJI_DEFAULT_FFMPEG;
  std::string m=md+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.asr_model=m+"model.int8.onnx"; c.tokens=m+"tokens.txt"; c.vad_model=md+"/silero_vad.onnx";
  c.punct_model=md+"/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
  c.provider=Provider::Cpu; c.num_threads=4; return c; }
TEST_CASE("end to end transcribe" * doctest::timeout(180)) {
  Transcript t; std::string err; auto c=cfg();
  std::string in = std::string(SUJI_DEFAULT_MODELS_DIR)+
    "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav";
  REQUIRE_MESSAGE(transcribe_file(c, in, t, err), err);
  REQUIRE(t.segments.size() >= 1);
  CHECK_FALSE(t.full_text.empty());
  // 全局时间单调
  for (size_t i=1;i<t.segments.size();++i) CHECK(t.segments[i].start >= t.segments[i-1].start);
}
```

- [ ] **Step 2: 确认失败**.
- [ ] **Step 3: 实现**
`pipeline.h`
```cpp
#pragma once
#include "core/types.h"
#include "core/config.h"
#include <string>
namespace suji { bool transcribe_file(const EngineConfig& cfg, const std::string& input, Transcript& out, std::string& err); }
```
`pipeline.cpp`
```cpp
#include "core/pipeline.h"
#include "core/media_decode.h"
#include "core/vad.h"
#include "core/asr.h"
#include "core/punctuation.h"
#include "core/segment_merge.h"
namespace suji {
bool transcribe_file(const EngineConfig& cfg, const std::string& input, Transcript& out, std::string& err) {
  AudioBuffer audio;
  if (!decode_to_pcm(cfg.ffmpeg_path, input, audio, err)) return false;
  Vad vad(cfg); if (!vad.ok()) { err = "VAD init failed"; return false; }
  Asr asr(cfg); if (!asr.ok()) { err = "ASR init failed"; return false; }
  Punctuator punct(cfg); // 失败则原样返回,不致命

  auto segs = vad.segment(audio);
  std::vector<Token> global_tokens;
  for (const auto& s : segs) {
    auto r = asr.transcribe(s.samples.data(), (int)s.samples.size());
    double base = (double)s.start_sample / 16000.0;
    for (size_t i = 0; i < r.tokens.size(); ++i) {
      Token tk; tk.text = r.tokens[i]; tk.start = base + r.timestamps[i];
      global_tokens.push_back(tk);
    }
  }
  out.segments = merge_tokens(global_tokens, cfg.merge_gap, cfg.merge_max_dur);
  out.full_text.clear();
  for (auto& seg : out.segments) {
    seg.text = punct.add(seg.text);     // 每段过标点
    out.full_text += seg.text;
  }
  return true;
}
}
```

- [ ] **Step 4: 确认通过** Run: `build/Release/suji_tests.exe -tc="end to end*"`. PASS。
- [ ] **Step 5: Commit** `git commit -am "feat: single-file end-to-end pipeline"`

---

## Task 14: CLI（参数解析 + 写出四种格式）+ e2e CLI 测试

**Files:** Modify `src/cli/main.cpp`;Create `src/core/output/writer_facade.h`, `.cpp`(把 Transcript 落 4 文件),`tests/integration/test_cli_e2e.cpp`

**Interfaces:**
- Produces: `bool suji::write_outputs(const Transcript& t, const std::string& out_base, const EngineConfig& cfg, const std::string& title);`(生成 `<base>.srt/.vtt/.json/.md`,UTF-8 无 BOM,防重名见 Phase 3)
- CLI:`suji_cli <input> [-o <out_dir>] [--provider cpu|cuda] [--rule-fsts <fst>] [--no-srt] ...`,缺省模型路径用编译期默认。

- [ ] **Step 1: 失败 CLI e2e 测试**（直接调 `write_outputs` + 检查文件,避免起子进程)
```cpp
#include "doctest/doctest.h"
#include "core/output/writer_facade.h"
#include "core/pipeline.h"
#include "core/config.h"
#include <fstream>
using namespace suji;
TEST_CASE("write four outputs" * doctest::timeout(180)) {
  EngineConfig c; std::string md=SUJI_DEFAULT_MODELS_DIR; std::string m=md+
    "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.ffmpeg_path=SUJI_DEFAULT_FFMPEG; c.asr_model=m+"model.int8.onnx"; c.tokens=m+"tokens.txt";
  c.vad_model=md+"/silero_vad.onnx";
  c.punct_model=md+"/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
  Transcript t; std::string err;
  REQUIRE(transcribe_file(c, m+"test_wavs/0.wav", t, err));
  REQUIRE(write_outputs(t, "cli_e2e_out", c, "test"));
  for (auto ext : {".srt",".vtt",".json",".md"}) {
    std::ifstream in(std::string("cli_e2e_out")+ext, std::ios::binary);
    REQUIRE(in.good());
    std::string data((std::istreambuf_iterator<char>(in)),{});
    CHECK(data.size() > 0);
    CHECK_FALSE((unsigned char)data[0]==0xEF); // 无 BOM
  }
}
```

- [ ] **Step 2: 确认失败**.
- [ ] **Step 3: 实现 `writer_facade`**
`writer_facade.h`
```cpp
#pragma once
#include "core/types.h"
#include "core/config.h"
#include <string>
namespace suji { bool write_outputs(const Transcript& t, const std::string& out_base, const EngineConfig& cfg, const std::string& title); }
```
`writer_facade.cpp`
```cpp
#include "core/output/writer_facade.h"
#include "core/output/srt_writer.h"
#include "core/output/vtt_writer.h"
#include "core/output/json_writer.h"
#include "core/output/md_writer.h"
#include "core/utf8_file.h"
namespace suji {
bool write_outputs(const Transcript& t, const std::string& base, const EngineConfig& cfg, const std::string& title) {
  bool ok = true;
  if (cfg.out_srt)  ok &= write_utf8_no_bom(base + ".srt",  to_srt(t));
  if (cfg.out_vtt)  ok &= write_utf8_no_bom(base + ".vtt",  to_vtt(t));
  if (cfg.out_json) ok &= write_utf8_no_bom(base + ".json", to_json(t));
  if (cfg.out_md)   ok &= write_utf8_no_bom(base + ".md",   to_markdown(t, title));
  return ok;
}
}
```
- [ ] **Step 4: 实现 `src/cli/main.cpp`**
```cpp
#include "core/config.h"
#include "core/pipeline.h"
#include "core/output/writer_facade.h"
#include "core/log.h"
#include <string>
#include <cstdio>
using namespace suji;
static std::string stem(const std::string& p){
  size_t a=p.find_last_of("/\\"); size_t b=p.find_last_of('.');
  size_t s=(a==std::string::npos)?0:a+1;
  return (b==std::string::npos||b<s)?p.substr(s):p.substr(s,b-s);
}
int main(int argc, char** argv){
  if (argc < 2){ std::puts("usage: suji_cli <input> [-o out_dir] [--provider cpu|cuda] [--rule-fsts f.fst]"); return 2; }
  EngineConfig c;
  std::string md = SUJI_DEFAULT_MODELS_DIR, m = md+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.ffmpeg_path=SUJI_DEFAULT_FFMPEG; c.asr_model=m+"model.int8.onnx"; c.tokens=m+"tokens.txt";
  c.vad_model=md+"/silero_vad.onnx";
  c.punct_model=md+"/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
  std::string input=argv[1], out_dir=".";
  for (int i=2;i<argc;++i){ std::string a=argv[i];
    if (a=="-o" && i+1<argc) out_dir=argv[++i];
    else if (a=="--provider" && i+1<argc) c.provider = (std::string(argv[++i])=="cuda")?Provider::Cuda:Provider::Cpu;
    else if (a=="--rule-fsts" && i+1<argc) c.rule_fsts=argv[++i];
    else if (a=="--no-srt") c.out_srt=false; else if (a=="--no-vtt") c.out_vtt=false;
    else if (a=="--no-json") c.out_json=false; else if (a=="--no-md") c.out_md=false;
  }
  Transcript t; std::string err;
  log_info("transcribing: " + input);
  if (!transcribe_file(c, input, t, err)){ log_err("failed: "+err); return 1; }
  std::string base = out_dir + "/" + stem(input);
  if (!write_outputs(t, base, c, stem(input))){ log_err("write failed"); return 1; }
  log_info("done: "+base+".{srt,vtt,json,md}  segments="+std::to_string(t.segments.size()));
  return 0;
}
```
- [ ] **Step 5: 确认通过 + 真跑 CLI**
Run:
```bash
cmake --build build --config Release
build/Release/suji_tests.exe -tc="write four outputs*"
build/Release/suji_cli.exe "models/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/3.wav" -o build
```
Expected: 测试 PASS;CLI 打印 `done: build/3.{srt,...} segments=N`;`build/3.srt` 是 UTF-8 无 BOM 中文。
- [ ] **Step 6: Commit** `git commit -am "feat: suji_cli + 4-format output writer facade"`

---

## Task 15: ITN（内置 FST,可选）接线 + 验证脚本

把 `--rule-fsts` 已接到 recognizer(Task 11)。本 Task 取得中文数字 ITN FST 并验证;若取不到则保持"关",不阻塞 Phase 1。

**Files:** Create `scripts/itn_compare.py`(开发期对照,不入产品);更新 `RUNLOG.md`

**Interfaces:** 无新 C++ 接口。Consumes 现有 `--rule-fsts`。

- [ ] **Step 1: 取 zh ITN FST**
Run:
```bash
curl.exe -L --fail -o vendor/itn_zh_number.fst https://github.com/k2-fsa/sherpa-onnx/releases/download/itn-models/itn_zh_number.fst
```
Expected: 成功 → 有 fst 文件;若 404(资产名不确定),记 RUNLOG 待核实,本 Task 标"ITN 关",跳到 Step 4。

- [ ] **Step 2: 带 ITN 跑 CLI 对照**
Run:
```bash
build/Release/suji_cli.exe "models/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/1.wav" -o build/with_itn --rule-fsts vendor/itn_zh_number.fst
build/Release/suji_cli.exe "models/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/1.wav" -o build/no_itn
```
Expected: 含数字/日期处 with_itn 为written-form(如"二零二六"→"2026");人工对比两份 .md。

- [ ] **Step 3: (可选)wetext 开发期对照** `scripts/itn_compare.py`
```python
# 仅开发期:pip install wetext;对 no_itn 文本跑 wetext,与内置 FST 结果比对,判断够不够用
from wetext import Normalizer
import sys
n = Normalizer(lang="zh", operator="itn")
print(n.normalize(sys.stdin.read()))
```
Run: `type build\no_itn\1.md | python scripts/itn_compare.py`(对照,非自动断言)。

- [ ] **Step 4: 记录结论到 RUNLOG**（内置 FST 够用?是否启用?wetext 是否需要?）
- [ ] **Step 5: Commit** `git add scripts/ RUNLOG.md && git commit -m "feat: optional built-in FST ITN wiring + dev compare script"`

---

## Phase 1 完成验收（Stop Checkpoint）

- 全部单元测试 + 集成测试 PASS:`build/Release/suji_tests.exe`(末行 `assertions: … all passed`)。
- `suji_cli.exe <讲课文件>` 产出 `<名>.srt/.vtt/.json/.md`,UTF-8 无 BOM,中文不乱码,时间戳单调对齐。
- 真跑一个**多分钟真实讲课**文件(非 test_wavs)验证:VAD 切段、段落可读、标点合理。
- 产出 Phase 1 设计小结 + 更新 RUNLOG。**确认正确再进 Phase 2(GPU 批量 + 自适应)。**

---

## Self-Review(写完即查)

**Spec 覆盖**:媒体 I/O(T9)✓、VAD+单流 ASR+全局时间戳(T10/11/13)✓、标点(T12)✓、ITN(T15)✓、段落重建(T3)✓、SRT/VTT/JSON/MD + UTF-8 无 BOM(T1–7/14)✓、CLI(T14)✓。批量/GPU/编排/GUI/打包**不在 Phase 1**(后续 plan)。
**Placeholder 扫描**:无 TBD;每个代码步骤含完整代码。
**类型一致性**:`AsrResult{text,tokens,timestamps}`、`SpeechSeg{start_sample,samples}`、`Token{text,start}`、`Segment{start,end,text,tokens}`、`Transcript{segments,full_text}` 全程一致;函数名 `decode_to_pcm/segment/transcribe/add/merge_tokens/to_srt/to_vtt/to_json/to_markdown/write_outputs/transcribe_file` 各 Task 引用一致;sherpa C API 名以 §"已核实 C API" 为准(`SherpaOfflinePunctuationAddPunct` 无 Onnx;`SherpaOnnxDestroySpeechSegment` 释放段)。
**已知风险**:ITN FST 资产名待 Step 1 实证;ASR 集成测试较慢(加载 740MB,timeout 已放宽)。
