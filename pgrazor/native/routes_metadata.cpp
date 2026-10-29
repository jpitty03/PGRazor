#include "routes_metadata.h"
#include <cstring>
#include <mutex>
#include <unistd.h>

namespace {
using namespace pgr_routes_schema;
std::mutex resolving;
bool attached(const PgrRoutesMetadataConfig &c){
    return c.owner_tid&&c.owner_tid==static_cast<uint64_t>(gettid())&&
        reinterpret_cast<void *(*)()>(c.thread_current)()!=nullptr;
}
const PgrMetaApi *metadata(){return static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(1));}
int class_index(const char *key){for(uint32_t i=0;i<kClassCount;i++)if(!std::strcmp(kClasses[i].key,key))return static_cast<int>(i);return -1;}
int field_index(uint32_t owner,const char *name){for(uint32_t i=0;i<kFieldCount;i++)if(kFields[i].owner==owner&&!std::strcmp(kFields[i].name,name))return static_cast<int>(i);return -1;}
int method_index(const char *key){for(uint32_t i=0;i<kMethodCount;i++)if(!std::strcmp(kMethods[i].key,key))return static_cast<int>(i);return -1;}
int method(const PgrMetaApi *md,const void *owner,const char *name,const char *ret,const char *arg,bool inflated,const void **out){
    PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=owner;q.name=name;q.return_type=ret;
    q.return_match=ret?PGR_MD_MATCH_EXACT:PGR_MD_MATCH_ANY;q.param_count=arg?1:0;
    q.param_types[0]=arg;q.param_match[0]=arg?PGR_MD_MATCH_EXACT:PGR_MD_MATCH_ANY;q.allow_inflated=inflated?1:0;
    PgrMetaMethodResult r{};int rc=md->validate_method(&q,&r);if(!rc)*out=r.method;return rc;
}
int resolve(const PgrRoutesMetadataConfig *input,PgrRoutesMetadataOutput *out){
    if(!out)return -1;
    std::memset(out,0,sizeof(*out));
    if(!input||input->version!=1||input->size!=sizeof(*input)||!input->thread_current)return -2;
    // No shared scratch escapes, and recursive metadata callbacks cannot deadlock.
    std::unique_lock<std::mutex> lock(resolving,std::try_to_lock);if(!lock.owns_lock())return -27;
    PgrRoutesMetadataConfig c=*input;
    const uintptr_t module_base=reinterpret_cast<uintptr_t>(c.binding.module_base);
    const uintptr_t thread_address=reinterpret_cast<uintptr_t>(c.thread_current);
    if(!module_base||!c.binding.module_size||c.binding.module_size>UINTPTR_MAX-module_base)return PGR_MD_ERR_CODE_RANGE;
    // Production uses the default resolver and real module exports. Explicit
    // native segment callbacks are the existing feature47 fixture boundary and
    // may intentionally supply exports outside their synthetic module buffer.
    if(!c.binding.seg_query&&(thread_address<module_base||thread_address-module_base>c.binding.module_size||
        4>c.binding.module_size-(thread_address-module_base)))return PGR_MD_ERR_CODE_RANGE;
    if(!attached(c))return -30;
    const auto *md=metadata();if(!md||md->version!=1||md->size!=sizeof(PgrMetaApi))return -31;
    if(!c.binding.seg_query){
        // Frida reports a page-rounded mapping, whereas the compiled resolver
        // reports the PT_LOAD extent. Bind the canonical extent used by54 too.
        const void *canonical=nullptr;uint64_t extent=0;
        int status=md->resolve_module("libil2cpp.so",&canonical,&extent);if(status)return status;
        const long page=sysconf(_SC_PAGESIZE);
        if(canonical!=c.binding.module_base||!extent||extent>c.binding.module_size||page<=0||
           c.binding.module_size-extent>=static_cast<uint64_t>(page))return PGR_MD_ERR_CODE_RANGE;
        c.binding.module_size=extent;
    }
    int rc=md->bind(&c.binding);if(rc)return rc;
    PgrRoutesMetadataOutput result{};result.version=1;result.size=sizeof(result);
    const void *field_classes[kFieldCount]{};
    for(uint32_t i=0;i<kClassCount;i++){
        if(!attached(c))return -30;
        const auto &s=kClasses[i];PgrMetaClassQuery q{};q.version=1;q.size=sizeof(q);
        q.name_space=s.name_space;q.name=s.name;q.canonical=s.canonical;q.canonical_match=PGR_MD_MATCH_EXACT;
        PgrMetaClassResult r{};if((rc=md->resolve_class(&q,&r)))return rc;result.classes[i]=r.klass;
    }
    for(uint32_t i=0;i<kFieldCount;i++){
        if(!attached(c))return -30;
        const auto &s=kFields[i];if(s.owner>=kClassCount)return -32;
        PgrMetaFieldQuery q{};q.version=1;q.size=sizeof(q);q.owner=result.classes[s.owner];q.name=s.name;
        q.offset=s.offset;q.forbidden_mask=0x50;q.kind=s.kind;q.value_size=s.value_size;
        q.type_name=s.type_name;q.type_match=s.type_name?PGR_MD_MATCH_EXACT:PGR_MD_MATCH_ANY;
        PgrMetaFieldResult r{};if((rc=md->validate_field(&q,&r)))return rc;
        result.fields[i]=r.field;field_classes[i]=r.type_class;
    }
    for(uint32_t i=0;i<kMethodCount;i++){
        if(!attached(c))return -30;
        const auto &s=kMethods[i];if(s.owner>=kClassCount||s.param_count>1)return -32;
        if((rc=method(md,result.classes[s.owner],s.name,s.return_type,s.param_count?s.param0:nullptr,false,&result.methods[i])))return rc;
    }
    const int nearby=class_index("nearby"),shared=class_index("shared"),waypoint=class_index("waypoint"),start=class_index("startData");
    if(nearby<0||shared<0||waypoint<0||start<0)return -32;
    const int list=field_index(nearby,"<Routes>k__BackingField"),wp=field_index(shared,"Waypoints"),dismiss=method_index("dismissGui"),confirm=method_index("confirmComplete");
    if(list<0||wp<0||dismiss<0||confirm<0||!field_classes[list]||!field_classes[wp])return -32;
    if(!attached(c))return -30;
    if((rc=method(md,field_classes[list],"get_Count","System.Int32",nullptr,true,&result.list_count))||
       (rc=method(md,field_classes[list],"get_Item",kClasses[shared].canonical,"System.Int32",true,&result.list_item))||
       (rc=method(md,field_classes[wp],"get_Count","System.Int32",nullptr,true,&result.waypoint_count))||
       (rc=method(md,field_classes[wp],"get_Item",kClasses[waypoint].canonical,"System.Int32",true,&result.waypoint_item))||
       (rc=method(md,result.classes[start],".ctor","System.Void",nullptr,false,&result.constructor)))return rc;
    // GuiController.Dismiss is already owned by the shared presentation module.
    // Its patched prefix is intentionally not compared; verify metadata address.
    const void *dismiss_code=nullptr;std::memcpy(&dismiss_code,result.methods[dismiss],sizeof(dismiss_code));
    const uintptr_t base=reinterpret_cast<uintptr_t>(c.binding.module_base);
    if(base>UINTPTR_MAX-0xa375344u||reinterpret_cast<uintptr_t>(dismiss_code)!=base+0xa375344u)return -33;
    static constexpr unsigned char confirm_prefix[]={0xfe,0x5f,0xbd,0xa9,0xf6,0x57,0x01,0xa9,0xf4,0x4f,0x02,0xa9,0xd4,0x50,0x02,0xb0,0xf3,0x03,0x00,0xaa,0x88,0x3a,0x4e,0x39};
    if(base>UINTPTR_MAX-174801868u)return -33;
    const void *confirm_code=nullptr;std::memcpy(&confirm_code,result.methods[confirm],sizeof(confirm_code));
    if(reinterpret_cast<uintptr_t>(confirm_code)!=base+174801868u)return -33;
    PgrMetaCodeQuery code{};code.version=1;code.size=sizeof(code);code.address=reinterpret_cast<const void *>(base+174801868u);
    code.length=sizeof(confirm_prefix);code.prologue=confirm_prefix;code.prologue_len=sizeof(confirm_prefix);
    if((rc=md->validate_code(&code)))return rc;
    if(!attached(c))return -30;
    *out=result;return 0;
}
const PgrRoutesMetadataApi api={1,sizeof(PgrRoutesMetadataApi),sizeof(PgrRoutesMetadataConfig),sizeof(PgrRoutesMetadataOutput),resolve};
}
extern "C" const void *pgrazor_routes_metadata_api(uint32_t version){return version==1?&api:nullptr;}
