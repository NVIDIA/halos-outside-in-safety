// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

import { useState } from 'react';
import yaml from 'js-yaml';
import { generateCurvedPath } from '../utils/bezier';

/**
 * ExportPanel - Export waypoints to JSON (with interpolated poses); import JSON or YAML
 */
export default function ExportPanel({ mapConfig, waypoints, origin, onImport }) {
  const [fileName, setFileName] = useState('waypoints');
  const [showImport, setShowImport] = useState(false);

  const generateJson = () => {
    // Convert waypoints to world coordinates for path generation
    // INCLUDE ORIGIN as the starting point for full path
    const worldWaypoints = origin ? [
      // Origin is the first waypoint (start position)
      {
        x: origin.x,
        y: origin.y,
        theta_deg: origin.theta_deg ?? 0,
        reverse: false,  // Origin is never reverse
      },
      // Then all user waypoints
      ...waypoints.map(wp => ({
        x: origin.x + wp.x,
        y: origin.y + wp.y,
        theta_deg: wp.theta_deg,
        reverse: wp.reverse || false,
      }))
    ] : [];

    // Generate curved path with intermediate poses
    const pathData = worldWaypoints.length >= 2 
      ? generateCurvedPath(worldWaypoints, 20) 
      : { poses: [], segments: [] };

    const data = {
      // Which warehouse these metres belong to. The controller ignores unknown
      // keys, but a person reading two exported files cannot otherwise tell them
      // apart — the two scenes' origins are only a metre away from each other,
      // so a path from the wrong warehouse looks entirely plausible.
      map: {
        id: mapConfig.mapId,
        scene: mapConfig.scene || null,
      },
      origin: origin ? {
        world_x: parseFloat(origin.x.toFixed(3)),
        world_y: parseFloat(origin.y.toFixed(3)),
        // The heading the truck starts at. The controller reads only world_x
        // and world_y, but this is what shaped the first segment's poses, so
        // without it re-importing the file would silently redraw them from 0.
        theta_deg: Math.round(origin.theta_deg ?? 0),
      } : null,
      waypoints: waypoints.map((wp, index) => ({
        // Odom coordinates (relative to origin)
        x: parseFloat(wp.x.toFixed(3)),
        y: parseFloat(wp.y.toFixed(3)),
        theta_deg: Math.round(wp.theta_deg),
        reverse: wp.reverse || false,
        // World coordinates (absolute)
        world_x: origin ? parseFloat((origin.x + wp.x).toFixed(3)) : null,
        world_y: origin ? parseFloat((origin.y + wp.y).toFixed(3)) : null,
        velocity: wp.velocity || 0.5,
        note: wp.note || `Waypoint ${index + 1}`,
      })),
    };

    // Include intermediate poses for curve following
    if (pathData.poses.length > 0) {
      data.poses = pathData.poses.map(p => ({
        x: parseFloat(p.x.toFixed(4)),
        y: parseFloat(p.y.toFixed(4)),
        theta: parseFloat(p.theta.toFixed(4)),
      }));
      data.segments = pathData.segments.map((seg, i) => ({
        from_waypoint: i,
        to_waypoint: i + 1,
        reverse: seg.reverse,
        pose_count: seg.points.length,
      }));
    }

    return JSON.stringify(data, null, 2);
  };

  const handleExport = () => {
    const content = generateJson();
    const blob = new Blob([content], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);
    
    const a = document.createElement('a');
    a.href = url;
    a.download = `${fileName}.json`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  };

  const handleCopyToClipboard = () => {
    const content = generateJson();
    navigator.clipboard.writeText(content).then(() => {
      alert('Copied to clipboard!');
    });
  };

  const handleImportFile = (e) => {
    const file = e.target.files?.[0];
    if (!file) return;

    const reader = new FileReader();
    reader.onload = (event) => {
      try {
        const content = event.target?.result;
        let data;
        
        if (file.name.endsWith('.yaml') || file.name.endsWith('.yml')) {
          data = yaml.load(content);
        } else {
          data = JSON.parse(content);
        }
        
        if (data.waypoints && Array.isArray(data.waypoints)) {
          onImport?.(data);
          setShowImport(false);
          alert(`Imported ${data.waypoints.length} waypoints`);
        } else {
          alert('Invalid waypoint file format');
        }
      } catch (err) {
        alert(`Import error: ${err.message}`);
      }
    };
    reader.readAsText(file);
  };

  const previewContent = generateJson();

  return (
    <div className="export-panel">
      <h3>Export / Import</h3>
      
      <div className="export-options">
        <div className="filename-input">
          <label>Filename:</label>
          <input
            type="text"
            value={fileName}
            onChange={(e) => setFileName(e.target.value)}
          />
          <span>.json</span>
        </div>
        
        <div className="export-buttons">
          <button 
            className="btn-export" 
            onClick={handleExport}
            disabled={waypoints.length === 0}
          >
            Download
          </button>
          <button 
            className="btn-copy" 
            onClick={handleCopyToClipboard}
            disabled={waypoints.length === 0}
          >
            Copy
          </button>
          <button 
            className="btn-import" 
            onClick={() => setShowImport(!showImport)}
          >
            Import
          </button>
        </div>
        
        {showImport && (
          <div className="import-section">
            <input
              type="file"
              accept=".yaml,.yml,.json"
              onChange={handleImportFile}
            />
          </div>
        )}
      </div>
      
      <div className="preview-section">
        <h4>Preview</h4>
        <pre className="preview-content">
          {waypoints.length > 0 ? previewContent : '# No waypoints to export'}
        </pre>
      </div>
    </div>
  );
}

