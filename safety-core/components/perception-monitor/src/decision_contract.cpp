/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "decision_contract.h"
#include "pcm_log.h"
#include "NvPSFMsgCodec.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace {
std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() :
        s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1U);
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}
bool eventCode(const std::string& s) {
    if (s.compare(0U, 6U, "EVENT_") != 0 || s.size() < 7U || s.size() > 8U)
        return false;
    if (s.size() == 8U && s[6] == '0') return false;
    unsigned n = 0U;
    for (std::size_t i = 6U; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        n = n * 10U + static_cast<unsigned>(s[i] - '0');
    }
    return n <= 23U;
}
bool fail(const std::string& why) {
    PcmLogLine(PcmLogLevel::Err) << "DVM decision contract: " << why;
    return false;
}
bool baselineToken(const std::string& value) {
    if (value.empty() || value.size() > 256U) return false;
    for (unsigned char c : value) {
        if (std::isspace(c) || std::iscntrl(c) || c == '=' || c == '#') return false;
    }
    return true;
}
std::string field(const NvPSFMsgCodecMsg* msg, const char* key) {
    const auto r = NvPSFMsgCodecGetField(msg, key);
    if (r.type != NvPSF_VALUE_STRING || r.data.s == nullptr) return {};
    const std::string out(r.data.s);
    free(const_cast<char*>(r.data.s));
    return out;
}
struct FreeMessage {
    void operator()(NvPSFMsgCodecMsg* msg) const { NvPSFMsgCodecFreeMsg(msg); }
};
using Message = std::unique_ptr<NvPSFMsgCodecMsg, FreeMessage>;

/* Read the existing ATL/PXC binding formats as data, never execute them. The
 * contract selects by exact output_event, not by an application's name or by
 * every dormant rule present in a combined mapping. */
bool readSubscriptions(const std::string& path, std::set<std::string>& events) {
    std::ifstream in(path.c_str());
    if (!in) return fail("cannot read SDM config " + path);
    std::map<std::string, std::string> entries;
    for (std::string line; std::getline(in, line);) {
        line = trim(line.substr(0U, line.find('#')));
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || entries.size() >= 32U)
            return fail("invalid SDM config " + path);
        const auto key = trim(line.substr(0U, eq));
        const auto value = trim(line.substr(eq + 1U));
        if (key.empty() || value.empty() || !entries.emplace(key, value).second)
            return fail("empty or duplicate SDM key in " + path);
    }
    if (in.bad() || entries.empty()) return fail("empty or unreadable SDM config " + path);
    const char* const atl[] = {"forklift_entered", "forklift_exited", "person_entered",
                              "person_exited", "roi_violation", "roi_cleared"};
    const char* const groups[] = {"humanoid_person", "forklift_person", "transporter_person",
                                  "fourier_humanoid_person"};
    std::set<std::string> expected;
    bool isAtl = false;
    for (const auto key : atl) if (entries.count(key)) isAtl = true;
    if (isAtl) {
        expected.insert(atl, atl + 6U);
    } else {
        std::set<std::string> selected = {groups[0], groups[1], groups[2]};
        const auto selection = entries.find("pair_groups");
        if (selection != entries.end()) {
            selected.clear();
            std::istringstream parts(selection->second);
            std::string item;
            while (std::getline(parts, item, ',')) {
                item = trim(item);
                if (std::find(groups, groups + 4U, item) == groups + 4U ||
                    !selected.insert(item).second)
                    return fail("invalid pair_groups in " + path);
            }
            if (selected.empty() || selection->second.back() == ',')
                return fail("empty pair_groups in " + path);
            entries.erase(selection);
        }
        for (const auto& group : selected) {
            for (const char* tier : {"normal", "reduce", "stop"})
                expected.insert(group + "_" + tier);
        }
    }
    if (entries.size() != expected.size()) return fail("incomplete SDM binding in " + path);
    std::set<std::string> local;
    for (const auto& kv : entries) {
        if (!expected.count(kv.first) || !eventCode(kv.second) || !local.insert(kv.second).second)
            return fail("unknown, duplicate or invalid SDM binding in " + path);
    }
    for (const auto& event : local) {
        if (!events.insert(event).second) return fail("SDM configs overlap on " + event);
    }
    return true;
}

bool readRule(const NvPSFMsgCodecMsg* rule, DvmDecisionContract& out) {
    const auto source = lower(field(rule, "message_source"));
    const auto kind = lower(field(rule, "alert_type"));
    const auto type = lower(field(rule, "object_type"));
    const auto id = field(rule, "rule_id");
    const auto restricted = lower(field(rule, "restricted_area_violation"));
    const auto confined = lower(field(rule, "confined_area_violation"));
    if (source == "mdx-events" && kind == "tripwire") {
        if (!baselineToken(id) || !baselineToken(type) || type == "any")
            return fail("tripwire contract requires a concrete identity and class");
        out.hasTripwireRules = true;
        return true;
    }
    if (source != "mdx-frames") return fail("selected rule has an unsupported message source");
    if (kind == "roi") {
        if (!baselineToken(id) || !baselineToken(type) || type == "any" || restricted.empty() == confined.empty())
            return fail("ROI rule requires identity, class and exactly one violation filter");
        const auto& filter = restricted.empty() ? confined : restricted;
        if (filter != "true" && filter != "false" && filter != "any")
            return fail("ROI rule has an invalid violation filter");
        DvmRoiRule roi;
        roi.id = id; roi.type = type; roi.confined = restricted.empty();
        const auto same = [&](const DvmRoiRule& r) {
            return r.id == roi.id && r.type == roi.type && r.confined == roi.confined;
        };
        if (std::find_if(out.roiRules.begin(), out.roiRules.end(), same) == out.roiRules.end())
            out.roiRules.push_back(roi);
        return true;
    }
    if (kind != "social_distancing" ||
        !NvPSFMsgCodecGetFieldPresence(rule, "proximity_violation"))
        return fail("selected frame rule is outside the supported ROI/proximity contract");
    auto primary = lower(field(rule, "object_type_primary"));
    if (primary.empty()) primary = type;
    auto secondary = lower(field(rule, "object_type_secondary"));
    if (secondary.empty()) secondary = primary;
    if (!baselineToken(primary) || !baselineToken(secondary) || primary == "any" || secondary == "any" ||
        !baselineToken(id) || !restricted.empty() || !confined.empty() ||
        !field(rule, "social_distancing_violation").empty())
        return fail("invalid proximity pair rule");
    const auto violation = NvPSFMsgCodecGetField(rule, "proximity_violation");
    if (violation.type != NvPSF_VALUE_BOOL) return fail("invalid proximity selector");
    const bool hasDistance = NvPSFMsgCodecGetFieldPresence(rule, "distance_threshold_meters");
    double distance = 0.0;
    if (violation.data.b) {
        const auto value = NvPSFMsgCodecGetField(rule, "distance_threshold_meters");
        if (!hasDistance || value.type != NvPSF_VALUE_DOUBLE ||
            !std::isfinite(value.data.d) || value.data.d <= 0.0)
            return fail("invalid proximity distance threshold");
        distance = value.data.d;
    } else if (hasDistance) {
        return fail("NORMAL proximity rule must not supply a distance threshold");
    }
    /* MDX pairs are unordered; canonicalize for stable baseline comparison. */
    if (secondary < primary) std::swap(primary, secondary);
    for (auto& pair : out.pairRules) {
        if (pair.primaryType == primary && pair.secondaryType == secondary) {
            pair.warningDistance = std::max(pair.warningDistance, distance);
            return true;
        }
    }
    DvmPairRule pair;
    pair.primaryType = primary; pair.secondaryType = secondary; pair.warningDistance = distance;
    out.pairRules.push_back(pair);
    return true;
}
} // namespace

bool loadDvmDecisionContract(const std::string& mappingPath,
                            const std::vector<std::string>& sdmConfigPaths,
                            DvmDecisionContract& out) {
    out = DvmDecisionContract{};
    if (mappingPath.empty()) {
        if (!sdmConfigPaths.empty()) return fail("--sdm-config requires --event-mapping");
        PcmLogLine(PcmLogLevel::Warning)
            << "DVM: no current event mapping; LEARN records camera/timing only. "
            << "ACTIVE may use stored rules but cannot verify current SDM bindings";
        return true;
    }
    if (sdmConfigPaths.size() > 24U) return fail("too many SDM configs");
    std::set<std::string> selected;
    for (const auto& path : sdmConfigPaths) if (!readSubscriptions(path, selected)) return false;
    if (sdmConfigPaths.empty()) {
        PcmLogLine(PcmLogLevel::Warning)
            << "DVM: no SDM subscription configs supplied; checking all rules in the event mapping";
    }
    NvPSFMsgCodecMsg* raw = nullptr;
    const auto decoded = NvPSFMsgCodecDecodeFromFile(mappingPath.c_str(), NvPSF_MSG_EVENT_MAPPING, &raw);
    Message mapping(raw);
    if (decoded != NvPSFMSGCODEC_SUCCESS || raw == nullptr) return fail("cannot decode mapping " + mappingPath);
    const int count = NvPSFMsgCodecGetRepeatedCount(mapping.get(), "rules");
    if (count <= 0 || count > 4096) return fail("mapping has no rules or too many rules");
    DvmDecisionContract candidate;
    candidate.configured = true;
    std::set<std::string> found;
    for (int i = 0; i < count; ++i) {
        const auto path = "rules[" + std::to_string(i) + "]";
        raw = nullptr;
        if (NvPSFMsgCodecGetSubMsg(mapping.get(), path.c_str(), &raw) != NvPSFMSGCODEC_SUCCESS || !raw)
            return fail("cannot read mapping rule");
        Message rule(raw);
        const auto event = field(rule.get(), "output_event");
        if (!eventCode(event)) return fail("mapping output_event must be EVENT_0..EVENT_23");
        if (!selected.empty() && !selected.count(event)) continue;
        found.insert(event);
        if (!readRule(rule.get(), candidate)) {
            return fail("unsupported or invalid selected rule " + std::to_string(i) + " (" + event + ")");
        }
    }
    if (!selected.empty() && found != selected) return fail("SDM subscriptions missing from the event mapping");
    for (const auto& pair : candidate.pairRules) {
        if (pair.warningDistance <= 0.0)
            return fail("selected proximity pair has no warning or critical threshold");
    }
    std::sort(candidate.roiRules.begin(), candidate.roiRules.end(), [](const DvmRoiRule& a, const DvmRoiRule& b) {
        return a.id != b.id ? a.id < b.id : (a.type != b.type ? a.type < b.type : a.confined < b.confined);
    });
    std::sort(candidate.pairRules.begin(), candidate.pairRules.end(), [](const DvmPairRule& a, const DvmPairRule& b) {
        return a.primaryType != b.primaryType ? a.primaryType < b.primaryType : a.secondaryType < b.secondaryType;
    });
    out = std::move(candidate);
    PcmLogLine(PcmLogLevel::Info) << "DVM decision contract: " << out.roiRules.size()
        << " ROI rule(s), " << out.pairRules.size() << " proximity pair(s)";
    if (out.hasTripwireRules) PcmLogLine(PcmLogLevel::Warning)
        << "DVM: tripwire rules use mdx-events; this monitor validates mdx-frames only";
    return true;
}
