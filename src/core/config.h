#pragma once
#include <string>
namespace suji {
enum class Provider { Cpu, Cuda, Hetero };
inline const char* provider_str(Provider p){
  switch(p){
    case Provider::Cuda:   return "cuda";
    case Provider::Hetero: return "hetero";
    default:               return "cpu";
  }
}
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
  // GPU helper – if set, AddDllDirectory is called before CUDA init (Windows only)
  std::string cuda_dll_dir;
  // 输出开关
  bool out_srt=true, out_vtt=true, out_json=true, out_md=true;
};
}
