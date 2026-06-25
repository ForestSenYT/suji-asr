# 定位 Phase 0 已下载的 vendor SDK / ffmpeg / models。允许命令行 -D 覆盖。
set(SUJI_VENDOR_DIR "${CMAKE_SOURCE_DIR}/vendor/sherpa-onnx-v1.13.3-cuda-12.x-cudnn-9.x-win-x64-cuda" CACHE PATH "sherpa-onnx prebuilt SDK dir")
set(SUJI_MODELS_DIR "${CMAKE_SOURCE_DIR}/models" CACHE PATH "models dir")
set(SUJI_FFMPEG "${CMAKE_SOURCE_DIR}/vendor/ffmpeg-master-latest-win64-lgpl/bin/ffmpeg.exe" CACHE FILEPATH "ffmpeg.exe")

if(NOT EXISTS "${SUJI_VENDOR_DIR}/include/sherpa-onnx/c-api/c-api.h")
  message(FATAL_ERROR "sherpa-onnx SDK not found at ${SUJI_VENDOR_DIR} (run Phase 0 downloads)")
endif()

if(NOT EXISTS "${SUJI_VENDOR_DIR}/lib/sherpa-onnx-c-api.lib")
  message(FATAL_ERROR "sherpa-onnx import lib not found at ${SUJI_VENDOR_DIR}/lib/sherpa-onnx-c-api.lib (run Phase 0 downloads/extract)")
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
