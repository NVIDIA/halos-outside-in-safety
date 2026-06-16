/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file  atl_cmd_pkt.h
 * @brief  64-byte command / acknowledgement packet definition.
 *
 * This header is self-contained and can be included by any application
 * (sender, receiver, logger, test harness, etc.) that needs to build
 * or parse decision command packets.
 *
 * Byte Map (64 bytes total):
 *   [0]       Identifier             (uint8_t)   – magic byte 0xA2
 *   [1-2]     Sequence Number        (uint16_t)
 *   [3]       Command                (uint8_t)
 *   [4-11]    Timestamp seconds UTC  (uint64_t)  – seconds since epoch
 *   [12-19]   Timestamp microseconds (uint64_t)  – µs component
 *   [20-23]   CRC-32                 (uint32_t)
 *   [24-43]   Object Record 0        (20 bytes)
 *   [44-63]   Object Record 1        (20 bytes)
 */

#ifndef ATL_CMD_PACKET_H
#define ATL_CMD_PACKET_H

#include <cstdint>
#include <cstddef>
#include <cstring>

/* ======================== Constants ======================== */

#define COMMAND_HEADER_SIZE   24
#define OBJECT_SIZE           20
#define ATL_PACKET_IDENTIFIER 0xA2
#define COMMAND_NUM_OBJECTS   2
#define COMMAND_PACKET_SIZE   (COMMAND_HEADER_SIZE + (COMMAND_NUM_OBJECTS * OBJECT_SIZE))

/* Command opcodes */
#define CMD_HEARTBEAT     0x00
#define CMD_HW_ERROR      0x01
#define CMD_MUTE          0x02   /* Allow Operation   */
#define CMD_SW_ERROR      0x03
#define CMD_UNMUTE        0x07   /* Prevent Operation   */

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
    uint8_t         identifier;                    /**< Byte  0     : magic 0xA2               */
    uint16_t        seq;                           /**< Bytes 1-2   : sequence number          */
    uint8_t         command;                       /**< Byte  3     : command opcode           */
    uint64_t        ts_seconds;                    /**< Bytes 4-11  : UTC seconds since epoch  */
    uint64_t        ts_microseconds;               /**< Bytes 12-19 : microseconds component   */
    uint32_t        crc32;                         /**< Bytes 20-23 : CRC-32                   */
    ObjectRecord objects[COMMAND_NUM_OBJECTS];     /**< Bytes 24-63 : 2×20-byte object records */
} CmdPacket;               /* 64 bytes total */

#pragma pack(pop)

/* ======================== CRC-32 Utility ======================== */

/**
 * @brief  Compute CRC-32 (ISO 3309 / ITU-T V.42) over a byte buffer.
 * @param  data   Pointer to input buffer.
 * @param  length Number of bytes.
 * @return CRC-32 value.
 */
static inline uint32_t computeCRC32(const void* data, size_t length)
{
    static uint32_t table[256];
    static bool     tableReady = false;

    if (!tableReady) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
            table[i] = crc;
        }
        tableReady = true;
    }

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
    /* Hash header before CRC field: bytes 0-19 */
    uint32_t crc = 0xFFFFFFFF;
    {
        static uint32_t table[256];
        static bool     tableReady = false;
        if (!tableReady) {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int j = 0; j < 8; j++)
                    c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0u);
                table[i] = c;
            }
            tableReady = true;
        }
        for (size_t i = 0; i < 20; i++)
            crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFF];
        for (size_t i = 24; i < COMMAND_PACKET_SIZE; i++)
            crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFF];
    }
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
        case CMD_MUTE:      return "MUTE (ALLOW OPERATION)";
        case CMD_SW_ERROR:  return "SOFTWARE ERROR";
        case CMD_UNMUTE:    return "UNMUTE (PREVENT OPERATION)";
        default:            return "UNKNOWN";
    }
}

#endif /* ATL_CMD_PACKET_H */