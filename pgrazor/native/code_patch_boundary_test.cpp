#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "dobby.h"

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "code-patch-boundary: %s\n", message);
        std::abort();
    }
}
}

int main() {
    const long page_result = sysconf(_SC_PAGESIZE);
    require(page_result > 0, "page size");
    const size_t page = static_cast<size_t>(page_result);
    auto *region = static_cast<uint8_t *>(mmap(nullptr, 2 * page,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(region != MAP_FAILED, "map two pages");
    std::memset(region, 0x5a, 2 * page);
    require(mprotect(region, page, PROT_READ | PROT_EXEC) == 0,
        "protect first page");

    // End-exclusive rounding used to change this unrelated writable page to RX.
    uint8_t boundary_bytes[] = {0x1f, 0x20, 0x03, 0xd5};
    require(DobbyCodePatch(region + page - sizeof(boundary_bytes),
        boundary_bytes, sizeof(boundary_bytes)) == 0, "boundary patch");
    require(std::memcmp(region + page - sizeof(boundary_bytes), boundary_bytes,
        sizeof(boundary_bytes)) == 0, "boundary bytes");
    require(region[page - sizeof(boundary_bytes) - 1] == 0x5a,
        "preceding byte unchanged");
    require(region[page] == 0x5a && region[2 * page - 1] == 0x5a,
        "neighbor sentinels unchanged");
    volatile uint8_t *neighbor = region + page;
    neighbor[0] = 0xa5; // The original Dobby implementation faults here.
    neighbor[page - 1] = 0xc3;
    require(neighbor[0] == 0xa5 && neighbor[page - 1] == 0xc3,
        "neighbor remains writable");

    // A real crossing must still make both touched executable pages writable.
    require(mprotect(region + page, page, PROT_READ | PROT_EXEC) == 0,
        "protect second page for crossing");
    uint8_t crossing_bytes[] = {0x1f, 0x20, 0x03, 0xd5, 0xc0, 0x03, 0x5f, 0xd6};
    require(DobbyCodePatch(region + page - 4, crossing_bytes,
        sizeof(crossing_bytes)) == 0, "cross-page patch");
    require(std::memcmp(region + page - 4, crossing_bytes,
        sizeof(crossing_bytes)) == 0, "cross-page bytes");
    require(region[page - 5] == 0x5a && region[page + 4] == 0x5a,
        "cross-page surrounding bytes unchanged");
    require(region[2 * page - 1] == 0xc3, "far sentinel unchanged");
    require(munmap(region, 2 * page) == 0, "unmap fixture");
    std::puts("code-patch-boundary: PASS");
    return 0;
}
