#pragma once
#include "iv_display.h"
#include "runtime_metadata.h"
// Optional reflection only. Failure leaves the ordinary IV metadata and overlay intact.
bool pgr_resolve_pvp_metadata(const PgrMetaExports &exports,const PgrMetaApi &metadata,
    void *static_field_reader,void *pokemon_class,void *text_class,PgrIvPvpConfig &out);
bool pgr_resolve_iv_cells(const PgrMetaExports &exports,const PgrMetaApi &metadata,
    void *type_is_byref,PgrIvCellConfig &out);
