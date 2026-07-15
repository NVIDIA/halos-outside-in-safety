// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Single-clip view: video + 4 synchronized panels (PSF / GT / BA / pss).

import { fetchCsv } from '../lib/csv.js';
import { fetchJson, loadClipManifest } from '../lib/manifest.js';
import { videoToWall, fmtClock, fmtVideoTime } from '../lib/time.js';
import { createPanel } from './panel.js';
import { registerTeardown } from '../app.js';

// Static per-clip perception summary (not time-synced). Surfaces the per-class
// detect-fail / tracking-loss, trailer-boundary recall, split and pos offset-vs-jitter
// so the worst object (forklift) is not hidden behind a blended number.
function phase2Card(p2) {
  if (!p2) return '';
  const pct = v => (v == null ? '—' : `${v}%`);
  const num = v => (v == null ? '—' : v);
  const fail = v => (v == null ? null : Math.round((100 - v) * 10) / 10); // tracking-loss% = 100 − tracking-ok%
  const cls = (p2.per_class) || {};
  const row = (name, d) => d ? `
    <tr><td>${name}</td>
      <td>${num(d.recall)}</td>
      <td class="${(d.track_loss_pct ?? 0) >= 25 ? 'warn' : ''}">${pct(d.track_loss_pct)}</td>
      <td class="${(fail(d.tracking_ok_pct) ?? 0) > 10 ? 'warn' : ''}">${pct(fail(d.tracking_ok_pct))}</td>
      <td>${num(d.pos_offset_m)} m</td>
      <td>${num(d.pos_jitter_p95_m)} m</td></tr>` : '';
  const fb = p2.forklift_boundary || {};
  const sp = p2.split || {};
  const rg = p2.recall_at_gate || {};
  const ids = sp.unique_tids_by_class || {};
  return `
    <div class="gt-summary phase2-card">
      <h4>perception metrics (clip)</h4>
      <table class="mini">
        <thead><tr><th>class</th><th>recall</th><th>detect-fail</th><th>tracking-loss</th><th>offset</th><th>jitter p95</th></tr></thead>
        <tbody>
          ${row('🧍 Person', cls.Person)}
          ${row('🚜 Forklift', cls.Forklift)}
        </tbody>
      </table>
      <dl>
        <dt>recall @0.5/1.0/1.5m</dt><dd>${num(rg['0.5'])} / ${num(rg['1.0'])} / ${num(rg['1.5'])}</dd>
        <dt>tracking-loss (clip)</dt><dd>${pct(fail(p2.tracking_ok_pct))}</dd>
        <dt>🚜 boundary detect-fail</dt><dd>${pct(fb.track_loss_pct)} (n=${num(fb.present)})</dd>
        <dt>split frames</dt><dd>${pct(sp.frames_with_split_pct)} · ids P${num(ids.Person)}/F${num(ids.Forklift)}</dd>
        <dt>id-switches</dt><dd>${num(p2.total_id_switches)}</dd>
        <dt>coverage</dt><dd>${p2.coverage_mode || '—'}${p2.coverage_pad_m != null ? ` (pad ${p2.coverage_pad_m} m)` : ''}</dd>
        ${p2.forklift_origin_offset_m ? `<dt>🚜 GT origin→centre</dt><dd>long ${num(p2.forklift_origin_offset_m[0])} m / lat ${num(p2.forklift_origin_offset_m[1])} m (corrected)</dd>` : ''}
      </dl>
      <p class="hint">detect-fail = % frames object not seen (DETECTION miss); tracking-loss = % detected frames it did NOT keep one id (TRACKING failure). offset = residual GT-vs-box-centre error after the forklift origin correction (jitter = p95 spread). coverage pad 0 = region is the exact detection convex hull (no outward bleed past the camera footprint).</p>
    </div>`;
}

export async function render(main, header, runUrl, scenario, clipId) {
  const baseUrl = `${runUrl}/${scenario}/clip_logs/${clipId}`;
  const manifest = await loadClipManifest(runUrl, scenario, clipId);

  // ---- header / breadcrumbs ----
  header.querySelector('#crumbs').innerHTML = `
    <a href="?">data/</a>
    <span class="sep">›</span>
    <a href="?run=${encodeURIComponent(runUrl)}">${runUrl.split('/').pop()}</a>
    <span class="sep">›</span>
    <span>${scenario}</span>
    <span class="sep">›</span>
    <span>${clipId}</span>
  `;
  const v = (manifest.verdict || 'unknown').toLowerCase();
  header.querySelector('#meta').innerHTML = `
    <span class="verdict ${v}">${manifest.verdict || '?'}</span>
    match ${manifest.match_pct != null ? manifest.match_pct.toFixed(2) + '%' : '—'} ·
    char_in_roi ${manifest.gt_summary?.char_in_roi_pct ?? '—'}% ·
    fk_trailer ${manifest.gt_summary?.forklift_in_trailer_pct ?? '—'}% ·
    duration ${manifest.duration_s.toFixed(1)}s ·
    <span id="now-clock">—</span>
  `;
  const nowClockEl = header.querySelector('#now-clock');

  main.innerHTML = `
    <div class="clip-view">
      <div class="video-side">
        <video id="vid" controls preload="metadata"></video>
        <div class="gt-summary">
          <dl>
            <dt>scenario</dt><dd>${scenario}</dd>
            <dt>clip start (wall)</dt><dd>${fmtClock(manifest.wall_time_start)} UTC</dd>
            <dt>duration</dt><dd>${manifest.duration_s.toFixed(2)} s</dd>
            <dt>samples</dt><dd>${manifest.counts?.samples ?? '—'}</dd>
            <dt>BA events (±${manifest.padding_s}s)</dt><dd>${manifest.counts?.ba_events ?? '—'}</dd>
            <dt>pss lines (raw)</dt><dd>${manifest.counts?.pss_lines_raw ?? '—'}</dd>
            <dt>expected mute</dt><dd>${manifest.gt_summary?.expected_mute_pct ?? '—'}%</dd>
          </dl>
        </div>
        ${phase2Card(manifest.phase2_metrics)}
      </div>
      <div id="panels" class="panels"></div>
    </div>
  `;

  const video = document.getElementById('vid');
  if (manifest.video) video.src = `${baseUrl}/${manifest.video}`;

  const panels = document.getElementById('panels');
  panels.innerHTML = '<p class="empty">Loading panels…</p>';
  const sync = await mountPanels(panels, baseUrl, manifest);

  // sync loop — drives all panels off video.currentTime
  let raf = 0;
  function tick() {
    if (!video.paused && !video.seeking) {
      const wallT = videoToWall(manifest, video.currentTime);
      sync(wallT);
      nowClockEl.textContent = `t=${fmtVideoTime(video.currentTime)} (${fmtClock(wallT)})`;
    }
    raf = requestAnimationFrame(tick);
  }
  // Also fire on seek (so scrub works while paused).
  function seekSync() {
    const wallT = videoToWall(manifest, video.currentTime);
    sync(wallT);
    nowClockEl.textContent = `t=${fmtVideoTime(video.currentTime)} (${fmtClock(wallT)})`;
  }
  video.addEventListener('seeked', seekSync);
  video.addEventListener('timeupdate', seekSync);
  video.addEventListener('loadedmetadata', seekSync);
  raf = requestAnimationFrame(tick);

  registerTeardown(() => {
    cancelAnimationFrame(raf);
    video.removeEventListener('seeked', seekSync);
    video.removeEventListener('timeupdate', seekSync);
    video.removeEventListener('loadedmetadata', seekSync);
    video.pause();
    video.removeAttribute('src');
    video.load();   // releases buffered MP4 + decoder threads
  });
}

async function mountPanels(container, baseUrl, manifest) {
  // track_id renderer: keep "miss" ONLY for truly absent ids (null / empty).
  // A real id of 0 must still show as "0" (csv.js coerces it to a falsy number).
  const tid = v => (v == null || v === '') ? '<span class="warn">miss</span>' : v;

  const tasks = [
    loadAndBuild('PSF state — /safety/command + is_muted', `${baseUrl}/psf_timeline.csv`, [
      { key: 'wall_time', label: 't (rel)', fmt: t => relSec(t, manifest) },
      { key: 'is_muted',  label: 'mute' },
      { key: 'command_name', label: 'command' },
      { key: 'status_name',  label: 'status' },
      { key: 'is_alarm',  label: 'alarm' },
      { key: 'sequence',  label: 'seq' },
    ], manifest),
    loadAndBuild('GT positions + computed flags', `${baseUrl}/gt_positions.csv`, [
      { key: 'wall_time', label: 't (rel)', fmt: t => relSec(t, manifest) },
      { key: 'any_char_in_roi', label: 'in_roi' },
      { key: 'forklift_in_trailer', label: 'in_trailer' },
      { key: 'expected_mute', label: 'expect_mute' },
      { key: 'forklift_x', label: 'fk_x', fmt: v => v?.toFixed?.(2) ?? '—' },
      { key: 'forklift_y', label: 'fk_y', fmt: v => v?.toFixed?.(2) ?? '—' },
    ], manifest),
    loadAndBuild('BA events (Kafka mdx-events)', `${baseUrl}/ba_events.csv`, [
      { key: 'wall_time', label: 't (rel)', fmt: t => relSec(t, manifest) },
      { key: 'event_type', label: 'event' },
      { key: 'classes', label: 'class' },
      { key: 'direction', label: 'dir' },
      { key: 'ids', label: 'ids' },
      { key: 'in_clip', label: 'in_clip' },
    ], manifest),
  ];

  // Phase 2 (3D pipeline) — detections + tracker timeline, only when present.
  if (manifest.streams?.detections) {
    tasks.push(loadAndBuild('Detections (mdx-bev) vs GT', `${baseUrl}/detections.csv`, [
      { key: 'wall_time', label: 't (rel)', fmt: t => relSec(t, manifest) },
      { key: 'track_id', label: 'tid' },
      { key: 'class', label: 'class' },
      { key: 'x', label: 'x', fmt: v => v?.toFixed?.(2) ?? v ?? '—' },
      { key: 'y', label: 'y', fmt: v => v?.toFixed?.(2) ?? v ?? '—' },
      { key: 'conf', label: 'conf', fmt: v => v?.toFixed?.(2) ?? v ?? '—' },
      { key: 'matched_gt', label: 'matched GT',
        fmt: v => v ? v : '<span class="warn">FP</span>' },
      { key: 'dist_m', label: 'dist (m)', fmt: v => (v === '' || v == null) ? '—' : v },
      { key: 'in_coverage', label: 'in_cov',
        fmt: v => (String(v) === 'false' || v === false) ? '<span class="warn">out</span>' : 'yes' },
    ], manifest));
    tasks.push(loadAndBuild('Tracker state — track_id per actor', `${baseUrl}/tracker_state.csv`, [
      { key: 'wall_time', label: 't (rel)', fmt: t => relSec(t, manifest) },
      { key: 'n_dets', label: '#det' },
      // NOTE: track_id 0 is a VALID id — csv.js coerces "0" → number 0 (falsy) and
      // an empty cell → null, so `v || 'miss'` wrongly flags a real id 0 as a miss.
      // Only render "miss" when the value is actually absent (null / '').
      { key: 'char_0_tid', label: 'char_0', fmt: tid },
      { key: 'char_1_tid', label: 'char_1', fmt: tid },
      { key: 'char_2_tid', label: 'char_2', fmt: tid },
      { key: 'forklift_tid', label: 'forklift', fmt: tid },
    ], manifest));
  }

  // BA-reported positions (mdx-behavior) vs GT — BA localisation accuracy.
  if (manifest.streams?.ba_positions) {
    tasks.push(loadAndBuild('BA positions (mdx-behavior) vs GT', `${baseUrl}/ba_positions.csv`, [
      { key: 'wall_time', label: 't (rel)', fmt: t => relSec(t, manifest) },
      { key: 'track_id', label: 'tid' },
      { key: 'x', label: 'x', fmt: v => v?.toFixed?.(2) ?? v ?? '—' },
      { key: 'y', label: 'y', fmt: v => v?.toFixed?.(2) ?? v ?? '—' },
      { key: 'speed', label: 'speed', fmt: v => v?.toFixed?.(2) ?? v ?? '—' },
      { key: 'direction', label: 'dir' },
      { key: 'matched_gt', label: 'matched GT',
        fmt: v => v ? v : '<span class="warn">no GT</span>' },
      { key: 'dist_m', label: 'err (m)', fmt: v => (v === '' || v == null) ? '—' : v },
    ], manifest));
  }

  tasks.push(loadPssAndBuild(`${baseUrl}/pss_log.jsonl`, manifest));
  const built = await Promise.all(tasks);
  container.innerHTML = '';
  built.forEach(p => container.appendChild(p.root));
  return wallT => built.forEach(p => p.sync && p.sync(wallT));
}

function errorPanel(title, e) {
  const root = document.createElement('section');
  root.className = 'panel error';
  root.innerHTML = `<header><span>${title}</span></header>
    <div>panel failed to load.<pre>${escapeHtml(e.message || String(e))}</pre></div>`;
  return { root, sync: () => {} };
}
function escapeHtml(s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); }

async function loadAndBuild(title, url, cols, manifest) {
  try {
    const rows = await fetchCsv(url);
    rows.sort((a, b) => a.wall_time - b.wall_time);
    console.log(`[panel] ${title}: ${rows.length} rows from ${url}`);
    return createPanel({ title, rows, cols, clipStart: manifest.wall_time_start });
  } catch (e) {
    console.warn(`panel ${title}:`, e);
    return errorPanel(title, e);
  }
}

async function loadPssAndBuild(url, manifest) {
  let rows = [];
  try {
    const res = await fetch(url);
    const text = res.ok ? await res.text() : '';
    rows = text.split(/\r?\n/).filter(l => l.length > 0).map(l => JSON.parse(l));
    rows.sort((a, b) => a.wall_time - b.wall_time);
    console.log(`[panel] pss.log: ${rows.length} rows from ${url}`);
  } catch (e) {
    console.warn('pss panel:', e);
    return errorPanel('pss.log (safety modules)', e);
  }
  return createPanel({
    title: `pss.log (safety modules)`,
    rows,
    clipStart: manifest.wall_time_start,
    cols: [
      { key: 'wall_time', label: 't (rel)', fmt: t => relSec(t, manifest) },
      { key: 'module', label: 'module' },
      { key: 'endpoint', label: 'endpoint' },
      { key: 'msg', label: 'msg' },
    ],
  });
}

function relSec(wallT, manifest) {
  if (wallT == null) return '—';
  const t = wallT - manifest.wall_time_start;
  return (t >= 0 ? '+' : '') + t.toFixed(2) + 's';
}
