#include "app/presentation/ui.hpp"
#include "features/settings/domain/controller_layout.hpp"
#include <SDL_image.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace fog::ui {
namespace {
constexpr SDL_Color White{247,248,255,255}, Muted{179,185,209,255};
constexpr SDL_Color Cyan{61,191,245,255}, Green{86,235,173,255};
constexpr SDL_Color Card{37,36,65,255};
void color(SDL_Renderer* r, SDL_Color c) { SDL_SetRenderDrawColor(r,c.r,c.g,c.b,c.a); }
void fill(SDL_Renderer* r, SDL_Rect rect, SDL_Color c) { color(r,c); SDL_RenderFillRect(r,&rect); }
SDL_Rect inset(SDL_Rect r,int n) { return {r.x+n,r.y+n,r.w-2*n,r.h-2*n}; }
void roundRect(SDL_Renderer* renderer, SDL_Rect r, int radius, SDL_Color c) {
    if(r.w<=0 || r.h<=0) return;
    radius=std::min(radius,std::min(r.w,r.h)/2);
    color(renderer,c);
    SDL_Rect middle{r.x,r.y+radius,r.w,r.h-2*radius};
    SDL_RenderFillRect(renderer,&middle);
    for(int row=0;row<radius;++row) {
        const double dy=radius-row-0.5;
        const int cut=radius-static_cast<int>(std::sqrt(radius*radius-dy*dy));
        SDL_RenderDrawLine(renderer,r.x+cut,r.y+row,r.x+r.w-cut-1,r.y+row);
        SDL_RenderDrawLine(renderer,r.x+cut,r.y+r.h-row-1,r.x+r.w-cut-1,r.y+r.h-row-1);
    }
}
std::string clean(const std::string& raw) {
    std::string out;
    for(std::size_t i=0;i<raw.size() && out.size()<2048;) {
        const auto c=static_cast<unsigned char>(raw[i]);
        std::size_t n=c<0x80?1:(c>=0xc2 && c<=0xdf?2:(c>=0xe0 && c<=0xef?3:(c>=0xf0 && c<=0xf4?4:0)));
        bool valid=n!=0 && i+n<=raw.size();
        for(std::size_t j=1;valid && j<n;++j) valid=(static_cast<unsigned char>(raw[i+j])&0xc0)==0x80;
        if(valid && n==3) valid=!(c==0xe0 && static_cast<unsigned char>(raw[i+1])<0xa0) && !(c==0xed && static_cast<unsigned char>(raw[i+1])>=0xa0);
        if(valid && n==4) valid=!(c==0xf0 && static_cast<unsigned char>(raw[i+1])<0x90) && !(c==0xf4 && static_cast<unsigned char>(raw[i+1])>=0x90);
        if(!valid) { out+='?'; ++i; continue; }
        if(out.size()+n>2048) break;
        if(n>1 || c>=32 || c=='\n') out.append(raw,i,n);
        else if(c=='\t') out+=' ';
        i+=n;
    }
    return out;
}
std::size_t nextChar(const std::string& s,std::size_t pos) {
    ++pos;
    while(pos<s.size() && (static_cast<unsigned char>(s[pos])&0xc0)==0x80) ++pos;
    return pos;
}
void popChar(std::string& s) {
    if(s.empty()) return;
    auto p=s.size()-1;
    while(p>0 && (static_cast<unsigned char>(s[p])&0xc0)==0x80) --p;
    s.resize(p);
}
int measure(TTF_Font* font,const std::string& s) {
    int w=0,h=0;
    if(!s.empty()) TTF_SizeUTF8(font,s.c_str(),&w,&h);
    return w;
}
std::string ellipsis(TTF_Font* font,std::string s,int width) {
    const std::string dots="…";
    while(!s.empty() && measure(font,s+dots)>width) popChar(s);
    return measure(font,dots)<=width?s+dots:std::string{};
}
std::vector<std::string> wrap(TTF_Font* font,const std::string& raw,int width,int limit) {
    const auto value=clean(raw);
    std::vector<std::string> lines;
    std::size_t pos=0;
    while(pos<value.size() && static_cast<int>(lines.size())<limit) {
        while(pos<value.size() && value[pos]==' ') ++pos;
        auto end=pos;
        auto word=pos;
        while(end<value.size() && value[end]!='\n') {
            const auto next=nextChar(value,end);
            if(measure(font,value.substr(pos,next-pos))>width) break;
            if(value[end]==' ') word=end;
            end=next;
        }
        const bool newline=end<value.size() && value[end]=='\n';
        if(end==pos && !newline) end=nextChar(value,pos);
        if(!newline && end<value.size() && word>pos) end=word;
        auto line=value.substr(pos,end-pos);
        while(!line.empty() && line.back()==' ') line.pop_back();
        pos=end;
        if(pos<value.size() && value[pos]=='\n') ++pos;
        while(pos<value.size() && value[pos]==' ') ++pos;
        if(measure(font,line)>width || (static_cast<int>(lines.size())==limit-1 && pos<value.size())) line=ellipsis(font,line,width);
        lines.push_back(std::move(line));
    }
    return lines;
}
void icon(SDL_Renderer* r,int x,int y,int type,SDL_Color c) {
    color(r,c);
    if(type==0) {
        for(int a=0;a<360;++a) { const double t=a*3.141592653589793/180.; SDL_RenderDrawPoint(r,x+static_cast<int>(8*std::cos(t)),y+static_cast<int>(8*std::sin(t))); }
        SDL_RenderDrawLine(r,x+6,y+6,x+13,y+13);
    } else if(type==1) {
        roundRect(r,{x-15,y-10,30,21},6,c); roundRect(r,{x-12,y-7,24,15},4,Card);
        color(r,c); SDL_RenderDrawLine(r,x-7,y-4,x-7,y+4); SDL_RenderDrawLine(r,x-11,y,x-3,y);
        fill(r,{x+6,y-3,3,3},c); fill(r,{x+2,y+2,3,3},c);
    } else if(type==2) {
        roundRect(r,{x-13,y-10,26,19},3,c); roundRect(r,{x-10,y-7,20,13},1,Card);
        fill(r,{x-2,y+9,4,4},c); fill(r,{x-8,y+13,16,2},c);
    } else if(type==3) {
        roundRect(r,{x-11,y-13,22,26},4,c); roundRect(r,{x-8,y-10,16,20},2,Card);
        for(int i=0;i<3;++i) fill(r,{x-5,y-6+i*5,10,2},c);
    } else {
        for(int a=0;a<360;++a) { const double t=a*3.141592653589793/180.; SDL_RenderDrawPoint(r,x+static_cast<int>(11*std::cos(t)),y+static_cast<int>(11*std::sin(t))); }
        for(int a=0;a<8;++a) { const double t=a*3.141592653589793/4.; SDL_RenderDrawLine(r,x+static_cast<int>(9*std::cos(t)),y+static_cast<int>(9*std::sin(t)),x+static_cast<int>(15*std::cos(t)),y+static_cast<int>(15*std::sin(t))); }
        roundRect(r,{x-4,y-4,8,8},4,c);
    }
}
}
struct Ui::Impl {
    struct Entry { SDL_Texture* texture=nullptr; int w=0,h=0; std::uint64_t used=0; };
    SDL_Renderer* renderer;
    TTF_Font* regular;
    TTF_Font* large;
    std::unordered_map<std::string,Entry> cache;
    struct Wrapped { std::vector<std::string> lines; std::uint64_t used; };
    std::unordered_map<std::string,Wrapped> wrappedCache;
    const std::vector<std::string>& wrapped(const std::string& raw,int width,int limit,bool big) {
        const auto value = clean(raw);
        const auto key = std::string(big ? "L:" : "R:") + std::to_string(width) + ":" + std::to_string(limit) + ":" + value;
        auto found = wrappedCache.find(key);
        if (found == wrappedCache.end()) {
            if (wrappedCache.size() >= 192) {
                const auto oldest = std::min_element(wrappedCache.begin(),wrappedCache.end(),
                    [](const auto& a,const auto& b) { return a.second.used < b.second.used; });
                wrappedCache.erase(oldest);
            }
            found = wrappedCache.emplace(key,Wrapped{wrap(big ? large : regular,value,width,limit),++serial}).first;
        }
        found->second.used = ++serial;
        return found->second.lines;
    }
    std::size_t pixels=0;
    std::uint64_t serial=0;
    Chrome chrome;
    bool xboxLayout=false;
    SDL_Texture* background=nullptr;
    std::array<SDL_Texture*,5> branding{}; // Logo, search, games, history, settings.
    void loadBranding() {
#ifdef __SWITCH__
        const std::string root="romfs:/ui/";
#else
        const std::string root="romfs/ui/";
#endif
        const char* files[]{"logo.png","magnifying-glass.svg","game-controller.svg","writing.svg","setting.svg"};
        for(std::size_t i=0;i<branding.size();++i) {
            branding[i]=IMG_LoadTexture(renderer,(root+files[i]).c_str());
            if(branding[i]) SDL_SetTextureBlendMode(branding[i],SDL_BLENDMODE_BLEND);
            else SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,"Bundled UI asset unavailable: %s",files[i]);
        }
    }
    void menuIcon(int x,int y,int index,int fallback,SDL_Color tint) {
        auto* texture=branding[static_cast<std::size_t>(index)+1];
        if(!texture) { icon(renderer,x,y,fallback,tint); return; }
        SDL_SetTextureColorMod(texture,tint.r,tint.g,tint.b);
        SDL_SetTextureAlphaMod(texture,tint.a);
        const SDL_Rect destination{x-16,y-16,32,32};
        SDL_RenderCopy(renderer,texture,nullptr,&destination);
    }
        Impl(SDL_Renderer* r,TTF_Font* f,TTF_Font* l):renderer(r),regular(f),large(l) {
        loadBranding();
        auto* surface=SDL_CreateRGBSurfaceWithFormat(0,Width,Height,32,SDL_PIXELFORMAT_RGBA32);
        if(!surface) return;
        for(int y=0;y<Height;++y) {
            auto* row=reinterpret_cast<Uint32*>(static_cast<Uint8*>(surface->pixels)+y*surface->pitch);
            for(int x=0;x<Width;++x) {
                const double dx=(x-1080)/440.,dy=(y-260)/380.;
                const double glow=std::max(0.,1.-(dx*dx+dy*dy));
                const double bx=(x-270)/550.,by=(y-710)/340.;
                const double blue=std::max(0.,1.-(bx*bx+by*by));
                row[x]=SDL_MapRGBA(surface->format,static_cast<Uint8>(10+49*glow),static_cast<Uint8>(12+6*glow+7*blue),static_cast<Uint8>(27+22*glow+20*blue),255);
            }
        }
        background=SDL_CreateTextureFromSurface(renderer,surface);
        SDL_FreeSurface(surface);
    }
    ~Impl() { for(auto* texture:branding) if(texture) SDL_DestroyTexture(texture); if(background) SDL_DestroyTexture(background); }
    void erase(std::unordered_map<std::string,Entry>::iterator i) { pixels-=static_cast<std::size_t>(i->second.w)*i->second.h; SDL_DestroyTexture(i->second.texture); cache.erase(i); }
    void clear() { for(auto& e:cache) SDL_DestroyTexture(e.second.texture); cache.clear(); wrappedCache.clear(); pixels=0; }
    void line(const std::string& value,int x,int y,SDL_Color c,bool big,int fitWidth=0,double scale=1.) {
        if(value.empty()) return;
        const std::string key=std::string(big?"L":"R")+std::to_string(c.r)+","+std::to_string(c.g)+","+std::to_string(c.b)+","+std::to_string(c.a)+":"+value;
        auto found=cache.find(key);
        if(found==cache.end()) {
            auto* surface=TTF_RenderUTF8_Blended(big?large:regular,value.c_str(),c);
            if(!surface) return;
            Entry e; e.w=surface->w; e.h=surface->h; e.used=++serial;
            e.texture=SDL_CreateTextureFromSurface(renderer,surface); SDL_FreeSurface(surface);
            if(!e.texture) return;
            const auto count=static_cast<std::size_t>(e.w)*e.h;
            while(!cache.empty() && (cache.size()>=192 || pixels+count>2097152)) {
                auto victim=cache.begin();
                for(auto i=cache.begin();i!=cache.end();++i) if(i->second.used<victim->second.used) victim=i;
                erase(victim);
            }
            if(count>2097152) { SDL_DestroyTexture(e.texture); return; }
            pixels+=count;
            found=cache.emplace(key,e).first;
        }
        found->second.used=++serial;
                SDL_Rect dest{x,y,static_cast<int>(found->second.w*scale),static_cast<int>(found->second.h*scale)};
        if(fitWidth>0 && dest.w>fitWidth) {
            dest.h=std::max(1,static_cast<int>(static_cast<double>(dest.h)*fitWidth/dest.w));
            dest.w=fitWidth;
        }
        SDL_RenderCopy(renderer,found->second.texture,nullptr,&dest);
    }
    void compactText(const std::string& value,int x,int y,int width,SDL_Color tint,int lines=2) {
        // Reuse the bounded regular-font texture cache at a consistent 20 px size.
        constexpr double scale=20./23.;
        for(const auto& lineValue:wrapped(value,static_cast<int>(width/scale),lines,false)) {
            line(lineValue,x,y,tint,false,0,scale);
            y+=static_cast<int>(TTF_FontLineSkip(regular)*scale)+2;
        }
    }
};
Ui::Ui(SDL_Renderer* renderer,TTF_Font* regular,TTF_Font* large) {
    if(!renderer || !regular || !large) throw std::invalid_argument("UI requires renderer and fonts");
    impl_ = std::make_unique<Impl>(renderer,regular,large);
}
Ui::~Ui() { impl_->clear(); }
void Ui::setXboxLayout(bool enabled) { impl_->xboxLayout=enabled; }
std::string Ui::controllerHints(const std::string& value) const {
    return controller::uiHints(value,controller::Settings{impl_->xboxLayout});
}
void Ui::clearTextCache() { impl_->clear(); }
std::size_t Ui::cachedTextCount() const { return impl_->cache.size(); }
SDL_Rect Ui::content() const { const int x=impl_->chrome.sidebarExpanded?258:116; return {x,116,Width-x-30,508}; }
void Ui::text(const std::string& value,int x,int y,int width,SDL_Color c,bool large,int lines) {
    if(width<=0 || lines<=0) return;
    const auto* state=impl_.get();
    auto* font=large?state->large:state->regular;
    const auto& wrapped=impl_->wrapped(value,width,lines,large);
    for(const auto& line:wrapped) { impl_->line(line,x,y,c,large); y+=TTF_FontLineSkip(font)+3; }
}
void Ui::panel(SDL_Rect r,bool focused,bool danger) {
    auto* renderer=impl_->renderer;
    if(focused) { roundRect(renderer,{r.x-4,r.y-4,r.w+8,r.h+8},19,{146,126,220,80}); roundRect(renderer,r,15,White); }
    else roundRect(renderer,r,15,{73,64,105,255});
    roundRect(renderer,inset(r,focused?3:1),focused?12:14,danger?SDL_Color{65,31,54,255}:Card);
    const auto inner=inset(r,focused?4:2);
    color(renderer,{122,72,130,35});
    SDL_RenderDrawLine(renderer,inner.x+12,inner.y+1,inner.x+inner.w-12,inner.y+1);
}
void Ui::button(const std::string& label,SDL_Rect rect,bool focused,bool enabled) {
    panel(rect,focused);
    text(label,rect.x+16,rect.y+(rect.h-TTF_FontHeight(impl_->regular))/2,rect.w-32,enabled?White:Muted);
}
void Ui::frame(const Chrome& chrome) {
    impl_->chrome=chrome;
    auto* r=impl_->renderer;
    SDL_RenderSetClipRect(r,nullptr);
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    if(impl_->background) SDL_RenderCopy(r,impl_->background,nullptr,nullptr);
    else fill(r,{0,0,Width,Height},{10,12,27,255});
    const int rail=chrome.sidebarExpanded?230:88;
    fill(r,{0,0,rail,Height},{27,30,56,230}); fill(r,{rail-1,0,1,Height},{97,81,132,90});
    roundRect(r,{30,27,28,28},14,Cyan); roundRect(r,{37,30,14,14},7,White); roundRect(r,{34,44,20,10},5,White);
    if(chrome.sidebarExpanded) text(chrome.account,20,68,190,Muted,false,2);
    const std::string names[]{"Поиск","Игры","Журнал сессий","Настройки"};
    const Section sections[]{Section::Search,Section::Games,Section::History,Section::Settings};
    const int types[]{0,1,3,4};
    for(int i=0;i<4;++i) {
        const int y=i==3?573:180+i*73;
        const bool selected=chrome.section==sections[i] || (i==1 && chrome.section==Section::Computers);
        if(selected) {
            roundRect(r,{12,y-24,rail-24,50},10,chrome.sidebarFocused?SDL_Color{89,86,133,255}:SDL_Color{54,51,88,255});
            if(chrome.sidebarFocused) { roundRect(r,{12,y-24,rail-24,50},10,White); roundRect(r,{15,y-21,rail-30,44},8,{63,58,102,255}); }
            else roundRect(r,{3,y-17,3,34},1,White);
        }
        impl_->menuIcon(44,y,i,types[i],selected?White:Muted);
        if(chrome.sidebarExpanded) impl_->line(names[i],73,y-13,selected?White:Muted,false,rail-85);
    }
    const auto area=content();
    const int logoX=area.x+area.w-151;
    if(impl_->branding[0]) image(impl_->branding[0],{logoX,27,151,38});
    else text("FOG PLAY NX",logoX,27,151,White,false,2);
    text(chrome.title,area.x,27,area.w-175,White,true);
    text(chrome.subtitle,area.x,77,area.w-80,Muted);
    fill(r,{rail,650,Width-rail,70},{12,16,32,248});
    text(controllerHints(chrome.hints),area.x,681,area.w,Muted);
}
void Ui::image(SDL_Texture* texture,SDL_Rect rect,bool cover) {
    if(!texture || rect.w<=0 || rect.h<=0) return;
    int w=0,h=0;
    if(SDL_QueryTexture(texture,nullptr,nullptr,&w,&h)!=0 || w<=0 || h<=0) return;
    SDL_Rect src{0,0,w,h},dst=rect;
    const double aspect=static_cast<double>(rect.w)/rect.h;
    if(cover) {
        if(static_cast<double>(w)/h>aspect) { src.w=static_cast<int>(h*aspect); src.x=(w-src.w)/2; }
        else { src.h=static_cast<int>(w/aspect); src.y=(h-src.h)/2; }
    } else {
        const double scale=std::min(static_cast<double>(rect.w)/w,static_cast<double>(rect.h)/h);
        dst.w=static_cast<int>(w*scale); dst.h=static_cast<int>(h*scale);
        dst.x+=(rect.w-dst.w)/2; dst.y+=(rect.h-dst.h)/2;
    }
    if(!cover) { SDL_RenderCopy(impl_->renderer,texture,&src,&dst); return; }
    SDL_Rect previous{};
    const bool hadClip=SDL_RenderIsClipEnabled(impl_->renderer)==SDL_TRUE;
    SDL_RenderGetClipRect(impl_->renderer,&previous);
    const int radius=std::min(10,std::min(dst.w,dst.h)/2);
    SDL_Rect clip{dst.x,dst.y+radius,dst.w,dst.h-2*radius};
    if(hadClip) SDL_IntersectRect(&clip,&previous,&clip);
    SDL_RenderSetClipRect(impl_->renderer,&clip);
    SDL_RenderCopy(impl_->renderer,texture,&src,&dst);
    for(int row=0;row<radius;++row) {
        const double dy=radius-row-0.5;
        const int cut=radius-static_cast<int>(std::sqrt(radius*radius-dy*dy));
        for(int edge=0;edge<2;++edge) {
            clip={dst.x+cut,edge==0?dst.y+row:dst.y+dst.h-row-1,dst.w-2*cut,1};
            if(hadClip) SDL_IntersectRect(&clip,&previous,&clip);
            SDL_RenderSetClipRect(impl_->renderer,&clip);
            SDL_RenderCopy(impl_->renderer,texture,&src,&dst);
        }
    }
    SDL_RenderSetClipRect(impl_->renderer,hadClip?&previous:nullptr);
}
void Ui::games(const std::vector<GameCard>& cards,std::size_t selected,std::size_t first,const std::string& heading,int top,std::size_t rows) {
    const auto area=content(); if(!heading.empty()) text(heading,area.x,top-42,area.w,White);
    if(cards.empty()) { empty("Игры пока не найдены","Измените поиск или обновите каталог."); return; }
    const int width=(area.w-3*18)/4;
    for(std::size_t index=first;index<cards.size() && index<first+HistoryColumns*std::min(rows,std::size_t(2));++index) {
        const auto i=index-first; const int x=area.x+static_cast<int>(i%4)*(width+18),y=top+static_cast<int>(i/4)*244;
        const auto& c=cards[index]; panel({x,y,width,232},index==selected);
        const SDL_Rect art{x+6,y+6,width-12,152};
        if(c.cover) image(c.cover,art,true);
        else {
            roundRect(impl_->renderer,art,10,{44,40,81,255});
            icon(impl_->renderer,x+width/2,y+82,1,{144,111,214,255});

        }
        impl_->compactText(c.name,x+14,y+169,width-28,White);
    }
}
void Ui::genres(const std::vector<std::string>& labels,std::size_t focused,std::size_t selected) {
    if(labels.empty()) return;
    const auto area=content();
    const auto count=std::min(labels.size(),std::size_t(6));
    const int width=(area.w-static_cast<int>(count-1)*10)/static_cast<int>(count);
    for(std::size_t i=0;i<count;++i) {
        const SDL_Rect chip{area.x+static_cast<int>(i)*(width+10),82,width,44};
        const bool active=i==selected,focus=i==focused;
        if(focus) roundRect(impl_->renderer,chip,12,White);
        else roundRect(impl_->renderer,chip,12,active?SDL_Color{40,156,180,255}:SDL_Color{66,62,91,255});
        roundRect(impl_->renderer,inset(chip,focus?3:1),focus?9:11,
                  active?SDL_Color{24,81,102,255}:SDL_Color{24,27,48,255});
        const auto label=clean(labels[i]);
        constexpr double scale=20./23.;
        const int fit=static_cast<int>((width-20)/scale);
        const auto clipped=measure(impl_->regular,label)>fit?ellipsis(impl_->regular,label,fit):label;
        const int textWidth=static_cast<int>(measure(impl_->regular,clipped)*scale);
        impl_->line(clipped,chip.x+(width-textWidth)/2,chip.y+9,active?Cyan:White,false,0,scale);
    }
}
void Ui::genreDialog(const std::vector<std::string>& labels,std::size_t focused,std::size_t first,std::size_t selected) {
    auto* renderer=impl_->renderer;
    fill(renderer,{0,0,Width,Height},{5,7,17,220});
    const SDL_Rect box{210,74,860,572};
    panel(box);
    text("ЖАНРЫ",box.x+28,box.y+20,box.w-56,White,true);
    text(controllerHints("A Выбрать    B Назад    ↑ / ↓ Прокрутка"),box.x+28,box.y+box.h-40,box.w-56,Muted);
    const int width=(box.w-72)/2;
    first-=first%2;
    for(std::size_t index=first;index<labels.size() && index<first+12;++index) {
        const auto offset=index-first;
        const SDL_Rect chip{box.x+28+static_cast<int>(offset%2)*(width+16),box.y+85+static_cast<int>(offset/2)*65,width,53};
        panel(chip,index==focused);
        if(index==selected) roundRect(renderer,{chip.x+12,chip.y+20,12,12},6,Cyan);
        impl_->compactText(labels[index],chip.x+34,chip.y+14,width-48,index==selected?Cyan:White,1);
    }
    if(first>0) text("↑",box.x+box.w-50,box.y+48,26,Muted);
    if(first+12<labels.size()) text("↓",box.x+box.w-50,box.y+box.h-44,26,Muted);
}
void Ui::recentGames(const std::vector<GameCard>& cards,std::size_t selected,std::size_t first) {
    const auto area=content();
    text("Недавно играли",area.x,142,area.w-200,White);
    if(cards.empty()) return;
    text(std::to_string(first+1)+"–"+std::to_string(std::min(cards.size(),first+4))+" из "+std::to_string(cards.size()),
         area.x+area.w-170,142,170,Muted);
    const int width=(area.w-3*18)/4;
    for(std::size_t index=first;index<cards.size() && index<first+HistoryColumns;++index) {
        const int x=area.x+static_cast<int>(index-first)*(width+18),y=176;
        panel({x,y,width,166},index==selected);
        const SDL_Rect art{x+6,y+6,width-12,104};
        if(cards[index].cover) image(cards[index].cover,art,true);
        else {
            roundRect(impl_->renderer,art,10,{44,40,81,255});
            icon(impl_->renderer,x+width/2,y+58,1,{144,111,214,255});
        }
        impl_->compactText(cards[index].name,x+14,y+116,width-28,White);
    }
}
void Ui::computers(const std::vector<ComputerCard>& cards,std::size_t selected,std::size_t first,int top) {
    const auto area=content();
    if(cards.empty()) { empty("Нет доступных компьютеров","Обновите список или выберите другую игру."); return; }
    const int width=(area.w-3*18)/4;
    const int height=std::min(top>280?186:232,637-top);
    for(std::size_t index=first;index<cards.size() && index<first+8;++index) {
        const auto i=index-first; const int x=area.x+static_cast<int>(i%4)*(width+18),y=top+static_cast<int>(i/4)*(height+18);
        if(height<70 || y+height>637) break;
        const auto& c=cards[index]; panel({x,y,width,height},index==selected);
        const auto pingTint=c.ping.find("мс")!=std::string::npos && c.ping.find_first_of("0123456789")!=std::string::npos?Green:Muted;
        if(height<=125) {
            impl_->compactText(c.name,x+14,y+7,width-28,White,1);
            impl_->compactText(c.ping.empty()?"Пинг не измерен":c.ping,x+14,y+35,width-28,pingTint,1);
            impl_->compactText(c.available?c.price:"Недоступен",x+14,y+height-29,width-28,c.available?White:Muted,1);
            continue;
        }
        text(c.name,x+14,y+12,width-28,White);
        if(height>125) {
            if(!c.ping.empty()) text(c.ping,x+14,y+43,width-28,pingTint);
            else text("Пинг не измерен",x+14,y+43,width-28,Muted);
        }
        if(height>200) {
            text(c.accountNote.empty()?c.platform:c.accountNote,x+14,y+73,width-28,Cyan);
            text(c.gpu,x+14,y+105,width-28,White,false,2);
            text(c.cpu,x+14,y+168,width-28,Muted);
        } else if(height>135) {
            text(c.platform,x+14,y+73,width-28,Cyan);
            text(c.gpu,x+14,y+105,width-28,White);
        }
        text(c.available?c.price:"Недоступен",x+14,y+height-37,width-28,c.available?White:Muted);
    }
}
int Ui::gameDetail(const GameDetail& detail,int focus) {
    const auto a=content();
    int x=a.x;
    for(const auto& genre:detail.genres) {
        const auto label=clean(genre); const int width=std::min(measure(impl_->regular,label)+22,220);
        if(x+width>a.x+a.w) break;
        roundRect(impl_->renderer,{x,116,width,32},8,{15,82,130,255}); text(label,x+11,117,width-22,White); x+=width+10;
    }
    text(detail.description,a.x,166,a.w-20,White,false,2);
    const SDL_Rect toggle{a.x,239,332,46}; panel(toggle,focus==0);
    roundRect(impl_->renderer,{a.x+13,251,43,22},11,detail.ownSteamAccount?Green:SDL_Color{94,94,118,255});
    roundRect(impl_->renderer,{a.x+(detail.ownSteamAccount?36:16),254,16,16},8,White);
    text("Свой Steam аккаунт",a.x+69,246,247,White);
    text(detail.rating.empty()?"":"Рейтинг: "+detail.rating,a.x+359,248,a.w-360,Muted);
    if(detail.choices.empty()) return 300;
    text("Автоподбор сервера",a.x,300,a.w,White,true);
    const int width=(a.w-36)/3;
    for(int i=0;i<3;++i) {
        const int cardX=a.x+i*(width+18);
        panel({cardX,351,width,135},focus==i+1);
        icon(impl_->renderer,cardX+28,380,2,Cyan);
        const AutoChoice fallback{"Не доступно","Данные пока не загружены","",false};
        const auto& choice=static_cast<std::size_t>(i)<detail.choices.size()?detail.choices[i]:fallback;
        text(choice.title,cardX+53,364,width-68,choice.available?White:Muted);
        text(choice.description,cardX+15,393,width-30,Muted,false,2);
        text(choice.available?choice.price:"Нет подходящих ПК",cardX+15,454,width-30,choice.available?White:Muted);
    }
    text("Все серверы",a.x,503,a.w,White);
    return 542;
}
void Ui::history(const std::vector<HistoryCard>& cards,std::size_t selected,std::size_t first) {
    const auto a=content();
    if(cards.empty()) { empty("История сессий пуста","Здесь появятся завершённые сессии вашего аккаунта."); return; }
    const int width=(a.w-18*(static_cast<int>(HistoryColumns)-1))/static_cast<int>(HistoryColumns);
    for(std::size_t index=first;index<cards.size() && index<first+HistoryColumns;++index) {
        const auto& c=cards[index]; const int x=a.x+static_cast<int>(index-first)*(width+18),y=154;
        panel({x,y,width,370},index==selected);
        text(c.name,x+15,y+15,width-30,White,true);
        text("Начало",x+15,y+71,width-30,Muted); text(c.start,x+15,y+99,width-30,White,false,2);
        text("Окончание",x+15,y+157,width-30,Muted); text(c.end,x+15,y+185,width-30,White,false,2);
        text(c.duration,x+15,y+250,width-30,Muted); text(c.amount,x+15,y+284,width-30,White);
        text(c.note,x+15,y+330,width-30,Muted);
    }
}
void Ui::settings(const std::vector<SettingCard>& cards,std::size_t selected) {
    const auto a=content(); const int width=(a.w-18)/2;
    for(std::size_t i=0;i<cards.size() && i<6;++i) {
        const int x=a.x+static_cast<int>(i%2)*(width+18),y=154+static_cast<int>(i/2)*160;
        const auto& c=cards[i]; panel({x,y,width,146},i==selected);
        text(c.title,x+20,y+15,width-40,White);
        int valueX=x+20;
        if(c.checked.has_value()) {
            const SDL_Rect checkbox{x+20,y+50,26,26};
            roundRect(impl_->renderer,checkbox,5,*c.checked?Cyan:Muted);
            roundRect(impl_->renderer,inset(checkbox,2),3,*c.checked?Cyan:Card);
            if(*c.checked) {
                color(impl_->renderer,{10,28,42,255});
                for(int offset=-1;offset<=1;++offset) {
                    SDL_RenderDrawLine(impl_->renderer,checkbox.x+6,checkbox.y+13+offset,checkbox.x+11,checkbox.y+18+offset);
                    SDL_RenderDrawLine(impl_->renderer,checkbox.x+11,checkbox.y+18+offset,checkbox.x+21,checkbox.y+7+offset);
                }
            }
            valueX+=38;
        }
        text(c.value,valueX,y+48,x+width-20-valueX,Cyan);
        text(c.description,x+20,y+83,width-40,Muted,false,2);
    }
}
void Ui::dialog(const Dialog& dialog) {
    auto* r=impl_->renderer;
    fill(r,{0,0,Width,Height},{5,7,17,205});
    const SDL_Rect box{263,112,754,488}; panel(box,false,dialog.danger);
    text(dialog.title,box.x+34,box.y+26,box.w-68,White,true,2);
    const int bodyY=box.y+113;
    text(dialog.body,box.x+34,bodyY,box.w-68,Muted,false,dialog.image?4:8);
    if(dialog.image) image(dialog.image,{box.x+34,bodyY+137,box.w-68,105});
    if(!dialog.primary.empty()) button(dialog.primary,{box.x+34,box.y+box.h-76,338,48},dialog.primaryFocused);
    if(!dialog.secondary.empty()) button(dialog.secondary,{box.x+390,box.y+box.h-76,330,48},!dialog.primaryFocused);
}
void Ui::status(const std::string& message,bool busy,bool error) {
    const auto a=content();
    fill(impl_->renderer,{a.x,650,a.w,31},{12,16,32,255});
    text((busy?"● ":"")+message,a.x,650,a.w,error?SDL_Color{255,142,164,255}:busy?Cyan:White);
}
void Ui::empty(const std::string& title,const std::string& detail) {
    const auto a=content(); const SDL_Rect card{a.x,196,a.w,251}; panel(card);
    icon(impl_->renderer,a.x+47,247,1,Muted);
    text(title,a.x+90,224,a.w-118,White,true,2);
    text(detail,a.x+32,321,a.w-64,Muted,false,3);
}
}