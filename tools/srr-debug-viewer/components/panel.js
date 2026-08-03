// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Generic time-keyed table panel. Each panel renders a CSV's rows; the row
// whose `wall_time` is closest to the current video time gets the `.now`
// class. Rows with wall_time before the clip start are dimmed (.before-clip).

import { findClosestByWall } from '../lib/time.js';

/**
 * @param {object} opts
 * @param {string} opts.title    — panel header label
 * @param {Array<object>} opts.rows — sorted ascending by wall_time
 * @param {Array<{key: string, label: string, fmt?: (v) => string}>} opts.cols — columns to render
 * @param {number} opts.clipStart — wall_time_start (rows before are dimmed)
 * @returns {{root: HTMLElement, sync: (wallT: number) => void}}
 */
export function createPanel({ title, rows, cols, clipStart }) {
  const root = document.createElement('section');
  root.className = 'panel';
  root.innerHTML = `
    <header><span>${title}</span><span class="count">${rows.length} rows</span></header>
    <div class="scroll">
      <table>
        <thead><tr>${cols.map(c => `<th>${c.label}</th>`).join('')}</tr></thead>
        <tbody></tbody>
      </table>
    </div>
  `;
  const tbody = root.querySelector('tbody');
  const trs = rows.map(r => {
    const tr = document.createElement('tr');
    if (clipStart != null && r.wall_time < clipStart) tr.classList.add('before-clip');
    tr.innerHTML = cols.map(c => {
      const v = r[c.key];
      // fmt output is trusted HTML (some columns emit <span class="warn">…</span>);
      // raw values are escaped. Data is internal/trusted so this is safe here.
      const cell = c.fmt ? c.fmt(v, r) : (v == null ? '—' : escapeHtml(v));
      return `<td>${cell}</td>`;
    }).join('');
    return tr;
  });
  for (const tr of trs) tbody.appendChild(tr);

  let lastIdx = -1;
  const scroller = root.querySelector('.scroll');
  function sync(wallT) {
    const idx = findClosestByWall(rows, wallT);
    if (idx === lastIdx) return;
    if (lastIdx >= 0 && trs[lastIdx]) trs[lastIdx].classList.remove('now');
    if (idx >= 0 && trs[idx]) {
      trs[idx].classList.add('now');
      // Keep highlighted row in view (skip if user is actively scrolling).
      // Scroll ONLY this panel's scroller — never call scrollIntoView, which
      // also scrolls ancestors (incl. the window) and makes the whole page
      // jump when several panels sync each frame.
      if (!scroller.matches(':hover')) {
        const tr = trs[idx];
        const sr = scroller.getBoundingClientRect();
        const tre = tr.getBoundingClientRect();
        if (tre.top < sr.top || tre.bottom > sr.bottom) {
          const delta = (tre.top - sr.top) - (scroller.clientHeight - tr.offsetHeight) / 2;
          scroller.scrollTop += delta;
        }
      }
    }
    lastIdx = idx;
  }
  return { root, sync };
}

function escapeHtml(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}
