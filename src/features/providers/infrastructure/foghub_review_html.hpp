#pragma once
#include <optional>
#include <map>
#include <string>

namespace fog::foghub {
struct SessionReviewForm { std::string csrf; };
SessionReviewForm parseSessionReviewForm(const std::string& html,const std::string& rental);
// Validates the whole history page before reading the exact finished row.
std::optional<int> sessionHistoryRating(const std::string& html,const std::string& rental);
std::map<std::string,std::optional<int>> sessionHistoryRatings(const std::string& html);
inline bool validSessionReviewMessage(const std::string& value) noexcept {
    // HTML maxlength counts UTF-16 units. Reject malformed UTF-8 and nontext controls.
    std::size_t units=0;
    for(std::size_t i=0;i<value.size();) {
        const auto first=static_cast<unsigned char>(value[i++]); unsigned cp=first; unsigned tail=0;
        if(first>=0xc2 && first<=0xdf) {cp=first&31;tail=1;}
        else if(first>=0xe0 && first<=0xef) {cp=first&15;tail=2;}
        else if(first>=0xf0 && first<=0xf4) {cp=first&7;tail=3;}
        else if(first>=128) return false;
        if(value.size()-i<tail) return false;
        for(unsigned n=0;n<tail;++n) {const auto c=static_cast<unsigned char>(value[i++]);if((c&0xc0)!=0x80)return false;cp=(cp<<6)|(c&63);}
        if((tail==1&&cp<128)||(tail==2&&cp<2048)||(tail==3&&cp<65536)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;
        if((cp<32 && cp!='\n' && cp!='\r' && cp!='\t') || cp==127) return false;
        units+=cp>0xffff?2:1; if(units>2000)return false;
    }
    return true;
}
}
