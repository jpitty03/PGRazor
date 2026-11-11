#include "s2_geometry.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace pgr::radar;
int main(){
 assert(!valid_l15(0)&&!valid_l15(1)&&!valid_l15(uint64_t(6)<<61));
 const double lat[]={-35.264389682754654,-35.264389682754654,35.264389682754654,35.264389682754654,35.264389682754654,-35.264389682754654};
 const double lon[]={-45,45,45,135,-135,-135};
 for(unsigned f=0;f<6;++f){uint64_t id=(uint64_t(f)<<61)|(uint64_t(1)<<30);std::array<LatLng,4> corners{};assert(cell_vertices(id,corners));assert(std::fabs(corners[0].lat-lat[f])<1e-10&&std::fabs(corners[0].lon-lon[f])<1e-10);assert(cell_contains(id,cell_center(id)));assert(!cell_contains(id,{-lat[f],lon[f]+(lon[f]>0?-180:180)}));
  for(unsigned p=0;p<256;++p){uint64_t cell=(uint64_t(f)<<61)|(uint64_t(p)<<53)|(uint64_t(1)<<30);assert(cell_contains(cell,cell_center(cell)));assert(cell_vertices(cell,corners));for(auto v:corners)assert(cell_contains(cell,v));}}
 assert(distance_m({0,179.999},{0,-179.999})<223);assert(distance_m({90,0},{90,170})<1e-6);
 uint64_t id=0x1000000040000000ULL;LatLng center=cell_center(id);assert(std::fabs(center.lat)<.003&&std::fabs(center.lon)<.003);
 PocketSample s{center,200,75};auto p=solve_pocket(id,&s,1);assert(p.region&&p.destination_valid&&cell_contains(id,p.destination));double d=distance_m(center,p.destination);assert(d>=75&&d<=200);assert(p.coverage_radius_m>0&&p.approximate);
 PocketSample impossible[2]={{center,20,0},{{center.lat+1,center.lon},20,0}};assert(!solve_pocket(id,impossible,2).region);
 s.inner_m=201;assert(!solve_pocket(id,&s,1).destination_valid);assert(!solve_pocket(id,nullptr,0).destination_valid);
 std::puts("PASS S2: six faces, independent cube vertices, Hilbert cells, dateline/poles, feasible annulus destination and empty intersection");
}
