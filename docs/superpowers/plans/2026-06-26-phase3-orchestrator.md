# Phase 3 — 批处理编排:断点续跑 + ETA + 汇总 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]`.

**Goal:** 给 `suji_batch` 加**断点续跑**(已完成的文件按"输出存在且段数>0"跳过)、**ETA/进度**、和**汇总**(已完成/跳过/失败 + 聚合吞吐)。队列/并发/错误隔离/输出防重名已在 Phase 2 完成。

**Architecture:** 编排逻辑加在 CLI 层(`suji_batch`),引擎(`transcribe_batch_files`)保持纯粹。新增一个可测的纯函数 `transcript_complete(out_base, cfg)` 判断某文件输出是否已完成。CLI 在跑前按它把输入分成"已完成→跳过"和"待办→处理",只把待办交给引擎;进度回调里算 ETA;结束打印汇总。

**Tech Stack:** C++17 · std::filesystem · 复用 Phase-1/2 单元。

## Global Constraints
- 平台 Windows x64,MSVC,C++17,`/utf-8`,`/W4` pristine。cmake 全路径 `F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe`。
- 复用:`EngineConfig`(out_srt/vtt/json/md)、`write_outputs`、`transcribe_batch_files`、`BatchProgress`。
- **"完成"判据**:输出**存在且段数>0**——不能把空结果当完成(spec 明确)。判据:启用的各输出文件都存在,且 SRT(若启用)非空 / 否则 JSON 含至少一个 segment。空 transcript → 空 SRT(0 字节)→ **不**算完成 → 重跑。
- 默认 `--resume` 开;`--no-resume` 关。
- DRY/YAGNI/TDD/频繁本地提交(不 push)。不做 GUI/打包。

## File Structure
```
src/core/resume.h .cpp           # transcript_complete(out_base, cfg)
src/cli/batch_main.cpp           # (MODIFY) resume 分区 + ETA + 汇总
tests/test_resume.cpp            # TDD(临时文件)
```

---

## Task 1: `transcript_complete` 续跑判据(纯函数,TDD)

**Files:** Create `src/core/resume.h`, `src/core/resume.cpp`, `tests/test_resume.cpp`

**Interfaces — Produces:**
```cpp
// resume.h
#pragma once
#include "core/config.h"
#include <string>
namespace suji {
// true 当且仅当 out_base 对应的、cfg 中启用的所有输出都存在,且内容非空(段数>0)。
// 判据:每个启用的扩展名文件存在;若 out_srt 则 <base>.srt 必须 size>0(空 SRT=无段);
//   若 out_srt 关而 out_md 开,则 <base>.md 必须 size > len("# <stem>\n\n"); 否则 out_json 则 .json 含 '"start":'.
bool transcript_complete(const std::string& out_base, const EngineConfig& cfg);
}
```

- [ ] **Step 1: 失败测试** `tests/test_resume.cpp`
```cpp
#include "doctest/doctest.h"
#include "core/resume.h"
#include "core/utf8_file.h"
#include "core/config.h"
#include <cstdio>
using namespace suji;
TEST_CASE("transcript_complete: all outputs present + non-empty SRT") {
  EngineConfig c; // defaults: all 4 enabled
  std::string b="resume_tmp_done";
  write_utf8_no_bom(b+".srt", "1\n00:00:01,000 --> 00:00:02,000\nhi\n\n");
  write_utf8_no_bom(b+".vtt", "WEBVTT\n\n...");
  write_utf8_no_bom(b+".json", "{\"full_text\":\"hi\",\"segments\":[{\"start\":1.000}]}");
  write_utf8_no_bom(b+".md", "# x\n\n**[00:00:01]** hi\n\n");
  CHECK(transcript_complete(b, c));
  for(auto e:{".srt",".vtt",".json",".md"}) std::remove((b+e).c_str());
}
TEST_CASE("transcript_complete: empty SRT (no segments) -> not complete") {
  EngineConfig c; std::string b="resume_tmp_empty";
  write_utf8_no_bom(b+".srt", "");                  // empty = no segments
  write_utf8_no_bom(b+".vtt", "WEBVTT\n\n");
  write_utf8_no_bom(b+".json", "{\"full_text\":\"\",\"segments\":[]}");
  write_utf8_no_bom(b+".md", "# x\n\n");
  CHECK_FALSE(transcript_complete(b, c));
  for(auto e:{".srt",".vtt",".json",".md"}) std::remove((b+e).c_str());
}
TEST_CASE("transcript_complete: missing a file -> not complete") {
  EngineConfig c; std::string b="resume_tmp_missing";
  write_utf8_no_bom(b+".srt", "1\n...\nhi\n\n");      // only srt
  CHECK_FALSE(transcript_complete(b, c));
  std::remove((b+".srt").c_str());
}
TEST_CASE("transcript_complete: respects disabled outputs") {
  EngineConfig c; c.out_vtt=false; c.out_json=false; c.out_md=false; // only srt enabled
  std::string b="resume_tmp_srtonly";
  write_utf8_no_bom(b+".srt", "1\n...\nhi\n\n");
  CHECK(transcript_complete(b, c));                  // only srt required, and it's non-empty
  std::remove((b+".srt").c_str());
}
```

- [ ] **Step 2: build → fail.**
- [ ] **Step 3: implement** `resume.cpp`
```cpp
#include "core/resume.h"
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
namespace suji {
static bool exists_nonempty(const std::string& p){ std::error_code ec; return fs::exists(p,ec) && fs::file_size(p,ec) > 0; }
static bool exists_any(const std::string& p){ std::error_code ec; return fs::exists(p,ec); }
static bool file_contains(const std::string& p, const std::string& needle){
  std::ifstream in(p, std::ios::binary); if(!in) return false;
  std::string data((std::istreambuf_iterator<char>(in)), {}); return data.find(needle)!=std::string::npos;
}
bool transcript_complete(const std::string& out_base, const EngineConfig& cfg){
  // every enabled output must at least exist
  if(cfg.out_srt  && !exists_any(out_base+".srt"))  return false;
  if(cfg.out_vtt  && !exists_any(out_base+".vtt"))  return false;
  if(cfg.out_json && !exists_any(out_base+".json")) return false;
  if(cfg.out_md   && !exists_any(out_base+".md"))   return false;
  // and content must be non-empty (segments>0), checked via the best available signal
  if(cfg.out_srt)  return exists_nonempty(out_base+".srt");        // empty SRT == no segments
  if(cfg.out_json) return file_contains(out_base+".json", "\"start\":"); // a token/segment has a start
  if(cfg.out_md)   return file_contains(out_base+".md", "**[");    // a segment anchor
  if(cfg.out_vtt)  return file_contains(out_base+".vtt", "-->");   // a cue
  return false; // nothing enabled -> never "complete"
}
}
```
- [ ] **Step 4: build → run** `-tc="transcript_complete*"` → pass; full suite green.
- [ ] **Step 5: commit** `git add src/core/resume.h src/core/resume.cpp tests/test_resume.cpp && git commit -m "feat: resume completion check (outputs exist + segments>0)"`

---

## Task 2: `suji_batch` 续跑分区 + ETA + 汇总

**Files:** Modify `src/cli/batch_main.cpp`

**Behavior:**
- 新 flag:`--resume`(默认开)/`--no-resume`。
- 跑前分区:对每个 input,`base = out_dir + "/" + stem(input)`;若 resume 开且 `transcript_complete(base, c)` → 跳过(计 skipped,log 一行);否则进 todo 列表。**注意**:分区时要用与写出时**相同**的去重后 base(沿用 Phase-2 的 stem 去重逻辑,保证 resume 判据和实际输出路径一致)。简化:Phase 3 先按"无撞名"假设(大多数批次 stem 唯一);撞名去重与 resume 的交互记为已知限制(撞名文件不参与 resume 跳过,总会重跑——安全)。
- 只把 todo 交给 `transcribe_batch_files`。
- ETA:CLI 记 `start=steady_clock::now()`;进度回调里 `elapsed=now-start`,`eta = elapsed * (files_total-files_done)/max(1,files_done)`,打印 `\r[done/total] <audioSec>s done, ETA <m>m<s>s`。
- 汇总:`done: <ok>/<todo> ok, skipped(resumed)=<n>, failed=<f>, wall=<s>s, throughput=<audio_done/wall>x realtime`(audio_done 取最后一次回调的 `audio_seconds_done`)。
- 退出码:有 ok 或全 skipped → 0;否则(有 todo 但全失败)→ 1;usage → 2。

- [ ] **Step 1: 实现 CLI 改动**(在现有 batch_main.cpp 基础上加 resume 分区 + ETA + 汇总;保留 Phase-2 的自适应/CUDA 安全/撞名去重/写出逻辑)。关键片段:
```cpp
  // after inputs collected, before run:
  bool resume = true;
  // (parse --resume / --no-resume in the arg loop; default true)
  std::vector<std::string> todo; int skipped=0;
  for(auto& in : inputs){
    std::string base = out_dir + "/" + stem(in);
    if(resume && transcript_complete(base, c)){ ++skipped; log_info("resume: skip (done) "+in); }
    else todo.push_back(in);
  }
  if(todo.empty()){ std::printf("nothing to do: %d already complete (resumed)\n", skipped); return 0; }
  // ... probe+decide+fallback (unchanged) ...
  auto t0 = std::chrono::steady_clock::now();
  double last_audio = 0;
  auto res = transcribe_batch_files(todo, c, tune, [&](const BatchProgress& b){
    last_audio = b.audio_seconds_done;
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
    double eta = b.files_done>0 ? el*(double)(b.files_total-b.files_done)/b.files_done : 0;
    std::fprintf(stderr,"\r[%d/%d] %.0fs audio, ETA %dm%02ds   ", b.files_done,b.files_total,b.audio_seconds_done,(int)eta/60,(int)eta%60);
  });
  double wall = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
  // write outputs (with the Phase-2 stem-dedup loop) ...
  int okc=0,failc=0; for(auto& r:res){ if(r.ok) okc++; else { failc++; log_err("FAILED "+r.input+": "+r.err); } }
  std::printf("\ndone: %d/%zu ok, skipped(resumed)=%d, failed=%d, wall=%.1fs, throughput=%.1fx realtime\n",
              okc, todo.size(), skipped, failc, wall, wall>0?last_audio/wall:0.0);
  return (okc>0||skipped>0)?0:1;
```
- [ ] **Step 2: build → full suite green**(CLI 改动不影响 suji_tests)。
- [ ] **Step 3: 真跑验证(resume)**:
```
build\Release\suji_batch.exe "<test_wavs 目录>" -o build/resume_test --provider cpu     # 第一次:全部处理
build\Release\suji_batch.exe "<test_wavs 目录>" -o build/resume_test --provider cpu     # 第二次:全部 skipped(resumed),秒回
```
确认第二次输出 `nothing to do: N already complete` 或 `skipped(resumed)=N`,且很快返回。捕获两次输出。
- [ ] **Step 4: commit** `git add src/cli/batch_main.cpp && git commit -m "feat: suji_batch resume (skip completed) + ETA + run summary"`

---

## Phase 3 完成验收
- `transcript_complete` 单测绿;`suji_batch` 第二次跑全跳过(resume);ETA + 汇总打印正确。全套测试绿。

## Self-Review
- 覆盖 spec Phase 3:队列/并发(P2)、错误隔离(P2)、输出防重名(P2 stem dedup)、**断点续跑(本 Phase,输出存在且段数>0)**、**进度+ETA+聚合吞吐(本 Phase)**。
- 已知限制:撞名去重文件不参与 resume 跳过(总重跑,安全);记入 PROGRESS。
- 类型一致:transcript_complete(out_base,cfg);复用 BatchProgress/EngineConfig/write_outputs。
