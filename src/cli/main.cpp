#include "core/config.h"
#include "core/paths.h"
#include "core/pipeline.h"
#include "core/output/writer_facade.h"
#include "core/log.h"
#include <filesystem>
#include <string>
#include <cstdio>
using namespace suji;
static std::string stem(const std::string& p){
  size_t a=p.find_last_of("/\\"); size_t b=p.find_last_of('.');
  size_t s=(a==std::string::npos)?0:a+1;
  return (b==std::string::npos||b<s)?p.substr(s):p.substr(s,b-s);
}
int main(int argc, char** argv){
  if (argc < 2){ std::puts("usage: suji_cli <input> [-o out_dir] [--provider cpu|cuda|hetero] [--rule-fsts f.fst] [--no-srt|--no-vtt|--no-json|--no-md]"); return 2; }
  EngineConfig c;
  std::string md = models_dir(), m = md+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.ffmpeg_path=ffmpeg_path(); c.asr_model=m+"model.int8.onnx"; c.tokens=m+"tokens.txt";
  c.vad_model=md+"/silero_vad.onnx";
  c.punct_model=md+"/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
  std::string input=argv[1], out_dir=".";
  for (int i=2;i<argc;++i){ std::string a=argv[i];
    if (a=="-o"){
      if (i+1>=argc){ log_err("'-o' requires an argument"); return 2; }
      out_dir=argv[++i];
    } else if (a=="--provider"){
      if (i+1>=argc){ log_err("'--provider' requires an argument"); return 2; }
      std::string prov=argv[++i];
      if (prov=="cuda") c.provider=Provider::Cuda;
      else if (prov=="cpu") c.provider=Provider::Cpu;
      else if (prov=="hetero"){ log_info("hetero has no effect on a single file; using CPU"); c.provider=Provider::Cpu; }
      else { log_err("unknown provider '"+prov+"' (use cpu|cuda|hetero)"); return 2; }
    } else if (a=="--rule-fsts"){
      if (i+1>=argc){ log_err("'--rule-fsts' requires an argument"); return 2; }
      c.rule_fsts=argv[++i];
    } else if (a=="--no-srt") c.out_srt=false;
    else if (a=="--no-vtt") c.out_vtt=false;
    else if (a=="--no-json") c.out_json=false;
    else if (a=="--no-md") c.out_md=false;
    else { log_err(std::string("unknown argument '")+a+"'"); return 2; }
  }
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec && !std::filesystem::exists(out_dir)) { log_err("cannot create output dir '"+out_dir+"': "+ec.message()); return 1; }
  Transcript t; std::string err;
  log_info("transcribing: " + input);
  if (!transcribe_file(c, input, t, err)){ log_err("failed: "+err); return 1; }
  std::string base = out_dir + "/" + stem(input);
  if (!write_outputs(t, base, c, stem(input))){ log_err("write failed"); return 1; }
  log_info("done: "+base+".{srt,vtt,json,md}  segments="+std::to_string(t.segments.size()));
  return 0;
}
