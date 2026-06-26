#include "core/output/writer_facade.h"
#include "core/output/srt_writer.h"
#include "core/output/vtt_writer.h"
#include "core/output/json_writer.h"
#include "core/output/md_writer.h"
#include "core/utf8_file.h"
namespace suji {
bool write_outputs(const Transcript& t, const std::string& base, const EngineConfig& cfg, const std::string& title) {
  bool ok = true;
  if (cfg.out_srt)  ok &= write_utf8_no_bom(base + ".srt",  to_srt(t, cfg));
  if (cfg.out_vtt)  ok &= write_utf8_no_bom(base + ".vtt",  to_vtt(t, cfg));
  if (cfg.out_json) ok &= write_utf8_no_bom(base + ".json", to_json(t));
  if (cfg.out_md)   ok &= write_utf8_no_bom(base + ".md",   to_markdown(t, title));
  return ok;
}
}
