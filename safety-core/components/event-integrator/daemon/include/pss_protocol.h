/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PSS_PROTOCOL_H
#define PSS_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_SENSORS_DATA_SUMMARY_SIZE 8
#define MAX_TRAJECTORY_COORDINATES 10
#define MAX_INDENTIFIER_LENGTH 64
#define MAX_SUPPORTED_PIPELINES 8

/**
 * Protocol schema version.  Increment on any wire-incompatible change
 * to SafetyEvent, FusedSafetyEvent, or DecisionRequest.
 *
 * v5: dropped ROI_ENTRY/ROI_EXIT/TW_CROSSING_ENTRY/TW_CROSSING_EXIT and added
 * EVENT_23. Every enumerator from SW_FAIL onwards shifts down by 3, so a v4
 * peer reads SW_FAIL(27) as SENSOR_VALID and SENSOR_INVALID(29) as
 * AI_PIPELINE_VALID -- a failure report seen as a health report, in the
 * dangerous direction. The version check in validateSafetyEvent /
 * validateDecisionRequest is the only thing that catches this, so it is the
 * bump rather than the enum edit that makes the change safe: without it two
 * incompatible layouts both stamp 4 and the check passes.
 *
 * All PSF components must therefore be rebuilt AND redeployed together. A
 * peer built from v4 sources validates this field and will reject every
 * request rather than act on a misread one.
 */
#define PSS_SCHEMA_VERSION 5U

#pragma pack(push, 1)

/**
 * @enum EventType
 * @brief Enumeration for detected safety event type
 */
typedef enum {
    //Generic events
    EVENT_0 = 0,
    EVENT_1,
    EVENT_2,
    EVENT_3,
    EVENT_4,
    EVENT_5,
    EVENT_6,
    EVENT_7,
    EVENT_8,
    EVENT_9,
    EVENT_10,
    EVENT_11,
    EVENT_12,
    EVENT_13,
    EVENT_14,
    EVENT_15,
    EVENT_16,
    EVENT_17,
    EVENT_18,
    EVENT_19,
    EVENT_20,
    EVENT_21,
    EVENT_22,
    EVENT_23,
    // Add  event types as needed
    //Special events
    SW_FAIL,
    EVENT_UNKNOWN,  // Unrecognized output_event string from config
    /* Sensor/AI pipeline trust reports from Safety AI Monitor module */
    SENSOR_INVALID,
    SENSOR_VALID,
    AI_PIPELINE_INVALID,
    AI_PIPELINE_VALID,
    PSS_STATUS_NOOP
}EventType;

/**
 * @enum ReccomendedAction
 * @brief Enumeration for recommended safety action
 */
typedef enum {
    ESTOP,
    AUDIO_WARNING,
    VISUAL_WARNING,
    IMPLEMENT_SAFETY_CONTROL,
    NO_ACTION_REQUIRED
    // Add other event types as needed
}RecommendedAction;

/**
 * @enum ObjectType
 * @brief Enumeration for detected object type
 */
typedef enum {
    TYPE_0 = 0,
    TYPE_1,
    TYPE_2,
    TYPE_3,
    TYPE_4,
    TYPE_5,
    TYPE_6,
    // ....
    PERSON,
    VEHICLE,
    OBJECT
    // Add other event types as needed
}ObjectType;

/**
 * @brief Fixed-width downstream event severity for PSS daemon-built
 *        FusedSafetyEvent entries.
 *
 * Client SafetyEvent messages do not carry severity. PSS daemon assigns one
 * of these values when constructing DecisionRequest payloads for PSD/SDM
 * routing.
 */
typedef uint8_t SeverityLevel;

enum {
    OPERATIONAL = 0U,  /* Normal operational/no-op traffic */
    CRITICAL = 1U      /* Fault-safe latch or recovery evidence */
};

/**
 * @brief Why an event was reported, orthogonal to what it reports.
 *
 * `type` says what happened; `EventCause` says why. Fixed width for wire
 * stability, following the same shape as SeverityLevel and SafetyEventStatus.
 *
 * On the failed-sensor path, exactly one value is not diagnostic.
 * CAUSE_SAIM_INIT marks SAIM's pre-frame fail-safe seed, which every sensor
 * emits during startup; SDMs leave the sensor's failed bit unchanged for it, so
 * it does not contribute to the all-safety-relevant-sensors-failed latch.
 * **Every other value that reaches that path, including one an SDM does not
 * recognise, is treated as a genuine failure and latches.** That asymmetry is
 * deliberate and fails closed.
 *
 * CAUSE_PSS_COMM_RECOVERED is not an exception to the rule above: it rides a
 * PSS_STATUS_NOOP, and updateSensorHealthFromDecisionRequest() skips
 * PSS_STATUS_NOOP outright, so it never reaches the failed-sensor path at all.
 *
 * ADDING A VALUE IS NOT FREE, despite what an earlier version of this comment
 * claimed. It is safe for SDM *latch* logic -- an unrecognised cause latches.
 * It is NOT safe for the *validator*: validateCommonEventFields() rejects
 * cause >= CAUSE_COUNT against the RECEIVING build's bound, and a rejected
 * DecisionRequest is dropped before freshness is refreshed. A peer built
 * before the value existed therefore misses the fault the event carried and,
 * if the condition repeats, latches on a spurious freshness timeout. Every
 * validating runtime must be rebuilt together, which is already required for
 * a release.
 *
 * CAUSE_NONE and CAUSE_SAIM_UNKNOWN are behaviourally identical -- both latch.
 * They differ only in what they record: nothing set a cause, versus SAIM had a
 * missing or contradictory one. Do not collapse them.
 */
typedef uint8_t EventCause;

enum {
    CAUSE_NONE = 0U,                  /* No cause stated. Producers that have nothing
                                         to say leave the zero-initialised value. */
    /* SENSOR_INVALID causes, reported by the Safety AI Monitor. */
    CAUSE_SAIM_INIT = 1U,             /* First-registration fail-safe seed, pre-frames.
                                         The ONLY value that suppresses the failed bit. */
    CAUSE_SAIM_INPUT_DEGRADED = 2U,   /* Frame-quality score below threshold */
    CAUSE_SAIM_INTERNAL_ERROR = 3U,   /* Analyzer init or per-frame compute failure */
    CAUSE_SAIM_STREAM_DISCONNECT = 4U,/* Connect-retry exhaustion or reconnect */
    CAUSE_SAIM_FRAME_DROP = 5U,       /* FU-A NAL drops */
    CAUSE_SAIM_UNKNOWN = 6U,          /* INVALID with a missing or contradictory cause */
    /* SENSOR_VALID cause. */
    CAUSE_SAIM_SENSOR_HEALTHY = 7U,   /* Recovery edge, sustained good streak */
    /* SW_FAIL causes. */
    CAUSE_HEARTBEAT_FAULT = 8U,       /* RPC heartbeat miss budget exhausted. The client
                                         is in fusionMetadata.clientID. */
    CAUSE_GATEWAY_SEQUENCE_GAP = 9U,  /* Gateway decision-sequence discontinuity */
    /* SW_FAIL causes synthesized by the PSD gateway. These three events are
       otherwise identical on the wire -- same type, severity, status, clientID
       and sensorIdentifier "PSD_GATEWAY" -- so the cause is their ONLY
       discriminator. Do not collapse them into CAUSE_NONE. */
    CAUSE_PSS_REQUEST_SEQUENCE_GAP = 10U, /* PSS DecisionRequest sequence discontinuity */
    CAUSE_PSS_REQUEST_ID_INVALID = 11U,   /* PSS request id outside the accepted range */
    CAUSE_PSS_ERROR = 12U,                /* PSS reported ERROR; gateway broadcasts SW_FAIL */
    CAUSE_CLIENT_INPUT_UNUSABLE = 13U,    /* PSS RPC: a required source's reports were rejected
                                             consecutively past client_reject_error_threshold,
                                             so the input is live but unusable. The client is in
                                             fusionMetadata.clientID, and its input identity
                                             (PSS_INPUT_STATE_MASK bit) in objectID[0]. Sent once
                                             per episode, but the episode is ALSO reported as a
                                             level on every presence report (see
                                             PSS_INPUT_STATE_SHIFT), and an SDM clears it only
                                             when that report shows the same identity confirmed
                                             usable again -- never on other sources' traffic. */
    /* PSS_STATUS_NOOP causes (14 and 16). The only causes that accompany neither SENSOR_* nor
       SW_FAIL, and the only ones that are not fault reports. 14 can never latch anything. 16
       can, but only by what it leaves OUT: a source an SDM knows of, missing from the report,
       raises that source's lost sub-cause (see CAUSE_SOURCE_PRESENCE). */
    CAUSE_PSS_COMM_RECOVERED = 14U,   /* PSD gateway: a synthetic SW_FAIL condition it previously
                                         broadcast (causes 10-12) has cleared. Rides a
                                         PSS_STATUS_NOOP, NEVER a SW_FAIL, so it cannot latch:
                                         nothing in the SDMs raises a cause from it, and
                                         updateSensorHealthFromDecisionRequest() skips
                                         PSS_STATUS_NOOP outright. It clears an ACTIVE cause
                                         only; the safe-state latch still requires a validated
                                         manual release. */
    /* Causes 15, 17 and 18 carry the lost source's PSS_PRESENCE_* bit in
       fusionMetadata.objectID[0], so an SDM that never saw the source present still knows
       which one must come back. */
    CAUSE_SAFETY_MONITOR_LOST = 15U,  /* PSS RPC: the registered Safety AI Monitor HUNG
                                         (heartbeat budget exhausted), CRASHED (connection
                                         dropped), UNREGISTERED itself, or was dropped by PSS
                                         for a protocol error -- every way a registered monitor
                                         stops being one. Rides a SW_FAIL. Unlike an ordinary
                                         SW_FAIL it is NOT cleared by ordinary traffic: only the
                                         presence report (16) naming the monitor clears it,
                                         because only the monitor itself coming back is evidence
                                         that the site is monitored again. The client is in
                                         fusionMetadata.clientID. */
    CAUSE_SOURCE_PRESENCE = 16U,      /* PSS status no-op: the PRESENCE REPORT.
                                         fusionMetadata.objectID[0] holds the PSS_PRESENCE_* set
                                         of monitors registered, heartbeated since registering,
                                         and not heartbeat-faulted, sampled when PSS BUILT the
                                         request. Stamped on every PSS periodic no-op, with an
                                         empty set when none is present, so it is affirmative in
                                         both directions: a set bit clears that monitor's lost
                                         sub-cause; a clear bit, for a monitor the SDM has seen
                                         present, raises it -- which is what catches a loss PSS
                                         could not report, such as PSS restarting while the
                                         monitor was dead. Numbered MDX instances are in the set
                                         too (PSS_PRESENCE_MDX_INSTANCE).
                                         fusionMetadata.objectID[1] holds the EXPECTED set:
                                         every tracked source that has heartbeated since this
                                         PSS started (registration alone is not enough: a
                                         source is expected only once it can be present),
                                         plus the sources nvpss.conf requires once the heartbeat
                                         budget since PSS start has run out. An SDM adds it to
                                         the sources it knows of, so an SDM that RESTARTED while
                                         a source was down relearns it from PSS. A no-op
                                         WITHOUT this cause (the gateway's 14) says nothing
                                         about presence either way. Never rides a SW_FAIL.
                                         Bits 16..31 of both words carry the INPUT-STATE
                                         report instead: see PSS_INPUT_STATE_SHIFT. */
    CAUSE_PERCEPTION_MONITOR_LOST = 17U,/* PSS RPC: as CAUSE_SAFETY_MONITOR_LOST, for the
                                         Perception Monitor -- the only source of AI-pipeline
                                         verdicts. Without it, AI_PIPELINE_INVALID can no longer
                                         be reported, so its loss must not clear on MDX traffic
                                         either. Cleared only by the presence report naming it. */
    CAUSE_MDX_CLIENT_LOST = 18U,      /* PSS RPC: a NUMBERED MDX client instance (registered with
                                         an instance id 1..PSS_MDX_INSTANCE_MAX) stopped being
                                         registered, however it happened -- or an instance
                                         nvpss.conf requires never registered at all. Ordinary
                                         traffic, including from other MDX clients, does not
                                         clear it: only the presence report naming that instance
                                         does. Anonymous MDX clients (instance 0) keep cause 8. */

    CAUSE_COUNT                       /* Range-check bound; never sent on the wire */
};

/*
 * CAUSE_SOURCE_PRESENCE membership bits, carried in the presence report's
 * fusionMetadata.objectID[0] (present) and objectID[1] (expected), and in
 * objectID[0] of a source-lost SW_FAIL. A bit's position is part of the wire
 * contract: never renumber, only append. Unassigned bits are sent as zero and
 * ignored.
 */
#define PSS_PRESENCE_SAFETY_MONITOR      0x00000001U
#define PSS_PRESENCE_PERCEPTION_MONITOR  0x00000002U
/* Numbered MDX client instances 1..8: bits 8..15. Any k outside 1..8 yields 0,
 * never a shift by an out-of-range amount. */
#define PSS_PRESENCE_MDX_MASK            0x0000FF00U
#define PSS_PRESENCE_MDX_INSTANCE(k) \
    (((uint32_t)(k) >= 1U && (uint32_t)(k) <= 8U) ? (0x00000100U << ((uint32_t)(k) - 1U)) : 0U)

/*
 * INPUT-STATE report: which sources' reports PSS is currently rejecting.
 *
 * Identities use the PSS_PRESENCE_* layout plus PSS_INPUT_ANONYMOUS_CLIENT, which
 * stands for every anonymous MDX client (instance 0) together: those are not
 * individually tracked. The anonymous bit appears only in the input-state sets,
 * never in the presence or expected set.
 *
 * Carried in the presence report's bits 16..31:
 *   objectID[0] >> PSS_INPUT_STATE_SHIFT   UNUSABLE: identities with a cause-13
 *                                          episode open now
 *   objectID[1] >> PSS_INPUT_STATE_SHIFT   CONFIRMED USABLE: identities from which
 *                                          PSS has accepted a report since the last
 *                                          cause-13 escalation for that identity
 * A cause-13 SW_FAIL carries its identity bit, unshifted, in objectID[0].
 *
 * An SDM holds the input-unusable cause for an identity until a report shows it
 * confirmed usable and not unusable. Both sets start empty when PSS starts, so a
 * PSS restart never clears it: only the source's next accepted report does.
 */
#define PSS_INPUT_ANONYMOUS_CLIENT       0x00000004U
#define PSS_INPUT_STATE_MASK \
    (PSS_PRESENCE_SAFETY_MONITOR | PSS_PRESENCE_PERCEPTION_MONITOR | \
     PSS_INPUT_ANONYMOUS_CLIENT | PSS_PRESENCE_MDX_MASK)
#define PSS_INPUT_STATE_SHIFT            16U

/**
 * @enum PSSOperationalMode
 * @brief Enumeration for PSS operational modes
 */
typedef enum {
    NORMAL,   /* Normal operational mode */
    DEGRADED, /* Degraded operational mode */
    ERROR     /* Error operational mode */
}OperationalMode;

/**
 * @enum SafetyEventStatus
 * @brief Enumeration for SafetyEvent Fusion status
 */
typedef enum {
    FUSED,        /* Fused Safety Event */
    PASSTHROUGH,  /* Safety Event pass-through withut fusion */
    STALE,        /* Safety Event arrivd with high latency */
    UNKNOWN       /* Error operational mode or Fusion not enabled*/
}SafetyEventStatus;

/**
 * @struct TracjectoryCoordinates
 * @brief Structure for representing spatial coordinates of an event
 */
typedef struct {
    float x;
    float y;
} TrajectoryCoordinates;

/**
 * @struct EventFusionMetadata
 * @brief Structure containing metadata for event fusion
 */
typedef struct {
    TrajectoryCoordinates coordinates[MAX_TRAJECTORY_COORDINATES];  /* Trajectory coordinates of the event */
    uint8_t pipelineID;            /* Sensor producing the data */
    uint8_t clientID;              /* AI inference pipeline identifier */
    ObjectType objectType[2];      /* Type of object, example Person, Vehicle, Face */
    float speed;                   /* Speed of Object */
    uint32_t objectID[2];          /* Object IDs from mdx-events/mdx-frames */
} EventFusionMetadata;

/**
 * @struct MessageIntegrity
 * @brief End-of-message integrity trailer for SafetyEvent and DecisionRequest.
 *
 * Must be the **last** member of every top-level wire struct.  CRC-32
 * (ISO 3309 / IEEE, polynomial 0xEDB88320 reflected) is computed over all
 * bytes of the enclosing struct from offset 0 up to, but not including,
 * the crc32 field (i.e. sizeof(struct) - sizeof(uint32_t) bytes).
 */
typedef struct {
    uint16_t schemaVersion;   /* PSS_SCHEMA_VERSION at build time; receiver rejects mismatches */
    uint32_t crc32;           /* CRC-32 over preceding bytes; see struct comment for offset */
} MessageIntegrity;

/**
 * @struct SafetyEvent
 * @brief Structure for safety event data with fusion support
 */
typedef struct {
    uint32_t id;                   /* Unique identifier for the safety event */
    char sensorIdentifier[MAX_INDENTIFIER_LENGTH]; /* Name of sensor generating the Safety Event */
    EventType type;                /* Type of the safety event */
    uint64_t timestamp;            /* Monotonic nanoseconds (CLOCK_MONOTONIC or equivalent).
                                      Both sender and receiver must use the same monotonic epoch.
                                      Receivers should reject events older than a configurable
                                      maximum age threshold. */
    float confidenceLevel;         /* Confidence level of the event, range [0.0f, 1.0f] */
    bool processed;                /* Flag indicating if event has been processed for fusion */
    EventFusionMetadata fusionMetadata;  /* Fusion metadata */
    EventCause cause;              /* Why the event was reported; see EventCause. Must stay
                                      BEFORE integrity so the CRC covers it. */
    MessageIntegrity integrity;    /* Must be last member; crc32 covers all preceding bytes */
} SafetyEvent;

/**
 * @struct FusedSafetyEvent
 * @brief Structure for fused safety events from dual pipelines
 */
typedef struct {
    uint32_t id;                   /* Unique identifier for the fused event */
    char sensorIdentifier[MAX_INDENTIFIER_LENGTH]; /* Name of primary sensor generating the Safety Event */
    EventType type;                /* Type of the safety event */
    EventCause cause;              /* Why the event was reported; see EventCause */
    SeverityLevel severity;        /* PSS daemon-owned OPERATIONAL/CRITICAL routing severity */
    uint64_t timestamp;            /* Monotonic nanoseconds of the earliest source event
                                      (same epoch as SafetyEvent.timestamp) */
    float confidenceLevel;         /* Calculated fusion confidence */
    SafetyEventStatus status;      /* Flag indicating status of event after fusion */
    EventFusionMetadata fusionMetadata;  /* Fusion metadata */
} FusedSafetyEvent;

/**
 * @struct SensorData
 * @brief Structure for sensor data summary
 */
typedef struct {
    uint32_t clientID;      /* AI inference pipeline / source client identifier */
    bool isHealthy;         /* Sensor health: false when SENSOR_INVALID reported
                               for this event's pipelineID by SAI */
    bool isTrustedSource;   /* AI pipeline trust: false when AI_PIPELINE_INVALID
                               reported for this event's clientID by SAI */
    FusedSafetyEvent event; /* Safety Event Data */
}SensorData;

/**
 * @struct SystemStatus
 * @brief Structure for system status
 */
typedef struct {
    bool hardwareErrorFlag; /* HW error flags */
    bool softwareErrorFlag; /* SW error flags */
    OperationalMode mode;   /* Current operational mode */
}SystemStatus;

/**
 * @struct DecisionRequest
 * @brief Structure for decision request
 */
typedef struct {
    uint32_t requestId;             /* Unique identifier for the decision request*/
    uint8_t sensorDataSummarySize;  /* Number of sensor summary infomation, <= MAX_SENSORS_DATA_SUMMARY_SIZE */
    SensorData sensorDataSummary[MAX_SENSORS_DATA_SUMMARY_SIZE]; /* Summary ofSensor Data Info*/
    SystemStatus pssStatus;    /* System status information */
    MessageIntegrity integrity;    /* Must be last member; crc32 covers all preceding bytes */
}DecisionRequest;

/**
 * @struct DecisionResponse
 * @brief Structure for decision response
 */
typedef struct {
    uint32_t decisionId;       /* Unique identifier for the decision response*/
    RecommendedAction action;  /* Recommended action based on the decision */
    float confidenceLevel; /* Confidence level of the decision */
}DecisionResponse;

#pragma pack(pop)

#endif
