#include "core/output/json_writer.h"
#include <cstdio>
namespace suji {
std::string json_escape(const std::string& s){
  std::string o; o.reserve(s.size()+8);
  for(unsigned char c : s){
    switch(c){
      case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
      default:
        if(c < 0x20){ char b[8]; std::snprintf(b,sizeof(b),"\\u%04x",c); o += b; }
        else o += (char)c; // UTF-8 字节原样
    }
  }
  return o;
}
static std::string num(double d){ char b[32]; std::snprintf(b,sizeof(b),"%.3f",d); return b; }
std::string to_json(const Transcript& t){
  std::string o = "{\"full_text\":\"" + json_escape(t.full_text) + "\",\"segments\":[";
  for(size_t i=0;i<t.segments.size();++i){
    const auto& s = t.segments[i];
    o += "{\"start\":"+num(s.start)+",\"end\":"+num(s.end)+",\"text\":\""+json_escape(s.text)+"\",\"tokens\":[";
    for(size_t k=0;k<s.tokens.size();++k){
      o += "{\"t\":\""+json_escape(s.tokens[k].text)+"\",\"start\":"+num(s.tokens[k].start)+"}";
      if(k+1<s.tokens.size()) o += ",";
    }
    o += "]}"; if(i+1<t.segments.size()) o += ",";
  }
  o += "]}";
  return o;
}
}
