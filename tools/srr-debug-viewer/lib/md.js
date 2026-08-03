// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Minimal markdown renderer for SRR summary.md and per-clip reports.
// Handles only the constructs aggregator.py emits:
//   #/##/###  headings
//   - bullet lists
//   | table | rows |   with optional |---:| alignment row
//   **bold**, `inline code`, [link](url), emoji passthrough
//   blank lines = paragraph breaks
// Skips code blocks (none in our outputs) and other constructs.

function escHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function renderInline(s) {
  s = escHtml(s);
  // links [text](url) — render before bold so [**foo**](u) works
  s = s.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_, t, u) => `<a href="${u}">${t}</a>`);
  // bold **...**
  s = s.replace(/\*\*([^*]+)\*\*/g, (_, t) => `<strong>${t}</strong>`);
  // inline `code`
  s = s.replace(/`([^`]+)`/g, (_, t) => `<code>${t}</code>`);
  return s;
}

export function renderMd(text) {
  const lines = text.split(/\r?\n/);
  const out = [];
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];

    // headings
    const h = line.match(/^(#{1,6})\s+(.*)$/);
    if (h) {
      const level = h[1].length;
      out.push(`<h${level}>${renderInline(h[2])}</h${level}>`);
      i++; continue;
    }

    // tables — block of lines starting with `|`
    if (line.startsWith('|')) {
      const tbl = [];
      while (i < lines.length && lines[i].startsWith('|')) {
        tbl.push(lines[i]);
        i++;
      }
      out.push(renderTable(tbl));
      continue;
    }

    // bullet lists — block of lines starting with `- `
    if (/^- /.test(line)) {
      const items = [];
      while (i < lines.length && /^- /.test(lines[i])) {
        items.push(`<li>${renderInline(lines[i].slice(2))}</li>`);
        i++;
      }
      out.push(`<ul>${items.join('')}</ul>`);
      continue;
    }

    // blank line → paragraph break (already handled by adjacent <p>s)
    if (line.trim() === '') { i++; continue; }

    // paragraph: collect until blank line / heading / table / list
    const paras = [];
    while (i < lines.length && lines[i].trim() !== ''
           && !lines[i].startsWith('|') && !/^- /.test(lines[i])
           && !/^#{1,6}\s/.test(lines[i])) {
      paras.push(lines[i]);
      i++;
    }
    out.push(`<p>${renderInline(paras.join(' '))}</p>`);
  }
  return out.join('\n');
}

function renderTable(rows) {
  if (rows.length === 0) return '';
  const cells = rows.map(r =>
    r.replace(/^\|/, '').replace(/\|$/, '').split('|').map(c => c.trim())
  );
  // Detect alignment row (e.g. ["---", "---:", ":---"]) and skip it
  let aligns = [];
  let bodyStart = 1;
  if (cells.length >= 2 && cells[1].every(c => /^:?-+:?$/.test(c))) {
    aligns = cells[1].map(c => {
      if (c.startsWith(':') && c.endsWith(':')) return 'center';
      if (c.endsWith(':')) return 'right';
      if (c.startsWith(':')) return 'left';
      return '';
    });
    bodyStart = 2;
  }
  const head = cells[0].map((c, j) =>
    `<th${aligns[j] ? ` style="text-align:${aligns[j]}"` : ''}>${renderInline(c)}</th>`
  ).join('');
  const body = cells.slice(bodyStart).map(row =>
    `<tr>${row.map((c, j) =>
      `<td${aligns[j] ? ` style="text-align:${aligns[j]}"` : ''}>${renderInline(c)}</td>`
    ).join('')}</tr>`
  ).join('');
  return `<table class="md-table"><thead><tr>${head}</tr></thead><tbody>${body}</tbody></table>`;
}
