#include "pvp_rank.cpp"
#include "pvp_label.cpp"
extern "C" {
unsigned cache_capacity(){return pgr_pvp::cache_capacity();}
unsigned cache_builds(){return static_cast<unsigned>(pgr_pvp::cache_builds());}
unsigned cache_hits(){return static_cast<unsigned>(pgr_pvp::cache_hits());}
unsigned cache_evictions(){return static_cast<unsigned>(pgr_pvp::cache_evictions());}
void *memset(void *p,int value,unsigned long count){volatile unsigned char *d=static_cast<unsigned char*>(p);for(unsigned long i=0;i<count;++i)d[i]=static_cast<unsigned char>(value);return p;}
void *memcpy(void *p,const void *from,unsigned long count){volatile unsigned char *d=static_cast<unsigned char*>(p);const unsigned char *s=static_cast<const unsigned char*>(from);for(unsigned long i=0;i<count;++i)d[i]=s[i];return p;}
pgr_pvp::Fit result[pgr_pvp::kMaxFits];
int query(int dex,int form,int a,int d,int s,double multiplier){return pgr_pvp::ranks(dex,form,a,d,s,multiplier,result,pgr_pvp::kMaxFits);}
const pgr_pvp::Fit *result_address(){return result;}
char cell_buffer[256];
const char *cell_summary(int dex,int form,int a,int d,int s,double multiplier){pgr_pvp::cell_summary(dex,form,a,d,s,multiplier,cell_buffer,sizeof(cell_buffer));return cell_buffer;}
const char *species_name(int index){return pgr_pvp::name(index);}
char label_buffer[2048];
const char *label(int dex,int form,int a,int d,int s,double multiplier,int owned){pgr_pvp::lines(dex,form,a,d,s,multiplier,owned!=0,label_buffer,2048);return label_buffer;}
}
