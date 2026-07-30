/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "atl_fault_report.h"

void AtlFaultReport(enum AtlFaultReportKind kind,
                    uint32_t miss_count,
                    int64_t elapsed_ms)
{
    (void)kind;
    (void)miss_count;
    (void)elapsed_ms;
}
