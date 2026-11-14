#include "NativeBuild.h"
#include <algorithm>
#include <cstring>
namespace pvz::build {
namespace {
void Trim(Image& im) {
    const auto oldW=im.width,oldH=im.height;
    unsigned x0=oldW,y0=oldH,x1=0,y1=0;
    for(unsigned y=0;y<oldH;++y)for(unsigned x=0;x<oldW;++x)
        if(im.rgba[(std::size_t(y)*oldW+x)*4+3]){x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x+1);y1=std::max(y1,y+1);}
    if(x1==0){x0=y0=0;x1=y1=1;}
    // Keep transparent guard texels for identical linear-filtered edges.
    x0=x0>2?x0-2:0;y0=y0>2?y0-2:0;x1=std::min(x1+2,oldW);y1=std::min(y1+2,oldH);
    const unsigned w=x1-x0,h=y1-y0;Bytes pixels(std::size_t(w)*h*4);
    for(unsigned y=0;y<h;++y)std::memcpy(pixels.data()+std::size_t(y)*w*4,im.rgba.data()+(std::size_t(y+y0)*oldW+x0)*4,w*4);
    Require(oldW%im.logicalWidth==0 && oldH%im.logicalHeight==0,"Compact UI needs integral source scale");
    const auto sx=oldW/im.logicalWidth,sy=oldH/im.logicalHeight;
    Require(x0%sx==0&&y0%sy==0&&w%sx==0&&h%sy==0,"UI crop must align with logical pixels");
    im.compacted=true;im.trimWidth=int(w/sx);im.trimHeight=int(h/sy);
    im.trimX=int(x0/sx);im.trimY=int(y0/sy);im.width=w;im.height=h;im.rgba=std::move(pixels);
}
}
void CompactUi(Json& scenes,const Json& zombatar,std::map<std::string,Image>& images,
    std::map<std::string,std::vector<std::string>>& symbols) {
    auto& scene=scenes.at("compiled/scene/almanac.compiled");
    constexpr const char* names[]={"ALMANAC_INDEXBACK","ALMANAC_PLANTBACK","ALMANAC_ZOMBIEBACK"};
    std::vector<Image*> backs;std::vector<std::string> keys;
    for(auto name:names)for(auto& sprite:scene["sprites"])if(sprite["name"]==name){const auto key=sprite["source"].get<std::string>();keys.push_back(key);backs.push_back(&images.at(key));}
    Require(backs.size()==3,"Missing Almanac background composition inputs");
    Image common=*backs[0];
    for(auto* image:backs)Require(image->width==common.width&&image->height==common.height,"Almanac composition size mismatch");
    for(std::size_t i=0;i<common.rgba.size();i+=4) {
        const bool same=std::memcmp(backs[0]->rgba.data()+i,backs[1]->rgba.data()+i,4)==0&&std::memcmp(backs[0]->rgba.data()+i,backs[2]->rgba.data()+i,4)==0;
        if(same)for(auto* image:backs)std::fill_n(image->rgba.data()+i,4,0);
        else std::fill_n(common.rgba.data()+i,4,0);
    }
    // The common and page-specific pixels are disjoint, so recomposition
    // preserves RGBA, including semitransparent frame corners, exactly.
    const std::string commonKey="generated:almanac_common";images[commonKey]=std::move(common);
    scene["sprites"].push_back({{"name","ALMANAC_COMMON_BACK"},{"source",commonKey},{"image",commonKey},{"scale",1.0}});
    symbols["compiled/scene/almanac.compiled"].push_back(commonKey);
    for(auto& key:keys)Trim(images.at(key));
    // Widget chrome and preview frame occupy disjoint areas in the same
    // parent canvas. Bake once offline; dynamic portraits/buttons stay above.
    // DISPLAY_WINDOW is drawn at (5,0); child widget starts at (25,25).
    std::string displayKey,widgetKey;
    for(const auto& row:zombatar["images"]) {
        if(row["name"]=="DISPLAY_WINDOW")displayKey=row["image"].get<std::string>();
        if(row["name"]=="WIDGET_BG")widgetKey=row["image"].get<std::string>();
    }
    auto& display=images.at(displayKey);auto& widget=images.at(widgetKey);
    Require(display.width==800&&display.height==600&&widget.width==560&&widget.height==493,
        "Zombatar chrome bake expects original logical geometry");
    display.files.insert(widget.files.begin(),widget.files.end());
    for(unsigned y=0;y<widget.height;++y)for(unsigned x=0;x<widget.width;++x){
        const auto from=(std::size_t(y)*widget.width+x)*4;
        const auto to=(std::size_t(y+25)*display.width+x+20)*4;
        Require(display.rgba[to+3]==0||widget.rgba[from+3]==0,"Zombatar baked layers overlap");
        if(widget.rgba[from+3])std::memcpy(display.rgba.data()+to,widget.rgba.data()+from,4);
    }
    widget.rgba.assign(4,0);widget.width=widget.height=1;
    widget.compacted=true;widget.trimWidth=widget.trimHeight=0;
    for(const auto& row:zombatar["images"]) {
        const auto key=row["image"].get<std::string>();
        // Only the UI sheets are compacted. Portrait components retain their
        // authored bounds and attachment geometry.
        const auto name=row["name"].get<std::string>();
        if(name=="DISPLAY_WINDOW"||name=="LOGO"||name.find("BUTTON")!=std::string::npos||name.ends_with("_HIGHLIGHT"))Trim(images.at(key));
    }
}
}
