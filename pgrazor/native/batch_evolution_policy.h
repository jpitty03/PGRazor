#ifndef PGR_BATCH_EVOLUTION_POLICY_H
#define PGR_BATCH_EVOLUTION_POLICY_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>

namespace pgr_evolution {

struct Command {
    uint64_t token=0;
    int mode=0;
    bool remove_tag=false;
    std::string tag;
};

// Accept well-formed UTF-8, excluding Unicode control code points and the wire delimiter.
inline bool valid_tag(const char* data,size_t size) {
    if(!data||size==0||size>64)return false;
    for(size_t i=0;i<size;) {
        const unsigned char first=static_cast<unsigned char>(data[i]);
        uint32_t cp=0;
        size_t count=0;
        if(first<0x80){cp=first;count=1;}
        else if(first>=0xC2&&first<=0xDF){cp=first&0x1F;count=2;}
        else if(first>=0xE0&&first<=0xEF){cp=first&0x0F;count=3;}
        else if(first>=0xF0&&first<=0xF4){cp=first&0x07;count=4;}
        else return false;
        if(i+count>size)return false;
        for(size_t j=1;j<count;j++) {
            const unsigned char next=static_cast<unsigned char>(data[i+j]);
            if((next&0xC0)!=0x80)return false;
            cp=(cp<<6)|(next&0x3F);
        }
        if((count==2&&cp<0x80)||(count==3&&cp<0x800)||(count==4&&cp<0x10000)
           ||cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF))return false;
        if(cp<0x20||(cp>=0x7F&&cp<=0x9F)||cp=='|')return false;
        i+=count;
    }
    return true;
}

inline bool parse(const char* data,size_t size,Command& out) {
    if(!data||size<7)return false;
    const char* end=data+size;
    if((data[0]!='1'&&data[0]!='2')||data[1]!='|')return false;
    const char* cursor=data+2;
    if(cursor==end||*cursor<'0'||*cursor>'9')return false;
    uint64_t token=0;
    const uint64_t limit=static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    while(cursor<end&&*cursor>='0'&&*cursor<='9') {
        unsigned digit=static_cast<unsigned>(*cursor-'0');
        if(token>(limit-digit)/10)return false;
        token=token*10+digit;
        ++cursor;
    }
    if(end-cursor<4||*cursor!='|')return false;
    ++cursor;
    if(*cursor<'0'||*cursor>'2'||cursor[1]!='|')return false;
    int mode=*cursor-'0';
    cursor+=2;
    bool remove_tag=false;
    if(data[0]=='2') {
        if(end-cursor<3||(cursor[0]!='0'&&cursor[0]!='1')||cursor[1]!='|')return false;
        remove_tag=cursor[0]=='1';cursor+=2;
    }
    size_t tag_size=static_cast<size_t>(end-cursor);
    if(!valid_tag(cursor,tag_size)||(mode!=0&&token==0))return false;
    Command parsed;
    parsed.token=token;parsed.mode=mode;parsed.remove_tag=remove_tag;parsed.tag.assign(cursor,tag_size);
    out=parsed;
    return true;
}

inline std::string normalize_tag(const std::string& tag) {
    size_t offset=(!tag.empty()&&tag[0]=='#')?1:0;
    std::string normalized=tag.substr(offset);
    for(char& c:normalized)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
    return normalized;
}

// The caller substitutes original_id for a partial response before checking.
// A replacement is confirmed only after the original ID leaves inventory.
inline bool reconciled_identity(uint64_t original_id,uint64_t result_id,int target,
                                bool original_present,int original_species,
                                bool result_present,int result_species) {
    if(original_id==0||result_id==0||target<=0)return false;
    if(result_id==original_id)return original_present&&original_species==target;
    return !original_present&&result_present&&result_species==target;
}

struct Candidate {
    uint64_t id=0;
    int species=0,target=0,form=0,family=0,candy=0,item=0,item_count=0;
};

struct Budget {
    std::map<int,int64_t> candy,items;

    bool reserve(const Candidate& candidate,int candy_stock,int item_stock) {
        if(candidate.id==0||candidate.species<=0||candidate.target<=0||candidate.family<=0
           ||candidate.form<0||candidate.candy<0||candidate.item<0||candidate.item_count<0
           ||candy_stock<0||item_stock<0||(candidate.item_count>0&&candidate.item==0))return false;

        const auto candy_it=candy.find(candidate.family);
        const int64_t candy_used=candy_it==candy.end()?0:candy_it->second;
        if(candy_used<0||candy_used>candy_stock
           ||candidate.candy>static_cast<int64_t>(candy_stock)-candy_used)return false;

        int64_t item_used=0;
        if(candidate.item_count>0) {
            const auto item_it=items.find(candidate.item);
            item_used=item_it==items.end()?0:item_it->second;
            if(item_used<0||item_used>item_stock
               ||candidate.item_count>static_cast<int64_t>(item_stock)-item_used)return false;
        }

        if(candidate.candy>0)candy[candidate.family]=candy_used+candidate.candy;
        if(candidate.item_count>0)items[candidate.item]=item_used+candidate.item_count;
        return true;
    }
};

} // namespace pgr_evolution

#endif
