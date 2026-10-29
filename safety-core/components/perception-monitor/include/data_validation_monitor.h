/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCM_DATA_VALIDATION_MONITOR_H
#define PCM_DATA_VALIDATION_MONITOR_H

#include "pcm_fault_aggregator.h"
#include "decision_contract.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * Decoded, in-memory form of one mdx-frames FrameMessage (ext.proto) and its
 * nested decision metadata (schema.proto). The validator sees these plain
 * structs rather than protobuf or Kafka types. Image fields are retained only
 * for diagnostics; they do not gate the verdict.
 */
struct DvmBbox
{
    /* schema.proto Bbox: top-left + bottom-right, in streammux frame space. */
    float leftX   = 0.0F;
    float topY    = 0.0F;
    float rightX  = 0.0F;
    float bottomY = 0.0F;
};

struct DvmObject
{
    std::string id;                    /* tracker-assigned, persistent across frames */
    std::string type;                  /* "Person", "forklift", ... (raw producer case) */
    float       confidence = 0.0F;     /* diagnostic; tracker may emit -0.1 */
    DvmBbox     bbox;
    bool        hasBbox = false;       /* bbox sub-message was present */
    float       speed = 0.0F;          /* m/s in world frame (Object.speed) */
    bool        hasCoordinate = false; /* world coordinate populated (Object.coordinate) */
    double      x = 0.0;
    double      y = 0.0;
    double      z = 0.0;
};

struct DvmRoi {
    std::string id;
    std::string type;
    std::vector<std::string> objectIds;
    bool hasRestricted = false;
    bool hasConfined = false;
    std::string restricted;
    std::string confined;
};

struct DvmProximity {
    bool present = false;
    double threshold = 0.0;
    int detections = 0;
    bool hasViolation = false;
    std::string violation;
};

/* One FOV/ROI per-type count entry (ext.proto TypeCount). */
struct DvmTypeCount
{
    std::string type;
    int         count = 0;
};

struct DvmFrame
{
    std::string               sensorId;        /* camera id; per-sensor state key */
    std::string               frameId;
    int64_t                   timestampMs = 0; /* from FrameMessage.timestamp */
    bool                      timestampValid = false;
    std::vector<DvmObject>    objects;
    std::vector<DvmTypeCount> fov;             /* diagnostic FOV-level counts */
    std::vector<DvmRoi>       rois;
    DvmProximity             proximity;
};

/* Versioned decision-data baseline. Image quality and occupancy are deliberately
 * absent: those belong to SAIM and the application decision respectively. */
struct DvmBaseline {
    bool modern = false;
    double fps = 0.0;
    std::int64_t maxFrameAgeMs = 0;
    bool proximityRequired = false;
    double proximityThreshold = 0.0;
    DvmDecisionContract contract;
};

struct DvmLearnState {
    std::uint64_t frames = 0;
    std::uint64_t badFrames = 0;
    std::uint64_t timestampFrames = 0;
    std::deque<double> rates;
    std::deque<std::int64_t> ages;
    std::deque<double> proximityThresholds;
};

enum class DvmMode
{
    Learn,
    Active
};

/*
 * Source of decoded perception frames for the data validator. The production
 * implementation wraps a librdkafka consumer on the mdx-frames topic and
 * decodes the FrameMessage protobuf into a DvmFrame. Polled only from the DVM
 * thread, so implementations need not be thread-safe.
 */
class IFrameSource
{
public:
    enum class Status
    {
        Frame,        /* out populated with a decoded frame */
        Timeout,      /* no usable frame or new source-availability evidence */
        Disconnected, /* broker/source unreachable: reported, not judged */
        InvalidData,  /* sustained received-but-undecodable data: a data fault */
    };

    virtual ~IFrameSource() = default;

    /* Poll for the next frame. timeoutMs bounds idle waiting; connection setup
     * may take longer. */
    virtual Status poll(DvmFrame& out, int timeoutMs) = 0;
};

/*
 * Placeholder source for the skeleton: never yields a frame (always Timeout),
 * so the data validator runs and integrates end-to-end with no Kafka/protobuf
 * dependency. Replaced by a KafkaFrameSource (librdkafka + FrameMessage decode).
 */
class NullFrameSource : public IFrameSource
{
public:
    Status poll(DvmFrame& out, int timeoutMs) override;
};

/*
 * The data-validation stage of PCM: validates the trustworthiness of the perception
 * DATA on the Kafka mdx-frames topic, complementing the container-health check which
 * watches container liveness. Runs its own thread that pulls frames from an IFrameSource and
 * checks learned camera identity, observation timing, and metadata required by
 * the selected ATL/PXC decision rules. Scene occupancy is never learned as a
 * required condition.
 *
 * The monitor owns no hysteresis or emit logic: in ACTIVE it reports one verdict
 * per window (Good / Bad, or Off while no baseline camera has been seen) to the
 * shared PcmFaultAggregator (passed in), which owns the time-based hysteresis and
 * the single emit. Only cameras seen in this run are frame-rate checked; a
 * baseline camera that never sends frames is reported, not judged. Kafka being
 * unavailable is reported, not judged. Sustained received-but-undecodable data
 * is Bad until decoded frames resume. In LEARN it is observe-only:
 * it records sensor ids seen on mdx-frames and atomically publishes the complete
 * camera set in pcm_baseline.cfg for ACTIVE to enforce (aggregator pointer null).
 */
class DataValidationMonitor
{
public:
    DataValidationMonitor(DvmMode mode, std::string baselinePath,
                          PcmFaultAggregator* aggregator,
                          std::unique_ptr<IFrameSource> source,
                          int badObjectsPercent = 0, int badFramesPercent = 0,
                          int startupGraceMs = 0,
                          const DvmDecisionContract& contract = DvmDecisionContract());
    ~DataValidationMonitor();

    DataValidationMonitor(const DataValidationMonitor&) = delete;
    DataValidationMonitor& operator=(const DataValidationMonitor&) = delete;
    DataValidationMonitor(DataValidationMonitor&&) = delete;
    DataValidationMonitor& operator=(DataValidationMonitor&&) = delete;

    /* Spawn the validation thread. ACTIVE requires configured percentages in
     * 1..100 and a positive startup grace; zero defaults are unconfigured and
     * are used only in LEARN. */
    bool start();

    /* Signal and join the validation thread. Idempotent. */
    void stop();

    /* LEARN only: atomically publish the complete observed camera set in
     * <baseline directory>/pcm_baseline.cfg. Call after stop(). False means no
     * new set was published; sustained undecodable data also rejects the run.
     * A post-publication directory flush failure warns
     * that power-loss durability is uncertain, but returns true. ACTIVE reads
     * legacy per-camera .cfg/.cfg.default files only when this bundle is absent. */
    bool writeBaseline() const;

    /* ACTIVE: true when a usable per-sensor baseline was loaded. When false there
     * is nothing to validate against and the caller turns data validation off. */
    bool hasSensorBaseline() const { return sensorCheckActive_; }

    /* ACTIVE: number of cameras in the loaded baseline. */
    std::size_t knownSensorCount() const { return knownSensors_.size(); }

private:
    struct FrameCounts {
        std::size_t total = 0U;
        std::size_t bad = 0U;
        std::size_t fresh = 0U;
    };

    void runLoop();
    /* pthread_create entry trampoline: forwards to runLoop(). */
    static void* runLoopThunk(void* self);

    /* ACTIVE: apply the identity, freshness and decision-data checks; true if the frame
     * deviates from the baseline (counted per camera in the 1 s window). */
    bool evaluateFrame(const DvmFrame& frame);
    /* ACTIVE: per received frame, log the Kafka source recovering and record the
     * camera's first frame (or warn once about a camera not in the baseline). */
    void noteFrameArrived(const std::string& sensorId);
    /* ACTIVE: the verdict for one closed window (see runLoop). */
    PcmCheckVerdict closeWindow(const std::unordered_map<std::string, FrameCounts>& windowCount,
                                std::chrono::steady_clock::time_point windowStart,
                                std::int64_t elapsedMs, bool countOverflow);
    /* ACTIVE: once the startup grace has passed, warn about baseline cameras
     * that have not sent any frame yet. */
    void reportNeverSeenAfterGrace(std::int64_t sinceStartMs);
    /* LEARN: accumulate timing and decision-metadata observations. */
    void observeFrame(const DvmFrame& frame);
    /* True when sensorId is in the learned whitelist. */
    bool isKnownSensor(const std::string& sensorId) const;

    void logDataFault(const std::string& sensor, const std::string& reason);
    bool isFreshObservation(const DvmFrame& frame);
    void closeLearnWindow(const std::unordered_map<std::string, FrameCounts>& counts,
                          std::chrono::steady_clock::time_point start,
                          std::int64_t elapsedMs);
    /* ACTIVE: true if any camera already sending before this
     * window began delivered too few frames vs its learned expected_fps. */
    bool checkFrameRate(const std::unordered_map<std::string, FrameCounts>& windowCount,
                        std::chrono::steady_clock::time_point windowStart,
                        std::int64_t elapsedMs) const;

    DvmMode                       mode_;
    std::string                   dvmBaselineDir_;
    PcmFaultAggregator*           aggregator_; /* emit target in ACTIVE; null in LEARN */
    std::unique_ptr<IFrameSource> source_;
    const int                     badObjectsPercent_;
    const int                     badFramesPercent_;
    const int                     startupGraceMs_;

    /* ACTIVE, DVM thread only: when each baseline camera sent its first frame in
     * this run, unknown camera ids already warned about (bounded), whether the
     * startup-grace report was made, and whether the Kafka source is down. */
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> firstSeen_;
    std::unordered_set<std::string> loggedUnknownSensors_;
    bool                            graceReported_ = false;
    bool                            sourceDown_ = false;

    /* ACTIVE: learned sensor whitelist + per-sensor expected frame rate (loaded
     * once in ctor from the baseline files, read-only after). A sensor with no
     * expected_fps is not frame-rate checked. */
    std::unordered_set<std::string> knownSensors_;
    std::unordered_map<std::string, double> expectedFps_;
    bool                            sensorCheckActive_ = false;

    DvmDecisionContract configuredContract_;
    std::unordered_map<std::string, DvmBaseline> baselines_;
    std::unordered_map<std::string, std::int64_t> lastTimestampMs_;
    std::unordered_set<std::string> loggedDataFaults_;

    std::atomic<bool> running_{false};
    pthread_t         thread_{};
    bool              threadValid_ = false;

    mutable std::mutex learnMutex_;
    std::unordered_map<std::string, DvmLearnState> learned_;
    /* Written by the LEARN thread, read by writeBaseline() only after stop(). */
    bool learnInvalidDataSeen_ = false;

};

#endif /* PCM_DATA_VALIDATION_MONITOR_H */
