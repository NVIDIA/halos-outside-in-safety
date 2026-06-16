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
 */
#define PSS_SCHEMA_VERSION 1U

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
    // Add  event types as needed
    //Special events
    ROI_ENTRY,
    ROI_EXIT,
    TW_CROSSING_ENTRY,
    TW_CROSSING_EXIT,
    SW_FAIL,
    EVENT_UNKNOWN,  // Unrecognized output_event string from config
    /* Sensor/AI pipeline trust reports from Safety AI Monitor module */
    SENSOR_INVALID,
    SENSOR_VALID,
    AI_PIPELINE_INVALID,
    AI_PIPELINE_VALID
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
 * @enum EventSeverity
 * @brief Enumeration for event severity levels
 */
typedef enum {
    LOW,     /* Low severity event */
    MEDIUM,  /* Medium severity event */
    HIGH,    /* High severity event */
    CRITICAL /* Critical severity event */
}SeverityLevel;

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
    char ruleIdentifier[MAX_INDENTIFIER_LENGTH]; /* Name of rule generating the Safety Event */
    EventType type;                /* Type of the safety event */
    SeverityLevel severity;        /* Severity level of the event */
    uint64_t timestamp;            /* Monotonic nanoseconds (CLOCK_MONOTONIC or equivalent).
                                      Both sender and receiver must use the same monotonic epoch.
                                      Receivers should reject events older than a configurable
                                      maximum age threshold. */
    float confidenceLevel;         /* Confidence level of the event, range [0.0f, 1.0f] */
    bool processed;                /* Flag indicating if event has been processed for fusion */
    EventFusionMetadata fusionMetadata;  /* Fusion metadata */
    MessageIntegrity integrity;    /* Must be last member; crc32 covers all preceding bytes */
} SafetyEvent;

/**
 * @struct FusedSafetyEvent
 * @brief Structure for fused safety events from dual pipelines
 */
typedef struct {
    uint32_t id;                   /* Unique identifier for the fused event */
    char sensorIdentifier[MAX_INDENTIFIER_LENGTH]; /* Name of primary sensor generating the Safety Event */
    char ruleIdentifier[MAX_INDENTIFIER_LENGTH]; /* Name of rule generating the Safety Event */
    EventType type;                /* Type of the safety event */
    SeverityLevel severity;        /* Severity level of the event */
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
