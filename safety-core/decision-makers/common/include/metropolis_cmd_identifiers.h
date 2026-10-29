/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef METROPOLIS_CMD_IDENTIFIERS_H
#define METROPOLIS_CMD_IDENTIFIERS_H

#include <stdint.h>

static const uint8_t METROPOLIS_ATL_PACKET_IDENTIFIER = 0xA2U;

/* The proximity SDM covers all three of its pair groups
 * (Agility_Digit_Humanoid<->Person, Forklift<->Person, Transporter<->Person)
 * behind this one identifier.
 *
 * It is 0xA5, the value the proximity app emitted before the forklift SDM was
 * merged into it. The merge briefly moved it to 0xA6 -- the forklift SDM's own
 * value -- but deployed receivers filter on 0xA2 and 0xA5 and drop everything
 * else, so a 0xA6 packet was rejected outright and the control loop never
 * closed. 0xA5 is what the installed base actually listens for.
 *
 * Reusing it is safe rather than merely convenient: the wire format and opcode
 * set are byte-for-byte what the 0xA5 app sent (see proximity_cmd_pkt.h), so an
 * existing receiver parses these packets correctly rather than misreading them.
 *
 * 0xA6 is now retired rather than free: a receiver built against the merged app
 * while it used 0xA6 may still be filtering on it, so handing 0xA6 to a new app
 * would let that receiver silently accept the wrong packets. */
static const uint8_t METROPOLIS_PROXIMITY_PACKET_IDENTIFIER = 0xA5U;
static const uint8_t METROPOLIS_CROSSING_JUNCTION_SAFETY_PACKET_IDENTIFIER = 0xA7U;

#endif /* METROPOLIS_CMD_IDENTIFIERS_H */
