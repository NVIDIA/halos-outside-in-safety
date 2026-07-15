// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

import { useEffect, useRef, useState } from 'react';
import { generateCurvedPath } from '../utils/bezier';
import './PathPreview.css';

function PathPreview({ origin, waypoints }) {
  const canvasRef = useRef(null);
  const largeCanvasRef = useRef(null);
  const [showLarge, setShowLarge] = useState(false);

  useEffect(() => {
    const drawPath = (canvas) => {
      if (!canvas || !origin) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    // Clear canvas
    ctx.clearRect(0, 0, width, height);

    // Calculate bounds to fit all points
    const allPoints = [origin, ...waypoints.map(wp => ({
      x: origin.x + wp.x,
      y: origin.y + wp.y
    }))];

    if (allPoints.length === 0) return;

    const minX = Math.min(...allPoints.map(p => p.x));
    const maxX = Math.max(...allPoints.map(p => p.x));
    const minY = Math.min(...allPoints.map(p => p.y));
    const maxY = Math.max(...allPoints.map(p => p.y));

    const rangeX = maxX - minX || 1;
    const rangeY = maxY - minY || 1;
    const scale = Math.min(width / rangeX, height / rangeY) * 0.8;
    const centerX = (minX + maxX) / 2;
    const centerY = (minY + maxY) / 2;

    // Transform function
    const toCanvas = (worldX, worldY) => ({
      x: width / 2 + (worldX - centerX) * scale,
      y: height / 2 - (worldY - centerY) * scale  // Flip Y
    });

    // Draw curved path
    if (waypoints.length > 0) {
      // Convert waypoints to world coordinates for curve generation
      const waypointsWorld = waypoints.map(wp => ({
        x: origin.x + wp.x,
        y: origin.y + wp.y,
        theta_deg: wp.theta_deg || 0,
        reverse: wp.reverse
      }));

      // Add origin as first waypoint
      const allWaypoints = [
        { x: origin.x, y: origin.y, theta_deg: waypointsWorld[0]?.theta_deg || 0 },
        ...waypointsWorld
      ];

      // Generate curved path with fewer steps for preview
      const curvedPath = generateCurvedPath(allWaypoints, canvas.width > 150 ? 15 : 8);

      if (curvedPath.segments && curvedPath.segments.length > 0) {
        // Draw each segment with appropriate color
        ctx.lineWidth = canvas.width > 150 ? 2 : 1.5;

        curvedPath.segments.forEach(segment => {
          if (!segment.points || segment.points.length === 0) return;

          // Use red for reverse segments, green for forward
          ctx.strokeStyle = segment.reverse ? '#ff5252' : '#76b900';
          ctx.beginPath();

          const firstPoint = toCanvas(segment.points[0].x, segment.points[0].y);
          ctx.moveTo(firstPoint.x, firstPoint.y);

          for (let i = 1; i < segment.points.length; i++) {
            const point = toCanvas(segment.points[i].x, segment.points[i].y);
            ctx.lineTo(point.x, point.y);
          }

          ctx.stroke();
        });

        // Draw direction arrows along segments (only on large preview)
        if (canvas.width > 150) {
          curvedPath.segments.forEach(segment => {
            if (!segment.points || segment.points.length < 5) return;

            const arrowColor = segment.reverse ? '#ff5252' : '#76b900';
            ctx.strokeStyle = arrowColor;
            ctx.fillStyle = arrowColor;
            ctx.lineWidth = 1.5;

            const arrowSpacing = Math.floor(segment.points.length / 3);
            
            for (let i = arrowSpacing; i < segment.points.length; i += arrowSpacing) {
              const pose = segment.points[i];
              const canvasPos = toCanvas(pose.x, pose.y);
              
              const arrowSize = 5;
              const angle = pose.theta;
              
              ctx.save();
              ctx.translate(canvasPos.x, canvasPos.y);
              ctx.rotate(angle);
              
              ctx.beginPath();
              ctx.moveTo(arrowSize, 0);
              ctx.lineTo(-arrowSize/2, arrowSize/2);
              ctx.lineTo(-arrowSize/2, -arrowSize/2);
              ctx.closePath();
              ctx.fill();
              
              ctx.restore();
            }
          });
        }
      }
    }

    // Draw origin
    const originCanvas = toCanvas(origin.x, origin.y);
    ctx.fillStyle = '#00a0dc';
    ctx.beginPath();
    ctx.arc(originCanvas.x, originCanvas.y, 4, 0, Math.PI * 2);
    ctx.fill();

    // Draw waypoints
    waypoints.forEach((wp, idx) => {
      const worldPos = toCanvas(origin.x + wp.x, origin.y + wp.y);
      
      // Draw waypoint circle
      ctx.fillStyle = wp.reverse ? '#ff5252' : '#76b900';
      ctx.beginPath();
      ctx.arc(worldPos.x, worldPos.y, 3, 0, Math.PI * 2);
      ctx.fill();

      // Draw heading indicator
      const headingRad = (wp.theta_deg || 0) * Math.PI / 180;
      const lineLength = 5;
      ctx.strokeStyle = wp.reverse ? '#ff5252' : '#76b900';
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(worldPos.x, worldPos.y);
      ctx.lineTo(
        worldPos.x + Math.cos(headingRad) * lineLength,
        worldPos.y - Math.sin(headingRad) * lineLength
      );
      ctx.stroke();
    });
    };

    drawPath(canvasRef.current);
    if (showLarge && largeCanvasRef.current) {
      drawPath(largeCanvasRef.current);
    }
  }, [origin, waypoints, showLarge]);

  if (!origin) {
    return (
      <div className="path-preview-empty">
        <span>No preview</span>
      </div>
    );
  }

  return (
    <div 
      className="path-preview-wrapper"
      onMouseEnter={() => setShowLarge(true)}
      onMouseLeave={() => setShowLarge(false)}
    >
      <canvas
        ref={canvasRef}
        width={120}
        height={80}
        className="path-preview-canvas"
      />
      {showLarge && (
        <div className="path-preview-large">
          <div className="preview-header">
            <span className="preview-label">Path Preview</span>
            <span className="preview-count">{waypoints.length} waypoint{waypoints.length !== 1 ? 's' : ''}</span>
          </div>
          <canvas
            ref={largeCanvasRef}
            width={300}
            height={200}
            className="path-preview-canvas-large"
          />
          <div className="preview-legend">
            <div className="legend-item">
              <span className="legend-dot origin"></span>
              <span>Origin</span>
            </div>
            <div className="legend-item">
              <span className="legend-dot waypoint"></span>
              <span>Waypoint</span>
            </div>
            <div className="legend-item">
              <span className="legend-dot reverse"></span>
              <span>Reverse</span>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

export default PathPreview;

