/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file  crossing_junction_safety_cmd_pkt.h
 * @brief  64-byte command / acknowledgement packet definition for the
 *         Crossing-Junction-Safety-Monitor SDM (forklift crossing an inbound
 *         tripwire while a person occupies the junction ROI).
 *
 * This header is self-contained and can be included by any application
 * (sender, receiver, logger, test harness, etc.) that needs to build
 * or parse decision command packets.
 *
 * Wire-compatible with the other metropolis SDM packets by design -- only the
 * identifier differs -- so one PLC-side parser handles every SDM and the source
 * of a command is never ambiguous.
 *
 * Byte Map (64 bytes total):
 *   [0]       Identifier             (uint8_t)   – magic byte 0xA7
 *   [1-2]     Sequence Number        (uint16_t)
 *   [3]       Command                (uint8_t)
 *   [4-11]    Timestamp seconds UTC  (uint64_t)  – seconds since epoch
 *   [12-19]   Timestamp microseconds (uint64_t)  – µs component
 *   [20-23]   CRC-32                 (uint32_t)
 *   [24-43]   Object Record 0        (20 bytes)
 *   [44-63]   Object Record 1        (20 bytes)
 */

#ifndef CROSSING_JUNCTION_SAFETY_CMD_PACKET_H
#define CROSSING_JUNCTION_SAFETY_CMD_PACKET_H

#include <cstdint>
#include <cstddef>
#include <cstring>

/* ======================== Constants ======================== */

#define COMMAND_PACKET_SIZE       64
#define CROSSING_JUNCTION_SAFETY_PACKET_IDENTIFIER 0xA7
#define COMMAND_NUM_OBJECTS        2

/* Command opcodes. Opcode values are shared across the metropolis SDMs, so an
 * opcode never means two different things depending on the sender.
 *
 * This SDM's own decision output is binary -- the junction siren is either on or
 * off -- so it emits only CMD_JUNCTION_ALARM and CMD_NORMAL. The remaining
 * opcodes belong to the fail-safe and heartbeat plumbing shared with the other
 * SDMs: CMD_STOP paired with CMD_SW_ERROR is the safe hold asserted on a PSS
 * fault, a gateway heartbeat loss or a stale DecisionRequest, and is deliberately
 * distinct from a junction alarm so the PLC can tell "the junction is unsafe"
 * from "the safety chain itself is unhealthy".
 *
 * CMD_REDUCE (0x05) and CMD_COMOVEMENT (0x0B) are proximity-monitor concepts
 * with no meaning at a crossing, and are intentionally absent. */
#define CMD_HEARTBEAT     0x00
#define CMD_HW_ERROR      0x01
#define CMD_STOP          0x02   /* Prevent Operation (fail-safe hold) */
#define CMD_SW_ERROR      0x03
#define CMD_NORMAL        0x07   /* Standard Operation / junction siren off */
#define CMD_SAFE_RELEASE_REQUEST 0x08
#define CMD_SAFE_RELEASE_ACK     0x09
#define CMD_SAFE_RELEASE_DENIED  0x0A
/* Junction siren: an inbound forklift and a person occupy the junction at the
 * same time (A1 AND A2). Its own opcode rather than CMD_STOP because the
 * required response is to warn the crossing, and because a siren raised by the
 * hazard must stay distinguishable from the safe hold raised by a fault. */
#define CMD_JUNCTION_ALARM 0x0C

/* ======================== Structures ======================== */

#pragma pack(push, 1)

/**
 * @struct ObjectRecord
 * @brief  20-byte per-object payload embedded in the command packet.
 */
typedef struct {
    uint32_t object_id;    /**< Application object identifier           */
    float    x;            /**< X coordinate (trajectory / world-space) */
    float    y;            /**< Y coordinate                            */
    float    z;            /**< Z coordinate (set to 0.0 if unused)     */
    uint32_t metadata;     /**< Object type / flags (app-specific)      */
} ObjectRecord;         /* 20 bytes */

/**
 * @struct CmdPacket
 * @brief  64-byte command / acknowledgement packet.
 */
typedef struct {
    uint8_t         identifier;                    /**< Byte  0     : magic 0xA7               */
    uint16_t        seq;                           /**< Bytes 1-2   : sequence number          */
    uint8_t         command;                       /**< Byte  3     : command opcode           */
    uint64_t        ts_seconds;                    /**< Bytes 4-11  : UTC seconds since epoch  */
    uint64_t        ts_microseconds;               /**< Bytes 12-19 : microseconds component   */
    uint32_t        crc32;                         /**< Bytes 20-23 : CRC-32                   */
    ObjectRecord objects[COMMAND_NUM_OBJECTS];     /**< Bytes 24-63 : 2×20-byte object records */
} CmdPacket;               /* 64 bytes total */

#pragma pack(pop)

/* ======================== CRC-32 Utility ======================== */

/* CRC-32 (ISO 3309 / ITU-T V.42) lookup table, shared by computeCRC32() and
 * cmdPacketCRC32().
 *
 * Each of those used to own a private `static uint32_t table[256]` filled
 * behind a `static bool tableReady` flag. That is not thread-safe: the array is
 * statically zero-initialised and filled imperatively, so the compiler emits no
 * initialisation guard. These helpers do run concurrently -- cmdPacketCRC32()
 * stamps outbound packets on the decision-send path while cmdPacketValidateCRC()
 * checks inbound ones on the ACK receiver thread -- so two threads could both
 * enter the fill, and a reader could observe an entry that was still zero and
 * compute a wrong CRC, dropping a valid ACK. Initialising a function-local
 * static from a call puts the fill under the C++11 thread-safe-static
 * guarantee, and leaves one table instead of two identical copies. */
struct Crc32Table {
    uint32_t entry[256];
};

static inline Crc32Table makeCrc32Table()
{
    Crc32Table built = {};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
        built.entry[i] = crc;
    }
    return built;
}

static inline const uint32_t* crc32Table()
{
    static const Crc32Table table = makeCrc32Table();
    return table.entry;
}

/**
 * @brief  Compute CRC-32 (ISO 3309 / ITU-T V.42) over a byte buffer.
 * @param  data   Pointer to input buffer.
 * @param  length Number of bytes.
 * @return CRC-32 value.
 */
static inline uint32_t computeCRC32(const void* data, size_t length)
{
    const uint32_t* table = crc32Table();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++)
        crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/**
 * @brief  Compute CRC-32 for a CmdPacket, excluding the crc32 field itself.
 *         Hashes bytes [0..19] and [24..63].
 * @param  pkt  Pointer to a filled CmdPacket (crc32 field is ignored).
 * @return CRC-32 value to store in pkt->crc32.
 */
static inline uint32_t cmdPacketCRC32(const CmdPacket* pkt)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(pkt);
    const uint32_t* table = crc32Table();
    /* Hash header before CRC field: bytes 0-19, then the payload: bytes 24-63 */
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < 20; i++)
        crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFF];
    for (size_t i = 24; i < COMMAND_PACKET_SIZE; i++)
        crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/**
 * @brief  Validate CRC-32 of a received CmdPacket.
 * @param  pkt  Pointer to a received CMdPacket.
 * @return true if CRC matches, false otherwise.
 */
static inline bool cmdPacketValidateCRC(const CmdPacket* pkt)
{
    return pkt->crc32 == cmdPacketCRC32(pkt);
}

/* ======================== Command Name Utility ======================== */

/**
 * @brief  Return a human-readable string for a command opcode.
 * @param  cmd  Command byte from CmdPacket.command.
 * @return Null-terminated string.
 */
static inline const char* commandName(uint8_t cmd)
{
    switch (cmd) {
        case CMD_HEARTBEAT: return "HEARTBEAT";
        case CMD_HW_ERROR:  return "HARDWARE ERROR";
        case CMD_STOP:      return "STOP (PREVENT OPERATION)";
        case CMD_SW_ERROR:  return "FAULT SAFE STATE / ALARM";
        case CMD_NORMAL:    return "NORMAL (JUNCTION CLEAR, SIREN OFF)";
        case CMD_SAFE_RELEASE_REQUEST: return "SAFE RELEASE REQUEST";
        case CMD_SAFE_RELEASE_ACK:     return "SAFE RELEASE ACK";
        case CMD_SAFE_RELEASE_DENIED:  return "SAFE RELEASE DENIED";
        case CMD_JUNCTION_ALARM: return "JUNCTION ALARM (SIREN ON)";
        default:            return "UNKNOWN";
    }
}

#endif /* CROSSING_JUNCTION_SAFETY_CMD_PACKET_H */
