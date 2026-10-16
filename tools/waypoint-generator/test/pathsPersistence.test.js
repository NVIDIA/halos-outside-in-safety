// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Saved paths across a page reload. Each reload() is a fresh page in the same
// browser profile: a new store reading the one localStorage every page shares.
// Run with `npm test`.

import { test, beforeEach } from 'node:test';
import assert from 'node:assert/strict';
import { makeStore } from '../src/redux/store.js';
import {
  PATHS_STORAGE_KEY,
  createPath,
  savePath,
  updateCurrentPath,
  renamePath,
  deletePath,
  loadPaths,
} from '../src/redux/pathsSlice.js';

const storage = new Map();
Object.defineProperty(globalThis, 'localStorage', {
  configurable: true,
  value: {
    getItem: (key) => (storage.has(key) ? storage.get(key) : null),
    setItem: (key, value) => { storage.set(key, String(value)); },
    removeItem: (key) => { storage.delete(key); },
  },
});

const mapId = 'warehouse_20x20';
const origin = { x: 1, y: -13.39, theta_deg: 0 };
const waypoints = [
  { x: 2, y: 0, theta_deg: 0, note: '', reverse: false },
  { x: 4, y: 1, theta_deg: 30, note: '', reverse: false },
  { x: 6, y: 3, theta_deg: 45, note: '', reverse: false },
];

// A path from an earlier session that nothing below touches
const unrelated = {
  id: '1700000000000',
  name: 'UNRELATED_KEEP',
  mapId,
  origin: { x: 1, y: 2, theta_deg: 0 },
  waypoints: [{ x: 1, y: 0, theta_deg: 0, note: '', reverse: false }],
  createdAt: '2026-01-01T00:00:00.000Z',
  updatedAt: '2026-01-01T00:00:00.000Z',
};

function reload() {
  const store = makeStore();
  store.dispatch(loadPaths());
  return store;
}

const savedPaths = (store) => store.getState().paths.paths;
const named = (store, name) => savedPaths(store).find((path) => path.name === name);

function saveTestPath() {
  const store = reload();
  store.dispatch(createPath({ name: 'SIL14_STORAGE_A', mapId, origin, waypoints }));
  return store;
}

beforeEach(() => {
  storage.clear();
  storage.set(PATHS_STORAGE_KEY, JSON.stringify([unrelated]));
});

test('a new path survives a reload', () => {
  saveTestPath();
  const path = named(reload(), 'SIL14_STORAGE_A');
  assert.ok(path);
  assert.equal(path.mapId, mapId);
  assert.deepEqual(path.origin, origin);
  assert.deepEqual(path.waypoints, waypoints);
});

test('saving the open path again survives a reload', () => {
  const store = saveTestPath();
  const extended = [...waypoints, { x: 8, y: 3, theta_deg: 0, note: '', reverse: false }];
  store.dispatch(savePath({ mapId, origin, waypoints: extended }));
  assert.deepEqual(named(reload(), 'SIL14_STORAGE_A').waypoints, extended);
});

test('an unsaved edit is not written', () => {
  const store = saveTestPath();
  store.dispatch(updateCurrentPath({ origin, waypoints: waypoints.slice(0, 1) }));
  assert.deepEqual(named(reload(), 'SIL14_STORAGE_A').waypoints, waypoints);
});

// Rename and delete act on the path as a later page loaded it, so they cannot
// pass on a path that was never stored.
function reopenTestPath() {
  saveTestPath();
  const page = reload();
  const path = named(page, 'SIL14_STORAGE_A');
  assert.ok(path, 'the saved path did not survive the first reload');
  return { page, id: path.id };
}

test('a rename survives a reload and keeps the path', () => {
  const { page, id } = reopenTestPath();
  page.dispatch(renamePath({ pathId: id, newName: 'SIL14_STORAGE_RENAMED' }));

  const after = reload();
  assert.equal(named(after, 'SIL14_STORAGE_A'), undefined);
  const renamed = named(after, 'SIL14_STORAGE_RENAMED');
  assert.equal(renamed.id, id);
  assert.equal(renamed.mapId, mapId);
  assert.deepEqual(renamed.origin, origin);
  assert.deepEqual(renamed.waypoints, waypoints);
  assert.deepEqual(named(after, unrelated.name), unrelated);
});

test('a delete survives a reload and leaves the other paths', () => {
  const { page, id } = reopenTestPath();
  page.dispatch(deletePath(id));

  const after = reload();
  assert.equal(savedPaths(after).some((path) => path.id === id), false);
  assert.deepEqual(savedPaths(after), [unrelated]);
});
