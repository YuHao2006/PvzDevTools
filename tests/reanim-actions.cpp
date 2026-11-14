#include "reanim/Pak64.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
namespace p=openpvz::reanim::pak64;
void Check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
std::vector<p::RawReanim> Parse(const std::string& source){std::vector<p::RawReanim> r;std::string error;Check(p::ParseModules(source,r,&error),error);return r;}
std::string Read(const std::filesystem::path& file){std::ifstream in(file,std::ios::binary);Check(bool(in),file.string());std::ostringstream s;s<<in.rdbuf();return s.str();}
int main(int argc,char**argv){try {
 const std::string head="export component Test { fps:12; frame-count:4; ";
 auto r=Parse(head+"Actions { anim_idle { first-frame:1; frame-count:2; } } }");
 Check(r[0].actions.size()==1&&r[0].tracks[0].actionMarkerOnly,"Action lowering missing");
 Check(r[0].tracks[0].frames[0].cel==-1&&r[0].tracks[0].frames[1].cel==0&&r[0].tracks[0].frames[3].cel==-1,"Action interval differs");
 auto mixed=Parse(head+R"(Actions { anim_idle { first-frame:1; frame-count:2; } } Track { source-name:"anim_idle"; frame-count:4; frame 0 { cel:0; image:"test"; } } Track { source-name:"anim_idle"; frame-count:4; frame 0 { x:9; } } })");
 auto again=Parse(p::ToDsl(mixed[0]));Check(again[0].actions.size()==1&&again[0].tracks.size()==2,"DSL roundtrip loses actions or duplicate track order");
 Check(mixed[0].tracks.size()==2&&!mixed[0].tracks[0].actionMarkerOnly&&mixed[0].tracks[0].frames[0].image=="test"&&mixed[0].tracks[1].frames[0].values[0]==9,"Mixed action/anchor or duplicate first-match changed");
 for(const std::string fields:{"first-frame:3; frame-count:2;","first-frame:0; frame-count:0;","first-frame:0; frame-count:1; frame-count:2;","first-frame:-1; frame-count:1;"}){
  std::string error;std::vector<p::RawReanim> output;Check(!p::ParseModules(head+"Actions { bad { "+fields+" } } }",output,&error),"Invalid interval accepted");
 }
 std::size_t definitions=0,actions=0,removed=0,frames=0;
 if(argc==3)for(const auto& file:std::filesystem::directory_iterator(argv[1]))if(file.path().extension()==".dsl"){
  auto old=Parse(Read(file.path()));auto now=Parse(Read(std::filesystem::path(argv[2])/file.path().filename()));
  Check(old.size()==1&&now.size()==1,"Unexpected corpus module count");auto&a=old[0];auto&b=now[0];
  Check(a.fps==b.fps&&a.tracks.size()==b.tracks.size(),file.path().string()+": legacy timeline or track count changed");
  for(std::size_t i=0;i<a.tracks.size();++i){const auto&x=a.tracks[i];const auto&y=b.tracks[i];Check(x.name==y.name&&x.frames.size()==y.frames.size(),a.name+": legacy order changed");
   if(y.actionMarkerOnly){++removed;frames+=x.frames.size();for(const auto&v:x.frames)Check(v.image.empty()&&v.font.empty()&&v.text.empty(),a.name+": drawing track removed");}
   else for(std::size_t f=0;f<x.frames.size();++f){const auto&v=x.frames[f];const auto&w=y.frames[f];Check(std::memcmp(v.values,w.values,sizeof(v.values))==0&&v.cel==w.cel&&v.image==w.image&&v.font==w.font&&v.text==w.text&&v.textSourceFrame==w.textSourceFrame,a.name+": retained transform changed");}
  }
  for(const auto&action:b.actions){auto t=std::find_if(a.tracks.begin(),a.tracks.end(),[&](const auto&t){return t.name==action.name;});Check(t!=a.tracks.end(),a.name+": new action missing old marker");int first=0,last=-1,cel=0;for(int f=0;f<int(t->frames.size());++f){if(t->frames[f].cel!=-10000)cel=t->frames[f].cel;if(cel>=0){if(last<0)first=f;last=f;}}Check(action.firstFrame==unsigned(first)&&action.frameCount==unsigned(last<0?1:last-first+1),a.name+": action range changed");++actions;}
  ++definitions;
 }
 std::cout<<"PASS action grammar, mixed anchors, duplicates, bounds; corpus definitions="<<definitions<<" actions="<<actions<<" removed="<<removed<<" removedFrames="<<frames<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
