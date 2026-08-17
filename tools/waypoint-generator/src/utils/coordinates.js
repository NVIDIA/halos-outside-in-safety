// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * Coordinate conversion utilities for waypoint generator
 * 
 * Coordinate systems:
 * - Pixel: Image coordinates (0,0 at top-left, Y increases downward)
 * - World: Isaac Sim global coordinates (meters)
 * - Odom: Odometry frame relative to robot start position (meters)
 */

/**
 * Every conversion below takes the map's calibration as an argument rather than
 * importing one. A module-level active map would let a component convert a
 * click before the map it belongs to was loaded, and the result — a waypoint
 * placed with another scene's scale — looks like a plausible number rather than
 * an error. Passing it makes that impossible to write.
 *
 * mapConfig comes from `src/maps/loadMaps.js` and carries `scaleFactor` and
 * `translationToGlobalCoordinates`.
 */

function calibrationOf(mapConfig) {
  if (!mapConfig) {
    throw new Error('coordinates: mapConfig is required (did the map finish loading?)');
  }
  return mapConfig;
}

/**
 * Convert pixel coordinates to world coordinates
 * @param {number} pixelX - X position in image pixels
 * @param {number} pixelY - Y position in image pixels
 * @param {object} mapConfig - The active map's calibration
 * @returns {{x: number, y: number}} World coordinates in meters
 */
export function pixelToWorld(pixelX, pixelY, mapConfig) {
  const { scaleFactor, translationToGlobalCoordinates } = calibrationOf(mapConfig);
  // Image Y is inverted (0 at top), and world Y axis is flipped
  const worldX = (pixelX / scaleFactor) - translationToGlobalCoordinates.x;
  const worldY = -((pixelY / scaleFactor) - translationToGlobalCoordinates.y);

  return { x: worldX, y: worldY };
}

/**
 * Convert world coordinates to pixel coordinates
 * @param {number} worldX - X position in meters
 * @param {number} worldY - Y position in meters
 * @param {object} mapConfig - The active map's calibration
 * @returns {{x: number, y: number}} Pixel coordinates
 */
export function worldToPixel(worldX, worldY, mapConfig) {
  const { scaleFactor, translationToGlobalCoordinates } = calibrationOf(mapConfig);
  const pixelX = (worldX + translationToGlobalCoordinates.x) * scaleFactor;
  const pixelY = (-worldY + translationToGlobalCoordinates.y) * scaleFactor;

  return { x: pixelX, y: pixelY };
}

/**
 * Convert world coordinates to odom coordinates (relative to origin)
 * @param {number} worldX - X position in world meters
 * @param {number} worldY - Y position in world meters
 * @param {{x: number, y: number}} origin - Origin point in world coordinates
 * @returns {{x: number, y: number}} Odom coordinates in meters
 */
export function worldToOdom(worldX, worldY, origin) {
  return {
    x: worldX - origin.x,
    y: worldY - origin.y
  };
}

/**
 * Convert odom coordinates to world coordinates
 * @param {number} odomX - X position in odom meters
 * @param {number} odomY - Y position in odom meters
 * @param {{x: number, y: number}} origin - Origin point in world coordinates
 * @returns {{x: number, y: number}} World coordinates in meters
 */
export function odomToWorld(odomX, odomY, origin) {
  return {
    x: odomX + origin.x,
    y: odomY + origin.y
  };
}

/**
 * Convert pixel coordinates directly to odom coordinates
 * @param {number} pixelX - X position in image pixels
 * @param {number} pixelY - Y position in image pixels
 * @param {{x: number, y: number}} origin - Origin point in world coordinates
 * @returns {{x: number, y: number}} Odom coordinates in meters
 */
export function pixelToOdom(pixelX, pixelY, origin, mapConfig) {
  const world = pixelToWorld(pixelX, pixelY, mapConfig);
  return worldToOdom(world.x, world.y, origin);
}

/**
 * Convert odom coordinates to pixel coordinates
 * @param {number} odomX - X position in odom meters
 * @param {number} odomY - Y position in odom meters
 * @param {{x: number, y: number}} origin - Origin point in world coordinates
 * @returns {{x: number, y: number}} Pixel coordinates
 */
export function odomToPixel(odomX, odomY, origin, mapConfig) {
  const world = odomToWorld(odomX, odomY, origin);
  return worldToPixel(world.x, world.y, mapConfig);
}

/**
 * Normalize angle to [-180, 180] degrees
 * @param {number} angleDeg - Angle in degrees
 * @returns {number} Normalized angle in degrees
 */
export function normalizeAngle(angleDeg) {
  while (angleDeg > 180) angleDeg -= 360;
  while (angleDeg < -180) angleDeg += 360;
  return angleDeg;
}

/**
 * Calculate distance between two points in pixels
 * @param {number} x1 
 * @param {number} y1 
 * @param {number} x2 
 * @param {number} y2 
 * @returns {number} Distance in pixels
 */
export function distance(x1, y1, x2, y2) {
  return Math.sqrt((x2 - x1) ** 2 + (y2 - y1) ** 2);
}

/**
 * Convert pixels to meters
 * @param {number} pixels
 * @param {object} mapConfig - The active map's calibration
 * @returns {number} meters
 */
export function pixelsToMeters(pixels, mapConfig) {
  return pixels / calibrationOf(mapConfig).scaleFactor;
}

/**
 * Convert meters to pixels
 * @param {number} meters
 * @param {object} mapConfig - The active map's calibration
 * @returns {number} pixels
 */
export function metersToPixels(meters, mapConfig) {
  return meters * calibrationOf(mapConfig).scaleFactor;
}

