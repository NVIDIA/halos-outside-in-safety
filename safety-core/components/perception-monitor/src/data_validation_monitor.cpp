/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "data_validation_monitor.h"

#include "pcm_fault_aggregator.h"
#include "pcm_log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>

namespace {

/* Generous timing tolerance is learned independently of the decision contract.
 * Short disturbances still pass through per-camera window voting and the
 * existing aggregator debounce. No image-quality score is calculated here. */
static constexpr std::int64_t kMinimumFrameAgeMs = 2000;
static constexpr std::int64_t kMaximumFrameAgeMs = 60000;
static constexpr std::size_t kMaxLearnSamples = 4096U;
static constexpr std::size_t kMinLearnWindows = 2U;

/* How long the frame source may block per poll before we re-check the stop flag. */
static constexpr int kFramePollTimeoutMs = 500;

/* ACTIVE: count bad/total frames separately for each camera over this window.
 * ONE verdict is reported to the aggregator, bad if any camera reaches the
 * configured bad-frame percentage or a source/frame-rate fault occurred. */
static constexpr int kDvmWindowMs = 1000;

/* ACTIVE frame-rate check: a camera already sending frames is
 * "too low" for a window if it delivered fewer than this fraction of its learned
 * rate (0 frames included). PLACEHOLDER policy knob (not safety-validated); the
 * aggregator's configured consecutive-bad rule debounces a one-off empty window. */
static constexpr double kDvmMinFrameRateRatio = 0.5;

/* ACTIVE: distinct unknown camera ids warned about, bounded so a stream of
 * garbage ids cannot grow memory or flood the log. */
static constexpr std::size_t kMaxLoggedUnknownSensors = 16U;

/* Folder that older launchers used for learned PCM baselines (the old default
 * /opt/nvidia/psf/bin/pcm_baseline.cfg). */
static constexpr const char* kLegacyBaselineDir = "/opt/nvidia/psf/bin";

/* Inclusive bad/total >= percent/100. The configured percent is validated in
 * 1..100 before ACTIVE starts. Split the product to avoid overflow even for
 * size_t-sized counts; ceil(total * percent / 100) stays in 1..total. */
static bool percentageReached(std::size_t bad, std::size_t total, int percent)
{
    if (total == 0U) return false;
    const std::size_t p = static_cast<std::size_t>(percent);
    const std::size_t required = (total / 100U) * p +
                                ((total % 100U) * p + 99U) / 100U;
    return bad >= required;
}

static std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

/* Split baseline rule fields on runs of whitespace. */
static std::vector<std::string> splitWhitespace(const std::string& s)
{
    std::vector<std::string> out;
    std::size_t i = 0U;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        const std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

static std::string trim(const std::string& in)
{
    const std::size_t first = in.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string::npos) return std::string();
    const std::size_t last = in.find_last_not_of(" \t\r\n\f\v");
    return in.substr(first, last - first + 1U);
}

/* The configured path denotes a directory. New LEARN runs atomically publish
 * its whole camera set in pcm_baseline.cfg; legacy per-camera files are read
 * only when that bundle is absent. */
static constexpr const char* kBaselineSetName = "pcm_baseline.cfg";
static constexpr const char* kBaselineSetHeader = "PCM_BASELINE_SET_V1";
static std::string baselineDirOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return std::string(".");
    if (slash == 0U) return std::string("/");
    return path.substr(0U, slash);
}

/* Write the whole buffer to @p fd, retrying short writes and EINTR. */
static bool writeAll(int fd, const char* data, std::size_t len)
{
    std::size_t off = 0U;
    while (off < len) {
        const ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) { errno = EIO; return false; }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

/* Replace <dirFd>/@p name with @p content atomically: write a new temporary file in
 * the same directory, flush it, then rename it over the target. Until the rename the
 * previous file is untouched, so a failed or interrupted write can never leave a
 * learned baseline empty or partial. @p displayPath is only for diagnostics.
 * On failure the temporary file is removed and false is returned (after logging). */
static bool replaceFileAtomically(int dirFd, const std::string& displayPath,
                                  const std::string& name, const std::string& content)
{
    /* Keep refusing a symlink or other non-regular file at the target name, as the
     * earlier in-place writer did (O_NOFOLLOW). renameat() replaces such an entry
     * rather than writing through it, so this check fails loudly; it is not what
     * keeps the write safe. */
    struct stat existing;
    const int inspected = ::fstatat(dirFd, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW);
    if ((inspected == 0 && !S_ISREG(existing.st_mode)) ||
        (inspected != 0 && errno != ENOENT)) {
        PcmLogLine(PcmLogLevel::Err) << "DVM: baseline write failed: " << displayPath
                                     << " exists and is not a regular file";
        return false;
    }

    /* A unique hidden temporary file is ignored by ACTIVE. O_EXCL avoids
     * following links and never overwrites another concurrent learner's file. */
    static std::atomic<unsigned long> sequence{0UL};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string tmpName = "." + name + "." + std::to_string(static_cast<long>(::getpid())) +
        "." + std::to_string(stamp) + "." + std::to_string(sequence.fetch_add(1UL)) + ".tmp";
    const int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
    const int fd = ::openat(dirFd, tmpName.c_str(), flags, 0644);
    if (fd < 0) {
        PcmLogLine(PcmLogLevel::Err) << "DVM: baseline write failed: cannot create a temporary "
                                     << "file for " << displayPath << " (" << std::strerror(errno)
                                     << "); the previous camera set is unchanged";
        return false;
    }

    /* fchmod the fd directly so 0644 is pinned with no chmod-by-path TOCTOU, and
     * fsync before the rename so the new content is durable when it becomes visible. */
    const char* step = nullptr;
    int error = 0;
    if (::fchmod(fd, 0644) != 0) {
        step = "set permissions for";
        error = errno;
    } else if (!writeAll(fd, content.data(), content.size())) {
        step = "write";
        error = errno;
    } else if (::fsync(fd) != 0) {
        step = "flush";
        error = errno;
    }
    if (::close(fd) != 0 && step == nullptr) {
        step = "close";
        error = errno;
    }
    if (step == nullptr && ::renameat(dirFd, tmpName.c_str(), dirFd, name.c_str()) != 0) {
        step = "replace";
        error = errno;
    }
    if (step != nullptr) {
        if (::unlinkat(dirFd, tmpName.c_str(), 0) != 0) {
            PcmLogLine(PcmLogLevel::Warning) << "DVM: could not remove temporary baseline file "
                                             << tmpName << " (" << std::strerror(errno) << ")";
        }
        PcmLogLine(PcmLogLevel::Err) << "DVM: baseline write failed: cannot " << step << " "
                                     << displayPath << " (" << std::strerror(error)
                                     << "); the previous camera set is unchanged";
        return false;
    }
    return true;
}

static bool publishBaselineSet(const std::string& directory,
                               const std::vector<std::pair<std::string, std::string>>& contents)
{
    /* A single rename is the commit point. The old bundle is untouched until
     * every camera record has been written, flushed and closed successfully. */
    std::ostringstream bundle;
    bundle << kBaselineSetHeader << "\ncameras = " << contents.size() << "\n";
    for (const auto& content : contents) bundle << content.second;
    const int dirFd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirFd < 0) {
        PcmLogLine(PcmLogLevel::Err) << "DVM: baseline write failed: cannot open directory " << directory;
        return false;
    }
    /* Discover unsupported directory flushes before the commit point. */
    if (::fsync(dirFd) != 0) {
        const int error = errno;
        if (::close(dirFd) != 0) {
            PcmLogLine(PcmLogLevel::Warning) << "DVM: cannot close baseline directory";
        }
        PcmLogLine(PcmLogLevel::Err) << "DVM: cannot flush baseline directory (" << std::strerror(error)
            << "); the previous camera set is unchanged";
        return false;
    }
    const bool published = replaceFileAtomically(dirFd, directory + "/" + kBaselineSetName,
                                                  kBaselineSetName, bundle.str());
    if (published && ::fsync(dirFd) != 0) {
        /* The rename already committed. ACTIVE sees the complete new set;
         * returning false here would incorrectly promise the old set. */
        PcmLogLine(PcmLogLevel::Warning) << "DVM: new baseline camera set published, but directory flush failed ("
            << std::strerror(errno) << "); durability across power loss is not confirmed";
    }
    if (::close(dirFd) != 0) {
        PcmLogLine(PcmLogLevel::Warning) << "DVM: cannot close baseline directory (" << std::strerror(errno) << ")";
    }
    PcmLogLine(published ? PcmLogLevel::Info : PcmLogLevel::Err) << "DVM: baseline: "
        << (published ? "published " : "failed publishing ") << contents.size() << " camera(s) in "
        << directory << "/" << kBaselineSetName
        << "; bbox/confidence/occupancy are not fault baselines";
    return published;
}

/* sensorId arrives on the (lower-integrity) mdx-frames stream, so it must be
 * sanitized before being spliced into a filesystem path: keep an allowlist, map
 * anything else to '_', and bound the length. The RAW sensorId is still written
 * inside the file (known_sensor=) and is what ACTIVE matches on, so this lossy
 * mapping only ever affects the filename, never the whitelist comparison. */
static std::string sanitizeSensorForFilename(const std::string& sensor)
{
    static const std::size_t kMaxLen = 128U;
    std::string out;
    out.reserve(std::min(sensor.size(), kMaxLen));
    for (const char c : sensor) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
        out.push_back(safe ? c : '_');
        if (out.size() >= kMaxLen) break;
    }
    if (out.empty()) out = "_";
    return out;
}

/* The raw sensorId is written verbatim inside the baseline (known_sensor=) and
 * trusted by ACTIVE's loadSensorsFromFile. A newline / carriage-return / '=' or any
 * control char in the id would inject extra parsed keys or lines, so such an id must
 * not be written. (The lossy filename sanitization does not protect the file body.) */
static bool isSensorIdSafeForBaseline(const std::string& sensor)
{
    for (const char c : sensor) {
        if (c == '\n' || c == '\r' || c == '=') return false;
        if (std::iscntrl(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

/* Legacy camera filename also detects ambiguous names before publishing a set. */
static std::string perSensorBaselineFilename(const std::string& sensor)
{
    return std::string("pcm_") + sanitizeSensorForFilename(sensor) + "_baseline.cfg";
}

/* True if @p name matches the per-sensor baseline convention pcm_<x>_baseline.cfg
 * (with a non-empty <x>). */
static bool isPerSensorBaselineFile(const std::string& name)
{
    static const std::string kPrefix = "pcm_";
    static const std::string kSuffix = "_baseline.cfg";
    if (name.size() <= kPrefix.size() + kSuffix.size()) return false;
    if (name.compare(0U, kPrefix.size(), kPrefix) != 0) return false;
    return name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

static bool parseNumber(const std::string& value, double& out)
{
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    out = std::strtod(value.c_str(), &end);
    return errno != ERANGE && end != value.c_str() && *end == '\0' && std::isfinite(out);
}

static bool safeToken(const std::string& value)
{
    return !value.empty() && value.size() <= 256U &&
        value.find_first_of(" \t\r\n=#") == std::string::npos && isSensorIdSafeForBaseline(value);
}

static std::vector<std::string> contractLines(const DvmDecisionContract& contract)
{
    std::vector<std::string> result;
    for (const auto& r : contract.roiRules) {
        result.push_back("roi_rule = " + r.id + " " + r.type +
                         (r.confined ? " confined" : " restricted"));
    }
    for (const auto& r : contract.pairRules) {
        std::ostringstream line;
        line.precision(17);
        line << "pair_rule = " << r.primaryType << " " << r.secondaryType << " " << r.warningDistance;
        result.push_back(line.str());
    }
    std::sort(result.begin(), result.end());
    return result;
}

/* Parse a complete candidate before publishing any of its values. Legacy
 * files retain identity/rate coverage; observed class/box ranges no longer
 * define which objects are allowed in a normal scene. */
static bool loadSensorsFromStream(std::istream& file, const std::string& path,
                                std::unordered_set<std::string>& knownSensors,
                                std::unordered_map<std::string, double>& expectedFps,
                                std::unordered_map<std::string, DvmBaseline>& baselines)
{
    DvmBaseline baseline;
    std::string sensor;
    std::unordered_set<std::string> seen;
    bool invalid = false;
    bool hasAge = false;
    bool hasContract = false;
    bool hasProximity = false;
    bool hasThreshold = false;
    std::string line;
    while (std::getline(file, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || (t.front() == '[' && t.back() == ']')) continue;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) { invalid = true; continue; }
        const std::string key = trim(t.substr(0U, eq));
        const std::string value = trim(t.substr(eq + 1U));
        if (key == "class") continue; // retired legacy diagnostic
        if (key != "roi_rule" && key != "pair_rule" && !seen.insert(key).second) invalid = true;
        double number = 0.0;
        if (key == "schema_version") {
            baseline.modern = true;
            if (value != "2") invalid = true;
        } else if (key == "known_sensor") {
            sensor = value;
            if (sensor.empty() || !isSensorIdSafeForBaseline(sensor)) invalid = true;
        } else if (key == "expected_fps") {
            if (!parseNumber(value, number) || number <= 0.0 || number > 10000.0) invalid = true;
            else baseline.fps = number;
        } else if (key == "max_frame_age_ms") {
            hasAge = true;
            if (!parseNumber(value, number) || number < 0.0 || number > kMaximumFrameAgeMs ||
                std::floor(number) != number) invalid = true;
            else baseline.maxFrameAgeMs = static_cast<std::int64_t>(number);
        } else if (key == "decision_contract") {
            hasContract = true;
            if (value != "0" && value != "1") invalid = true;
            baseline.contract.configured = value == "1";
        } else if (key == "proximity_required") {
            hasProximity = true;
            if (value != "0" && value != "1") invalid = true;
            baseline.proximityRequired = value == "1";
        } else if (key == "proximity_threshold") {
            hasThreshold = true;
            if (!parseNumber(value, number) || number < 0.0) invalid = true;
            else baseline.proximityThreshold = number;
        } else if (key == "roi_rule") {
            const auto tok = splitWhitespace(value);
            if (tok.size() != 3U || !safeToken(tok[0]) || !safeToken(tok[1]) ||
                (tok[2] != "restricted" && tok[2] != "confined")) invalid = true;
            else {
                DvmRoiRule rule; rule.id = tok[0]; rule.type = toLower(tok[1]); rule.confined = tok[2] == "confined";
                baseline.contract.roiRules.push_back(rule);
            }
        } else if (key == "pair_rule") {
            const auto tok = splitWhitespace(value);
            if (tok.size() != 3U || !safeToken(tok[0]) || !safeToken(tok[1]) ||
                !parseNumber(tok[2], number) || number <= 0.0) invalid = true;
            else {
                DvmPairRule rule; rule.primaryType = toLower(tok[0]); rule.secondaryType = toLower(tok[1]);
                rule.warningDistance = number; baseline.contract.pairRules.push_back(rule);
            }
        } else invalid = true;
        if (baseline.contract.roiRules.size() + baseline.contract.pairRules.size() > 256U) {
            invalid = true; break;
        }
    }
    if (file.bad() || sensor.empty() || baseline.fps <= 0.0) invalid = true;
    if (baseline.modern && (!hasAge || !hasContract || !hasProximity || !hasThreshold)) invalid = true;
    if (!baseline.modern && (hasAge || hasContract || hasProximity || hasThreshold ||
        !baseline.contract.roiRules.empty() || !baseline.contract.pairRules.empty())) invalid = true;
    if (baseline.modern && (baseline.proximityRequired != !baseline.contract.pairRules.empty() ||
        (!baseline.contract.configured && (!baseline.contract.roiRules.empty() || !baseline.contract.pairRules.empty())))) invalid = true;
    for (const auto& rule : baseline.contract.pairRules) {
        if (baseline.proximityThreshold < rule.warningDistance) invalid = true;
    }
    if (knownSensors.count(sensor) != 0U) invalid = true;
    if (invalid) {
        PcmLogLine(PcmLogLevel::Warning) << "DVM: malformed or conflicting baseline " << path
                                         << "; not used (re-run LEARN)";
        return false;
    }
    knownSensors.insert(sensor);
    expectedFps[sensor] = baseline.fps;
    baselines[sensor] = baseline;
    if (!baseline.modern) {
        PcmLogLine(PcmLogLevel::Warning) << "DVM: legacy baseline for " << sanitizeSensorForFilename(sensor)
            << "; identity/rate checks only, re-run LEARN for decision-data and freshness checks";
    } else if (baseline.maxFrameAgeMs == 0) {
        PcmLogLine(PcmLogLevel::Warning) << "DVM: baseline has no observation timestamps for "
            << sanitizeSensorForFilename(sensor) << "; observation age is not checked";
    }
    return true;
}

static bool loadSensorsFromFile(const std::string& path,
                                std::unordered_set<std::string>& knownSensors,
                                std::unordered_map<std::string, double>& expectedFps,
                                std::unordered_map<std::string, DvmBaseline>& baselines)
{
    std::ifstream file(path.c_str());
    return file.is_open() && loadSensorsFromStream(file, path, knownSensors, expectedFps, baselines);
}

/* One open pins one published inode even if LEARN replaces the bundle during
 * this read. Parse into temporary maps, then publish the entire set or nothing. */
static bool loadBaselineSet(const std::string& path,
                            std::unordered_set<std::string>& knownSensors,
                            std::unordered_map<std::string, double>& expectedFps,
                            std::unordered_map<std::string, DvmBaseline>& baselines)
{
    std::ifstream file(path.c_str());
    std::string line;
    if (!std::getline(file, line) || line != kBaselineSetHeader) return false;
    if (!std::getline(file, line) || line.compare(0U, 10U, "cameras = ") != 0) return false;
    const std::string count = line.substr(10U);
    std::unordered_set<std::string> sensors;
    std::unordered_map<std::string, double> fps;
    std::unordered_map<std::string, DvmBaseline> candidates;
    std::string record;
    bool started = false;
    auto loadRecord = [&]() {
        std::istringstream input(record);
        return loadSensorsFromStream(input, path, sensors, fps, candidates);
    };
    while (std::getline(file, line)) {
        const std::string t = trim(line);
        if (t == "[dvm_baseline]") {
            if (started && !loadRecord()) return false;
            started = true;
            record.clear();
        } else if (!started && !t.empty() && t[0] != '#') return false;
        if (started) record += line + "\n";
    }
    if (file.bad() || !started || !loadRecord() || sensors.empty() ||
        count != std::to_string(sensors.size())) return false;
    /* A LEARN run uses one decision contract for every camera. Reject a damaged
     * set with inconsistent contracts before ACTIVE could retain only a subset. */
    const auto& contract = candidates.begin()->second.contract;
    const auto rules = contractLines(contract);
    for (const auto& candidate : candidates) {
        if (!candidate.second.modern || candidate.second.contract.configured != contract.configured ||
            contractLines(candidate.second.contract) != rules) return false;
    }
    knownSensors.swap(sensors);
    expectedFps.swap(fps);
    baselines.swap(candidates);
    return true;
}

static void loadDvmBaselineDir(const std::string& dir,
                               std::unordered_set<std::string>& knownSensors,
                               std::unordered_map<std::string, double>& expectedFps,
                               std::unordered_map<std::string, DvmBaseline>& baselines)
{
    const std::string bundle = dir + "/" + kBaselineSetName;
    struct stat st;
    if (::lstat(bundle.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode) || !loadBaselineSet(bundle, knownSensors, expectedFps, baselines)) {
            PcmLogLine(PcmLogLevel::Warning) << "DVM: invalid or unreadable baseline camera set "
                << bundle << "; no legacy/default fallback, re-run LEARN";
        }
        return;
    }
    if (errno != ENOENT) {
        PcmLogLine(PcmLogLevel::Warning) << "DVM: cannot inspect baseline camera set " << bundle
            << "; no legacy/default fallback";
        return;
    }
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return;
    std::vector<std::string> names;
    for (struct dirent* ent = readdir(d); ent != nullptr; ent = readdir(d)) names.push_back(ent->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    const std::string suffix = ".default";
    for (const auto& name : names) {
        const bool isDefault = name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        const std::string learnedName = isDefault ? name.substr(0U, name.size() - suffix.size()) : name;
        if (!isPerSensorBaselineFile(learnedName)) continue;
        if (isDefault) {
            std::ifstream learned(dir + "/" + learnedName);
            if (learned.is_open()) continue;
            PcmLogLine(PcmLogLevel::Info) << "DVM: learned baseline '" << learnedName
                << "' unavailable; using reference default '" << name << "'";
        }
        loadSensorsFromFile(dir + "/" + name, knownSensors, expectedFps, baselines);
    }
}

static bool numericId(const std::string& value, std::uint32_t& id)
{
    if (value.empty()) return false;
    std::uint64_t n = 0;
    for (const unsigned char c : value) {
        if (c < '0' || c > '9') return false;
        n = n * 10U + static_cast<unsigned>(c - '0');
        if (n > std::numeric_limits<std::uint32_t>::max()) return false;
    }
    id = static_cast<std::uint32_t>(n);
    return true;
}

static bool usableCoordinate(const DvmObject& object)
{
    const double limit = std::numeric_limits<float>::max();
    return object.hasCoordinate && std::isfinite(object.x) && std::isfinite(object.y) &&
        std::isfinite(object.z) && std::fabs(object.x) <= limit && std::fabs(object.y) <= limit &&
        std::fabs(object.z) <= limit;
}

/* Match the ROI identifiers consumed by SafetyEventReporter: case-insensitive
 * full match, or the configured case-sensitive id followed by ':<type>'. */
static bool matchesRoiId(const std::string& configured, const std::string& observed)
{
    return toLower(configured) == toLower(observed) ||
        (observed.size() > configured.size() &&
         observed.compare(0U, configured.size(), configured) == 0 &&
         observed[configured.size()] == ':');
}

/* An invalid field matters only if an enabled consumer needs it, including
 * NORMAL/CLEAR candidates. Never exclude a pair using its suspect distance. */
static std::string decisionFault(const DvmFrame& frame, const DvmDecisionContract& contract,
                                  bool requireProximity)
{
    if (!contract.configured) return {};
    std::unordered_map<std::uint32_t, std::vector<const DvmObject*>> byId;
    std::unordered_map<std::uint32_t, std::size_t> proximityIds;
    std::unordered_set<std::string> types;
    for (const auto& object : frame.objects) {
        types.insert(toLower(object.type));
        std::uint32_t id = 0;
        if (numericId(object.id, id)) {
            byId[id].push_back(&object);
            if (!object.type.empty() && usableCoordinate(object)) ++proximityIds[id];
        }
    }
    for (const auto& roi : frame.rois) {
        for (const auto& rule : contract.roiRules) {
            if (!matchesRoiId(rule.id, roi.id) || toLower(roi.type) != rule.type) continue;
            const bool present = rule.confined ? roi.hasConfined : roi.hasRestricted;
            const std::string& value = rule.confined ? roi.confined : roi.restricted;
            if (!present || (value != "true" && value != "false")) return "roi-violation-value";
            /* Restricted clears may be synthesized without objects. Confined
             * entries still need valid references even for their clear event. */
            if (value == "false" && !rule.confined) continue;
            if (roi.objectIds.empty()) return "roi-object-reference";
            std::unordered_set<std::uint32_t> references;
            for (const auto& reference : roi.objectIds) {
                std::uint32_t id = 0;
                if (!numericId(reference, id) || !references.insert(id).second) return "roi-object-reference";
                const auto found = byId.find(id);
                if (found == byId.end() || found->second.size() != 1U ||
                    found->second.front()->id != reference ||
                    toLower(found->second.front()->type) != rule.type) return "roi-object-reference";
            }
        }
    }
    if (contract.pairRules.empty()) return {};
    if (!frame.proximity.present) return requireProximity ? "proximity-metadata-missing" : std::string();
    const auto& proximity = frame.proximity;
    if (!std::isfinite(proximity.threshold) || proximity.threshold <= 0.0 || proximity.detections < 0 ||
        !proximity.hasViolation || (proximity.violation != "true" && proximity.violation != "false") ||
        ((proximity.detections == 0) != (proximity.violation == "false"))) return "proximity-metadata-invalid";
    for (const auto& rule : contract.pairRules) {
        /* Match MDX's exact comparison: even a slightly smaller analytics range
         * makes it skip this pair group. Equal or larger ranges remain usable. */
        if (proximity.threshold < rule.warningDistance) return "proximity-range-too-small";
        const bool pairPresent = types.count(rule.primaryType) != 0U && types.count(rule.secondaryType) != 0U;
        if (!pairPresent) continue;
        for (const auto& object : frame.objects) {
            const std::string type = toLower(object.type);
            if (type != rule.primaryType && type != rule.secondaryType) continue;
            std::uint32_t id = 0;
            if (!numericId(object.id, id)) return "proximity-object-id";
            if (!usableCoordinate(object)) return "proximity-coordinate";
            if (proximityIds[id] != 1U) return "proximity-duplicate-id";
        }
    }
    return {};
}

static std::int64_t wallTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static bool usableTimestamp(const DvmFrame& frame)
{
    return frame.timestampValid && frame.timestampMs > 0 && frame.timestampMs <= 253402300799999LL;
}

/* ACTIVE: older launchers learned into kLegacyBaselineDir. If baselines are read
 * from another folder but learned files still sit there, they are silently
 * ignored (and reference defaults may be used instead), so say so. */
static void warnAboutLegacyBaselines(const std::string& dir)
{
    if (dir == kLegacyBaselineDir) return; /* that folder is the one in use */
    DIR* d = opendir(kLegacyBaselineDir);
    if (d == nullptr) return;
    std::size_t count = 0U;
    for (struct dirent* ent = readdir(d); ent != nullptr; ent = readdir(d)) {
        if (isPerSensorBaselineFile(ent->d_name) || ent->d_name == std::string(kBaselineSetName)) ++count;
    }
    closedir(d);
    if (count > 0U) {
        PcmLogLine(PcmLogLevel::Warning)
            << "DVM: " << count << " learned baseline file(s) found in the old folder "
            << kLegacyBaselineDir << "; this version reads " << dir
            << ". Move them there or re-run --mode LEARN, otherwise they are ignored";
    }
}

} // namespace

IFrameSource::Status NullFrameSource::poll(DvmFrame& /*out*/, int timeoutMs)
{
    /* No data source wired yet: idle for the timeout so the DVM thread still
     * honors its stop flag promptly, then report no frame. */
    if (timeoutMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
    }
    return Status::Timeout;
}

DataValidationMonitor::DataValidationMonitor(DvmMode mode,
                                             std::string baselinePath,
                                             PcmFaultAggregator* aggregator,
                                             std::unique_ptr<IFrameSource> source,
                                             int badObjectsPercent, int badFramesPercent,
                                             int startupGraceMs,
                                             const DvmDecisionContract& contract)
    : mode_(mode)
    /* The canonical pcm_baseline.cfg bundle lives in this directory, regardless
     * of the filename supplied to --baseline-config (the historical interface
     * uses only its directory). Absent a bundle, ACTIVE reads legacy per-camera
     * files/defaults. There is no world-writable fallback directory. */
    , dvmBaselineDir_(baselineDirOf(baselinePath))
    , aggregator_(aggregator)
    , source_(std::move(source))
    , badObjectsPercent_(badObjectsPercent)
    , badFramesPercent_(badFramesPercent)
    , startupGraceMs_(startupGraceMs)
    , configuredContract_(contract)
{
    if (mode_ == DvmMode::Active) {
        warnAboutLegacyBaselines(dvmBaselineDir_);
        loadDvmBaselineDir(dvmBaselineDir_, knownSensors_, expectedFps_, baselines_);
        if (configuredContract_.configured) {
            for (auto it = baselines_.begin(); it != baselines_.end();) {
                const auto& b = it->second;
                if (b.modern && (!b.contract.configured ||
                    contractLines(b.contract) != contractLines(configuredContract_))) {
                    PcmLogLine(PcmLogLevel::Warning) << "DVM: decision mapping changed for "
                        << sanitizeSensorForFilename(it->first) << "; baseline not used, re-run LEARN";
                    knownSensors_.erase(it->first); expectedFps_.erase(it->first);
                    it = baselines_.erase(it);
                } else ++it;
            }
        }
        sensorCheckActive_ = !knownSensors_.empty();
        if (sensorCheckActive_) {
            PcmLogLine(PcmLogLevel::Info) << "DVM: baseline loaded, cameras="
                                          << knownSensors_.size() << ", decision-data profiles="
                                          << baselines_.size() << " (from "
                                          << dvmBaselineDir_ << ")";
        } else {
            /* No learned/default baseline: nothing to validate against, so the
             * caller turns data validation off (a setup problem, not a fault). */
            PcmLogLine(PcmLogLevel::Warning)
                << "DVM: no PCM baseline found in " << dvmBaselineDir_
                << " (camera-set bundle or legacy per-camera files); data validation is OFF. "
                << "Run --mode LEARN or install the reference baselines";
        }
    }
}

DataValidationMonitor::~DataValidationMonitor()
{
    stop();
}

void* DataValidationMonitor::runLoopThunk(void* self)
{
    static_cast<DataValidationMonitor*>(self)->runLoop();
    return nullptr;
}

bool DataValidationMonitor::start()
{
    if (mode_ == DvmMode::Active &&
        (badObjectsPercent_ < 1 || badObjectsPercent_ > 100 ||
         badFramesPercent_ < 1 || badFramesPercent_ > 100 || startupGraceMs_ < 1)) {
        PcmLogLine(PcmLogLevel::Err) << "DVM: ACTIVE requires object/frame percentages in "
                                     << "1..100 and a positive startup grace";
        return false;
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return true;
    }
    const int rc = pthread_create(&thread_, nullptr,
                                  &DataValidationMonitor::runLoopThunk, this);
    if (rc != 0) {
        running_.store(false);
        PcmLogLine(PcmLogLevel::Err) << "DVM: failed to start validation thread: "
                                     << std::strerror(rc);
        return false;
    }
    threadValid_ = true;
    PcmLogLine(PcmLogLevel::Info) << "DVM: DataValidationMonitor started";
    if (mode_ == DvmMode::Active) {
        PcmLogLine(PcmLogLevel::Info) << "DVM: thresholds: bad_objects_percent="
                                      << badObjectsPercent_ << " (compatibility only), bad_frames_percent="
                                      << badFramesPercent_ << " (per camera), startup_grace_ms="
                                      << startupGraceMs_;
    }
    return true;
}

void DataValidationMonitor::stop()
{
    running_.store(false, std::memory_order_release);
    if (threadValid_) {
        pthread_join(thread_, nullptr);
        threadValid_ = false;
    }
}

void DataValidationMonitor::runLoop()
{
    bool countOverflow = false;
    bool sourceUnavailableInWindow = false;
    bool invalidDataPending = false;
    bool invalidDataInWindow = false;
    std::unordered_map<std::string, FrameCounts> windowCountBySensor;
    auto windowStart = std::chrono::steady_clock::now();
    const auto loopStart = windowStart;
    while (running_.load(std::memory_order_acquire)) {
        DvmFrame frame;
        const IFrameSource::Status status = source_->poll(frame, kFramePollTimeoutMs);
        if (status == IFrameSource::Status::Frame) {
            if (invalidDataPending) {
                PcmLogLine(PcmLogLevel::Info)
                    << "DVM: mdx-frames decode recovered; validating frames before verdict recovery";
                invalidDataPending = false;
            }
            if (mode_ == DvmMode::Active) noteFrameArrived(frame.sensorId);
            else {
                if (firstSeen_.count(frame.sensorId) == 0U) firstSeen_[frame.sensorId] = std::chrono::steady_clock::now();
                observeFrame(frame);
            }
            FrameCounts& counts = windowCountBySensor[frame.sensorId];
            if (counts.total == std::numeric_limits<std::size_t>::max()) countOverflow = true;
            else {
                ++counts.total;
                if (isFreshObservation(frame)) ++counts.fresh;
                if (mode_ == DvmMode::Active && evaluateFrame(frame)) ++counts.bad;
            }
        } else if (status == IFrameSource::Status::InvalidData) {
            if (mode_ == DvmMode::Active) {
                if (!invalidDataPending) {
                    PcmLogLine(PcmLogLevel::Warning)
                        << "DVM: sustained undecodable mdx-frames data; data check is bad "
                        << "until decoded frames resume (fault debounce applies)";
                }
                invalidDataPending = true;
                invalidDataInWindow = true;
            } else {
                learnInvalidDataSeen_ = true;
            }
        } else if (status == IFrameSource::Status::Disconnected) {
            sourceUnavailableInWindow = true;
            if (mode_ == DvmMode::Active && !sourceDown_) {
                sourceDown_ = true;
                PcmLogLine(PcmLogLevel::Warning)
                    << "DVM: Kafka source unavailable (consumer create/receive failed); "
                    << "data checks are off until usable frames arrive again, "
                    << "unless a received-data fault is already pending";
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - windowStart).count();
        if (elapsedMs >= kDvmWindowMs) {
            if (mode_ == DvmMode::Active) {
                /* An outage-spanning window has incomplete coverage, including
                 * the first partial window after reconnection. Keep seen-camera
                 * history so missing cameras are checked in the next full window. */
                /* Received corruption remains evidence even through silence or
                 * an outage. A decoded frame clears the pending condition, but
                 * cannot turn a window that observed corruption into Good. */
                const PcmCheckVerdict verdict = invalidDataInWindow ? PcmCheckVerdict::Bad :
                    sourceUnavailableInWindow ? PcmCheckVerdict::Off :
                    closeWindow(windowCountBySensor, windowStart, elapsedMs, countOverflow);
                reportNeverSeenAfterGrace(std::chrono::duration_cast<std::chrono::milliseconds>(now - loopStart).count());
                if (aggregator_ != nullptr) aggregator_->reportDvm(verdict);
            } else closeLearnWindow(windowCountBySensor, windowStart, elapsedMs);
            countOverflow = false;
            sourceUnavailableInWindow = sourceDown_;
            invalidDataInWindow = invalidDataPending;
            windowCountBySensor.clear();
            windowStart = now;
        }
    }
}

void DataValidationMonitor::logDataFault(const std::string& sensor, const std::string& reason)
{
    const std::string key = sensor + ":" + reason;
    if (loggedDataFaults_.size() < 128U && loggedDataFaults_.insert(key).second) {
        PcmLogLine(PcmLogLevel::Warning) << "DVM: decision-data fault: camera="
            << sanitizeSensorForFilename(sensor) << " reason=" << reason
            << " (per-camera bad-frame voting and fault debounce apply)";
    }
}

bool DataValidationMonitor::isFreshObservation(const DvmFrame& frame)
{
    const auto found = baselines_.find(frame.sensorId);
    const std::int64_t limit = mode_ == DvmMode::Learn ? kMaximumFrameAgeMs :
        (found == baselines_.end() ? 0 : found->second.maxFrameAgeMs);
    if (limit == 0) return true; // legacy or learned source without timestamps
    if (!usableTimestamp(frame)) return mode_ == DvmMode::Learn; // validated at LEARN completion
    const std::int64_t age = wallTimeMs() - frame.timestampMs;
    if (age < -kMinimumFrameAgeMs || age > limit) return false;
    std::int64_t& last = lastTimestampMs_[frame.sensorId];
    if (frame.timestampMs <= last) return false; // harmless duplicates/reordering do not count as progress
    last = frame.timestampMs;
    return true;
}

bool DataValidationMonitor::evaluateFrame(const DvmFrame& frame)
{
    if (sensorCheckActive_ && !isKnownSensor(frame.sensorId)) return true;
    const auto found = baselines_.find(frame.sensorId);
    if (found == baselines_.end() || !found->second.modern) return false;
    const DvmBaseline& baseline = found->second;
    if (baseline.maxFrameAgeMs > 0) {
        const std::int64_t age = usableTimestamp(frame) ? wallTimeMs() - frame.timestampMs : 0;
        if (!usableTimestamp(frame) || age > baseline.maxFrameAgeMs || age < -kMinimumFrameAgeMs) {
            logDataFault(frame.sensorId, "observation-age"); return true;
        }
    }
    const std::string reason = decisionFault(frame, baseline.contract, baseline.proximityRequired);
    if (!reason.empty()) { logDataFault(frame.sensorId, reason); return true; }
    return false;
}

void DataValidationMonitor::closeLearnWindow(
    const std::unordered_map<std::string, FrameCounts>& counts,
    std::chrono::steady_clock::time_point start, std::int64_t elapsedMs)
{
    std::lock_guard<std::mutex> lock(learnMutex_);
    for (auto& sensor : learned_) {
        const auto first = firstSeen_.find(sensor.first);
        if (first == firstSeen_.end() || first->second >= start) continue;
        const auto found = counts.find(sensor.first);
        const std::size_t fresh = found == counts.end() ? 0U : found->second.fresh;
        auto& rates = sensor.second.rates;
        if (rates.size() == kMaxLearnSamples) rates.pop_front();
        rates.push_back(static_cast<double>(fresh) * 1000.0 / static_cast<double>(elapsedMs));
    }
}

void DataValidationMonitor::noteFrameArrived(const std::string& sensorId)
{
    if (sourceDown_) {
        sourceDown_ = false;
        PcmLogLine(PcmLogLevel::Info) << "DVM: Kafka source delivering frames again";
    }
    if (isKnownSensor(sensorId)) {
        if (firstSeen_.find(sensorId) == firstSeen_.end()) {
            firstSeen_[sensorId] = std::chrono::steady_clock::now();
            if (graceReported_) {
                PcmLogLine(PcmLogLevel::Info) << "DVM: camera '" << sensorId
                                              << "' is now sending frames; validating it";
            }
        }
    } else if (loggedUnknownSensors_.size() < kMaxLoggedUnknownSensors &&
               loggedUnknownSensors_.insert(sensorId).second) {
        /* Redact the raw id (lower-integrity Kafka data) to its filename-safe form. */
        PcmLogLine(PcmLogLevel::Warning)
            << "DVM: frames from camera '" << sanitizeSensorForFilename(sensorId)
            << "' which is not in the PCM baseline; its frames count as bad "
            << "(run --mode LEARN to add it)";
    }
}

PcmCheckVerdict DataValidationMonitor::closeWindow(
    const std::unordered_map<std::string, FrameCounts>& windowCount,
    std::chrono::steady_clock::time_point windowStart,
    std::int64_t elapsedMs, bool countOverflow)
{
    if (sourceDown_) return PcmCheckVerdict::Off;
    bool bad = countOverflow;
    /* Healthy cameras must not dilute another camera's bad frames. A camera not in
     * the baseline has every frame bad, so it always reaches the percentage. */
    for (const auto& sensor : windowCount) {
        const FrameCounts& counts = sensor.second;
        if (percentageReached(counts.bad, counts.total, badFramesPercent_)) bad = true;
    }
    if (checkFrameRate(windowCount, windowStart, elapsedMs)) bad = true;
    if (bad) return PcmCheckVerdict::Bad;
    /* Until a baseline camera has sent a frame there is nothing to judge. */
    return firstSeen_.empty() ? PcmCheckVerdict::Off : PcmCheckVerdict::Good;
}

void DataValidationMonitor::reportNeverSeenAfterGrace(std::int64_t sinceStartMs)
{
    if (graceReported_ || sinceStartMs < static_cast<std::int64_t>(startupGraceMs_)) return;
    graceReported_ = true;

    std::vector<std::string> missing;
    for (const std::string& sensor : knownSensors_) {
        if (firstSeen_.find(sensor) == firstSeen_.end()) missing.push_back(sensor);
    }
    if (missing.empty()) return;
    std::sort(missing.begin(), missing.end());
    PcmLogLine line(PcmLogLevel::Warning);
    line << "DVM: " << missing.size() << " baseline camera(s) have sent no frames since start: ";
    for (std::size_t i = 0U; i < missing.size(); ++i) {
        line << (i == 0U ? "" : ", ") << missing[i];
    }
    line << " (not validated until frames arrive)";
}

bool DataValidationMonitor::checkFrameRate(
    const std::unordered_map<std::string, FrameCounts>& windowCount,
    std::chrono::steady_clock::time_point windowStart,
    std::int64_t elapsedMs) const
{
    if (!sensorCheckActive_ || elapsedMs <= 0) return false;

    bool bad = false;
    for (const auto& kv : expectedFps_) {
        const std::string& sensor = kv.first;
        const double fps = kv.second;
        if (fps <= 0.0) continue; /* no learned rate for this sensor -> skip */

        /* Only cameras already sending before this window began: a camera that
         * never sent a frame is reported (reportNeverSeenAfterGrace), not judged,
         * and the window holding a camera's first frame is only partly covered. */
        const auto seen = firstSeen_.find(sensor);
        if (seen == firstSeen_.end() || seen->second >= windowStart) continue;

        const auto it = windowCount.find(sensor);
        const std::size_t observed = (it != windowCount.end()) ? it->second.fresh : 0U;
        const double expected  = fps * (static_cast<double>(elapsedMs) / 1000.0);
        const double minNeeded = expected * kDvmMinFrameRateRatio;

        if (static_cast<double>(observed) < minNeeded) {
            PcmLogLine(PcmLogLevel::Warning) << "DVM: low frame rate: camera " << sensor
                                             << " got " << observed << " frame(s) in "
                                             << elapsedMs << "ms (expected ~" << expected
                                             << ", min " << minNeeded << ")";
            bad = true;
        }
    }
    return bad;
}

void DataValidationMonitor::observeFrame(const DvmFrame& frame)
{
    if (frame.sensorId.empty()) return;
    std::lock_guard<std::mutex> lock(learnMutex_);
    DvmLearnState& state = learned_[frame.sensorId];
    ++state.frames;
    if (!decisionFault(frame, configuredContract_, !configuredContract_.pairRules.empty()).empty()) ++state.badFrames;
    if (usableTimestamp(frame)) {
        ++state.timestampFrames;
        const std::int64_t age = wallTimeMs() - frame.timestampMs;
        if (state.ages.size() == kMaxLearnSamples) state.ages.pop_front();
        state.ages.push_back(age);
    }
    if (frame.proximity.present && std::isfinite(frame.proximity.threshold) && frame.proximity.threshold > 0.0) {
        if (state.proximityThresholds.size() == kMaxLearnSamples) state.proximityThresholds.pop_front();
        state.proximityThresholds.push_back(frame.proximity.threshold);
    }
}

bool DataValidationMonitor::isKnownSensor(const std::string& sensorId) const
{
    return knownSensors_.count(sensorId) > 0;
}

bool DataValidationMonitor::writeBaseline() const
{
    if (mode_ != DvmMode::Learn || dvmBaselineDir_.empty()) return false;
    if (learnInvalidDataSeen_) {
        PcmLogLine(PcmLogLevel::Warning)
            << "DVM: LEARN observed sustained undecodable data; previous baselines retained, re-run LEARN";
        return false;
    }
    std::unordered_map<std::string, DvmLearnState> states;
    { std::lock_guard<std::mutex> lock(learnMutex_); states = learned_; }
    if (states.empty()) {
        PcmLogLine(PcmLogLevel::Warning) << "DVM: baseline write skipped: no sensors observed (no mdx-frames received during LEARN)";
        return false;
    }
    std::unordered_set<std::string> filenames;
    std::vector<std::pair<std::string, std::string>> contents;
    /* Validate every camera before touching any baseline. LEARN assumes healthy
     * operation, but incomplete coverage and malformed decision data cannot
     * establish that assumption. Small isolated variations remain tolerated. */
    for (auto& entry : states) {
        const std::string& sensor = entry.first;
        DvmLearnState& state = entry.second;
        const std::string filename = perSensorBaselineFilename(sensor);
        std::sort(state.rates.begin(), state.rates.end());
        const double fps = state.rates.empty() ? 0.0 : state.rates[state.rates.size() / 2U];
        std::size_t lowWindows = 0U;
        for (double rate : state.rates) if (rate < fps * kDvmMinFrameRateRatio) ++lowWindows;
        const bool enough = state.rates.size() >= kMinLearnWindows && fps > 0.0 && fps <= 10000.0 &&
            state.rates.size() - lowWindows >= kMinLearnWindows &&
            lowWindows <= std::max<std::size_t>(1U, state.rates.size() / 20U);
        if (!enough || !isSensorIdSafeForBaseline(sensor) || !filenames.insert(filename).second ||
            percentageReached(state.badFrames, state.frames, 25)) {
            PcmLogLine(PcmLogLevel::Warning) << "DVM: baseline write skipped: incomplete/unstable LEARN or invalid decision data for "
                << sanitizeSensorForFilename(sensor) << "; previous baselines unchanged";
            return false;
        }
        std::int64_t ageLimit = 0;
        if (state.timestampFrames == 0U && configuredContract_.configured) {
            PcmLogLine(PcmLogLevel::Warning) << "DVM: baseline write skipped: decision-data LEARN requires observation timestamps";
            return false;
        }
        if (state.timestampFrames != 0U) {
            if (state.timestampFrames != state.frames) {
                PcmLogLine(PcmLogLevel::Warning) << "DVM: baseline write skipped: freshness LEARN requires usable timestamps on every frame; previous baselines unchanged";
                return false;
            }
            std::sort(state.ages.begin(), state.ages.end());
            const auto p99 = state.ages[(state.ages.size() - 1U) * 99U / 100U];
            const auto p01 = state.ages[(state.ages.size() - 1U) / 100U];
            if (p01 < -kMinimumFrameAgeMs || p99 > (kMaximumFrameAgeMs - 1000) / 4) {
                PcmLogLine(PcmLogLevel::Warning) << "DVM: baseline write skipped: observation age/clock outside learning range";
                return false;
            }
            ageLimit = std::max(kMinimumFrameAgeMs, std::max<std::int64_t>(0, p99) * 4 + 1000);
        } else {
            PcmLogLine(PcmLogLevel::Warning) << "DVM: timing-only baseline has no observation timestamps; frame age/replay checks are OFF";
        }
        double threshold = 0.0;
        if (!configuredContract_.pairRules.empty()) {
            if (state.proximityThresholds.empty()) return false;
            std::sort(state.proximityThresholds.begin(), state.proximityThresholds.end());
            threshold = state.proximityThresholds[state.proximityThresholds.size() / 2U];
        }
        for (const auto& rule : configuredContract_.roiRules) {
            if (!safeToken(rule.id) || !safeToken(rule.type)) return false;
        }
        for (const auto& rule : configuredContract_.pairRules) {
            if (!safeToken(rule.primaryType) || !safeToken(rule.secondaryType) ||
                !std::isfinite(rule.warningDistance) || rule.warningDistance <= 0.0 ||
                threshold < rule.warningDistance) return false;
        }
        std::ostringstream out;
        out.precision(17);
        out << "# PCM decision-data baseline; generated by --mode LEARN\n"
            << "[dvm_baseline]\nschema_version = 2\nknown_sensor = " << sensor
            << "\nexpected_fps = " << fps << "\nmax_frame_age_ms = " << ageLimit
            << "\ndecision_contract = " << (configuredContract_.configured ? 1 : 0)
            << "\nproximity_required = " << (!configuredContract_.pairRules.empty() ? 1 : 0)
            << "\nproximity_threshold = " << threshold << "\n";
        for (const auto& line : contractLines(configuredContract_)) out << line << "\n";
        contents.emplace_back(filename, out.str());
    }
    /* Publish all camera records in one file. A single rename is the commit
     * point, so I/O failure or interruption cannot mix old and new cameras. */
    std::sort(contents.begin(), contents.end());
    return publishBaselineSet(dvmBaselineDir_, contents);
}
