/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ATL_FAULT_REPORT_H
#define ATL_FAULT_REPORT_H

#include <stdint.h>

enum AtlFaultReportKind
{
    ATL_FAULT_REPORT_GATEWAY_HB_TIER2 = 0,
    ATL_FAULT_REPORT_GATEWAY_HB_TIER3,
    ATL_FAULT_REPORT_PSS_ERROR,
    ATL_FAULT_REPORT_SW_FAIL
};

void AtlFaultReport(enum AtlFaultReportKind kind,
                    uint32_t miss_count,
                    int64_t elapsed_ms);

#endif /* ATL_FAULT_REPORT_H */
