/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCM_DECISION_CONTRACT_H
#define PCM_DECISION_CONTRACT_H

#include <string>
#include <vector>

struct DvmRoiRule {
    std::string id;
    std::string type;
    bool confined = false;
};

struct DvmPairRule {
    std::string primaryType;
    std::string secondaryType;
    double warningDistance = 0.0;
};

/* Read-only snapshot of the enabled consumers. It describes required inputs,
 * never the current occupancy, object positions or decision output. */
struct DvmDecisionContract {
    bool configured = false;
    bool hasTripwireRules = false;
    std::vector<DvmRoiRule> roiRules;
    std::vector<DvmPairRule> pairRules;
};

bool loadDvmDecisionContract(const std::string& mappingPath,
                            const std::vector<std::string>& sdmConfigPaths,
                            DvmDecisionContract& out);

#endif
