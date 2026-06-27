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
  // P1: FireRedASR v1 AED (attention encoder-decoder). If BOTH non-empty,
  // asr.cpp uses the fire_red_asr (encoder+decoder) config and ignores asr_model;
  // otherwise it falls back to the fire_red_asr_ctc (single model) path.
  std::string asr_encoder;     // AED encoder.onnx (e.g. encoder.fp16.onnx)
  std::string asr_decoder;     // AED decoder.onnx (e.g. decoder.fp16.onnx)
  // Qwen3-ASR (sherpa model_type="qwen3_asr"). If qwen3_encoder AND qwen3_decoder
  // are both non-empty, asr.cpp uses the qwen3_asr config with PRECEDENCE over the
  // AED/CTC paths. Tokenizer is a DIRECTORY (containing vocab.json), not tokens.txt.
  std::string qwen3_conv_frontend; // conv-frontend.onnx
  std::string qwen3_encoder;       // encoder(.int8).onnx
  std::string qwen3_decoder;       // decoder(.int8).onnx (with KV cache)
  std::string qwen3_tokenizer;     // tokenizer DIR containing vocab.json
  std::string tokens;          // tokens.txt
  std::string vad_model;       // silero_vad.onnx
  std::string punct_model;     // CT punct model.int8.onnx
  std::string rule_fsts;       // 可选 ITN fst(空=关)
  std::string rule_fars;       // 可选 ITN far archive (空=关)
  // Qwen3-ASR hotwords (proper-noun/domain biasing). PATH to a hotwords file
  // (one term per line; '#' comment lines and blanks ignored). asr.cpp reads the
  // file and passes the terms as a comma-separated UTF-8 string to the Qwen3
  // model config's `hotwords` field. Empty = no biasing. Qwen3-only (the global
  // hotwords_file/hotwords_score path is for transducer/CTC, not the LLM decoder).
  std::string hotwords;
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
  // T3: ASR 解码方法(默认 greedy_search)
  std::string decoding_method = "greedy_search";
  // G10: 标点模型 provider / 线程数
  std::string punct_provider = "cpu";
  int punct_threads = 1;
  // T5: Auto-tuner caps (0 = uncapped/auto)
  int max_batch = 0;
  int max_threads = 0;
  // G5: SRT/VTT 行宽(0=不换行; >0=按 UTF-8 码点数断行)
  int srt_max_chars_per_line = 0;
};
}
