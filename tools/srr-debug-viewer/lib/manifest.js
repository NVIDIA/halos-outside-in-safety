// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Loaders for the SRR clip_logs evidence bundle JSON files.

export async function fetchJson(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`fetch ${url} → ${res.status}`);
  return res.json();
}

/**
 * Discover all scenarios in a multi-test run and load each scenario's index.
 * Strategy: parse the top-level summary.md to find scenario names, OR fall
 * back to a hard-coded canonical list.
 *
 * Returns: [{scenario, n_clips, clips: [...]}, ...]
 */
export async function loadRun(runUrl) {
  const summary = await fetch(`${runUrl}/summary.md`).then(r => r.ok ? r.text() : '');
  const scenarios = scenariosFromSummary(summary);
  if (scenarios.length === 0) throw new Error(`No scenarios discovered in ${runUrl}/summary.md`);

  const indexes = await Promise.all(scenarios.map(async scen => {
    try {
      const idx = await fetchJson(`${runUrl}/${scen}/clip_logs/index.json`);
      return { ...idx, scenario: scen };
    } catch (e) {
      return { scenario: scen, n_clips: 0, clips: [], error: String(e) };
    }
  }));
  return indexes;
}

/**
 * Pull scenario names from summary.md. The authoritative clip→scenario mapping
 * is the "Run" column (col 2) of the "## Per-clip detail" table:
 *   | [scn_0000](...) | in-roi-2min | ❌ | ... |
 * We read scenarios from there so we never mistake a clip id (e.g. the first
 * column of the Phase 2 rollup table) for a scenario name.
 */
function scenariosFromSummary(text) {
  const found = new Set();

  // Preferred: Run column of the Per-clip detail table.
  const idx = text.indexOf('## Per-clip detail');
  if (idx >= 0) {
    for (const line of text.slice(idx).split('\n')) {
      if (!line.startsWith('|')) continue;
      const cells = line.replace(/^\|/, '').replace(/\|$/, '').split('|').map(c => c.trim());
      if (!/\[scn_\d+\]/.test(cells[0] || '')) continue; // skip header/align rows
      const scen = cells[1];
      if (scen) found.add(scen);
    }
  }
  if (found.size) return [...found].sort();

  // Fallback (legacy bundles without a per-clip table): scan col-1 scenario
  // names, but never accept a clip id (scn_NNNN) as a scenario.
  const re = /^\|\s*([a-z][\w-]+(?:-\d+min)?)\s*\|/gm;
  let m;
  while ((m = re.exec(text)) !== null) {
    if (m[1] === 'Run' || m[1] === 'Scenario' || /^scn_\d+$/.test(m[1])) continue;
    found.add(m[1]);
  }
  return [...found].sort();
}

/**
 * Parse the per-clip detail table from summary.md. Returns a map keyed by
 * "<scenario>::<clip_id>" → { ba_roi, ba_tw_in, ba_tw_out } strings (e.g. "1/2").
 *
 * Per-clip table header (from aggregator render_summary):
 *   | Clip | Run | Verdict | match% | over-mute% | under-mute% | unmute_lag (med/max) |
 *   mute_lag (med/max) | %char_in_roi | %fk_trailer | BA ROI | BA TW in | BA TW out | video |
 */
export function parsePerClipTable(text) {
  const out = new Map();
  // Find the "## Per-clip detail" section
  const idx = text.indexOf('## Per-clip detail');
  if (idx < 0) return out;
  const lines = text.slice(idx).split('\n');
  for (const line of lines) {
    if (!line.startsWith('|')) continue;
    const cells = line.replace(/^\|/, '').replace(/\|$/, '').split('|').map(c => c.trim());
    // Skip header + alignment row
    if (cells[0] === 'Clip' || /^:?-+:?$/.test(cells[0])) continue;
    // First cell is markdown link `[scn_XXXX](...)`. Pull the clip id out.
    const linkMatch = cells[0].match(/\[(scn_\d+)\]/);
    if (!linkMatch) continue;
    const scnId = linkMatch[1];
    const scenario = cells[1];
    if (!scenario) continue;
    out.set(`${scenario}::${scnId}`, {
      ba_roi:    cells[10] ?? '',
      ba_tw_in:  cells[11] ?? '',
      ba_tw_out: cells[12] ?? '',
    });
  }
  return out;
}

export async function loadClipManifest(runUrl, scenario, clipId) {
  return fetchJson(`${runUrl}/${scenario}/clip_logs/${clipId}/manifest.json`);
}

/**
 * Parse the per-clip aggregator report (`<scenario>/reports/<clip_id>.md`)
 * for verdict-relevant metrics. The report is rendered by aggregator.py and
 * has a stable line format — see render_clip_report().
 *
 * Returns an object with all percentages as numbers; null if a field couldn't
 * be parsed (e.g. report missing, format drifted).
 */
export async function loadClipMetrics(runUrl, scenario, clipId) {
  const url = `${runUrl}/${scenario}/reports/${clipId}.md`;
  let text = '';
  try {
    const res = await fetch(url);
    if (res.ok) text = await res.text();
  } catch {}
  return parseClipReport(text);
}

function parseClipReport(text) {
  // "- any character in ROI: **100.0%** of frames"  →  100.0
  const pctBold = label => {
    const re = new RegExp(label + '[^*]*\\*\\*([\\d.]+)%?\\*\\*');
    const m = text.match(re);
    return m ? +m[1] : null;
  };
  // "- **over-mute** (actual=mute, expected=unmute): 829 frames (99.76%)"  →  99.76
  // Note: a parenthesized clarifier sits between **label** and "frames (NN%)" —
  // skip it with a non-greedy [\s\S]*? all the way to "frames (".
  const pctParens = label => {
    const re = new RegExp('\\*\\*' + label + '\\*\\*[\\s\\S]*?frames\\s*\\(([\\d.]+)%\\)');
    const m = text.match(re);
    return m ? +m[1] : null;
  };
  return {
    char_in_roi_pct:    pctBold('any character in ROI'),
    fk_trailer_pct:     pctBold('forklift in trailer'),
    expected_mute_pct:  pctBold('expected MUTE'),
    actual_mute_pct:    pctBold('actual MUTE'),
    match_pct:          pctBold('per-frame match rate'),
    over_mute_pct:      pctParens('over-mute'),
    under_mute_pct:     pctParens('under-mute'),
  };
}
