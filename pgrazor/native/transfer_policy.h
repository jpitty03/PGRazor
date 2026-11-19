#pragma once
#include <cstdint>
#include <cstring>
struct PgrTransferInput {
    uint32_t version, size;
    int32_t attack, defense, stamina, species;
    // keep_iv: 0 keeps the original 90% cutoff (callers that predate the setting); otherwise
    // PGR_TRANSFER_KEEP_IV_SET | percent (0..100) keeps every Pokemon at or above that IV percent.
    uint32_t protect, known, flags, keep_all, keep_count, keep_iv;
    const int32_t *keep_ids;
};
struct PgrTransferApi {
    uint32_t version, size;
    int (*decide)(const PgrTransferInput *);
};
const void *pgrazor_transfer_policy_api(uint32_t version);

enum : uint32_t { PGR_TRANSFER_KEEP_IV_SET = 0x100u, PGR_TRANSFER_KEEP_IV_DEFAULT = 90u };

// Transport string slot 25 ("K1|<percent>", percent 0..100) -> keep_iv. Anything else returns 0,
// which the policy reads as the original 90% cutoff.
inline uint32_t pgr_transfer_keep_iv_parse(const char *s, uint32_t n) {
    if (!s || n < 4 || n > 6 || std::memcmp(s, "K1|", 3) != 0) return 0;
    uint32_t value = 0;
    for (uint32_t i = 3; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return 0;
        value = value * 10 + uint32_t(s[i] - '0');
    }
    if ((n - 3 > 1 && s[3] == '0') || value > 100) return 0;
    return PGR_TRANSFER_KEEP_IV_SET | value;
}
// The percent a keep_iv value stands for, or -1 when it is malformed.
inline int pgr_transfer_keep_iv_percent(uint32_t keep_iv) {
    if (keep_iv == 0) return int(PGR_TRANSFER_KEEP_IV_DEFAULT);
    if ((keep_iv & ~0x1ffu) || !(keep_iv & PGR_TRANSFER_KEEP_IV_SET) || (keep_iv & 0xffu) > 100) return -1;
    return int(keep_iv & 0xffu);
}
