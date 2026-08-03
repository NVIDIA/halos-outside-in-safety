// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * Cubic Bezier curve utilities for smooth path generation
 * Supports both forward and reverse segments for forklift navigation
 */

// Forklift constraints
const MAX_STEER_RAD = Math.PI / 4; // 45 degrees max steering
const MIN_TURN_RADIUS = 2.0; // meters (approximate for forklift)

/**
 * Calculate a point on a cubic Bezier curve
 * @param {Object} p1 - Start point {x, y}
 * @param {Object} cp1 - First control point {x, y}
 * @param {Object} cp2 - Second control point {x, y}
 * @param {Object} p2 - End point {x, y}
 * @param {number} t - Parameter (0 to 1)
 * @returns {Object} Point on curve {x, y}
 */
export function cubicBezier(p1, cp1, cp2, p2, t) {
  const x =
    Math.pow(1 - t, 3) * p1.x +
    3 * Math.pow(1 - t, 2) * t * cp1.x +
    3 * (1 - t) * Math.pow(t, 2) * cp2.x +
    Math.pow(t, 3) * p2.x;

  const y =
    Math.pow(1 - t, 3) * p1.y +
    3 * Math.pow(1 - t, 2) * t * cp1.y +
    3 * (1 - t) * Math.pow(t, 2) * cp2.y +
    Math.pow(t, 3) * p2.y;

  return { x, y };
}

/**
 * Calculate distance between two points
 * @param {Object} p1 - First point {x, y}
 * @param {Object} p2 - Second point {x, y}
 * @returns {number} Distance
 */
export function getDistance(p1, p2) {
  return Math.sqrt(Math.pow(p2.x - p1.x, 2) + Math.pow(p2.y - p1.y, 2));
}

/**
 * Generate a control point based on a waypoint's position and heading
 * @param {Object} waypoint - Waypoint with {x, y, theta_deg}
 * @param {number} distance - Distance from waypoint to control point
 * @returns {Object} Control point {x, y}
 */
export function generateControlPoint(waypoint, distance) {
  const headingRad = (waypoint.theta_deg * Math.PI) / 180;
  return {
    x: waypoint.x + distance * Math.cos(headingRad),
    y: waypoint.y + distance * Math.sin(headingRad),
  };
}

/**
 * Calculate the minimum control point ratio to respect max steering angle
 * @param {number} distance - Distance between waypoints
 * @returns {number} Control point ratio (higher = gentler curve)
 */
export function calculateSafeControlPointRatio(distance) {
  // Minimum turn radius constraint: r = v / (tan(steer) * wheelbase)
  // For 45° steering, the curve should not be tighter than min turn radius
  // CP ratio affects curve tightness: higher ratio = gentler curve
  const minCpDistance = MIN_TURN_RADIUS * 0.8; // 80% of min turn radius
  const ratio = distance / Math.max(minCpDistance, distance / 4);
  return Math.max(2.5, Math.min(ratio, 4.0)); // Clamp between 2.5 and 4.0
}

/**
 * Generate a curved path segment between two waypoints
 * For reverse segments: swap poses, generate curve, then reverse point order
 * @param {Object} start - Start waypoint {x, y, theta_deg, reverse?}
 * @param {Object} end - End waypoint {x, y, theta_deg, reverse?}
 * @param {number} steps - Number of points in the segment
 * @returns {Object} Segment data {points: [{x, y, theta}], reverse: boolean}
 */
export function generateSegment(start, end, steps = 20) {
  // Check if this is a reverse segment (destination waypoint has reverse=true)
  const isReverse = end.reverse === true;
  
  // For reverse: swap start and end for curve generation
  let curveStart = isReverse ? end : start;
  let curveEnd = isReverse ? start : end;
  
  const d = getDistance(curveStart, curveEnd);
  
  // Use safe control point ratio based on distance
  const cpRatio = calculateSafeControlPointRatio(d);

  // Generate control points based on waypoint headings
  // Note: For reverse, we're using swapped poses, so headings are naturally correct
  const cp1 = generateControlPoint(curveStart, d / cpRatio);
  const cp2 = generateControlPoint(curveEnd, -d / cpRatio);

  const segment = [];
  for (let i = 0; i <= steps; i++) {
    const t = i / steps;
    const point = cubicBezier(curveStart, cp1, cp2, curveEnd, t);
    
    // Calculate heading at this point (tangent to curve)
    let theta = 0;
    if (i < steps) {
      const nextT = (i + 1) / steps;
      const nextPoint = cubicBezier(curveStart, cp1, cp2, curveEnd, nextT);
      theta = Math.atan2(nextPoint.y - point.y, nextPoint.x - point.x);
    } else if (segment.length > 0) {
      theta = segment[segment.length - 1].theta;
    }
    
    segment.push({ x: point.x, y: point.y, theta });
  }

  // Reverse the segment order for backward movement
  if (isReverse) {
    segment.reverse();
  }

  return {
    points: segment,
    reverse: isReverse,
    startWaypoint: start,
    endWaypoint: end,
  };
}

/**
 * Generate a full curved path through all waypoints
 * @param {Array} waypoints - Array of waypoints [{x, y, theta_deg, reverse?}, ...]
 * @param {number} stepsPerSegment - Points per segment
 * @returns {Object} Path data with segments and all poses
 */
export function generateCurvedPath(waypoints, stepsPerSegment = 20) {
  if (waypoints.length < 2) {
    return {
      poses: waypoints.map((wp) => ({
        x: wp.x,
        y: wp.y,
        theta: (wp.theta_deg * Math.PI) / 180,
      })),
      segments: [],
      waypoints: waypoints,
    };
  }

  const poses = [];
  const segments = [];

  for (let i = 0; i < waypoints.length - 1; i++) {
    const segment = generateSegment(waypoints[i], waypoints[i + 1], stepsPerSegment);
    segments.push(segment);

    // Avoid duplicate points at segment boundaries
    if (i === 0) {
      poses.push(...segment.points);
    } else {
      poses.push(...segment.points.slice(1));
    }
  }

  return {
    poses,
    segments,
    waypoints,
  };
}

/**
 * Calculate arrow points along a path for directional indicators
 * @param {Array} path - Array of path points [{x, y}, ...]
 * @param {number} spacing - Spacing between arrows in path units
 * @returns {Array} Array of arrow data [{x, y, angle}, ...]
 */
export function generatePathArrows(path, spacing = 5) {
  if (path.length < 2) return [];

  const arrows = [];
  let accumulatedDistance = 0;
  let lastArrowDistance = 0;

  for (let i = 1; i < path.length; i++) {
    const dx = path[i].x - path[i - 1].x;
    const dy = path[i].y - path[i - 1].y;
    const segmentLength = Math.sqrt(dx * dx + dy * dy);
    accumulatedDistance += segmentLength;

    if (accumulatedDistance - lastArrowDistance >= spacing) {
      const angle = Math.atan2(dy, dx);
      arrows.push({
        x: path[i].x,
        y: path[i].y,
        angle: angle,
      });
      lastArrowDistance = accumulatedDistance;
    }
  }

  return arrows;
}

/**
 * Export path in ROS-compatible format with poses and waypoints
 * @param {Object} pathData - Path data from generateCurvedPath
 * @param {Object} origin - Origin point {x, y}
 * @returns {Object} Path with poses and waypoints
 */
export function exportToRosFormat(pathData, origin) {
  const poses = pathData.poses.map((p, index) => ({
    position: {
      x: p.x,
      y: p.y,
      z: 0,
    },
    orientation: {
      x: 0,
      y: 0,
      z: Math.sin(p.theta / 2),
      w: Math.cos(p.theta / 2),
    },
  }));

  const waypoints = pathData.waypoints.map((wp, index) => {
    const theta = (wp.theta_deg * Math.PI) / 180;
    return {
      rosPose: {
        position: { x: wp.x, y: wp.y, z: 0 },
        orientation: {
          x: 0,
          y: 0,
          z: Math.sin(theta / 2),
          w: Math.cos(theta / 2),
        },
      },
      metadata: {
        index: index,
        time: 0,
        velocity: wp.velocity || 0.5,
        reverse: wp.reverse || false,
      },
    };
  });

  return { poses, waypoints };
}
