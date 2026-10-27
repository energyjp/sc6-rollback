/* Storage for the two forwarding families (shared by the plugin and the unit test). */
#include "fwd.h"

static void *lp_fn[FWD_MAXSLOTS], *lp_self[FWD_MAXSLOTS];
static uint64_t lp_counts[FWD_BANKS * FWD_MAXSLOTS], lp_nonzero[FWD_MAXSLOTS];
volatile uint32_t lp_bank;

static void *ifc_fn[FWD_MAXSLOTS];
static uint64_t ifc_counts[FWD_BANKS * FWD_MAXSLOTS], ifc_nonzero[FWD_MAXSLOTS];
static volatile uint32_t ifc_bank;

FwdFamily lp_family  = { lp_fn,  lp_self, lp_counts,  lp_nonzero,  &lp_bank,  0 };
FwdFamily ifc_family = { ifc_fn, 0,       ifc_counts, ifc_nonzero, &ifc_bank, 0 };
