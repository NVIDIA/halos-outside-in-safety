// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Minimal CSV parser. Sufficient for clip_logs CSVs (well-formed, no embedded
// quotes/newlines). Treats first row as header. Coerces numbers + booleans.

const NUM_RE = /^-?\d+(\.\d+)?(e[+-]?\d+)?$/i;

function coerce(v) {
  if (v === '' || v === undefined) return null;
  if (v === 'True'  || v === 'true')  return true;
  if (v === 'False' || v === 'false') return false;
  if (NUM_RE.test(v)) return Number(v);
  return v;
}

export function parseCsv(text) {
  const lines = text.split(/\r?\n/).filter(l => l.length > 0);
  if (lines.length === 0) return [];
  const header = lines[0].split(',');
  const out = new Array(lines.length - 1);
  for (let i = 1; i < lines.length; i++) {
    const cols = lines[i].split(',');
    const row = {};
    for (let j = 0; j < header.length; j++) {
      row[header[j]] = coerce(cols[j]);
    }
    out[i - 1] = row;
  }
  return out;
}

export async function fetchCsv(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`fetch ${url} → ${res.status}`);
  return parseCsv(await res.text());
}
