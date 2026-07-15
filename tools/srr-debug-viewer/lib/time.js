// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Time conversion helpers. Each clip has a wall_time_start (unix epoch sec) and
// duration. The video MP4 starts at video_t = 0, which corresponds to
// wall_time_start. So:
//
//     wall_time = wall_time_start + video_t
//     video_t   = wall_time - wall_time_start
//
// Note: the BA/pss panels include ±5 s padding outside the clip window —
// rows with wall_time < wall_time_start have video_t < 0 (gray them out).

export function videoToWall(manifest, videoT) {
  return manifest.wall_time_start + videoT;
}

export function wallToVideo(manifest, wallT) {
  return wallT - manifest.wall_time_start;
}

/**
 * Binary-search for the row whose `wall_time` is closest to `targetWallT`.
 * Assumes rows are sorted ascending by `wall_time`. Returns row index, or -1
 * if rows is empty.
 */
export function findClosestByWall(rows, targetWallT) {
  if (rows.length === 0) return -1;
  let lo = 0, hi = rows.length - 1;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (rows[mid].wall_time < targetWallT) lo = mid + 1;
    else hi = mid;
  }
  // lo is the first row with wall_time >= target — also check lo-1
  if (lo > 0 && Math.abs(rows[lo - 1].wall_time - targetWallT) < Math.abs(rows[lo].wall_time - targetWallT)) {
    return lo - 1;
  }
  return lo;
}

export function fmtClock(epochSec) {
  if (epochSec == null || isNaN(epochSec)) return '—';
  const d = new Date(epochSec * 1000);
  const hh = String(d.getUTCHours()).padStart(2, '0');
  const mm = String(d.getUTCMinutes()).padStart(2, '0');
  const ss = String(d.getUTCSeconds()).padStart(2, '0');
  const ms = String(Math.floor((epochSec * 1000) % 1000)).padStart(3, '0');
  return `${hh}:${mm}:${ss}.${ms}`;
}

export function fmtVideoTime(t) {
  if (t == null || isNaN(t)) return '—';
  const sign = t < 0 ? '-' : ' ';
  const a = Math.abs(t);
  const m = Math.floor(a / 60);
  const s = (a % 60).toFixed(2).padStart(5, '0');
  return `${sign}${String(m).padStart(2, '0')}:${s}`;
}
