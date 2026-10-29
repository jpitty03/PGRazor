#pragma once
#include <cstdint>
struct PgrTransferInput {
    uint32_t version, size;
    int32_t attack, defense, stamina, species;
    uint32_t protect, known, flags, keep_all, keep_count, reserved;
    const int32_t *keep_ids;
};
struct PgrTransferApi {
    uint32_t version, size;
    int (*decide)(const PgrTransferInput *);
};
const void *pgrazor_transfer_policy_api(uint32_t version);
