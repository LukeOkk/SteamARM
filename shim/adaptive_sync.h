// _STEAMARM_VRR is an XQuartz snapshot, not proof of physical VRR scanout.
#ifndef LXRT_ADAPTIVE_SYNC_H
#define LXRT_ADAPTIVE_SYNC_H
#include <stdint.h>

static inline int lxrt_adaptive_sync_eligible(const uint32_t v[5], uint32_t context)
{
    return v && v[0] == 1 && context && v[1] == context &&
           v[2] > 0 && v[3] <= 1000000 && v[3] > v[2] &&
           (uint64_t)(v[3] - v[2]) * 1000 > v[2] && v[4] == 1;
}
#endif
