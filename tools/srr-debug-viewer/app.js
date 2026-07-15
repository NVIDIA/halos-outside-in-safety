// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Tiny URL router: ?run=<path>[&scenario=<x>&clip=<y>]
//   no scenario+clip → top-level browser
//   with → single clip view
//
// SPA-style: query-only `<a href="?...">` clicks are intercepted to avoid full
// page reloads (and the browser-cache flakiness that comes with reloading the
// JS modules every navigation). Browser back/forward go through popstate.

import * as landing  from './components/landing.js';
import * as browser  from './components/browser.js';
import * as clipView from './components/clip-view.js';

const main = document.getElementById('main');
const header = document.getElementById('header');
const errOverlay = document.getElementById('error-overlay');

function showError(msg) {
  errOverlay.innerHTML = `<button>×</button><strong>error</strong>\n${msg}`;
  errOverlay.hidden = false;
  errOverlay.querySelector('button').onclick = () => errOverlay.hidden = true;
}
window.addEventListener('error', e =>
  showError(`${e.message}\n  at ${e.filename}:${e.lineno}:${e.colno}\n${e.error?.stack || ''}`));
window.addEventListener('unhandledrejection', e =>
  showError(`Unhandled promise rejection: ${e.reason?.message || e.reason}\n${e.reason?.stack || ''}`));

// Per-route teardown — clip-view registers a function to stop its rAF loop +
// detach listeners. app.js runs it before each new route render so there's no
// leftover state from the previous view.
let _teardown = null;
export function registerTeardown(fn) { _teardown = fn; }

async function route() {
  if (_teardown) { try { _teardown(); } catch (e) { console.warn('teardown:', e); } _teardown = null; }
  errOverlay.hidden = true;

  const params = new URLSearchParams(location.search);
  const run = params.get('run');
  const scenario = params.get('scenario');
  const clip = params.get('clip');

  main.innerHTML = '<p class="empty">Loading…</p>';
  try {
    if (!run) {
      await landing.render(main, header);
    } else if (scenario && clip) {
      await clipView.render(main, header, run, scenario, clip);
    } else {
      await browser.render(main, header, run);
    }
  } catch (e) {
    console.error(e);
    main.innerHTML = `<p class="empty" style="color:var(--fail)">${e.message}</p>`;
  }
}

document.addEventListener('click', e => {
  if (e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
  const a = e.target.closest('a');
  if (!a) return;
  const href = a.getAttribute('href');
  if (!href || !href.startsWith('?')) return;
  e.preventDefault();
  if (location.search !== href) history.pushState(null, '', href);
  route();
});

window.addEventListener('popstate', route);
route();
