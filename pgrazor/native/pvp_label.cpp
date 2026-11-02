#include "pvp_rank.h"
namespace pgr_pvp {
namespace {
struct Text {
 char *out;int cap,length=0;bool ok=true;
 void add(const char *s){for(;*s;++s){if(length+1>=cap){ok=false;return;}out[length++]=*s;}out[length]=0;}
 void number(int n,bool commas=false){char reverse[16];int used=0;do{reverse[used++]=char('0'+n%10);n/=10;}while(n&&used<16);
  for(int i=used-1;i>=0;--i){char part[2]={reverse[i],0};add(part);if(commas&&i&&i%3==0)add(",");}}
};
}
int lines(int dex,int form,int a,int d,int s,double m,bool owned,char *out,int capacity){
 if(!out||capacity<=0)return 0;out[0]=0;Fit fits[kMaxFits];const int count=ranks(dex,form,a,d,s,m,fits,kMaxFits);
 int current=-1;if(owned&&__builtin_isfinite(m)&&m>0)for(int i=0;i<=98;++i)if(__builtin_fabs(multiplier(i)-m)<0.00002){current=i;break;}
 const char *tags[]={"LL","GL","UL"},*colors[]={"#d6517f","#2b7bd6","#c08a12"};int written=0;
 for(int league=0;league<3;++league){int top=0,first=-1;
  for(int i=0;i<count;++i)if(fits[i].league==league){if(first<0)first=i;if(fits[i].rank<=50)++top;}
  if(first<0||(!top&&league==0))continue;int shown=0;
  for(int i=first;i<count&&fits[i].league==league;++i){if(top&&fits[i].rank>50)continue;if(shown>=(top?3:1))break;
   const Fit &f=fits[i];char line[256]{};Text t{line,256};t.add("\n<color=\"");t.add(top?colors[league]:"#6b7f80");t.add("\">");t.add(tags[league]);t.add(" #");t.number(f.rank,true);
   if(top)t.add("</color>");t.add(" ");t.add(name(f.species));t.add(" \xc2\xb7 Lv ");t.number(f.level/2+1);if(f.level%2)t.add(".5");t.add(" \xc2\xb7 CP ");t.number(f.cp);
   if(current>=0&&current==f.level)t.add(" \xe2\x9c\x93");if(!top)t.add("</color>");
   if(!t.ok||written+t.length>=capacity)return written;
   for(int j=0;j<t.length;++j)out[written++]=line[j];out[written]=0;++shown;
  }
 }
 return written;
}
int cell_summary(int dex,int form,int a,int d,int s,double m,char *out,int capacity){
 if(!out||capacity<=0)return 0;out[0]=0;if(a<0||a>15||d<0||d>15||s<0||s>15)return 0;
 Text t{out,capacity};t.add("<color=\"#36b89e\">");t.number(static_cast<int>(__builtin_floor((a+d+s)/45.0*100+0.5)));
 t.add("% ");t.number(a);t.add("/");t.number(d);t.add("/");t.number(s);t.add("</color>");
 Fit fits[kMaxFits];int count=ranks(dex,form,a,d,s,m,fits,kMaxFits);
 const char *tags[]={"LL","GL","UL"},*colors[]={"#d6517f","#2b7bd6","#c08a12"};
 for(int league=0;league<3;++league)for(int i=0;i<count;++i)if(fits[i].league==league){
  if(fits[i].rank<=50){t.add(" <color=\"");t.add(colors[league]);t.add("\">");t.add(tags[league]);t.add("#");t.number(fits[i].rank);t.add("</color>");}break;
 }
 if(!t.ok){out[0]=0;return 0;}return t.length;
}
}
