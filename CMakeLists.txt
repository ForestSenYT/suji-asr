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
