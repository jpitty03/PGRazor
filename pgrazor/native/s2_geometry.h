#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace pgr::radar {
struct LatLng {double lat=0,lon=0;};
bool valid_l15(uint64_t id);
bool cell_vertices(uint64_t id,std::array<LatLng,4>& out);
LatLng cell_center(uint64_t id);
bool cell_contains(uint64_t id,LatLng point);
double distance_m(LatLng a,LatLng b);
struct PocketSample {LatLng position;double outer_m=200,inner_m=0;};
struct Pocket {bool region=false,destination_valid=false;bool approximate=true;LatLng destination;double coverage_radius_m=0;uint32_t feasible_tiles=0,boundary_tiles=0;};
// Explicit constraints only: absence from Wild does not automatically add an
// inner radius. A caller must establish request position and completeness first.
Pocket solve_pocket(uint64_t cell,const PocketSample *samples,size_t count);
}
