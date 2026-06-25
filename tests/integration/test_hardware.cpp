#include "doctest/doctest.h"
#include "core/hardware.h"
using namespace suji;
TEST_CASE("probe returns sane values on this box" * doctest::timeout(30)){
  auto h = probe_hardware();
  CHECK(h.cpu_threads >= 1);
  CHECK(h.ram_total_mb > 1000);
  // this dev box has an RTX 2080; if nvidia-smi present, gpu fields populated. Don't hard-require GPU.
  if(h.has_cuda_gpu){ CHECK(h.gpu_total_mb > 1000); CHECK(h.gpu_name.size() > 0); }
}
