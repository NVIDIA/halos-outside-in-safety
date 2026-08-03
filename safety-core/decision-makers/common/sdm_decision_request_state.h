/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SDM_DECISION_REQUEST_STATE_H
#define SDM_DECISION_REQUEST_STATE_H

#include "pss_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool SdmDecisionRequestIsNoopOnly(const DecisionRequest *req)
{
    return req != 0 &&
           req->sensorDataSummarySize == 1U &&
           req->sensorDataSummary[0].event.type == PSS_STATUS_NOOP;
}

static inline bool SdmDecisionRequestContainsSwFail(const DecisionRequest *req)
{
    if (req == 0) {
        return false;
    }

    uint8_t count = req->sensorDataSummarySize;
    if (count > MAX_SENSORS_DATA_SUMMARY_SIZE) {
        count = MAX_SENSORS_DATA_SUMMARY_SIZE;
    }

    for (uint8_t i = 0U; i < count; i++) {
        if (req->sensorDataSummary[i].event.type == SW_FAIL) {
            return true;
        }
    }

    return false;
}

static inline bool SdmDecisionRequestClearsPssFaultActive(const DecisionRequest *req)
{
    return req != 0 &&
           req->pssStatus.mode != ERROR &&
           !SdmDecisionRequestIsNoopOnly(req) &&
           !SdmDecisionRequestContainsSwFail(req);
}

#ifdef __cplusplus
}
#endif

#endif /* SDM_DECISION_REQUEST_STATE_H */
