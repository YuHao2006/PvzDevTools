#include "reanim/AtlasPacking.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace openpvz::reanim::atlas {
namespace {
constexpr unsigned Limit = 16384;
struct Rect { unsigned x, y, w, h; };
struct Layout {
    unsigned width = 0, height = 0;
    std::vector<Rect> rectangles;
    std::uint64_t Area() const { return std::uint64_t(width) * height; }
};
bool Better(const Layout& a, const Layout& b)
{
    return std::tuple(a.Area(), std::max(a.width,a.height), a.width, a.height) <
           std::tuple(b.Area(), std::max(b.width,b.height), b.width, b.height);
}
bool Contains(const Rect& a, const Rect& b)
{
    return b.x >= a.x && b.y >= a.y && b.x+b.w <= a.x+a.w && b.y+b.h <= a.y+a.h;
}
bool Intersects(const Rect& a, const Rect& b)
{
    return a.x < b.x+b.w && b.x < a.x+a.w && a.y < b.y+b.h && b.y < a.y+a.h;
}
std::vector<unsigned> Order(const std::vector<Rect>& sizes, unsigned mode)
{
    std::vector<unsigned> order(sizes.size());
    std::iota(order.begin(),order.end(),0);
    std::stable_sort(order.begin(),order.end(),[&](unsigned ai,unsigned bi) {
        const auto& a=sizes[ai]; const auto& b=sizes[bi];
        if(mode==0) return std::tuple(a.h,a.w) > std::tuple(b.h,b.w);
        if(mode==1) return std::tuple(std::uint64_t(a.w)*a.h,std::max(a.w,a.h),a.h) >
                           std::tuple(std::uint64_t(b.w)*b.h,std::max(b.w,b.h),b.h);
        if(mode==2) return std::tuple(std::max(a.w,a.h),std::min(a.w,a.h),a.h) >
                          std::tuple(std::max(b.w,b.h),std::min(b.w,b.h),b.h);
        if(mode==3) return std::tuple(a.w,a.h) > std::tuple(b.w,b.h);
        // Integer aspect comparison avoids floating point tie differences.
        const auto aspectA=std::uint64_t(a.w)*b.h,aspectB=std::uint64_t(b.w)*a.h;
        if(aspectA!=aspectB) return aspectA>aspectB;
        return std::tuple(a.w,a.h)>std::tuple(b.w,b.h);
    });
    return order;
}
Layout Shelf(const std::vector<Rect>& sizes)
{
    unsigned minimum=0;
    std::uint64_t area=0;
    for(const auto& r:sizes) { minimum=std::max(minimum,r.w); area+=std::uint64_t(r.w)*r.h; }
    std::set<unsigned> widths{minimum,std::max(minimum,unsigned(std::ceil(std::sqrt(double(area)))))};
    for(unsigned power=5;power<15;++power) if((1u<<power)>=minimum) widths.insert(1u<<power);
    Layout best; best.width=best.height=Limit+1;
    const auto order=Order(sizes,0);
    for(unsigned width:widths) {
        if(width>Limit) continue;
        Layout candidate; candidate.rectangles.resize(sizes.size());
        unsigned x=0,y=0,shelf=0;
        for(unsigned index:order) {
            const auto& r=sizes[index];
            if(x+r.w>width) { y+=shelf; x=shelf=0; }
            candidate.rectangles[index]={x,y,r.w,r.h};
            x+=r.w; shelf=std::max(shelf,r.h);
            candidate.width=std::max(candidate.width,x);
        }
        candidate.height=y+shelf;
        if(candidate.height<=Limit && Better(candidate,best)) best=std::move(candidate);
    }
    return best;
}
bool MaxRects(const std::vector<Rect>& sizes,const std::vector<unsigned>& order,
              unsigned width,unsigned height,unsigned heuristic,Layout& result,bool dynamic=false)
{
    std::vector<Rect> free{{0,0,width,height}};
    std::vector<bool> used(sizes.size(),false);
    result={}; result.rectangles.resize(sizes.size());
    for(unsigned step=0;step<sizes.size();++step) {
        std::tuple<std::uint64_t,unsigned,unsigned,unsigned,std::uint64_t,unsigned,unsigned,unsigned> score{
            std::numeric_limits<std::uint64_t>::max(),0,0,0,0,0,0,0};
        Rect placed{};
        unsigned selected=0;
        // Global mode scores every remaining rectangle against every free
        // space. Input order has no placement priority; index is only the last
        // tie after all geometry, and the output remains in original order.
        for(unsigned pool=0;pool<(dynamic ? sizes.size() : 1u);++pool) {
          const unsigned index=dynamic ? pool : order[step];
          if(used[index]) continue;
          const auto& size=sizes[index];
        for(const auto& space:free) {
            if(size.w>space.w || size.h>space.h) continue;
            const unsigned dx=space.w-size.w,dy=space.h-size.h;
            // Contact placement also considers the opposite free-rectangle
            // edges; this can close vertical holes without opening new strips.
            for(unsigned corner=0;corner<(heuristic==3 ? 4u : 1u);++corner) {
              const Rect candidate{space.x+(corner&1 ? dx : 0),
                                   space.y+(corner&2 ? dy : 0),size.w,size.h};
              unsigned contact=0;
              if(heuristic==3) {
                if(candidate.x==0 || candidate.x+candidate.w==width) contact+=candidate.h;
                if(candidate.y==0 || candidate.y+candidate.h==height) contact+=candidate.w;
                const auto overlap=[](unsigned a,unsigned lengthA,unsigned b,unsigned lengthB) {
                    const auto low=std::max(a,b),high=std::min(a+lengthA,b+lengthB);
                    return high>low ? high-low : 0u;
                };
                for(const auto& old:result.rectangles) {
                    if(!old.w) continue;
                    if(old.x+old.w==candidate.x || candidate.x+candidate.w==old.x)
                        contact+=overlap(old.y,old.h,candidate.y,candidate.h);
                    if(old.y+old.h==candidate.y || candidate.y+candidate.h==old.y)
                        contact+=overlap(old.x,old.w,candidate.x,candidate.w);
                }
              }
              const std::uint64_t primary=heuristic==0 ? std::min(dx,dy) :
                heuristic==1 ? std::uint64_t(space.w)*space.h-std::uint64_t(size.w)*size.h :
                heuristic==2 ? candidate.y+size.h : 2*(width+height)-contact;
              const unsigned secondary=heuristic==0 ? std::max(dx,dy) :
                heuristic==1 || heuristic==3 ? std::min(dx,dy) : candidate.x;
              const auto value=std::tuple(primary,secondary,candidate.y,candidate.x,
                  std::numeric_limits<std::uint64_t>::max()-std::uint64_t(size.w)*size.h,
                  Limit-size.w,Limit-size.h,index);
              if(value<score) { score=value; placed=candidate;selected=index; }
            }
        }
        }
        if(placed.w==0) return false;
        used[selected]=true;
        result.rectangles[selected]=placed;
        result.width=std::max(result.width,placed.x+placed.w);
        result.height=std::max(result.height,placed.y+placed.h);
        std::vector<Rect> next;
        next.reserve(free.size()+4);
        for(const auto& space:free) {
            if(!Intersects(space,placed)) { next.push_back(space); continue; }
            if(placed.x>space.x) next.push_back({space.x,space.y,placed.x-space.x,space.h});
            if(placed.x+placed.w<space.x+space.w)
                next.push_back({placed.x+placed.w,space.y,space.x+space.w-placed.x-placed.w,space.h});
            if(placed.y>space.y) next.push_back({space.x,space.y,space.w,placed.y-space.y});
            if(placed.y+placed.h<space.y+space.h)
                next.push_back({space.x,placed.y+placed.h,space.w,space.y+space.h-placed.y-placed.h});
        }
        // Maximal free rectangles overlap; contained regions add no placements.
        for(std::size_t a=0;a<next.size();) {
            bool contained=false;
            for(std::size_t b=0;b<next.size();++b)
                if(a!=b && Contains(next[b],next[a]) &&
                   (!Contains(next[a],next[b]) || b<a)) { contained=true; break; }
            if(contained) next.erase(next.begin()+a); else ++a;
        }
        free=std::move(next);
    }
    return true;
}
Layout OptimizeDirection(const std::vector<Rect>& sizes,unsigned& attempts)
{
    if(sizes.empty()) return {};
    Layout best=Shelf(sizes);
    if(best.height>Limit) { best={};best.width=best.height=Limit; }
    if(sizes.size()==1) return best;
    unsigned maxW=0,maxH=0;
    std::uint64_t area=0;
    for(const auto& r:sizes) { maxW=std::max(maxW,r.w);maxH=std::max(maxH,r.h);area+=std::uint64_t(r.w)*r.h; }
    const double square=std::sqrt(double(area));
    std::set<unsigned> widthSet{maxW,best.width};
    for(double factor:{0.50,0.60,0.70,0.80,0.90,1.0,1.10,1.25,1.40,1.60,1.85,2.20,2.70})
        widthSet.insert(std::max(maxW,unsigned(std::ceil(square*factor))));
    // Preserve useful exact strip boundaries, not just powers of two.
    auto byWidth=Order(sizes,1);
    unsigned sum=0;
    for(unsigned i:byWidth) { sum+=sizes[i].w; if(sum<=Limit) widthSet.insert(sum); if(widthSet.size()>=28) break; }
    std::vector<unsigned> widths(widthSet.begin(),widthSet.end());
    std::stable_sort(widths.begin(),widths.end(),[&](unsigned a,unsigned b) {
        return std::tuple(std::abs(double(a)-square),a) < std::tuple(std::abs(double(b)-square),b);
    });
    std::vector<std::vector<unsigned>> orders;
    for(unsigned mode=0;mode<5;++mode) orders.push_back(Order(sizes,mode));
    const std::vector<std::pair<unsigned,unsigned>> originalStrategies{
        {0,0},{1,0},{2,0},{0,1},{1,1},{2,1},{0,2},{1,2},{2,2}};
    const std::vector<std::pair<unsigned,unsigned>> gapStrategies{
        {3,0},{4,0},{3,1},{4,1},{0,3},{1,3},{2,3},{3,3},{4,3}};
    const auto searchWidths=[&](const std::vector<unsigned>& candidates,unsigned budget,
                               const auto& strategies) {
      for(unsigned width:candidates) {
        if(width>Limit) continue;
        const unsigned floor=std::max(maxH,unsigned((area+width-1)/width));
        for(const auto& [orderIndex,heuristic]:strategies) {
            if(attempts>=budget) break;
            const auto& order=orders[orderIndex];
            unsigned low=floor,high=unsigned(std::min<std::uint64_t>(Limit,best.Area()/width));
            if(low>high) continue;
            Layout candidate;
            ++attempts;
            if(!MaxRects(sizes,order,width,high,heuristic,candidate)) continue;
            if(best.rectangles.empty() || Better(candidate,best)) best=candidate;
            // Bounded binary height search. Packing heuristics are not monotone,
            // so every successful candidate remains eligible independently.
            for(unsigned step=0;low<high && step<14 && attempts<budget;++step) {
                const unsigned middle=low+(high-low)/2;
                ++attempts;
                if(MaxRects(sizes,order,width,middle,heuristic,candidate)) {
                    if(best.rectangles.empty() || Better(candidate,best)) best=candidate;
                    high=middle;
                } else low=middle+1;
            }
        }
      }
    };
    searchWidths(widths,2500,originalStrategies);
    // Exact pixel widths around the coarse winner remove shelf/ratio rounding
    // gaps. A fixed work budget keeps output reproducible across machines.
    std::set<unsigned> refined;
    for(int delta:{-64,-32,-16,-8,-4,-2,-1,0,1,2,4,8,16,32,64}) {
        const int width=int(best.width)+delta;
        if(width>=int(maxW) && width<=int(Limit)) refined.insert(unsigned(width));
    }
    std::vector<unsigned> nearby(refined.begin(),refined.end());
    std::stable_sort(nearby.begin(),nearby.end(),[&](unsigned a,unsigned b) {
        return std::tuple(std::abs(int(a)-int(best.width)),a) <
               std::tuple(std::abs(int(b)-int(best.width)),b);
    });
    searchWidths(nearby,5000,originalStrategies);
    // Keep every previous normal-search candidate, then spend additional
    // compiler work on width/aspect orders and four-corner contact filling.
    std::vector<unsigned> gapWidths=nearby;
    gapWidths.insert(gapWidths.end(),widths.begin(),widths.end());
    searchWidths(gapWidths,8000,gapStrategies);
    // A separate batch-global search does not commit to a sorted insertion
    // order. Keep it bounded independently so the previous 8,000 candidates
    // remain available and large corpora spend compiler time predictably.
    std::vector<unsigned> globalWidths{best.width};
    for(int delta:{-8,8}) {
        const int width=int(best.width)+delta;
        if(width>=int(maxW) && width<=int(Limit)) globalWidths.push_back(unsigned(width));
    }
    const unsigned squareWidth=std::max(maxW,unsigned(std::ceil(square)));
    if(squareWidth<=Limit && std::find(globalWidths.begin(),globalWidths.end(),squareWidth)==globalWidths.end())
        globalWidths.push_back(squareWidth);
    unsigned globalAttempts=0;
    for(unsigned width:globalWidths) {
        for(unsigned heuristic=0;heuristic<4 && globalAttempts<144;++heuristic) {
            unsigned low=std::max(maxH,unsigned((area+width-1)/width));
            unsigned high=unsigned(std::min<std::uint64_t>(Limit,best.Area()/width));
            if(low>high) continue;
            Layout candidate;
            ++globalAttempts;
            if(!MaxRects(sizes,orders[0],width,high,heuristic,candidate,true)) continue;
            if(best.rectangles.empty() || Better(candidate,best)) best=candidate;
            for(unsigned step=0;low<high && step<8 && globalAttempts<144;++step) {
                const unsigned middle=low+(high-low)/2;
                ++globalAttempts;
                if(MaxRects(sizes,orders[0],width,middle,heuristic,candidate,true)) {
                    if(best.rectangles.empty() || Better(candidate,best)) best=candidate;
                    high=middle;
                } else low=middle+1;
            }
        }
    }
    attempts+=globalAttempts;
    return best;
}
Layout Optimize(const std::vector<Rect>& sizes,unsigned& attempts,std::uint64_t& baseline)
{
    if(sizes.empty()) { baseline=0; return {}; }
    const auto shelf=Shelf(sizes);
    baseline=shelf.height<=Limit ? shelf.Area() : 0;
    unsigned normalAttempts=0,verticalAttempts=0;
    auto best=OptimizeDirection(sizes,normalAttempts);
    std::vector<Rect> transposed=sizes;
    for(auto& r:transposed) std::swap(r.w,r.h);
    auto vertical=OptimizeDirection(transposed,verticalAttempts);
    std::swap(vertical.width,vertical.height);
    for(auto& r:vertical.rectangles) { std::swap(r.x,r.y);std::swap(r.w,r.h); }
    attempts=normalAttempts+verticalAttempts;
    // A failed direction carries no placements and cannot suppress the other
    // direction. Virtual transposition never rotates a source image.
    if(best.rectangles.size()!=sizes.size() ||
       (vertical.rectangles.size()==sizes.size() && Better(vertical,best))) best=std::move(vertical);
    if(best.rectangles.size()!=sizes.size()) throw std::runtime_error("atlas exceeds dimension limit");
    return best;
}
std::uint32_t Read(std::istream& stream)
{
    unsigned char bytes[4];
    if(!stream.read(reinterpret_cast<char*>(bytes),4)) throw std::runtime_error("truncated atlas batch");
    return std::uint32_t(bytes[0]) | std::uint32_t(bytes[1])<<8 |
           std::uint32_t(bytes[2])<<16 | std::uint32_t(bytes[3])<<24;
}
void Write(std::ostream& stream,std::uint64_t value,unsigned bytes=4)
{
    for(unsigned i=0;i<bytes;++i) stream.put(char(value>>(8*i)));
}
}
int RunPackAtlasLayout(const std::vector<std::string>& args)
{
    if(args.size()!=5 || args[1]!="--input" || args[3]!="--output")
        throw std::runtime_error("pack-atlas-layout requires --input BATCH --output LAYOUT");
    std::ifstream input(args[2],std::ios::binary);
    if(!input) throw std::runtime_error("cannot open atlas batch");
    if(Read(input)!=0x314B5041 || Read(input)!=1) throw std::runtime_error("invalid atlas batch header");
    const unsigned records=Read(input);
    if(records>4096) throw std::runtime_error("too many atlas batch records");
    struct Record { unsigned padding;std::vector<Rect> sizes; };
    std::vector<Record> batch;
    std::size_t total=0;
    for(unsigned record=0;record<records;++record) {
        Record value; value.padding=Read(input);
        const unsigned count=Read(input);
        if(value.padding>32 || count>512 || (total+=count)>65536)
            throw std::runtime_error("invalid atlas batch collection");
        for(unsigned i=0;i<count;++i) {
            const unsigned w=Read(input),h=Read(input);
            if(!w || !h || w>Limit-2*value.padding || h>Limit-2*value.padding)
                throw std::runtime_error("invalid atlas image dimensions");
            value.sizes.push_back({0,0,w+2*value.padding,h+2*value.padding});
        }
        batch.push_back(std::move(value));
    }
    if(input.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("trailing atlas batch bytes");
    std::ofstream output(args[4],std::ios::binary|std::ios::trunc);
    if(!output) throw std::runtime_error("cannot create atlas layout");
    Write(output,0x314C5041);Write(output,1);Write(output,records);
    for(const auto& record:batch) {
        const auto start=std::chrono::steady_clock::now();
        unsigned attempts=0;std::uint64_t baseline=0;
        const auto result=Optimize(record.sizes,attempts,baseline);
        const auto micros=std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-start).count();
        Write(output,result.width);Write(output,result.height);Write(output,result.rectangles.size());
        Write(output,attempts);Write(output,baseline,8);Write(output,micros,8);
        for(const auto& r:result.rectangles) { Write(output,r.x+record.padding);Write(output,r.y+record.padding); }
    }
    if(!output) throw std::runtime_error("atlas layout write failed");
    return 0;
}
}
