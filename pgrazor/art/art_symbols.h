#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Read-only bounded ELF symbol lookup, including the platform's XZ mini-debug
// symbol table. Addresses must belong to a loaded PT_LOAD segment of libart.
class PgrArtSymbols {
 public:
  bool load();
  void *find(std::string_view name, bool prefix=false) const;
 private:
  struct Symbol {std::string name; uintptr_t address;};
  struct Range {uintptr_t begin,end;};
  uintptr_t base_=0;
  std::string path_;
  std::vector<Range> ranges_;
  std::vector<Symbol> symbols_;
  bool parse(const unsigned char *data,size_t size,bool mini);
  static int visit(struct dl_phdr_info *,size_t,void *);
};
