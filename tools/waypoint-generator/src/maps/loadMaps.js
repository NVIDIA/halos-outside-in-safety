// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * Map registry loader.
 *
 * A map is a plan-view render of one Isaac scene plus the calibration that
 * turns its pixels into that scene's world metres. Both live under
 * `public/maps/<id>/`, so adding a scene is dropping a folder and naming it in
 * `maps.json` — no rebuild, and no editing the code.
 *
 *   public/maps/maps.json                 the list, in the order the UI shows
 *   public/maps/<id>/map.png              the plan view
 *   public/maps/<id>/config.json          scale, translation, forklift start
 *
 * Before this existed the tool held one hardcoded `Top.png` and one
 * `calibration.json`, so retargeting it to a new scene meant overwriting both
 * — which is how the 20x20 map came to be deleted when the 40x20 one arrived.
 *
 * Deliberately NOT the schema used by the occupancy-map branch of this tool
 * (`resolution` / `origin` / `imageHeight`, the ROS map_server convention).
 * We stay on plan-view renders and the `scaleFactor` calibration that goes
 * with them.
 */

const REGISTRY_URL = '/maps/maps.json';

async function fetchJson(url, what) {
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`${what}: ${url} returned ${response.status} ${response.statusText}`);
  }
  return response.json();
}

function requireNumber(value, path, mapId) {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new Error(
      `maps/${mapId}/config.json: ${path} must be a finite number, got ${JSON.stringify(value)}`
    );
  }
  return value;
}

/**
 * Validate one map config.
 *
 * Checked rather than trusted because every one of these feeds a multiplication
 * that turns a click into world metres. A missing field yields NaN, and NaN
 * draws nothing and exports as null — so the tool would look merely empty while
 * quietly producing a waypoint file no controller can follow.
 */
function validateConfig(config, mapId) {
  if (!config || typeof config !== 'object') {
    throw new Error(`maps/${mapId}/config.json: expected an object`);
  }
  requireNumber(config.scaleFactor, 'scaleFactor', mapId);
  if (config.scaleFactor <= 0) {
    throw new Error(`maps/${mapId}/config.json: scaleFactor must be > 0`);
  }
  const translation = config.translationToGlobalCoordinates;
  if (!translation || typeof translation !== 'object') {
    throw new Error(`maps/${mapId}/config.json: translationToGlobalCoordinates is required`);
  }
  requireNumber(translation.x, 'translationToGlobalCoordinates.x', mapId);
  requireNumber(translation.y, 'translationToGlobalCoordinates.y', mapId);
  return config;
}

/**
 * Which map to open, in order of preference: the `?map=<id>` query parameter,
 * the registry entry flagged `default`, then the first entry.
 *
 * The query parameter is how a second map is reachable before the UI has a
 * selector, and it stays useful afterwards as a shareable link to a map.
 */
export function resolveActiveMapId(registry, search = '') {
  const ids = registry.map((entry) => entry.id);
  const requested = new URLSearchParams(search).get('map');
  if (requested) {
    if (!ids.includes(requested)) {
      throw new Error(
        `?map=${requested} is not a known map. Known maps: ${ids.join(', ')}`
      );
    }
    return requested;
  }
  const flagged = registry.find((entry) => entry.default);
  return (flagged || registry[0]).id;
}

export async function loadMapRegistry() {
  const registry = await fetchJson(REGISTRY_URL, 'map registry');
  if (!Array.isArray(registry) || registry.length === 0) {
    throw new Error(`${REGISTRY_URL}: expected a non-empty array of maps`);
  }
  const seen = new Set();
  for (const entry of registry) {
    if (!entry.id) throw new Error(`${REGISTRY_URL}: every map needs an id`);
    if (seen.has(entry.id)) {
      throw new Error(`${REGISTRY_URL}: duplicate map id '${entry.id}'`);
    }
    seen.add(entry.id);
  }
  return registry;
}

export async function loadMapConfig(mapId) {
  const config = await fetchJson(`/maps/${mapId}/config.json`, `map '${mapId}'`);
  validateConfig(config, mapId);
  return {
    ...config,
    mapId,
    imagePath: `/maps/${mapId}/${config.image || 'map.png'}`,
  };
}
