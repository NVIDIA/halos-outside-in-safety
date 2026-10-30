// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

import { useRef, useEffect, useState, useCallback } from 'react';
import * as coords from '../utils/coordinates';
import { generateCurvedPath, generatePathArrows } from '../utils/bezier';

const WAYPOINT_RADIUS = 12;
const ORIGIN_RADIUS = 16;
const ARROW_LENGTH = 30;

const DEFAULT_FORKLIFT = { length: 2.8, width: 1.0, forkLength: 1.0 };

/**
 * WaypointCanvas - 2D canvas for drawing waypoints on warehouse map
 */
export default function WaypointCanvas({
  mapConfig,
  waypoints,
  origin,
  selectedIndex,
  previewHeading,
  mode, // 'waypoint' | 'origin' | 'view'
  onAddWaypoint,
  onAddReverseWaypoint,
  onSelectWaypoint,
  onSetOrigin,
  onUpdatePreviewHeading,
  onMoveWaypoint,
}) {
  const canvasRef = useRef(null);
  const containerRef = useRef(null);
  const imageRef = useRef(null);
  
  // Pan and zoom state
  const [pan, setPan] = useState({ x: 0, y: 0 });
  const [zoom, setZoom] = useState(0.6);
  const [isPanning, setIsPanning] = useState(false);
  const [lastMouse, setLastMouse] = useState({ x: 0, y: 0 });
  const [loadedImagePath, setLoadedImagePath] = useState(null);
  
  // Dragging waypoint state
  const [draggingIndex, setDraggingIndex] = useState(null);
  
  // Mouse position for preview
  const [mousePos, setMousePos] = useState({ x: 0, y: 0 });

  // Texture images for spots and arrows
  const spotTextureRef = useRef(null);
  const arrowTextureRef = useRef(null);
  const [texturesLoaded, setTexturesLoaded] = useState(0); // Counter to trigger re-render

  // Bound to the active map so the drawing code below reads the same as before,
  // while the conversions themselves stay explicit about which map they mean.
  const worldToPixel = useCallback(
    (worldX, worldY) => coords.worldToPixel(worldX, worldY, mapConfig), [mapConfig]);
  const pixelToWorld = useCallback(
    (pixelX, pixelY) => coords.pixelToWorld(pixelX, pixelY, mapConfig), [mapConfig]);
  const metersToPixels = useCallback(
    (meters) => coords.metersToPixels(meters, mapConfig), [mapConfig]);

  const forklift = mapConfig.forkliftDimensions || DEFAULT_FORKLIFT;
  const FORKLIFT_LENGTH = forklift.length ?? DEFAULT_FORKLIFT.length;
  const FORKLIFT_WIDTH = forklift.width ?? DEFAULT_FORKLIFT.width;
  const FORK_LENGTH = forklift.forkLength ?? DEFAULT_FORKLIFT.forkLength;

  // Ready only once the image on screen is the one the active map asked for.
  // Derived rather than a flag, so switching maps blanks the canvas by itself
  // instead of drawing the new scene's waypoints over the old scene's picture.
  const imageLoaded = loadedImagePath === mapConfig.imagePath;

  // Load the active map's plan view, reloading when the map changes
  useEffect(() => {
    let cancelled = false;
    const img = new Image();
    img.src = mapConfig.imagePath;
    img.onload = () => {
      if (cancelled) return;
      imageRef.current = img;
      setLoadedImagePath(mapConfig.imagePath);
    };
    img.onerror = () => {
      console.error(`Cannot load map image: ${mapConfig.imagePath}`);
    };
    return () => { cancelled = true; };
  }, [mapConfig.imagePath]);

  // Waypoint and arrow textures are the same for every map, so load them once
  useEffect(() => {
    const spotImg = new Image();
    spotImg.src = '/spot.png';
    spotImg.onload = () => {
      spotTextureRef.current = spotImg;
      setTexturesLoaded(prev => prev + 1); // Trigger re-render
    };
    
    // Load arrow texture for path
    const arrowImg = new Image();
    arrowImg.src = '/arrow.png';
    arrowImg.onload = () => {
      arrowTextureRef.current = arrowImg;
      setTexturesLoaded(prev => prev + 1); // Trigger re-render
    };
  }, []);

  // Convert screen coordinates to image coordinates
  const screenToImage = useCallback((screenX, screenY) => {
    const canvas = canvasRef.current;
    if (!canvas) return { x: 0, y: 0 };
    
    const rect = canvas.getBoundingClientRect();
    const x = (screenX - rect.left - pan.x) / zoom;
    const y = (screenY - rect.top - pan.y) / zoom;
    return { x, y };
  }, [pan, zoom]);

  // Convert image coordinates to screen coordinates
  const imageToScreen = useCallback((imageX, imageY) => {
    return {
      x: imageX * zoom + pan.x,
      y: imageY * zoom + pan.y,
    };
  }, [pan, zoom]);

  // Find waypoint at position
  const findWaypointAt = useCallback((imageX, imageY) => {
    if (!origin) return -1;
    
    for (let i = waypoints.length - 1; i >= 0; i--) {
      const wp = waypoints[i];
      const pixel = worldToPixel(wp.x + origin.x, wp.y + origin.y);
      const dx = imageX - pixel.x;
      const dy = imageY - pixel.y;
      if (Math.sqrt(dx * dx + dy * dy) < WAYPOINT_RADIUS / zoom + 5) {
        return i;
      }
    }
    return -1;
  }, [waypoints, origin, zoom, worldToPixel]);

  // Draw everything
  useEffect(() => {
    const canvas = canvasRef.current;
    const ctx = canvas.getContext('2d');
    const img = imageRef.current;
    
    if (!canvas || !imageLoaded || !img) return;

    // Clear canvas
    ctx.fillStyle = '#1a1a2e';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    
    // Apply transform
    ctx.save();
    ctx.translate(pan.x, pan.y);
    ctx.scale(zoom, zoom);
    
    // Draw background image
    ctx.drawImage(img, 0, 0);
    
    // Draw grid overlay (every 5 meters)
    const gridSize = metersToPixels(5);
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.1)';
    ctx.lineWidth = 1 / zoom;
    for (let x = 0; x < img.width; x += gridSize) {
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, img.height);
      ctx.stroke();
    }
    for (let y = 0; y < img.height; y += gridSize) {
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(img.width, y);
      ctx.stroke();
    }
    
    // Draw origin point
    if (origin) {
      const originPixel = worldToPixel(origin.x, origin.y);
      
      // Origin marker (NVIDIA green circle with crosshair)
      ctx.beginPath();
      ctx.arc(originPixel.x, originPixel.y, ORIGIN_RADIUS, 0, Math.PI * 2);
      ctx.fillStyle = 'rgba(118, 185, 0, 0.2)';
      ctx.fill();
      ctx.strokeStyle = '#76b900';
      ctx.lineWidth = 2.5 / zoom;
      ctx.stroke();
      
      // Inner circle
      ctx.beginPath();
      ctx.arc(originPixel.x, originPixel.y, ORIGIN_RADIUS * 0.4, 0, Math.PI * 2);
      ctx.fillStyle = '#76b900';
      ctx.fill();
      
      // Crosshair
      ctx.beginPath();
      ctx.moveTo(originPixel.x - ORIGIN_RADIUS - 8, originPixel.y);
      ctx.lineTo(originPixel.x + ORIGIN_RADIUS + 8, originPixel.y);
      ctx.moveTo(originPixel.x, originPixel.y - ORIGIN_RADIUS - 8);
      ctx.lineTo(originPixel.x, originPixel.y + ORIGIN_RADIUS + 8);
      ctx.strokeStyle = '#76b900';
      ctx.lineWidth = 1.5 / zoom;
      ctx.stroke();
      
      // Draw forklift footprint at origin (showing starting position) - NVIDIA green
      drawForkliftFootprint(
        ctx, originPixel.x, originPixel.y, origin.theta_deg ?? 0, zoom,
        'rgba(118, 185, 0, 0.25)', '#76b900'
      );
    }
    
    // Helper function to draw forklift footprint (simple clean design)
    function drawForkliftFootprint(ctx, x, y, theta_deg, zoom, fillColor, strokeColor) {
      const lengthPx = metersToPixels(FORKLIFT_LENGTH);
      const widthPx = metersToPixels(FORKLIFT_WIDTH);
      const forkLenPx = metersToPixels(FORK_LENGTH);
      const headingRad = (theta_deg * Math.PI) / 180;
      
      ctx.save();
      ctx.translate(x, y);
      ctx.rotate(-headingRad);
      
      const bodyLen = lengthPx - forkLenPx;  // Body = total - forks
      const forkLen = forkLenPx;
      const forkWidth = widthPx * 0.1;       // Fork thickness ~10cm
      const forkGap = widthPx * 0.4;         // Gap between forks ~40cm
      const cornerRadius = widthPx * 0.1;
      
      // --- Forks (front, extending from body) ---
      ctx.fillStyle = strokeColor;
      ctx.globalAlpha = 0.85;
      
      // Left fork
      ctx.beginPath();
      ctx.roundRect(bodyLen / 2 - forkWidth, -forkGap / 2 - forkWidth, forkLen, forkWidth, forkWidth / 3);
      ctx.fill();
      
      // Right fork
      ctx.beginPath();
      ctx.roundRect(bodyLen / 2 - forkWidth, forkGap / 2, forkLen, forkWidth, forkWidth / 3);
      ctx.fill();
      ctx.globalAlpha = 1.0;
      
      // --- Main body (centered, forks extend forward) ---
      ctx.beginPath();
      ctx.roundRect(-bodyLen / 2, -widthPx / 2, bodyLen, widthPx, cornerRadius);
      ctx.fillStyle = fillColor;
      ctx.fill();
      ctx.strokeStyle = strokeColor;
      ctx.lineWidth = 2 / zoom;
      ctx.stroke();
      
      // --- Direction indicator (arrow pointing forward) ---
      ctx.beginPath();
      ctx.moveTo(bodyLen * 0.3, 0);
      ctx.lineTo(0, -widthPx * 0.18);
      ctx.lineTo(0, widthPx * 0.18);
      ctx.closePath();
      ctx.fillStyle = strokeColor;
      ctx.globalAlpha = 0.5;
      ctx.fill();
      ctx.globalAlpha = 1.0;
      
      ctx.restore();
    }
    
    // Draw curved path between waypoints (Cubic Bezier)
    if (waypoints.length >= 1 && origin) {
      // Create waypoints array including origin as first point
      const originWaypoint = {
        x: origin.x,
        y: origin.y,
        // The heading the truck starts at. It aims the first Bezier control
        // point, so a wrong value bends the opening segment out of the truck's
        // actual facing.
        theta_deg: origin.theta_deg ?? 0,
        reverse: false,
      };
      
      // Convert waypoints to world coordinates for Bezier calculation
      const worldWaypoints = [
        originWaypoint,
        ...waypoints.map(wp => ({
          x: wp.x + origin.x,
          y: wp.y + origin.y,
          theta_deg: wp.theta_deg,
          reverse: wp.reverse || false,
        }))
      ];
      
      // Generate curved path with segments
      const pathData = generateCurvedPath(worldWaypoints, 30);
      
      // Draw each segment as continuous arrow ribbon
      pathData.segments.forEach((segment, segIdx) => {
        const isReverse = segment.reverse;
        const pixelPath = segment.points.map(p => worldToPixel(p.x, p.y));
        const arrowTexture = arrowTextureRef.current;
        
        if (pixelPath.length < 2) return;
        
        ctx.save();
        
        // Configuration for stretched arrow ribbon
        // Use FIXED pixel sizes so arrows stay consistent regardless of zoom
        const arrowHeight = 12;  // Fixed pixel height
        const arrowWidth = 24;   // Fixed pixel width (stretched along path)
        const arrowSpacing = 20; // Fixed pixel spacing (overlap for continuous look)
        
        // Calculate total path length and collect arrow positions
        let totalLength = 0;
        const pathSegments = [];
        for (let i = 0; i < pixelPath.length - 1; i++) {
          const p1 = pixelPath[i];
          const p2 = pixelPath[i + 1];
          const dx = p2.x - p1.x;
          const dy = p2.y - p1.y;
          const len = Math.sqrt(dx * dx + dy * dy);
          const angle = Math.atan2(dy, dx);
          pathSegments.push({ p1, p2, len, angle, startDist: totalLength });
          totalLength += len;
        }
        
        // Draw stretched arrows at regular intervals along the path
        let currentDist = 0;
        while (currentDist < totalLength) {
          // Find which segment this distance falls into
          let segmentInfo = null;
          let distInSegment = 0;
          for (const seg of pathSegments) {
            if (currentDist >= seg.startDist && currentDist < seg.startDist + seg.len) {
              segmentInfo = seg;
              distInSegment = currentDist - seg.startDist;
              break;
            }
          }
          
          if (segmentInfo) {
            const t = distInSegment / segmentInfo.len;
            const x = segmentInfo.p1.x + (segmentInfo.p2.x - segmentInfo.p1.x) * t;
            const y = segmentInfo.p1.y + (segmentInfo.p2.y - segmentInfo.p1.y) * t;
            const displayAngle = isReverse ? segmentInfo.angle + Math.PI : segmentInfo.angle;
            
            ctx.save();
            ctx.translate(x, y);
            ctx.rotate(displayAngle);
            
            if (arrowTexture) {
              if (isReverse) {
                ctx.filter = 'hue-rotate(320deg) saturate(150%)';
              }
              // Draw stretched arrow: width along path, height perpendicular
              ctx.drawImage(
                arrowTexture,
                -arrowWidth / 2,
                -arrowHeight / 2,
                arrowWidth,
                arrowHeight
              );
              ctx.filter = 'none';
            } else {
              // Fallback: draw stretched chevron arrow
              ctx.strokeStyle = isReverse ? '#ff6b6b' : '#00ff88';
              ctx.lineWidth = 2 / zoom;
              ctx.lineCap = 'round';
              ctx.beginPath();
              ctx.moveTo(-arrowWidth / 3, -arrowHeight / 2);
              ctx.lineTo(arrowWidth / 3, 0);
              ctx.lineTo(-arrowWidth / 3, arrowHeight / 2);
              ctx.stroke();
            }
            ctx.restore();
          }
          
          currentDist += arrowSpacing;
        }
        
        // Draw subtle path line underneath
        ctx.strokeStyle = isReverse ? 'rgba(255, 107, 107, 0.5)' : 'rgba(0, 255, 136, 0.5)';
        ctx.lineWidth = 3 / zoom;
        ctx.lineCap = 'round';
        ctx.lineJoin = 'round';
        if (isReverse) {
          ctx.setLineDash([6 / zoom, 3 / zoom]);
        }
        ctx.beginPath();
        for (let i = 0; i < pixelPath.length; i++) {
          if (i === 0) {
            ctx.moveTo(pixelPath[i].x, pixelPath[i].y);
          } else {
            ctx.lineTo(pixelPath[i].x, pixelPath[i].y);
          }
        }
        ctx.stroke();
        ctx.setLineDash([]);
        
        ctx.restore();
      });
    }
    
    // Draw waypoints using textures
    if (origin) {
      const spotTexture = spotTextureRef.current;
      
      waypoints.forEach((wp, index) => {
        const pixel = worldToPixel(wp.x + origin.x, wp.y + origin.y);
        const isSelected = index === selectedIndex;
        const isDragging = index === draggingIndex;
        const isReverse = wp.reverse || false;
        
        // Draw forklift footprint at selected waypoint
        if (isSelected) {
          drawForkliftFootprint(
            ctx, pixel.x, pixel.y, wp.theta_deg, zoom,
            isReverse ? 'rgba(255, 82, 82, 0.25)' : 'rgba(118, 185, 0, 0.25)',
            isReverse ? '#ff5252' : '#76b900'
          );
        }
        
        const headingRad = (wp.theta_deg * Math.PI) / 180;
        const spotSize = 40 / zoom;
        
        // Use texture if loaded, otherwise fallback to drawing
        if (spotTexture) {
          ctx.save();
          ctx.translate(pixel.x, pixel.y);
          // tex_spot.png has arrow pointing RIGHT, rotate to match waypoint heading
          // In pixel coords, positive rotation is clockwise
          // headingRad=0 means pointing right (+X), which matches the texture default
          // headingRad=π/2 means pointing down in world, need to rotate CW by 90°
          // But since world Y is inverted from pixel Y, we negate
          ctx.rotate(-headingRad);
          
          // Apply color tint based on state
          if (isReverse) {
            ctx.filter = 'hue-rotate(320deg) saturate(150%)'; // Red tint
          } else if (isSelected) {
            ctx.filter = 'hue-rotate(180deg) saturate(120%)'; // Blue tint
          } else if (isDragging) {
            ctx.filter = 'hue-rotate(45deg) saturate(150%)'; // Yellow tint
          }
          // else: use original cyan/green color
          
          ctx.drawImage(
            spotTexture,
            -spotSize / 2,
            -spotSize / 2,
            spotSize,
            spotSize
          );
          ctx.filter = 'none';
          ctx.restore();
        } else {
          // Fallback: draw circles if texture not loaded
          ctx.beginPath();
          ctx.arc(pixel.x, pixel.y, (WAYPOINT_RADIUS + 3) / zoom, 0, Math.PI * 2);
          ctx.fillStyle = '#fff';
          ctx.fill();
          
          ctx.beginPath();
          ctx.arc(pixel.x, pixel.y, WAYPOINT_RADIUS / zoom, 0, Math.PI * 2);
          ctx.fillStyle = isReverse ? '#ff6b6b' : isSelected ? '#00aaff' : isDragging ? '#ffd93d' : '#00ff88';
          ctx.fill();
          
          // Draw heading arrow if no texture
          const arrowLen = ARROW_LENGTH / zoom;
          const arrowDir = isReverse ? headingRad + Math.PI : headingRad;
          const arrowX = pixel.x + Math.cos(arrowDir) * arrowLen;
          const arrowY = pixel.y - Math.sin(arrowDir) * arrowLen;
          
          ctx.beginPath();
          ctx.moveTo(pixel.x, pixel.y);
          ctx.lineTo(arrowX, arrowY);
          ctx.strokeStyle = '#fff';
          ctx.lineWidth = 3 / zoom;
          ctx.stroke();
          
          const headSize = 8 / zoom;
          const angle = Math.atan2(-(arrowY - pixel.y), arrowX - pixel.x);
          ctx.beginPath();
          ctx.moveTo(arrowX, arrowY);
          ctx.lineTo(arrowX - headSize * Math.cos(angle - Math.PI / 6), arrowY + headSize * Math.sin(angle - Math.PI / 6));
          ctx.lineTo(arrowX - headSize * Math.cos(angle + Math.PI / 6), arrowY + headSize * Math.sin(angle + Math.PI / 6));
          ctx.closePath();
          ctx.fillStyle = '#fff';
          ctx.fill();
        }
        
        // Waypoint number (always drawn on top)
        ctx.fillStyle = '#fff';
        ctx.font = `bold ${11 / zoom}px sans-serif`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.shadowColor = '#000';
        ctx.shadowBlur = 3 / zoom;
        ctx.fillText(String(index + 1), pixel.x, pixel.y);
        ctx.shadowBlur = 0;
        
        // Reverse indicator (R label)
        if (isReverse) {
          ctx.fillStyle = '#ff6b6b';
          ctx.font = `bold ${10 / zoom}px sans-serif`;
          ctx.fillText('R', pixel.x + (spotSize / 2 + 5) / zoom, pixel.y - 8 / zoom);
        }
      });
    }
    
    // Draw preview waypoint (when in waypoint mode and mouse is over canvas)
    if (mode === 'waypoint' && origin && mousePos.x > 0 && mousePos.y > 0) {
      // Draw forklift footprint preview (NVIDIA green)
      drawForkliftFootprint(
        ctx, mousePos.x, mousePos.y, previewHeading, zoom,
        'rgba(118, 185, 0, 0.15)', 'rgba(118, 185, 0, 0.5)'
      );
      
      ctx.beginPath();
      ctx.arc(mousePos.x, mousePos.y, WAYPOINT_RADIUS / zoom, 0, Math.PI * 2);
      ctx.fillStyle = 'rgba(118, 185, 0, 0.4)';
      ctx.fill();
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.6)';
      ctx.lineWidth = 2 / zoom;
      ctx.setLineDash([4 / zoom, 4 / zoom]);
      ctx.stroke();
      ctx.setLineDash([]);
      
      // Preview heading arrow
      const headingRad = (previewHeading * Math.PI) / 180;
      const arrowLen = ARROW_LENGTH / zoom;
      const arrowX = mousePos.x + Math.cos(headingRad) * arrowLen;
      const arrowY = mousePos.y - Math.sin(headingRad) * arrowLen;
      
      ctx.beginPath();
      ctx.moveTo(mousePos.x, mousePos.y);
      ctx.lineTo(arrowX, arrowY);
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.6)';
      ctx.lineWidth = 2 / zoom;
      ctx.stroke();
    }
    
    // Draw origin preview (when in origin mode). The body and arrow show the
    // heading the click is about to commit, so the start pose is not placed
    // blind and then only discovered in the exported poses.
    if (mode === 'origin' && mousePos.x > 0 && mousePos.y > 0) {
      drawForkliftFootprint(
        ctx, mousePos.x, mousePos.y, previewHeading, zoom,
        'rgba(0, 170, 255, 0.15)', 'rgba(0, 170, 255, 0.6)'
      );

      ctx.strokeStyle = 'rgba(0, 170, 255, 0.6)';
      ctx.lineWidth = 2 / zoom;
      ctx.setLineDash([4 / zoom, 4 / zoom]);
      ctx.strokeRect(
        mousePos.x - ORIGIN_RADIUS,
        mousePos.y - ORIGIN_RADIUS,
        ORIGIN_RADIUS * 2,
        ORIGIN_RADIUS * 2
      );
      ctx.setLineDash([]);

      const originHeadingRad = (previewHeading * Math.PI) / 180;
      const originArrowLen = ARROW_LENGTH / zoom;
      ctx.beginPath();
      ctx.moveTo(mousePos.x, mousePos.y);
      ctx.lineTo(
        mousePos.x + Math.cos(originHeadingRad) * originArrowLen,
        mousePos.y - Math.sin(originHeadingRad) * originArrowLen
      );
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.7)';
      ctx.lineWidth = 2 / zoom;
      ctx.stroke();
    }
    
    ctx.restore();
    
    // Draw zoom indicator
    ctx.fillStyle = 'rgba(0, 0, 0, 0.7)';
    ctx.fillRect(10, canvas.height - 35, 100, 25);
    ctx.fillStyle = '#fff';
    ctx.font = '12px monospace';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    ctx.fillText(`Zoom: ${(zoom * 100).toFixed(0)}%`, 20, canvas.height - 22);
    
    // Draw mode indicator
    const modeText = mode === 'origin' ? 'SET ORIGIN' : mode === 'waypoint' ? 'ADD WAYPOINT' : 'VIEW';
    ctx.fillStyle = mode === 'origin' ? '#00aaff' : mode === 'waypoint' ? '#00ff88' : '#888';
    ctx.font = 'bold 14px sans-serif';
    ctx.textAlign = 'right';
    ctx.fillText(modeText, canvas.width - 20, canvas.height - 22);
    
    // Draw debug panel (top-left)
    if (mousePos.x > 0 && mousePos.y > 0) {
      const worldPos = pixelToWorld(mousePos.x, mousePos.y);
      const odomPos = origin ? { x: worldPos.x - origin.x, y: worldPos.y - origin.y } : null;
      
      ctx.fillStyle = 'rgba(0, 0, 0, 0.8)';
      ctx.fillRect(10, 10, 220, origin ? 75 : 55);
      
      ctx.font = '11px monospace';
      ctx.textAlign = 'left';
      ctx.textBaseline = 'top';
      
      ctx.fillStyle = '#888';
      ctx.fillText(`Pixel: (${mousePos.x.toFixed(0)}, ${mousePos.y.toFixed(0)})`, 18, 18);
      
      ctx.fillStyle = '#00aaff';
      ctx.fillText(`World: (${worldPos.x.toFixed(2)}, ${worldPos.y.toFixed(2)})`, 18, 35);
      
      if (odomPos) {
        ctx.fillStyle = '#00ff88';
        ctx.fillText(`Odom:  (${odomPos.x.toFixed(2)}, ${odomPos.y.toFixed(2)})`, 18, 52);
      }
      
      ctx.fillStyle = '#ff6b6b';
      ctx.fillText(`θ: ${previewHeading}°`, 18, origin ? 69 : 52);
    }
    
  }, [waypoints, origin, selectedIndex, pan, zoom, imageLoaded, texturesLoaded, mode, mousePos, previewHeading, draggingIndex,
      worldToPixel, pixelToWorld, metersToPixels, FORKLIFT_LENGTH, FORKLIFT_WIDTH, FORK_LENGTH]);

  // Handle mouse down
  const handleMouseDown = (e) => {
    if (e.button === 1 || (e.button === 0 && e.altKey)) {
      // Middle click or Alt+Left click = pan
      setIsPanning(true);
      setLastMouse({ x: e.clientX, y: e.clientY });
      e.preventDefault();
    } else if (e.button === 0) {
      const imagePos = screenToImage(e.clientX, e.clientY);
      
      if (mode === 'origin') {
        const world = pixelToWorld(imagePos.x, imagePos.y);
        onSetOrigin?.({ x: world.x, y: world.y, theta_deg: previewHeading });
      } else if (mode === 'waypoint' && origin) {
        // Check if clicking on existing waypoint
        const wpIndex = findWaypointAt(imagePos.x, imagePos.y);
        if (wpIndex >= 0) {
          onSelectWaypoint?.(wpIndex);
          setDraggingIndex(wpIndex);
        } else {
          // Add new waypoint
          const world = pixelToWorld(imagePos.x, imagePos.y);
          onAddWaypoint?.({
            x: world.x - origin.x,
            y: world.y - origin.y,
            theta_deg: previewHeading,
          });
        }
      } else if (mode === 'view' && origin) {
        // View mode - only select waypoints
        const wpIndex = findWaypointAt(imagePos.x, imagePos.y);
        if (wpIndex >= 0) {
          onSelectWaypoint?.(wpIndex);
        }
      }
    }
  };

  // Handle mouse move
  const handleMouseMove = (e) => {
    const imagePos = screenToImage(e.clientX, e.clientY);
    setMousePos(imagePos);
    
    if (isPanning) {
      const dx = e.clientX - lastMouse.x;
      const dy = e.clientY - lastMouse.y;
      setPan(prev => ({ x: prev.x + dx, y: prev.y + dy }));
      setLastMouse({ x: e.clientX, y: e.clientY });
    } else if (draggingIndex !== null && origin) {
      // Dragging waypoint
      const world = pixelToWorld(imagePos.x, imagePos.y);
      onMoveWaypoint?.(draggingIndex, {
        x: world.x - origin.x,
        y: world.y - origin.y,
      });
    }
  };

  // Handle mouse up
  const handleMouseUp = () => {
    setIsPanning(false);
    setDraggingIndex(null);
  };

  // Handle mouse leave
  const handleMouseLeave = () => {
    setIsPanning(false);
    setDraggingIndex(null);
    setMousePos({ x: 0, y: 0 });
  };

  // Handle right-click for reverse waypoint
  const handleContextMenu = (e) => {
    e.preventDefault();
    
    if (mode === 'waypoint' && origin) {
      const imagePos = screenToImage(e.clientX, e.clientY);
      const world = pixelToWorld(imagePos.x, imagePos.y);
      
      // Add reverse waypoint
      onAddReverseWaypoint?.({
        x: world.x - origin.x,
        y: world.y - origin.y,
        theta_deg: previewHeading,
      });
    }
  };

  // Handle wheel for zoom and heading
  const handleWheel = (e) => {
    e.preventDefault();
    
    if (e.shiftKey && (mode === 'waypoint' || mode === 'origin')) {
      // Shift + scroll = adjust preview heading (the origin's own start heading
      // while in origin mode)
      const delta = e.deltaY > 0 ? -10 : 10;
      onUpdatePreviewHeading?.(coords.normalizeAngle(previewHeading + delta));
    } else {
      // Normal scroll = zoom
      const zoomFactor = e.deltaY > 0 ? 0.9 : 1.1;
      const newZoom = Math.min(Math.max(zoom * zoomFactor, 0.1), 3);
      
      // Zoom towards mouse position
      const rect = canvasRef.current.getBoundingClientRect();
      const mouseX = e.clientX - rect.left;
      const mouseY = e.clientY - rect.top;
      
      const newPanX = mouseX - (mouseX - pan.x) * (newZoom / zoom);
      const newPanY = mouseY - (mouseY - pan.y) * (newZoom / zoom);
      
      setZoom(newZoom);
      setPan({ x: newPanX, y: newPanY });
    }
  };

  // Resize canvas to container
  useEffect(() => {
    const handleResize = () => {
      const container = containerRef.current;
      const canvas = canvasRef.current;
      if (container && canvas) {
        canvas.width = container.clientWidth;
        canvas.height = container.clientHeight;
      }
    };
    
    handleResize();
    window.addEventListener('resize', handleResize);
    return () => window.removeEventListener('resize', handleResize);
  }, []);

  return (
    <div 
      ref={containerRef} 
      style={{ 
        width: '100%', 
        height: '100%', 
        overflow: 'hidden',
        cursor: isPanning ? 'grabbing' : mode === 'origin' ? 'crosshair' : 'default'
      }}
    >
      <canvas
        ref={canvasRef}
        onMouseDown={handleMouseDown}
        onMouseMove={handleMouseMove}
        onMouseUp={handleMouseUp}
        onMouseLeave={handleMouseLeave}
        onWheel={handleWheel}
        onContextMenu={handleContextMenu}
        style={{ display: 'block' }}
      />
    </div>
  );
}

