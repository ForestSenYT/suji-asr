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

# 把模型/ffmpeg/cuda-dll 路径作为编译期默认(测试/CLI 用)
target_compile_definitions(suji_core PUBLIC
  SUJI_DEFAULT_MODELS_DIR="${SUJI_MODELS_DIR}"
  SUJI_DEFAULT_FFMPEG="${SUJI_FFMPEG}"
  SUJI_DEFAULT_CUDA_DLL_DIR="${CMAKE_SOURCE_DIR}/vendor/cuda-redist/dll")

# CLI
add_executable(suji_cli src/cli/main.cpp)
target_link_libraries(suji_cli PRIVATE suji_core)
suji_copy_runtime_dlls(suji_cli)

# Batch CLI
add_executable(suji_batch src/cli/batch_main.cpp)
target_link_libraries(suji_batch PRIVATE suji_core)
suji_copy_runtime_dlls(suji_batch)

# 测试
enable_testing()
file(GLOB_RECURSE SUJI_TEST_SRC CONFIGURE_DEPENDS tests/*.cpp)
add_executable(suji_tests ${SUJI_TEST_SRC})
target_link_libraries(suji_tests PRIVATE suji_core)
suji_copy_runtime_dlls(suji_tests)
add_test(NAME suji_tests COMMAND suji_tests)

# GUI (Qt6) — optional; skipped gracefully when Qt6 is not found
find_package(Qt6 COMPONENTS Widgets QUIET)
if(Qt6_FOUND)
  qt_standard_project_setup()
  qt_add_executable(suji_gui
    src/gui/main.cpp
    src/gui/main_window.cpp
    src/gui/main_window.h
    src/gui/engine_worker.cpp
    src/gui/engine_worker.h
  )
  target_link_libraries(suji_gui PRIVATE suji_core Qt6::Widgets)
  target_include_directories(suji_gui PRIVATE src)
  suji_copy_runtime_dlls(suji_gui)
  # STARTUP (STEP 8): delay-load sherpa (and transitively onnxruntime, ~14MB) off the
  # pre-main loader map so the GUI window paints before the heavy DLL is mapped. The fault
  # now surfaces at first transcribe on the worker thread (EngineWorker::run), which has
  # crash-safe fallbacks + a log sink. SCOPED TO suji_gui ONLY — suji_cli/suji_batch/
  # suji_tests call sherpa immediately and must NOT be delay-loaded. Do NOT delay-load Qt.
  if(MSVC)
    target_link_options(suji_gui PRIVATE /DELAYLOAD:sherpa-onnx-c-api.dll)
    target_link_libraries(suji_gui PRIVATE delayimp)
  endif()
  message(STATUS "Qt6 found -> building suji_gui")
else()
  message(STATUS "Qt6 NOT found -> skipping suji_gui (configure with -DCMAKE_PREFIX_PATH=F:/Qt/6.8.1/msvc2022_64)")
endif()
