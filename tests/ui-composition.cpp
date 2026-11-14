#include "NativeBuild.h"
#include <algorithm>
#include <iostream>
using namespace pvz::build;
Image Make(unsigned w,unsigned h,unsigned char alpha){Image i;i.width=i.logicalWidth=w;i.height=i.logicalHeight=h;i.rgba.assign(std::size_t(w)*h*4,alpha);return i;}
int main(){try{
 std::map<std::string,Image> images;
 Json scene={{"sprites",Json::array()}},zombatar={{"images",Json::array()}};
 std::vector<Image> originals;
 const char* labels[]={"ALMANAC_INDEXBACK","ALMANAC_PLANTBACK","ALMANAC_ZOMBIEBACK"};
 for(unsigned n=0;n<3;++n){auto image=Make(40,30,255);const auto p=(std::size_t(8+n)*40+12)*4;image.rgba[p]=50+n;image.rgba[p+3]=100+n;
  originals.push_back(image);images[labels[n]]=image;scene["sprites"].push_back({{"name",labels[n]},{"source",labels[n]}});}
 auto display=Make(800,600,0),widget=Make(560,493,255);
 display.rgba[(std::size_t(100)*800+700)*4+3]=200;
 images["DISPLAY_WINDOW"]=display;images["WIDGET_BG"]=widget;
 for(auto name:{"DISPLAY_WINDOW","WIDGET_BG"})zombatar["images"].push_back({{"name",name},{"image",name}});
 Json scenes={{"compiled/scene/almanac.compiled",scene}};std::map<std::string,std::vector<std::string>> symbols;
 CompactUi(scenes,zombatar,images,symbols);
 const auto& common=images.at("generated:almanac_common");
 for(unsigned n=0;n<3;++n){const auto& patch=images.at(labels[n]);for(unsigned y=0;y<30;++y)for(unsigned x=0;x<40;++x){
  const auto p=(std::size_t(y)*40+x)*4;const unsigned char* actual=common.rgba.data()+p;
  if(int(x)>=patch.trimX&&int(y)>=patch.trimY&&x<unsigned(patch.trimX)+patch.width&&y<unsigned(patch.trimY)+patch.height){
   const auto* candidate=patch.rgba.data()+(std::size_t(y-patch.trimY)*patch.width+x-patch.trimX)*4;
   if(candidate[3]){Require(!actual[3],"Composition pixels overlap");actual=candidate;}}
  Require(std::equal(actual,actual+4,originals[n].rgba.data()+p),"Almanac reconstruction changed pixels");}}
 const auto& baked=images.at("DISPLAY_WINDOW");
 Require(images.at("WIDGET_BG").trimWidth==0,"Baked widget was not retired");
 for(unsigned y=0;y<600;++y)for(unsigned x=0;x<800;++x){const auto p=(std::size_t(y)*800+x)*4;
  const unsigned char* expected=display.rgba.data()+p;
  if(x>=20&&x<580&&y>=25&&y<518)expected=widget.rgba.data()+(std::size_t(y-25)*560+x-20)*4;
  unsigned char empty[4]{};const unsigned char* actual=empty;
  if(int(x)>=baked.trimX&&int(y)>=baked.trimY&&x<unsigned(baked.trimX)+baked.width&&y<unsigned(baked.trimY)+baked.height)
   actual=baked.rgba.data()+(std::size_t(y-baked.trimY)*baked.width+x-baked.trimX)*4;
  Require(std::equal(actual,actual+4,expected),"Zombatar reconstruction changed pixels");}
 std::cout<<"PASS exact Almanac decomposition, semitransparent pixels, Zombatar static bake and crop offsets\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
