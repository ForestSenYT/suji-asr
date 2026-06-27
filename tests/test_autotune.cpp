#include "doctest/doctest.h"
#include "core/hardware.h"
#include "core/config.h"
using namespace suji;
static HardwareInfo hw(bool gpu, int freemb, int cores, int ramfree, bool cuda_rt = false) {
  HardwareInfo h;
  h.has_cuda_gpu = gpu;
  h.gpu_free_mb = freemb;
  h.gpu_total_mb = 8192;
  h.cpu_threads = cores;
  h.ram_free_mb = ramfree;
  h.ram_total_mb = ramfree;
  h.cuda_runtime_available = cuda_rt;
  return h;
}

// ---- existing tests (must stay green) ----

TEST_CASE("autotune picks GPU when enough VRAM") {
  // 8 cores: gpu_ok=true, cpu_ok=false (< 12 cores) -> Cuda-only (not Hetero)
  auto t = decide(hw(true, 6000, 8, 40000, true), EngineConfig{});
  CHECK(t.provider == Provider::Cuda);
  CHECK(t.num_threads == 1);          // GPU -> 1
  CHECK(t.batch >= 8);
}
TEST_CASE("autotune falls to CPU when no GPU") {
  auto t = decide(hw(false, 0, 16, 40000), EngineConfig{});
  CHECK(t.provider == Provider::Cpu);
  CHECK(t.num_threads >= 4);          // uses cores
  CHECK(t.batch >= 1);
  CHECK(t.in_flight_files >= 1);
}
TEST_CASE("autotune falls to CPU when VRAM too low") {
  auto t = decide(hw(true, 1000, 16, 40000, true), EngineConfig{}); // <3GB free
  CHECK(t.provider == Provider::Cpu);
}
TEST_CASE("autotune falls to CPU when GPU present but no CUDA runtime") {
  auto t = decide(hw(true, 6000, 16, 40000, false), EngineConfig{});
  CHECK(t.provider == Provider::Cpu);
}
TEST_CASE("autotune in-flight scales with RAM but stays bounded") {
  // 8 cores: forces Cuda-only path, which uses the RAM clamp
  auto t = decide(hw(true, 6000, 8, 40000, true), EngineConfig{});
  CHECK(t.in_flight_files >= 2);
  CHECK(t.in_flight_files <= 8);
}

// ---- H1: provider_str covers all three variants ----

TEST_CASE("provider_str returns correct strings") {
  CHECK(std::string(provider_str(Provider::Cpu))    == "cpu");
  CHECK(std::string(provider_str(Provider::Cuda))   == "cuda");
  CHECK(std::string(provider_str(Provider::Hetero)) == "hetero");
}

// ---- H2: selection gate ----

TEST_CASE("autotune selection gate: GPU + >=12 cores -> Hetero") {
  auto t = decide(hw(true, 6000, 16, 40000, true), EngineConfig{});
  CHECK(t.provider == Provider::Hetero);
  CHECK(t.hetero == true);
}
TEST_CASE("autotune selection gate: GPU + 8 cores -> Cuda (cpu_ok fails)") {
  auto t = decide(hw(true, 6000, 8, 40000, true), EngineConfig{});
  CHECK(t.provider == Provider::Cuda);
  CHECK(t.hetero == false);
}
TEST_CASE("autotune selection gate: no GPU + 16 cores -> Cpu") {
  auto t = decide(hw(false, 0, 16, 40000, false), EngineConfig{});
  CHECK(t.provider == Provider::Cpu);
  CHECK(t.hetero == false);
}
TEST_CASE("autotune selection gate: gpu_free_mb<3000 -> not Hetero") {
  auto t = decide(hw(true, 2000, 16, 40000, true), EngineConfig{});
  CHECK(t.provider != Provider::Hetero);
}

// ---- H2: R1 oversubscription matrix ----
// For every (C, ram) combo where hetero fires: P + 1 + cpu_asr_threads <= C.
// Also verify per-engine floor values.

// Helper: make HW with GPU healthy enough for hetero
static HardwareInfo hw_hetero(int cores, int ramfree) {
  return hw(true, 6000, cores, ramfree, true);
}

static void check_no_oversubscription(int cores, int ramfree) {
  auto t = decide(hw_hetero(cores, ramfree), EngineConfig{});
  // Only meaningful when hetero actually fired
  if (!t.hetero) return;
  // P = in_flight_files, gpu_feed = 1
  const int gpu_feed = 1;
  INFO("C=" << cores << " ram=" << ramfree
       << " in_flight=" << t.in_flight_files
       << " cpu_asr=" << t.cpu_asr_threads
       << " sum=" << (t.in_flight_files + gpu_feed + t.cpu_asr_threads));
  CHECK(t.in_flight_files + gpu_feed + t.cpu_asr_threads <= cores);
  CHECK(t.cpu_asr_threads >= 2);
  CHECK(t.cpu_batch >= 2);
  CHECK(t.gpu_batch >= 8);
}

TEST_CASE("autotune hetero: no oversubscription C=12 x ram matrix") {
  for (int ram : {8000, 40000, 128000}) {
    check_no_oversubscription(12, ram);
  }
}
TEST_CASE("autotune hetero: no oversubscription C=16 x ram matrix (includes R1 bug case)") {
  // C=16, ram=40000 was the exact failing case: old code -> 8+1+11=20 > 16
  for (int ram : {8000, 40000, 128000}) {
    check_no_oversubscription(16, ram);
  }
}
TEST_CASE("autotune hetero: no oversubscription C=24 x ram matrix") {
  for (int ram : {8000, 40000, 128000}) {
    check_no_oversubscription(24, ram);
  }
}
TEST_CASE("autotune hetero: no oversubscription C=32 x ram matrix") {
  for (int ram : {8000, 40000, 128000}) {
    check_no_oversubscription(32, ram);
  }
}

// Explicit verification of the R1 regression: C=16, ram=40000
// Old code: in_flight_files=8 (from RAM clamp), cpu_asr_threads=11 -> 8+1+11=20 > 16.
// New code: P=clamp(16/4,2,6)=4, cpu_asr=max(2,16-4-1)=11 -> 4+1+11=16 <= 16.
TEST_CASE("autotune hetero R1 regression: C=16 ram=40000 exact values") {
  auto t = decide(hw_hetero(16, 40000), EngineConfig{});
  REQUIRE(t.hetero == true);
  CHECK(t.in_flight_files == 4);
  CHECK(t.cpu_asr_threads == 11);
  CHECK(t.gpu_batch >= 8);
  CHECK(t.gpu_batch <= 32);
  const int gpu_feed = 1;
  CHECK(t.in_flight_files + gpu_feed + t.cpu_asr_threads == 16); // exactly C, no oversubscription
}

// ---- H4: fill_hetero() produces the same tunables as decide()'s hetero branch ----

// fill_hetero(t, hw) must agree with decide() when hardware qualifies for hetero.
// Primary assertion: C=16, 6 GB free VRAM (gpu_free_mb=6000 -> headroom=4000 (2000MB reserved) ->
//   gpu_batch=clamp(4000/1024,8,12)=clamp(3,8,12)=8). The small-batch floor is the
//   benchmarked hetero optimum on the 2080 (int8 GPU is the slow half; small GPU
//   batch keeps work-stealing tilted toward the fast CPU). See fill_hetero comment.
TEST_CASE("fill_hetero: synthetic C=16 6GB gpu produces correct tunables") {
  HardwareInfo h;
  h.cpu_threads           = 16;
  h.has_cuda_gpu          = true;
  h.gpu_free_mb           = 6000;   // 6000-2000=4000; 4000/1024=3; clamp(3,8,12)=8
  h.gpu_total_mb          = 8192;
  h.cuda_runtime_available = true;
  h.ram_free_mb           = 40000;

  AutoTune t;
  // Multi-file count (>= P+1) -> conservative steady-state split (the legacy values).
  fill_hetero(t, h, 8);

  CHECK(t.hetero == true);
  CHECK(t.provider == Provider::Hetero);
  // P = clamp(16/4,2,6) = 4
  CHECK(t.in_flight_files == 4);
  // cpu_asr = max(2, 16-4-1) = 11 (steady-state cap)
  CHECK(t.cpu_asr_threads == 11);
  // cpu_batch = clamp(11/2,2,6) = clamp(5,2,6) = 5
  CHECK(t.cpu_batch == 5);
  // gpu_batch = clamp((6000-2000)/1024, 8, 12) = clamp(3, 8, 12) = 8 (benchmarked floor)
  CHECK(t.gpu_batch == 8);
  // postcondition: no oversubscription
  const int gpu_feed = 1;
  CHECK(t.in_flight_files + gpu_feed + t.cpu_asr_threads <= 16);
  // legacy mirrors
  CHECK(t.num_threads == t.cpu_asr_threads);
  CHECK(t.batch == t.gpu_batch);
}

// fill_hetero must produce the SAME values as decide() for the same HW.
TEST_CASE("fill_hetero agrees with decide() on het path") {
  HardwareInfo h;
  h.cpu_threads            = 16;
  h.has_cuda_gpu           = true;
  h.gpu_free_mb            = 6000;
  h.gpu_total_mb           = 8192;
  h.cuda_runtime_available = true;
  h.ram_free_mb            = 40000;

  AutoTune from_decide = decide(h, EngineConfig{});
  AutoTune from_fill;
  // decide() uses the num_files=0 sentinel (conservative multi-file); match it here.
  fill_hetero(from_fill, h, 0);

  REQUIRE(from_decide.hetero == true);
  CHECK(from_fill.in_flight_files  == from_decide.in_flight_files);
  CHECK(from_fill.cpu_asr_threads  == from_decide.cpu_asr_threads);
  CHECK(from_fill.cpu_batch        == from_decide.cpu_batch);
  CHECK(from_fill.gpu_batch        == from_decide.gpu_batch);
  CHECK(from_fill.num_threads      == from_decide.num_threads);
  CHECK(from_fill.batch            == from_decide.batch);
}

// ---- file-count-aware hetero thread split (reclaim idle producer cores) ----

// The empirical bench10.wav sweep (C=16/RTX2080) showed cpu_asr_threads > C-P-1
// REGRESSES single-file throughput, so the few-files branch caps at C-P-gpu_feed.
// Net effect on C=16: cpu_asr_threads stays 11 for ALL file counts (the cap binds),
// and in_flight_files stays P=4 always. We assert that invariance + the cap here.
TEST_CASE("fill_hetero: file-count-aware split never exceeds the benchmarked cap") {
  HardwareInfo h = hw_hetero(16, 40000);
  const int P = 4, gpu_feed = 1;          // C=16 -> P=clamp(4,2,6)=4
  const int cap = 16 - P - gpu_feed;      // = 11, the empirical optimum
  // Sweep single-file through many-file: cpu_asr never exceeds the cap, in_flight==P.
  for (int nf : {0, 1, 2, 3, 4, 6, 8, 50}) {
    AutoTune t;
    fill_hetero(t, h, nf);
    INFO("num_files=" << nf << " cpu_asr=" << t.cpu_asr_threads
         << " in_flight=" << t.in_flight_files);
    CHECK(t.cpu_asr_threads <= cap);
    CHECK(t.cpu_asr_threads >= 2);
    CHECK(t.in_flight_files == P);
    CHECK(t.num_threads == t.cpu_asr_threads);   // legacy mirror stays in sync
  }
}

// Steady-state (num_files >= P+1) MUST preserve the hard no-oversubscription
// invariant for every C the gate admits. The single-file/few-file path is allowed
// a brief decode-phase overshoot, so the invariant is asserted for the multi-file
// case only (mirrors fill_hetero's postcondition).
TEST_CASE("fill_hetero: multi-file no-oversubscription invariant holds for all C") {
  const int gpu_feed = 1;
  for (int C : {12, 16, 24, 32}) {
    HardwareInfo h = hw_hetero(C, 40000);
    const int P = (C / 4 < 2) ? 2 : (C / 4 > 6 ? 6 : C / 4);
    AutoTune t;
    fill_hetero(t, h, P + 1);   // multi-file -> steady-state split
    INFO("C=" << C << " in_flight=" << t.in_flight_files
         << " cpu_asr=" << t.cpu_asr_threads);
    CHECK(t.in_flight_files + gpu_feed + t.cpu_asr_threads <= C);
  }
}

// For few-files the formula must NEVER sustain a 2x oversubscription, even though a
// brief decode-phase overshoot of the strict steady-state bound is permitted.
TEST_CASE("fill_hetero: few-files split never doubles core count") {
  const int gpu_feed = 1;
  for (int C : {12, 16, 24, 32}) {
    HardwareInfo h = hw_hetero(C, 40000);
    AutoTune t;
    fill_hetero(t, h, 1);       // single file = most aggressive reclaim
    INFO("C=" << C << " in_flight=" << t.in_flight_files
         << " cpu_asr=" << t.cpu_asr_threads);
    // Active producers during ASR is at most 1 (single file); the worst-case busy
    // thread count is cpu_asr + gpu_feed + 1, which must stay well under 2*C.
    CHECK(t.cpu_asr_threads + gpu_feed + 1 <= 2 * C);
    CHECK(t.cpu_asr_threads <= C - 1);   // never claims every core (GPU host needs one)
  }
}

// ---- data-parallel CPU consumers: set_qwen3_cpu_consumers ----

// Qwen3 + CPU provider -> K>1 consumers with the cores split across them.
TEST_CASE("set_qwen3_cpu_consumers: Qwen3 on CPU splits cores into K consumers") {
  EngineConfig cfg; cfg.qwen3_encoder = "encoder.int8.onnx";   // marks Qwen3 active
  AutoTune t; t.provider = Provider::Cpu; t.num_threads = 16;
  set_qwen3_cpu_consumers(t, cfg, 16);
  CHECK(t.cpu_consumers == 3);                 // 16 logical -> clamp(16/2,1,3)=3
  CHECK(t.num_threads == 5);                   // clamp(16/3,2,16)=5 per consumer
  CHECK(t.cpu_consumers * t.num_threads <= 16 + t.cpu_consumers);  // ~no oversubscription
}

// Non-Qwen3 models must NOT get extra consumers (they batch fine in sherpa).
TEST_CASE("set_qwen3_cpu_consumers: non-Qwen3 model keeps cpu_consumers=1 (no-op)") {
  EngineConfig cfg;   // qwen3_encoder empty -> not Qwen3
  AutoTune t; t.provider = Provider::Cpu; t.num_threads = 16;
  set_qwen3_cpu_consumers(t, cfg, 16);
  CHECK(t.cpu_consumers == 1);
  CHECK(t.num_threads == 16);                  // untouched
}

// Qwen3 on a non-CPU provider runs a single recognizer (int8 won't use CUDA EP anyway).
TEST_CASE("set_qwen3_cpu_consumers: Qwen3 on CUDA keeps cpu_consumers=1 (no-op)") {
  EngineConfig cfg; cfg.qwen3_encoder = "encoder.int8.onnx";
  AutoTune t; t.provider = Provider::Cuda; t.num_threads = 1;
  set_qwen3_cpu_consumers(t, cfg, 16);
  CHECK(t.cpu_consumers == 1);
  CHECK(t.num_threads == 1);                    // untouched
}

// Tiny box (few logical cores): K must fall back toward 1 so each consumer keeps >=2 threads.
TEST_CASE("set_qwen3_cpu_consumers: tiny box falls back to 1 consumer") {
  EngineConfig cfg; cfg.qwen3_encoder = "encoder.int8.onnx";
  AutoTune t; t.provider = Provider::Cpu; t.num_threads = 4;
  set_qwen3_cpu_consumers(t, cfg, 2);          // 2 logical -> clamp(1,1,3)=1
  CHECK(t.cpu_consumers == 1);
  CHECK(t.num_threads == 2);                    // clamp(2/1,2,2)=2
}

// Default decide() on a CPU box with Qwen3 cfg picks the multi-consumer split.
TEST_CASE("decide: CPU + Qwen3 cfg sets cpu_consumers>1") {
  EngineConfig cfg; cfg.qwen3_encoder = "encoder.int8.onnx";
  auto t = decide(hw(false, 0, 16, 40000), cfg);   // no GPU -> CPU path
  CHECK(t.provider == Provider::Cpu);
  CHECK(t.cpu_consumers == 3);
  CHECK(t.num_threads == 5);
}

// Default decide() on a CPU box WITHOUT Qwen3 keeps the single-consumer default.
TEST_CASE("decide: CPU without Qwen3 keeps cpu_consumers=1") {
  auto t = decide(hw(false, 0, 16, 40000), EngineConfig{});
  CHECK(t.provider == Provider::Cpu);
  CHECK(t.cpu_consumers == 1);
}

// ---- T5: decide() optional max_batch / max_threads caps ----

TEST_CASE("T5: max_batch=0 leaves batch unchanged (auto)") {
  EngineConfig cfg;
  cfg.max_batch = 0;
  // GPU + 8 cores = Cuda path, batch >= 8
  auto t = decide(hw(true, 6000, 8, 40000, true), cfg);
  CHECK(t.batch >= 8);
}

TEST_CASE("T5: max_batch caps batch") {
  EngineConfig cfg;
  cfg.max_batch = 8;
  // GPU + 8 cores = Cuda path; uncapped batch=(6000-2000)/150=26 -> capped to 8
  auto t = decide(hw(true, 6000, 8, 40000, true), cfg);
  CHECK(t.batch <= 8);
}

TEST_CASE("T5: max_batch caps gpu_batch") {
  EngineConfig cfg;
  cfg.max_batch = 9;
  auto t = decide(hw(true, 6000, 8, 40000, true), cfg);
  CHECK(t.gpu_batch <= 9);
}

TEST_CASE("T5: max_threads=0 leaves num_threads unchanged (auto)") {
  EngineConfig cfg;
  cfg.max_threads = 0;
  // CPU path with 16 cores -> num_threads >= 16
  auto t = decide(hw(false, 0, 16, 40000), cfg);
  CHECK(t.num_threads >= 16);
}

TEST_CASE("T5: max_threads caps num_threads") {
  EngineConfig cfg;
  cfg.max_threads = 6;
  // CPU path with 16 cores -> uncapped would be 16
  auto t = decide(hw(false, 0, 16, 40000), cfg);
  CHECK(t.num_threads <= 6);
}

TEST_CASE("T5: max_batch and max_threads both active") {
  EngineConfig cfg;
  cfg.max_batch = 4;
  cfg.max_threads = 4;
  auto t = decide(hw(false, 0, 16, 40000), cfg);
  CHECK(t.batch <= 4);
  CHECK(t.num_threads <= 4);
}
