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
