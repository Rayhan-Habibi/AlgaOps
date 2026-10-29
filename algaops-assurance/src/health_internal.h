/*
 * health_internal.h — helpers shared by the health method sources
 * (health_methods.c, health_fixed_threshold.c, health_mahalanobis.c).
 */

#ifndef HEALTH_INTERNAL_H
#define HEALTH_INTERNAL_H

#include "../include/health_methods.h"

#include <string.h>

#define HM_F ALGA_MAX_FEATURES

static inline void health_method_reset(AlgaMethodResult *out, int status)
{
    memset(out, 0, sizeof(*out));
    out->status = status;
    out->top_feature = -1;
}

#endif /* HEALTH_INTERNAL_H */
