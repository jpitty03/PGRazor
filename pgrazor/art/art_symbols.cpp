#include "art_symbols.h"
#include <link.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <limits>
#include <mutex>
extern "C" {
#include "xz.h"
}
namespace {
constexpr size_t kFileMax=128u*1024*1024,kMiniMax=64u*1024*1024,kSymbolsMax=500000;
bool within(size_t size,uint64_t offset,uint64_t count){return offset<=size&&count<=size-offset;}
template<class T>bool copy(const unsigned char *data,size_t size,uint64_t off,T &out){
 if(!within(size,off,sizeof(T)))return false;std::memcpy(&out,data+off,sizeof(T));return true;
}
bool name_at(const unsigned char *data,size_t size,uint64_t offset,std::string_view &out){
 if(offset>=size)return false;
 const char *start=reinterpret_cast<const char *>(data+offset);
 const size_t left=size-offset,limit=left<4096?left:4096;
 const void *end=std::memchr(start,0,limit);if(!end)return false;
 out=std::string_view(start,static_cast<const char *>(end)-start);return true;
}
}
int PgrArtSymbols::visit(dl_phdr_info *info,size_t,void *context){
 auto &self=*static_cast<PgrArtSymbols *>(context);
 if(!info->dlpi_name)return 0;
 const std::string_view path(info->dlpi_name);
 if(path.size()<10||path.substr(path.size()-10)!="/libart.so")return 0;
 self.base_=info->dlpi_addr;self.path_=path;
 for(size_t i=0;i<info->dlpi_phnum;i++){
  const auto &p=info->dlpi_phdr[i];if(p.p_type!=PT_LOAD||!p.p_memsz||!(p.p_flags&PF_R))continue;
  if(p.p_vaddr>UINTPTR_MAX-self.base_||p.p_memsz>UINTPTR_MAX-(self.base_+p.p_vaddr))return 1;
  self.ranges_.push_back({self.base_+p.p_vaddr,self.base_+p.p_vaddr+p.p_memsz});
 }
 return 1;
}
bool PgrArtSymbols::load(){
 symbols_.clear();ranges_.clear();path_.clear();base_=0;
 dl_iterate_phdr(visit,this);if(path_.empty()||ranges_.empty())return false;
 const int fd=open(path_.c_str(),O_RDONLY|O_CLOEXEC);if(fd<0)return false;
 struct stat st{};if(fstat(fd,&st)||st.st_size<64||uint64_t(st.st_size)>kFileMax){close(fd);return false;}
 const size_t size=st.st_size;void *mapping=mmap(nullptr,size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
 if(mapping==MAP_FAILED)return false;
 bool okay=false;
 try{okay=parse(static_cast<const unsigned char *>(mapping),size,false);}catch(...){okay=false;}
 munmap(mapping,size);if(!okay)symbols_.clear();return okay&&!symbols_.empty();
}
bool PgrArtSymbols::parse(const unsigned char *data,size_t size,bool mini){
 Elf64_Ehdr eh{};if(!copy(data,size,0,eh)||std::memcmp(eh.e_ident,ELFMAG,SELFMAG)||
 eh.e_ident[EI_CLASS]!=ELFCLASS64||eh.e_ident[EI_DATA]!=ELFDATA2LSB||eh.e_shentsize!=sizeof(Elf64_Shdr)||
 !eh.e_shnum||eh.e_shnum>8192||eh.e_shstrndx>=eh.e_shnum||
 !within(size,eh.e_shoff,uint64_t(eh.e_shnum)*sizeof(Elf64_Shdr)))return false;
#if defined(__aarch64__)
 if(eh.e_machine!=EM_AARCH64)return false;
#elif defined(__x86_64__)
 if(eh.e_machine!=EM_X86_64)return false;
#else
 return false;
#endif
 std::vector<Elf64_Shdr> sections(eh.e_shnum);
 std::memcpy(sections.data(),data+eh.e_shoff,sections.size()*sizeof(Elf64_Shdr));
 const auto &names=sections[eh.e_shstrndx];
 if(names.sh_type!=SHT_STRTAB||!within(size,names.sh_offset,names.sh_size))return false;
 for(const auto &section:sections){
  if(section.sh_type!=SHT_SYMTAB&&section.sh_type!=SHT_DYNSYM)continue;
  if(section.sh_entsize!=sizeof(Elf64_Sym)||section.sh_size%sizeof(Elf64_Sym)||
     !within(size,section.sh_offset,section.sh_size)||section.sh_link>=sections.size())return false;
  const auto &strings=sections[section.sh_link];
  if(strings.sh_type!=SHT_STRTAB||!within(size,strings.sh_offset,strings.sh_size))return false;
  const size_t count=section.sh_size/sizeof(Elf64_Sym);
  if(count>kSymbolsMax||symbols_.size()>kSymbolsMax-count)return false;
  for(size_t i=0;i<count;i++){
   Elf64_Sym sym{};std::memcpy(&sym,data+section.sh_offset+i*sizeof(sym),sizeof(sym));
   if(sym.st_shndx==SHN_UNDEF||sym.st_shndx==SHN_ABS||sym.st_value>UINTPTR_MAX-base_)continue;
   const auto type=ELF64_ST_TYPE(sym.st_info);if(type!=STT_FUNC&&type!=STT_OBJECT&&type!=STT_NOTYPE)continue;
   const uintptr_t address=base_+sym.st_value;bool mapped=false;
   for(const auto &r:ranges_)if(address>=r.begin&&address<r.end){mapped=true;break;}
   if(!mapped)continue;
   std::string_view name;if(!name_at(data+strings.sh_offset,strings.sh_size,sym.st_name,name))return false;
   if(!name.empty())symbols_.push_back({std::string(name),address});
  }
 }
 if(mini)return true;
 for(const auto &section:sections){
  std::string_view name;if(!name_at(data+names.sh_offset,names.sh_size,section.sh_name,name))return false;
  if(name!=".gnu_debugdata")continue;
  if(!within(size,section.sh_offset,section.sh_size)||!section.sh_size)return false;
  static std::once_flag crc;std::call_once(crc,[]{xz_crc32_init();xz_crc64_init();});
  xz_dec *decoder=xz_dec_init(XZ_DYNALLOC,kMiniMax);if(!decoder)return false;
  std::vector<unsigned char> output;
  try{output.resize(kMiniMax);}catch(...){xz_dec_end(decoder);throw;}
  xz_buf buffer{};buffer.in=data+section.sh_offset;buffer.in_size=section.sh_size;
  buffer.out=output.data();buffer.out_size=output.size();
  const auto result=xz_dec_run(decoder,&buffer);xz_dec_end(decoder);
  if(result!=XZ_STREAM_END||buffer.in_pos!=buffer.in_size||!buffer.out_pos)return false;
  if(!parse(output.data(),buffer.out_pos,true))return false;
  break;
 }
 return true;
}
void *PgrArtSymbols::find(std::string_view name,bool prefix)const{
 if(name.empty()||name.size()>4095)return nullptr;
 for(const auto &symbol:symbols_)
  if(prefix?std::string_view(symbol.name).substr(0,name.size())==name:symbol.name==name)
   return reinterpret_cast<void *>(symbol.address);
 return nullptr;
}
