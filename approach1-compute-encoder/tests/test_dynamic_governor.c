/* bc250-encoding-decoding-fix v0.4.0 - https://github.com/simpmix/bc250-encoding-decoding-fix */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * test_dynamic_governor.c - Unit test for Dynamic Asymmetric Governor
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include "dynamic_governor.h"

static void test_governor_transitions(void)
{
    printf("[TEST] Testing dynamic governor tier transitions & hysteresis...\n");

    dynamic_governor_t gov;
    dynamic_governor_init(&gov);
    assert(dynamic_governor_get_tier(&gov) == GOV_TIER_0_GPU_FULL);

    /* 1. Low latency: stays Tier 0 */
    for (int i = 0; i < 10; i++) {
        governor_tier_t t = dynamic_governor_update(&gov, 4.5);
        assert(t == GOV_TIER_0_GPU_FULL);
    }

    /* 2. Moderate latency spike (>8ms): transitions to Tier 1 */
    for (int i = 0; i < 5; i++) {
        dynamic_governor_update(&gov, 9.5);
    }
    assert(dynamic_governor_get_tier(&gov) == GOV_TIER_1_GPU_FAST);

    /* 3. Severe latency spike (>12ms): transitions to Tier 2 (CPU offload) */
    for (int i = 0; i < 5; i++) {
        dynamic_governor_update(&gov, 13.5);
    }
    assert(dynamic_governor_get_tier(&gov) == GOV_TIER_2_CPU_OFFLOAD);

    /* 4. Single-frame emergency spike (>15.5ms): trips Tier 3 (Failover) */
    governor_tier_t emergency = dynamic_governor_update(&gov, 16.5);
    assert(emergency == GOV_TIER_3_FAILOVER);

    /* 5. Next frame drops from Tier 3 back to Tier 2 */
    governor_tier_t post_emergency = dynamic_governor_update(&gov, 13.0);
    assert(post_emergency == GOV_TIER_2_CPU_OFFLOAD);

    /* 6. Hysteresis test: lower latency (4.0ms) requires 15 stable frames to drop back to Tier 0 */
    for (int i = 0; i < 14; i++) {
        dynamic_governor_update(&gov, 4.0);
        /* Should NOT have dropped back to Tier 0 yet due to hysteresis */
        assert(dynamic_governor_get_tier(&gov) >= GOV_TIER_1_GPU_FAST);
    }
    /* 15th frame triggers step down */
    dynamic_governor_update(&gov, 4.0);
    assert(dynamic_governor_get_tier(&gov) == GOV_TIER_0_GPU_FULL);

    printf("  ✓ Governor transitions and hysteresis verified!\n");
}

int main(void)
{
    printf("========================================\n");
    printf("BC-250 Dynamic Governor Unit Test\n");
    printf("========================================\n");

    test_governor_transitions();

    printf("\nALL DYNAMIC GOVERNOR TESTS PASSED!\n");
    return 0;
}
