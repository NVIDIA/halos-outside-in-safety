// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Run browser: top section renders the run's summary.md (markdown → HTML),
// then per-scenario clip tables. Each clip row is enriched in parallel with
// metrics from the per-clip aggregator report.

import { loadRun, loadClipMetrics, parsePerClipTable } from '../lib/manifest.js';
import { renderMd } from '../lib/md.js';

export async function render(main, header, runUrl) {
  const indexes = await loadRun(runUrl);

  // Reload summary.md (loadRun also fetched it but didn't return body)
  const summaryText = await fetch(`${runUrl}/summary.md`).then(r => r.ok ? r.text() : '');
  const baTable = parsePerClipTable(summaryText);

  // ---- header ----
  const total = indexes.reduce((s, i) => s + (i.clips?.length || 0), 0);
  const verdicts = { PASS: 0, NEAR: 0, FAIL: 0 };
  for (const idx of indexes) {
    for (const c of idx.clips || []) {
      if (verdicts[c.verdict] != null) verdicts[c.verdict]++;
    }
  }
  header.querySelector('#crumbs').innerHTML = `
    <a href="?">data/</a>
    <span class="sep">›</span>
    <a href="?run=${encodeURIComponent(runUrl)}">${runUrl.split('/').pop() || runUrl}</a>
  `;
  header.querySelector('#meta').innerHTML =
    `${total} clips · <span class="verdict pass">${verdicts.PASS}P</span>` +
    ` <span class="verdict near">${verdicts.NEAR}N</span>` +
    ` <span class="verdict fail">${verdicts.FAIL}F</span>`;

  // ---- main: summary.md block + scenario sections ----
  const sectionsHtml = indexes.map(idx => sectionShell(runUrl, idx)).join('') ||
    '<p class="empty">No scenarios found.</p>';
  // Drop the Per-clip detail section — it's duplicated below as the scenario
  // tables (with verdict pills + clickable rows). Keep everything above it
  // (headline + per-run rollup).
  const cutAt = summaryText.indexOf('## Per-clip detail');
  const headerSummary = cutAt >= 0 ? summaryText.slice(0, cutAt).trimEnd() : summaryText;
  const summaryHtml = headerSummary
    ? `<section id="summary" class="md-block">
         <h2 class="md-block-title">
           <button class="md-toggle" aria-expanded="true">▾</button>
           Run summary
           <a class="md-raw" href="${runUrl}/summary.md" target="_blank" rel="noopener">raw .md ↗</a>
         </h2>
         <div class="md-body">${renderMd(headerSummary)}</div>
       </section>`
    : '';
  const navHtml = `<nav class="run-nav">
    <a href="#summary">📊 Summary</a>
    <a href="#scenarios">📂 Scenarios &amp; clips</a>
  </nav>`;
  main.innerHTML = navHtml + summaryHtml +
    `<div id="scenarios">${sectionsHtml}</div>`;

  // Wire up collapse toggle for the summary block.
  const toggle = main.querySelector('.md-toggle');
  if (toggle) {
    toggle.addEventListener('click', e => {
      e.preventDefault(); e.stopPropagation();
      const body = main.querySelector('.md-body');
      const expanded = toggle.getAttribute('aria-expanded') === 'true';
      toggle.setAttribute('aria-expanded', String(!expanded));
      toggle.textContent = expanded ? '▸' : '▾';
      body.style.display = expanded ? 'none' : '';
    });
  }

  // Fan out per-clip metric fetches in parallel.
  const tasks = [];
  for (const idx of indexes) {
    for (const c of (idx.clips || [])) {
      tasks.push(enrichRow(runUrl, idx.scenario, c, baTable));
    }
  }
  await Promise.all(tasks);
}

function sectionShell(runUrl, idx) {
  const clips = idx.clips || [];
  if (idx.error) {
    return `<section class="scenario-section">
      <h2>${idx.scenario}</h2>
      <p class="empty">${idx.error}</p>
    </section>`;
  }
  return `<section class="scenario-section">
    <h2>${idx.scenario} <span style="color: var(--fg2); font-weight: 400">(${clips.length})</span></h2>
    <table class="clip-list">
      <thead>
        <tr>
          <th>Clip</th>
          <th>Verdict</th>
          <th class="right">match%</th>
          <th class="right" title="of frames GT-expected mute, fraction PSF correctly muted">Mute correct%</th>
          <th class="right" title="of frames GT-expected unmute, fraction PSF correctly unmuted">Unmute correct%</th>
          <th class="right">%char_in_roi</th>
          <th class="right">%fk_trailer</th>
          <th class="right">BA ROI entry</th>
          <th class="right">BA TW in</th>
          <th class="right">BA TW out</th>
        </tr>
      </thead>
      <tbody>${clips.map(c => clipRowSkeleton(runUrl, idx.scenario, c)).join('')}</tbody>
    </table>
  </section>`;
}

function clipRowSkeleton(runUrl, scenario, c) {
  const v = (c.verdict || 'unknown').toLowerCase();
  const m = c.match_pct != null ? c.match_pct.toFixed(2) : '—';
  const url = `?run=${encodeURIComponent(runUrl)}&scenario=${scenario}&clip=${c.clip_id}`;
  return `<tr id="row-${scenario}-${c.clip_id}">
    <td><a href="${url}">${c.clip_id}</a></td>
    <td><span class="verdict ${v}">${c.verdict || '?'}</span></td>
    <td class="right">${m}</td>
    <td class="right metric-mute-correct"><span class="hint">…</span></td>
    <td class="right metric-unmute-correct"><span class="hint">…</span></td>
    <td class="right metric-roi"><span class="hint">…</span></td>
    <td class="right metric-trailer"><span class="hint">…</span></td>
    <td class="right metric-ba-roi"><span class="hint">…</span></td>
    <td class="right metric-ba-twin"><span class="hint">…</span></td>
    <td class="right metric-ba-twout"><span class="hint">…</span></td>
  </tr>`;
}

async function enrichRow(runUrl, scenario, c, baTable) {
  let metrics = {};
  try {
    metrics = await loadClipMetrics(runUrl, scenario, c.clip_id);
  } catch {}
  const row = document.getElementById(`row-${scenario}-${c.clip_id}`);
  if (!row) return;

  // Mute correct% per clip — derive from expected_mute% and under_mute%.
  // mute_correct = (a)/(a+b) where a = correctly-muted, b = under-mute.
  // a/N = expected_mute% - under_mute%, (a+b)/N = expected_mute%.
  const muteCorrect = pctRatio(
    (metrics.expected_mute_pct ?? 0) - (metrics.under_mute_pct ?? 0),
    metrics.expected_mute_pct);
  // Unmute correct% — same, on the unmute side.
  const expectedUnmute = metrics.expected_mute_pct == null ? null : 100 - metrics.expected_mute_pct;
  const unmuteCorrect = pctRatio(
    (expectedUnmute ?? 0) - (metrics.over_mute_pct ?? 0),
    expectedUnmute);

  row.querySelector('.metric-mute-correct').innerHTML   = pctCell(muteCorrect, /*warnBelow*/ 80, /*nullLabel*/ 'n/a');
  row.querySelector('.metric-unmute-correct').innerHTML = pctCell(unmuteCorrect, 80, 'n/a');
  row.querySelector('.metric-roi').innerHTML            = pctCell(metrics.char_in_roi_pct);
  row.querySelector('.metric-trailer').innerHTML        = pctCell(metrics.fk_trailer_pct);

  const ba = baTable.get(`${scenario}::${c.clip_id}`) || {};
  row.querySelector('.metric-ba-roi').innerHTML   = baCell(ba.ba_roi);
  row.querySelector('.metric-ba-twin').innerHTML  = baCell(ba.ba_tw_in);
  row.querySelector('.metric-ba-twout').innerHTML = baCell(ba.ba_tw_out);
}

function pctRatio(numerator, denominator) {
  if (denominator == null || denominator <= 0) return null;
  return Math.max(0, 100 * numerator / denominator);
}

function pctCell(p, warnBelow = null, nullLabel = '—') {
  if (p == null || isNaN(p)) return `<span class="hint">${nullLabel}</span>`;
  const cls = warnBelow != null && p < warnBelow ? 'pct warn' : 'pct';
  return `<span class="${cls}">${p.toFixed(1)}</span>`;
}

function baCell(v) {
  if (!v || v === '–' || v === '-' || v === '—') return '<span class="hint">—</span>';
  // values look like "1/2" — warn if matched < total
  const m = v.match(/^(\d+)\/(\d+)$/);
  if (m && m[1] !== m[2]) return `<span class="warn">${v}</span>`;
  return v;
}
