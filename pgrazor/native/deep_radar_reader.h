#pragma once
#include "deep_radar_store.h"
#include <cstdint>

namespace pgr::radar {
enum ClassSlot { ResponseClass,CellClass,NearbyClass,WildClass,MapClass,FortClass,
 PokemonClass,DisplayClass,SnapshotClass,PinnedClass,StringClass,ClassCount };
enum FieldSlot {
 ResponseStatus,ResponseSnapshot,ResponsePinned,CellId,CellTime,CellTruncated,
 NearbySpecies,NearbyDistance,NearbyId,NearbyFort,NearbyDisplay,
 WildId,WildLat,WildLon,WildSpawn,WildPokemon,WildTth,
 MapId,MapLat,MapLon,MapSpawn,MapSpecies,MapExpiry,MapDisplay,
 FortId,FortLat,FortLon,PokemonSpecies,PokemonDisplay,DisplayForm,DisplayCostume,DisplayGender,
 SnapshotStatus,PinnedStatus,PinnedPokemon,DisplayShiny,FieldCount
};
enum ListSlot { CellsList,FortsList,NearbyList,WildList,MapList,SnapshotList,ListCount };
struct RepeatedBinding {const void *field=nullptr,*klass=nullptr,*array=nullptr,*count=nullptr,*array_class=nullptr,*element_class=nullptr;};
// Every function here is an allocation-free IL2CPP read export. Deliberately no
// invoke/GC-handle/allocation functions: a borrowed callback frame is copied
// synchronously before forwarding, and no managed reference crosses a safepoint.
struct ReaderConfig {
 void *(*object_class)(void*)=nullptr;
 void (*field_get)(void*,const void*,void*)=nullptr;
 uintptr_t (*array_length)(void*)=nullptr;
 int32_t (*string_length)(void*)=nullptr;
 const uint16_t *(*string_chars)(void*)=nullptr;
 uint64_t (*clock_ms)()=nullptr;
 bool (*allowed)()=nullptr;
 const void *classes[ClassCount]{};
 const void *fields[FieldCount]{};
 RepeatedBinding lists[ListCount]{};
 int32_t success=1,snapshot_complete=1,pinned_included=2;
};
struct ReadStats {
 uint32_t cells=0,nearby=0,offstop=0,wild=0,catchable=0,top_level=0;
 uint32_t distance_count=0,distance_200=0,non_l15=0,short_tth=0;
 uint32_t display_count=0,shiny_true=0,offstop_distance_count=0,offstop_distance_200=0;
 uint32_t missing_positions=0;
 float offstop_distance_min=0,offstop_distance_max=0;
 float distance_min=0,distance_max=0;
};
enum ReadStatus { ReadOk=1,ReadIgnored=0,ReadMetadata=-1,ReadBounds=-2,ReadExpired=-3,ReadValue=-4 };
// Caller owns heap-allocated scratch. On failure its counts are zero; no partial
// packet is eligible for publication. Decoder calls no managed code, RPC or lock.
int read_response(const ReaderConfig&,void *response,Packet&,ReadStats&,uint64_t deadline);
}
