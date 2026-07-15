// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Landing page — discovers all multi-test runs under data/ and lists them
// with a one-line summary per run. Auto-scrapes Python http.server's
// directory listing (the simplest way to enumerate without a backend).

import { fetchJson } from '../lib/manifest.js';

export async function render(main, header) {
  header.querySelector('#crumbs').innerHTML = `<strong>SRR Debug Viewer</strong>`;
  header.querySelector('#meta').textContent = '';

  main.innerHTML = '<p class="empty">Scanning data/ for runs…</p>';

  const runs = await discoverRuns();
  if (runs.length === 0) {
    main.innerHTML = `<p class="empty">
      No runs found under <code>data/</code>.<br><br>
      Symlink or copy a multi-test dir into <code>data/</code>:<br>
      <code>ln -s /path/to/multi-test-XXX data/</code>
    </p>`;
    return;
  }

  const previews = await Promise.all(runs.map(loadRunPreview));
  const cards = previews.map(renderCard).join('');
  main.innerHTML = `
    <p class="hint">${previews.length} run(s) under <code>data/</code> — click to browse.</p>
    <div class="run-grid">${cards}</div>
  `;
}

/**
 * Scrape `data/` for `multi-test-*` subdirectories. Tries the directory
 * listing HTML first (Python's http.server returns one). Falls back to
 * `data/index.json` if a custom server suppresses listings.
 */
async function discoverRuns() {
  // 1. Static index.json (preferred when present — explicit + ordered)
  try {
    const idx = await fetchJson('data/index.json');
    if (Array.isArray(idx?.runs)) return idx.runs;
  } catch {}

  // 2. Scrape Python http.server directory listing
  try {
    const html = await fetch('data/').then(r => r.ok ? r.text() : '');
    const found = new Set();
    const re = /<a href="(multi-test-[\w.-]+)\/">/g;
    let m;
    while ((m = re.exec(html)) !== null) found.add(m[1]);
    return [...found].sort();
  } catch {
    return [];
  }
}

async function loadRunPreview(name) {
  const runUrl = `data/${name}`;
  try {
    const summary = await fetch(`${runUrl}/summary.md`).then(r => r.ok ? r.text() : '');
    const headline = parseHeadline(summary);
    return { name, runUrl, ...headline };
  } catch {
    return { name, runUrl, error: 'summary.md not found' };
  }
}

/**
 * Pull headline metrics from a multi-test summary.md. Returns the run-level
 * GT-vs-PSF mute%/unmute% comparison plus BA detection rates and worst lags
 * — the things you'd want on a one-line at-a-glance card.
 */
function parseHeadline(text) {
  const out = {
    totalClips: null, pass: null, near: null, fail: null,
    muteCorrect: null, muteN: null,        // % frames PSF muted when GT-expected mute
    unmuteCorrect: null, unmuteN: null,    // % frames PSF unmuted when GT-expected unmute
    expectedMutePct: null, actualMutePct: null,
    baRoiMatched: null, baRoiTotal: null,
    baTwInMatched: null, baTwInTotal: null,
    baTwOutMatched: null, baTwOutTotal: null,
    worstUnmuteLag: null, worstMuteLag: null,
    overMuteFrames: null, underMuteFrames: null,
  };
  // total clips: **97**
  const t = text.match(/total clips:\s*\*\*(\d+)\*\*/);
  if (t) out.totalClips = +t[1];
  // verdicts: ✅ **40 PASS** (49%) · ⚠ 20 NEAR (24%) · ❌ **22 FAIL** (27%)
  const p = text.match(/(\d+)\s*PASS\*?\*?[^·]*·\s*[⚠*]+\s*\*?\*?(\d+)\s*NEAR[^·]*·\s*[❌*]+\s*\*?\*?(\d+)\s*FAIL/);
  if (p) { out.pass = +p[1]; out.near = +p[2]; out.fail = +p[3]; }
  // Mute correct%** ... : **42.4% (n=10327)**
  const mc = text.match(/Mute correct%[\s\S]*?\*\*([\d.]+)%\s*\(n=(\d+)\)\*\*/);
  if (mc) { out.muteCorrect = +mc[1]; out.muteN = +mc[2]; }
  const uc = text.match(/Unmute correct%[\s\S]*?\*\*([\d.]+)%\s*\(n=(\d+)\)\*\*/);
  if (uc) { out.unmuteCorrect = +uc[1]; out.unmuteN = +uc[2]; }
  // Compute aggregate expected/actual mute%
  if (out.muteN != null && out.unmuteN != null && out.muteCorrect != null && out.unmuteCorrect != null) {
    const n = out.muteN + out.unmuteN;
    out.expectedMutePct = 100 * out.muteN / n;
    // a + c where a = correctly-muted, c = over-mute (= unmuteN × (1 - unmuteCorrect))
    const a = out.muteN * (out.muteCorrect / 100);
    const c = out.unmuteN * (1 - out.unmuteCorrect / 100);
    out.actualMutePct = 100 * (a + c) / n;
  }
  // ROI entries (Person enters work zone): **63/68** (92.6%)
  const roi = text.match(/ROI entries[^*]*\*\*(\d+)\/(\d+)\*\*/);
  if (roi) { out.baRoiMatched = +roi[1]; out.baRoiTotal = +roi[2]; }
  const twi = text.match(/TW IN[^*]*\*\*(\d+)\/(\d+)\*\*/);
  if (twi) { out.baTwInMatched = +twi[1]; out.baTwInTotal = +twi[2]; }
  const two = text.match(/TW OUT[^*]*\*\*(\d+)\/(\d+)\*\*/);
  if (two) { out.baTwOutMatched = +two[1]; out.baTwOutTotal = +two[2]; }
  // worst UNMUTE lag (danger response): **30700.0 ms**
  const wu = text.match(/worst UNMUTE lag[^*]*\*\*([\d.]+)\s*ms\*\*/);
  if (wu) out.worstUnmuteLag = +wu[1];
  const wm = text.match(/worst MUTE lag[^*]*\*\*([\d.]+)\s*ms\*\*/);
  if (wm) out.worstMuteLag = +wm[1];
  // mismatch frame totals — over-mute: 8347 · under-mute: 5295
  const mm = text.match(/over-mute:\s*(\d+)\s*·\s*under-mute:\s*(\d+)/);
  if (mm) { out.overMuteFrames = +mm[1]; out.underMuteFrames = +mm[2]; }
  return out;
}

function renderCard(r) {
  if (r.error) {
    return `<a class="run-card error" href="?run=${encodeURIComponent(r.runUrl)}">
      <div class="run-name">${r.name}</div>
      <div class="run-meta">${r.error}</div>
    </a>`;
  }
  const passPct = r.totalClips ? Math.round(100 * r.pass / r.totalClips) : null;
  const fmt = v => v == null ? '—' : v.toFixed(1);
  const correctCell = (pct, n) => {
    if (pct == null) return '<span class="hint">—</span>';
    const cls = pct < 80 ? 'pct warn' : 'pct';
    return `<span class="${cls}">${fmt(pct)}%</span> <span class="hint">(n=${n ?? '?'})</span>`;
  };
  const baCell = (m, t) => m == null ? '—' :
    `<span class="${m === t ? '' : 'warn'}">${m}/${t}</span>`;
  return `<div class="run-card-wrap">
    <a class="run-card" href="?run=${encodeURIComponent(r.runUrl)}">
      <div class="run-name">${r.name}</div>
      <div class="run-meta">
        <span>${r.totalClips ?? '?'} clips</span>
        <span class="verdict pass">${r.pass ?? '?'}P</span>
        <span class="verdict near">${r.near ?? '?'}N</span>
        <span class="verdict fail">${r.fail ?? '?'}F</span>
        ${passPct != null ? `<span class="pass-pct">${passPct}% pass</span>` : ''}
      </div>
      <table class="run-summary">
        <tr><td>GT expected mute %</td><td>${fmt(r.expectedMutePct)}%</td></tr>
        <tr><td>Mute correct %</td><td>${correctCell(r.muteCorrect, r.muteN)}</td></tr>
        <tr><td>Unmute correct %</td><td>${correctCell(r.unmuteCorrect, r.unmuteN)}</td></tr>
        <tr><td>BA ROI entry</td><td>${baCell(r.baRoiMatched, r.baRoiTotal)}</td></tr>
        <tr><td>BA TW in</td><td>${baCell(r.baTwInMatched, r.baTwInTotal)}</td></tr>
        <tr><td>BA TW out</td><td>${baCell(r.baTwOutMatched, r.baTwOutTotal)}</td></tr>
        <tr><td>worst unmute lag</td><td>${fmtMs(r.worstUnmuteLag)}</td></tr>
        <tr><td>worst mute lag</td><td>${fmtMs(r.worstMuteLag)}</td></tr>
      </table>
    </a>
    <div class="run-card-actions">
      <a href="?run=${encodeURIComponent(r.runUrl)}#summary">📊 view full summary</a>
      <span class="sep">·</span>
      <a href="${r.runUrl}/summary.md" target="_blank" rel="noopener">raw .md ↗</a>
    </div>
  </div>`;
}

function fmtMs(ms) {
  if (ms == null) return '—';
  if (ms < 1000) return `${ms.toFixed(0)} ms`;
  return `${(ms / 1000).toFixed(1)} s`;
}
