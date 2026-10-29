// Compiled descriptors for the existing spawn, VGP and map prepare boundaries.
// Included inside shared_metadata.cpp's private namespace.
int do_spawn(Ctx &c) {
  constexpr unsigned char pro[]={0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0x94,0x97,0x02,0xf0,0xf3,0x03,0x00,0xaa,0x88,0x52,0x56,0x39,0xe8,0x02,0x00,0x37};
  return check_method(c,c.out->opt_classes[OC_SPAWN],"cmwe","System.Double",0,nullptr,nullptr,false,false,0x9dd7438,pro,sizeof(pro),&c.out->opt_handles[H_SP_METHOD],&c.out->opt_handles[H_SP_CODE]);
}

int map_method(Ctx &c,OptHandle slot,OptClassId owner,const char *name,const char *ret,
               const char *arg=nullptr,bool stat=false) {
  return check_method(c,c.out->opt_classes[owner],name,ret,arg?1:0,arg,nullptr,stat,false,0,nullptr,0,&c.out->opt_handles[slot]);
}

int do_vgp_marker(Ctx &c) {
  int rc;
  if((rc=map_method(c,H_VGPM_CAMERA,OC_VGP_CAMERA,"get_main","UnityEngine.Camera",nullptr,true)))return rc;
  if((rc=map_method(c,H_VGPM_TRANSFORM,OC_COMPONENT,"get_transform","UnityEngine.Transform")))return rc;
  if((rc=map_method(c,H_VGPM_POSITION,OC_TRANSFORM,"get_position","UnityEngine.Vector3")))return rc;
  if((rc=map_method(c,H_VGPM_PROJECT,OC_VGP_CAMERA,"WorldToScreenPoint","UnityEngine.Vector3","UnityEngine.Vector3")))return rc;
  if((rc=map_method(c,H_VGPM_WIDTH,OC_VGP_SCREEN,"get_width","System.Int32",nullptr,true)))return rc;
  return map_method(c,H_VGPM_HEIGHT,OC_VGP_SCREEN,"get_height","System.Int32",nullptr,true);
}

void nested_optional(Ctx &c,OptionalId slot,int (*fn)(Ctx&)) {
  char previous[kReasonMax];std::memcpy(previous,c.reason,sizeof(previous));
  c.reason[0]=0;const int rc=fn(c);
  c.out->optional[slot]=rc?PGR_SHARED_OPT_UNAVAILABLE:PGR_SHARED_OPT_AVAILABLE;
  c.out->optional_error[slot]=rc;
  if(rc)std::snprintf(c.out->optional_reason[slot],kReasonMax,"%s",c.reason);
  std::memcpy(c.reason,previous,sizeof(previous));
}

int do_vgp(Ctx &c) {
  if(c.out->optional[OPT_ITEMS]!=PGR_SHARED_OPT_AVAILABLE)return set_err(c,PGR_MD_ERR_NOT_FOUND,"vgp-dependencies");
  for(int i=OC_VGP_CAMERA;i<=OC_VGP_TYPE;++i)if(!c.out->opt_classes[i])return set_err(c,PGR_MD_ERR_NOT_FOUND,"vgp-dependencies");
  auto H=c.out->opt_handles;auto K=c.out->opt_classes;int rc;
  if((rc=check_method_ex(c,K[OC_VGP_WILD],"SendEncounterRequest",nullptr,pred_vgp_encounter_resp,0,nullptr,false,false,0,nullptr,0,&H[H_VGP_M_ENCOUNTER])))return rc;
  MParam capture[]={{"Niantic.Holoholo.Encounter.PokeballThrow",nullptr},{"Holoholo.Rpc.ARPlusEncounterValuesProto",nullptr}};
  if((rc=check_method_ex(c,K[OC_VGP_POKEMON],"TryCapture",nullptr,pred_vgp_capture_resp,2,capture,false,false,0,nullptr,0,&H[H_VGP_M_CAPTURE])))return rc;
  constexpr unsigned char manual[]={0xff,0x83,0x03,0xd1,0xfd,0x7b,0x08,0xa9,0xfc,0x6f,0x09,0xa9,0xfa,0x67,0x0a,0xa9,0xf8,0x5f,0x0b,0xa9,0xf6,0x57,0x0c,0xa9,0xf4,0x4f,0x0d,0xa9,0x54,0x1a,0x02,0xf0};
  if((rc=check_method(c,K[OC_VGP_NAMEPLATE],"SetPokemonUI","System.Void",2,"Niantic.Holoholo.Map.IMapPokemon","Niantic.Rpc.GameplayWeatherProto.Types.WeatherCondition",false,false,0xad84364,manual,sizeof(manual),&H[H_VGP_M_MANUAL_UI],&H[H_VGP_M_MANUAL_UI_CODE])))return rc;
  struct M {OptHandle slot;OptClassId owner;const char *name,*ret,*arg;};
  const M methods[]={
    {H_VGP_M_LOCATION,OC_VGP_POKEMON,"get_Location","Niantic.LatLng",nullptr},
    {H_VGP_M_ID,OC_VGP_WILD,"get_EncounterId","System.UInt64",nullptr},
    {H_VGP_M_VISIBLE,OC_VGP_WILD,"get_VisibleOnMap","System.Boolean",nullptr},
    {H_VGP_M_MAX_ITEMS,OC_VGP_PLAYER,"get_MaxItems","System.Int32",nullptr},
    {H_VGP_M_MAX_PK,OC_VGP_PLAYER,"get_MaxPokemon","System.Int32",nullptr},
    {H_VGP_M_ITEM_COUNT,OC_IBAG,"GetItemCount","System.Int32","Holoholo.Rpc.Item"},
    {H_VGP_M_TOTAL_ITEMS,OC_IBAG,"GetTotalItems","System.Int32",nullptr},
    {H_VGP_M_PK_COUNT,OC_BAG,"get_Count","System.Int32",nullptr},
    {H_VGP_M_ACTIVE,OC_VGP_PLACE,"get_IsActive","System.Boolean",nullptr},
    {H_VGP_M_RANGE,OC_VGP_PLACE,"get_IsPlayerInRange","System.Boolean",nullptr},
    {H_VGP_M_CLOSED,OC_VGP_PLACE,"get_IsClosed","System.Boolean",nullptr},
    {H_VGP_M_COOLING,OC_VGP_PLACE,"get_IsCoolingDown","System.Boolean",nullptr},
    {H_VGP_M_FORT,OC_VGP_PLACE,"get_FortData","Holoholo.Rpc.PokemonFortProto",nullptr},
    {H_VGP_M_COOLDOWN,OC_VGP_PLACE,"SetCooldownTime","System.Void","System.Int64"},
    {H_VGP_M_CTOR,OC_VGP_REQUEST,".ctor","System.Void",nullptr}};
  for(const auto &m:methods)if((rc=map_method(c,m.slot,m.owner,m.name,m.ret,m.arg)))return rc;
  auto next=reinterpret_cast<const void *(*)(const void*,void**)>(c.core->class_get_methods);
  auto name=reinterpret_cast<const char *(*)(const void*)>(c.core->method_get_name);
  auto generic=reinterpret_cast<bool (*)(const void*)>(c.core->method_is_generic);
  auto count=reinterpret_cast<uint32_t (*)(const void*)>(c.core->method_get_param_count);
  auto param=reinterpret_cast<const void *(*)(const void*,uint32_t)>(c.core->method_get_param);
  auto ret=reinterpret_cast<const void *(*)(const void*)>(c.core->method_get_return_type);
  void *iter=nullptr;
  for(unsigned i=0;i<256;++i){const void *m=next(c.out->classes[4],&iter);if(!m)break;std::string n,p,r;
    if(!read_utf8(name(m),&n))return set_err(c,PGR_MD_ERR_NAME_TOO_LONG,"name-limit");
    if(n!="FindObjectsOfType"||generic(m)||count(m)!=1)continue;
    if(!get_type_name(c,param(m,0),&p))return set_err(c,PGR_MD_ERR_NULL_TYPE_NAME,"null-type-name");
    if(p!="System.Type")continue;
    if(!get_type_name(c,ret(m),&r))return set_err(c,PGR_MD_ERR_NULL_TYPE_NAME,"null-type-name");
    if(r=="UnityEngine.Object[]"){H[H_VGP_M_FIND_ALL]=m;break;}}
  if(!H[H_VGP_M_FIND_ALL])return set_err(c,PGR_MD_ERR_NOT_FOUND,"vgp-find-objects");
  struct F {OptHandle slot;OptClassId owner;const char *name;uint64_t off;const char *type;};
  const F fields[]={
    {H_VGP_F_FORT_ID,OC_VGP_FORT,"FortId",0x10,"System.String"},
    {H_VGP_F_FORT_LAT,OC_VGP_FORT,"Latitude",0x20,"System.Double"},
    {H_VGP_F_FORT_LON,OC_VGP_FORT,"Longitude",0x28,"System.Double"},
    {H_VGP_F_ID,OC_VGP_REQUEST,"Id",0x10,"System.String"},
    {H_VGP_F_LAT,OC_VGP_REQUEST,"PlayerLatDegrees",0x18,"System.Double"},
    {H_VGP_F_LON,OC_VGP_REQUEST,"PlayerLngDegrees",0x20,"System.Double"},
    {H_VGP_F_TARGET_LAT,OC_VGP_REQUEST,"FortLatDegrees",0x28,"System.Double"},
    {H_VGP_F_TARGET_LON,OC_VGP_REQUEST,"FortLngDegrees",0x30,"System.Double"},
    {H_VGP_F_UNTIL,OC_SPINRESULT,"CooldownComplete",0x38,"System.Int64"},
    {H_VGP_F_BAG_GM,OC_BAG,"cypm",0x38,"IGameMasterData"}};
  for(const auto &f:fields)if((rc=typed_field(c,K[f.owner],f.name,f.off,f.type,"vgp-field-type",&H[f.slot])))return rc;
  const void *spin=nullptr,*caught=nullptr;
  if((rc=enum_i32_field(c,K[OC_SPINRESULT],"Result",0x10,&H[H_VGP_F_RESULT],&spin)))return rc;
  if((rc=enum_i32_field(c,c.out->classes[3],"Status",0x10,&H[H_VGP_F_CATCH_STATUS],&caught)))return rc;
  if((rc=enum_literal(c,spin,"Success",false,"vgp-enum",&c.out->opt_ints[I_VGP_SPIN_SUCCESS])))return rc;
  if((rc=enum_literal(c,K[OC_VGP_METHOD],"FortSearch",false,"vgp-enum",&c.out->opt_ints[I_VGP_RPC_ID])))return rc;
  const char *states[]={"CatchSuccess","CatchEscape","CatchFlee","CatchMissed","CatchError","CatchItemReplacement"};
  for(unsigned i=0;i<6;++i)if((rc=enum_literal(c,caught,states[i],false,"vgp-enum",&c.out->opt_ints[I_VGP_CS_SUCCESS+i])))return rc;
  H[H_VGP_EXP_PARENT]=c.ex->class_get_parent;H[H_VGP_EXP_ARRAY]=c.ex->array_new;
  H[H_VGP_EXP_BARRIER]=c.ex->gc_wbarrier_set_field;H[H_VGP_EXP_UNREFLECT]=c.ex->method_get_from_reflection;
  for(int i=H_VGP_EXP_PARENT;i<=H_VGP_EXP_UNREFLECT;++i)if(!H[i])return set_err(c,PGR_MD_ERR_MISSING_EXPORT,"vgp-export");
  nested_optional(c,OPT_VGP_MARKER,do_vgp_marker);return 0;
}

int map_field(Ctx &c,const void *owner,const char *name,const char *type,const void **out,const char *alternative=nullptr) {
  if(!owner)return set_err(c,PGR_MD_ERR_NOT_FOUND,"map-field",name);
  auto lookup=reinterpret_cast<const void *(*)(const void*,const char*)>(c.core->class_get_field_from_name);
  auto flags=reinterpret_cast<uint32_t (*)(const void*)>(c.core->field_get_flags);
  auto ft=reinterpret_cast<const void *(*)(const void*)>(c.core->field_get_type);
  const void *f=lookup(owner,name);std::string actual;
  if(!f||(flags(f)&16)||!get_type_name(c,ft(f),&actual)||(actual!=type&&(!alternative||actual!=alternative)))return set_err(c,PGR_MD_ERR_TYPE,"map-field",name);
  *out=f;return 0;
}
const void *map_field_class(Ctx &c,const void *field) {
  return reinterpret_cast<const void *(*)(const void*)>(c.core->class_from_type)(reinterpret_cast<const void *(*)(const void*)>(c.core->field_get_type)(field));
}
int map_enum(Ctx &c,const void *klass,PgrSharedEnumEntry *out,uint32_t *length,uint32_t cap) {
  bool complete=false;
  struct PublishOnlyComplete {uint32_t *count;bool &complete;~PublishOnlyComplete(){if(!complete)*count=0;}} publication{length,complete};
  auto is_enum=reinterpret_cast<bool (*)(const void*)>(c.core->class_is_enum);
  auto base=reinterpret_cast<const void *(*)(const void*)>(c.core->class_enum_basetype);
  std::string type;
  if(!klass||!is_enum(klass)||!get_type_name(c,base(klass),&type)||type!="System.Int32")return set_err(c,PGR_MD_ERR_TYPE,"enum-basetype");
  if(!c.ex->class_get_fields||!c.ex->field_get_name)return set_err(c,PGR_MD_ERR_MISSING_EXPORT,"map-enum-export");
  auto next=reinterpret_cast<const void *(*)(const void*,void**)>(c.ex->class_get_fields);
  auto flags=reinterpret_cast<uint32_t (*)(const void*)>(c.core->field_get_flags);
  auto name=reinterpret_cast<const char *(*)(const void*)>(c.ex->field_get_name);
  auto read=reinterpret_cast<void (*)(const void*,void*)>(c.ex->field_static_get_value);
  *length=0;void *iter=nullptr;
  for(unsigned i=0;i<4096;++i){const void *f=next(klass,&iter);if(!f)break;if((flags(f)&0x8050)!=0x8050)continue;
    int32_t value=0;read(f,&value);std::string text;if(!read_utf8(name(f),&text)||text.size()>=sizeof(out[0].name))return set_err(c,PGR_MD_ERR_NAME_TOO_LONG,"map-enum-name");
    uint32_t pos=0;while(pos<*length&&out[pos].value!=value)++pos;if(pos>=cap)return set_err(c,PGR_MD_ERR_LIMIT,"map-enum-limit");
    if(pos==*length)++*length;out[pos].value=value;std::memcpy(out[pos].name,text.c_str(),text.size()+1);
  }complete=true;return 0;
}
int do_map_poi_extra(Ctx &c) {
  auto H=c.out->opt_handles;auto K=c.out->opt_classes;int rc;
  if((rc=map_field(c,K[OC_RAID_INFO],"RaidLevel","Holoholo.Rpc.RaidLevel",&H[H_MPE_LEVEL])))return rc;
  if((rc=map_enum(c,map_field_class(c,H[H_MPE_LEVEL]),c.out->raid_tiers,&c.out->raid_tier_count,kRaidTierMax)))return rc;
  c.out->enrichment_flags|=1u;
  if(K[OC_PK]){H[H_MPE_BOSS_CLASS]=K[OC_PK];
    if((rc=map_field(c,K[OC_RAID_INFO],"RaidPokemon","Holoholo.Rpc.PokemonProto",&H[H_MPE_BOSS])))return rc;
    if((rc=map_field(c,K[OC_PK],"pokemonId_","System.Int32",&H[H_MPE_BOSS_ID])))return rc;}
  if((rc=map_method(c,H_MPE_TEAM_M,OC_MAP_GYM,"get_GymDisplayTeam","Niantic.Holoholo.Team")))return rc;
  const void *team_type=reinterpret_cast<const void *(*)(const void*)>(c.core->method_get_return_type)(H[H_MPE_TEAM_M]);
  const void *team=reinterpret_cast<const void *(*)(const void*)>(c.core->class_from_type)(team_type);
  if((rc=map_enum(c,team,c.out->team_names,&c.out->team_name_count,kTeamNameMax)))return rc;
  c.out->enrichment_flags|=2u;
  if(K[OC_GYM_DISPLAY]){H[H_MPE_GYM_DISP_CLASS]=K[OC_GYM_DISPLAY];
    if((rc=map_field(c,K[OC_VGP_FORT],"GymDisplay","Holoholo.Rpc.GymDisplayProto",&H[H_MPE_GYM_DISP])))return rc;
    if((rc=map_field(c,K[OC_GYM_DISPLAY],"SlotsAvailable","System.Int32",&H[H_MPE_SLOTS])))return rc;}
  return 0;
}
int do_map_poi(Ctx &c) {
  if(c.out->optional[OPT_VGP]!=PGR_SHARED_OPT_AVAILABLE)return set_err(c,PGR_MD_ERR_NOT_FOUND,"map-dependencies");
  auto H=c.out->opt_handles;auto K=c.out->opt_classes;int rc;
  if(!K[OC_MAP_GYM]||!K[OC_RAID_INFO])return set_err(c,PGR_MD_ERR_NOT_FOUND,"map-classes");
  H[H_MP_GYM]=K[OC_MAP_GYM];H[H_MP_RAID]=K[OC_RAID_INFO];
  if((rc=map_field(c,K[OC_VGP_FORT],"RaidInfo","Holoholo.Rpc.RaidInfoProto",&H[H_MP_INFO])))return rc;
  if((rc=map_field(c,K[OC_RAID_INFO],"RaidEndMs","System.Int64",&H[H_MP_END],"System.UInt64")))return rc;
  if((rc=map_field(c,K[OC_RAID_INFO],"RaidSpawnMs","System.Int64",&H[H_MP_SPAWN],"System.UInt64")))return rc;
  if((rc=map_field(c,K[OC_RAID_INFO],"RaidBattleMs","System.Int64",&H[H_MP_BATTLE],"System.UInt64")))return rc;
  if((rc=map_field(c,K[OC_RAID_INFO],"IsRaidHidden","System.Boolean",&H[H_MP_HIDDEN])))return rc;
  nested_optional(c,OPT_MAP_POI_EXTRA,do_map_poi_extra);return 0;
}
