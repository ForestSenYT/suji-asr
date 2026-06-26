#include "doctest/doctest.h"
#include "core/hardware.h"
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
TEST_CASE("autotune picks GPU when enough VRAM") {
  auto t = decide(hw(true, 6000, 16, 40000, true), EngineConfig{});
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
  auto t = decide(hw(true, 6000, 16, 40000, true), EngineConfig{});
  CHECK(t.in_flight_files >= 2);
  CHECK(t.in_flight_files <= 8);
}
