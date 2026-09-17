/* bc250-encoding-decoding-fix v0.4.0 - https://github.com/simpmix/bc250-encoding-decoding-fix */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * dynamic_governor.c - Real-time dynamic CPU/GPU load governor for BC-250
 */

#include "dynamic_governor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void dynamic_governor_init(dynamic_governor_t *gov)
{
    if (!gov) return;
    memset(gov, 0, sizeof(*gov));
    gov->tier1_threshold_ms = 8.0;
    gov->tier2_threshold_ms = 12.0;
    gov->tier3_threshold_ms = 15.5;
    gov->step_down_hysteresis = 15;
    gov->current_tier = GOV_TIER_0_GPU_FULL;
    gov->enabled = true;
    gov->forced_tier = -1;

    const char *env_enable = getenv("BC250_GOVERNOR_ENABLE");
    if (env_enable && (strcmp(env_enable, "0") == 0 || strcmp(env_enable, "false") == 0)) {
        gov->enabled = false;
    }

    const char *env_force = getenv("BC250_FORCE_TIER");
    if (env_force) {
        int ft = atoi(env_force);
        if (ft >= 0 && ft <= 3) {
            gov->forced_tier = ft;
            gov->current_tier = (governor_tier_t)ft;
        }
    }
}

governor_tier_t dynamic_governor_update(dynamic_governor_t *gov, double gpu_latency_ms)
{
    if (!gov) return GOV_TIER_0_GPU_FULL;

    if (gov->forced_tier >= 0) {
        return (governor_tier_t)gov->forced_tier;
    }

    if (!gov->enabled) {
        return GOV_TIER_0_GPU_FULL;
    }

    if (gpu_latency_ms < 0.0) {
        gpu_latency_ms = 0.0;
    }

    gov->last_latency_ms = gpu_latency_ms;

    /* Initialize or update Exponential Moving Average (EMA) with alpha=0.25 */
    if (gov->ema_latency_ms <= 0.0) {
        gov->ema_latency_ms = gpu_latency_ms;
    } else {
        gov->ema_latency_ms = 0.75 * gov->ema_latency_ms + 0.25 * gpu_latency_ms;
    }

    double metric = gov->ema_latency_ms;

    /* Emergency spike trip-wire: single frame over threshold triggers failover */
    if (gpu_latency_ms >= gov->tier3_threshold_ms) {
        gov->current_tier = GOV_TIER_3_FAILOVER;
        gov->stable_frames_count = 0;
        gov->total_failover_frames++;
        return gov->current_tier;
    }

    /* Upward tier transitions happen immediately to prevent dropped frames */
    if (metric >= gov->tier2_threshold_ms) {
        if (gov->current_tier != GOV_TIER_2_CPU_OFFLOAD) {
            gov->current_tier = GOV_TIER_2_CPU_OFFLOAD;
            gov->stable_frames_count = 0;
        }
        gov->total_offload_frames++;
        return gov->current_tier;
    } else if (metric >= gov->tier1_threshold_ms) {
        if (gov->current_tier < GOV_TIER_1_GPU_FAST) {
            gov->current_tier = GOV_TIER_1_GPU_FAST;
            gov->stable_frames_count = 0;
        }
    }

    /* Downward tier transitions require hysteresis to avoid fluttering */
    if (gov->current_tier == GOV_TIER_3_FAILOVER) {
        /* Drop from Tier 3 to Tier 2 immediately after the emergency frame */
        gov->current_tier = GOV_TIER_2_CPU_OFFLOAD;
        gov->stable_frames_count = 0;
    } else if (gov->current_tier == GOV_TIER_2_CPU_OFFLOAD) {
        if (metric < gov->tier2_threshold_ms) {
            gov->stable_frames_count++;
            if (gov->stable_frames_count >= gov->step_down_hysteresis) {
                gov->current_tier = (metric >= gov->tier1_threshold_ms) ? GOV_TIER_1_GPU_FAST : GOV_TIER_0_GPU_FULL;
                gov->stable_frames_count = 0;
            }
        } else {
            gov->stable_frames_count = 0;
        }
    } else if (gov->current_tier == GOV_TIER_1_GPU_FAST) {
        if (metric < gov->tier1_threshold_ms) {
            gov->stable_frames_count++;
            if (gov->stable_frames_count >= gov->step_down_hysteresis) {
                gov->current_tier = GOV_TIER_0_GPU_FULL;
                gov->stable_frames_count = 0;
            }
        } else {
            gov->stable_frames_count = 0;
        }
    }

    return gov->current_tier;
}

governor_tier_t dynamic_governor_get_tier(const dynamic_governor_t *gov)
{
    if (!gov) return GOV_TIER_0_GPU_FULL;
    if (gov->forced_tier >= 0) return (governor_tier_t)gov->forced_tier;
    if (!gov->enabled) return GOV_TIER_0_GPU_FULL;
    return gov->current_tier;
}

void dynamic_governor_reset(dynamic_governor_t *gov)
{
    if (!gov) return;
    gov->ema_latency_ms = 0.0;
    gov->last_latency_ms = 0.0;
    gov->stable_frames_count = 0;
    gov->current_tier = (gov->forced_tier >= 0) ? (governor_tier_t)gov->forced_tier : GOV_TIER_0_GPU_FULL;
}

void dynamic_governor_notify_failover_handled(dynamic_governor_t *gov)
{
    if (!gov) return;
    if (gov->current_tier == GOV_TIER_3_FAILOVER) {
        gov->current_tier = GOV_TIER_2_CPU_OFFLOAD;
        gov->stable_frames_count = 0;
    }
}
