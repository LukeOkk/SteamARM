#include <stdio.h>
#include <assert.h>
#include "../../shim/adaptive_sync.h"

int main(void)
{
    uint32_t v[5] = {1, 42, 6000, 20000, 1};
    assert(lxrt_adaptive_sync_eligible(v, 42));
    assert(!lxrt_adaptive_sync_eligible(v, 41));
    assert(!lxrt_adaptive_sync_eligible(v, 0));
    assert(!lxrt_adaptive_sync_eligible(0, 42));
    v[4] = 0; assert(!lxrt_adaptive_sync_eligible(v, 42));
    v[4] = 2; assert(!lxrt_adaptive_sync_eligible(v, 42));
    v[4] = 1; v[3] = 6000; assert(!lxrt_adaptive_sync_eligible(v, 42));
    v[3] = 6006; assert(!lxrt_adaptive_sync_eligible(v, 42));
    v[3] = 0; assert(!lxrt_adaptive_sync_eligible(v, 42));
    v[3] = 1000001; assert(!lxrt_adaptive_sync_eligible(v, 42));
    v[3] = 20000; v[2] = 0; assert(!lxrt_adaptive_sync_eligible(v, 42));
    v[2] = 6000; v[0] = 2; assert(!lxrt_adaptive_sync_eligible(v, 42));
    puts("Adaptive Sync eligibility: PASS (synthetic policy, not a hardware VRR test)");
    return 0;
}
