#include "s2_geometry.h"
#include <cmath>
#include <algorithm>
#include <limits>
namespace pgr::radar { namespace {
constexpr double pi=3.14159265358979323846,R=6371008.8;
struct V {double x,y,z;};
V unit(LatLng p){double a=p.lat*pi/180,b=p.lon*pi/180,c=std::cos(a);return{c*std::cos(b),c*std::sin(b),std::sin(a)};}
V cross(V a,V b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
LatLng ll(V a){return{std::atan2(a.z,std::hypot(a.x,a.y))*180/pi,std::atan2(a.y,a.x)*180/pi};}
double uv(double s){return s>=.5?(4*s*s-1)/3:(1-4*(1-s)*(1-s))/3;}
V face(int f,double u,double v){switch(f){case 0:return{1,u,v};case 1:return{-u,1,v};case 2:return{-u,-v,1};case 3:return{-1,-v,-u};case 4:return{v,-1,-u};default:return{v,u,-1};}}
struct Cell {int f;uint32_t i=0,j=0;};
Cell decode(uint64_t id){Cell c{int(id>>61)};unsigned orientation=c.f&1;
 // Hilbert position-to-(i,j) maps with swap/invert orientation bits.
 constexpr unsigned map[4][4]={{0,1,3,2},{0,2,3,1},{3,2,0,1},{3,1,0,2}},change[4]={1,0,0,3};
 for(int level=1;level<=15;++level){unsigned p=unsigned((id>>(61-2*level))&3),ij=map[orientation][p];c.i=(c.i<<1)|(ij>>1);c.j=(c.j<<1)|(ij&1);orientation^=change[p];}return c;}
LatLng point(Cell c,double i,double j){return ll(face(c.f,uv((c.i+i)/32768.0),uv((c.j+j)/32768.0)));}
bool valid_point(LatLng p){return std::isfinite(p.lat)&&std::isfinite(p.lon)&&std::fabs(p.lat)<=90&&std::fabs(p.lon)<=180;}
bool feasible(LatLng p,const PocketSample*s,size_t n){for(size_t k=0;k<n;++k){double d=distance_m(p,s[k].position);if(d>s[k].outer_m||d<s[k].inner_m)return false;}return true;}
}
bool valid_l15(uint64_t id){return (id>>61)<6&&(id&((uint64_t(1)<<31)-1))==(uint64_t(1)<<30);}
bool cell_vertices(uint64_t id,std::array<LatLng,4>&out){out={};if(!valid_l15(id))return false;auto c=decode(id);out={point(c,0,0),point(c,1,0),point(c,1,1),point(c,0,1)};return true;}
LatLng cell_center(uint64_t id){if(!valid_l15(id))return{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN()};return point(decode(id),.5,.5);}
double distance_m(LatLng a,LatLng b){if(!valid_point(a)||!valid_point(b))return std::numeric_limits<double>::quiet_NaN();V x=unit(a),y=unit(b),z=cross(x,y);return R*std::atan2(std::sqrt(dot(z,z)),std::clamp(dot(x,y),-1.0,1.0));}
bool cell_contains(uint64_t id,LatLng p){std::array<LatLng,4> v;if(!valid_point(p)||!cell_vertices(id,v))return false;V q=unit(p);for(int k=0;k<4;++k)if(dot(cross(unit(v[k]),unit(v[(k+1)%4])),q)<-1e-14)return false;return true;}
Pocket solve_pocket(uint64_t id,const PocketSample*s,size_t n){Pocket result;if(!valid_l15(id)||!s||!n||n>8)return result;
 for(size_t k=0;k<n;++k)if(!valid_point(s[k].position)||!std::isfinite(s[k].outer_m)||!std::isfinite(s[k].inner_m)||s[k].outer_m<=0||s[k].outer_m>2000||s[k].inner_m<0||s[k].inner_m>=s[k].outer_m)return result;
 const Cell c=decode(id);constexpr int N=96;V mean{};
 // Keep uncertain boundary tiles. Distance bounds enclose each small spherical
 // quadrilateral; two opposite-corner distances deliberately overbound radius.
 auto visit=[&](auto fn){for(int i=0;i<N;++i)for(int j=0;j<N;++j){double x=(i+.5)/N,y=(j+.5)/N;LatLng q=point(c,x,y);
   double radius=distance_m(point(c,double(i)/N,double(j)/N),point(c,double(i+1)/N,double(j+1)/N))+.02;
   bool possible=true;for(size_t k=0;k<n;++k){double d=distance_m(q,s[k].position);if(d-radius>s[k].outer_m||d+radius<s[k].inner_m){possible=false;break;}}
   if(possible)fn(q,radius,feasible(q,s,n));}};
 visit([&](LatLng q,double,bool inside){result.region=true;if(inside){++result.feasible_tiles;V v=unit(q);mean.x+=v.x;mean.y+=v.y;mean.z+=v.z;}else ++result.boundary_tiles;});
 if(!result.feasible_tiles)return result;LatLng centroid=ll(mean);double best=std::numeric_limits<double>::infinity();
 visit([&](LatLng q,double,bool inside){if(inside){double d=distance_m(q,centroid);if(d<best){best=d;result.destination=q;result.destination_valid=true;}}});
 visit([&](LatLng q,double radius,bool){result.coverage_radius_m=std::max(result.coverage_radius_m,distance_m(q,result.destination)+radius);});return result;
}
}
