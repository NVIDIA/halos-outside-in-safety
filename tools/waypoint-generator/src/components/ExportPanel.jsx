// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

import { useState } from 'react';
import yaml from 'js-yaml';
import { generateCurvedPath } from '../utils/bezier';

/**
 * ExportPanel - Export waypoints to YAML/JSON
 */
export default function ExportPanel({ waypoints, origin, onImport }) {
  const [exportFormat, setExportFormat] = useState('yaml');
  const [includePoses, setIncludePoses] = useState(true);  // Default on for curve following
  const [fileName, setFileName] = useState('waypoints');
  const [showImport, setShowImport] = useState(false);

  const generateYaml = () => {
    const data = {
      waypoints: waypoints.map((wp, index) => ({
        x: parseFloat(wp.x.toFixed(3)),
        y: parseFloat(wp.y.toFixed(3)),
        theta_deg: Math.round(wp.theta_deg),
        ...(wp.reverse ? { reverse: true } : {}),
        ...(wp.note ? { note: wp.note } : {}),
      })),
    };
    return yaml.dump(data, { 
      indent: 2,
      lineWidth: -1,
      noRefs: true,
    });
  };

  const generateJson = () => {
    // Convert waypoints to world coordinates for path generation
    // INCLUDE ORIGIN as the starting point for full path
    const worldWaypoints = origin ? [
      // Origin is the first waypoint (start position)
      {
        x: origin.x,
        y: origin.y,
        theta_deg: origin.theta_deg || 0,
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
      origin: origin ? {
        world_x: parseFloat(origin.x.toFixed(3)),
        world_y: parseFloat(origin.y.toFixed(3)),
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

    // Optionally include intermediate poses for curve following
    if (includePoses && pathData.poses.length > 0) {
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
    const content = exportFormat === 'yaml' ? generateYaml() : generateJson();
    const blob = new Blob([content], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);
    
    const a = document.createElement('a');
    a.href = url;
    a.download = `${fileName}.${exportFormat === 'yaml' ? 'yaml' : 'json'}`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  };

  const handleCopyToClipboard = () => {
    const content = exportFormat === 'yaml' ? generateYaml() : generateJson();
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

  const previewContent = exportFormat === 'yaml' ? generateYaml() : generateJson();

  return (
    <div className="export-panel">
      <h3>Export / Import</h3>
      
      <div className="export-options">
        <div className="format-selector">
          <label>
            <input
              type="radio"
              name="format"
              value="yaml"
              checked={exportFormat === 'yaml'}
              onChange={(e) => setExportFormat(e.target.value)}
            />
            YAML
          </label>
          <label>
            <input
              type="radio"
              name="format"
              value="json"
              checked={exportFormat === 'json'}
              onChange={(e) => setExportFormat(e.target.value)}
            />
            JSON
          </label>
        </div>
        
        {exportFormat === 'json' && (
          <label className="include-poses-toggle">
            <input
              type="checkbox"
              checked={includePoses}
              onChange={(e) => setIncludePoses(e.target.checked)}
            />
            Include intermediate poses (for curve following)
          </label>
        )}
        
        <div className="filename-input">
          <label>Filename:</label>
          <input
            type="text"
            value={fileName}
            onChange={(e) => setFileName(e.target.value)}
          />
          <span>.{exportFormat === 'yaml' ? 'yaml' : 'json'}</span>
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

