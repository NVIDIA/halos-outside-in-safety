// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

import { useState } from 'react';
import { AiOutlineUp, AiOutlineDown, AiOutlineEdit, AiOutlineDelete } from 'react-icons/ai';

/**
 * WaypointList - Display and edit waypoint list
 */
export default function WaypointList({
  waypoints,
  origin,
  selectedIndex,
  onSelectWaypoint,
  onUpdateWaypoint,
  onDeleteWaypoint,
  onReorderWaypoint,
}) {
  const [showWorld, setShowWorld] = useState(true);
  const [editingIndex, setEditingIndex] = useState(null);
  const [editValues, setEditValues] = useState({});

  const handleStartEdit = (index, wp) => {
    setEditingIndex(index);
    setEditValues({
      x: wp.x.toFixed(2),
      y: wp.y.toFixed(2),
      theta_deg: wp.theta_deg.toFixed(0),
      note: wp.note || '',
      reverse: wp.reverse || false,
    });
  };

  const handleSaveEdit = (index) => {
    onUpdateWaypoint?.(index, {
      x: parseFloat(editValues.x) || 0,
      y: parseFloat(editValues.y) || 0,
      theta_deg: parseFloat(editValues.theta_deg) || 0,
      note: editValues.note,
      reverse: editValues.reverse || false,
    });
    setEditingIndex(null);
  };

  const handleCancelEdit = () => {
    setEditingIndex(null);
  };

  const handleMoveUp = (index) => {
    if (index > 0) {
      onReorderWaypoint?.(index, index - 1);
    }
  };

  const handleMoveDown = (index) => {
    if (index < waypoints.length - 1) {
      onReorderWaypoint?.(index, index + 1);
    }
  };

  // Calculate world coords from odom
  const getWorldCoords = (wp) => {
    if (!origin) return null;
    return {
      x: origin.x + wp.x,
      y: origin.y + wp.y,
    };
  };

  return (
    <div className="waypoint-list">
      <div className="waypoint-list-header">
        <h3>Waypoints ({waypoints.length})</h3>
        <label className="toggle-world" title="Show world coordinates">
          <input
            type="checkbox"
            checked={showWorld}
            onChange={(e) => setShowWorld(e.target.checked)}
          />
          World
        </label>
      </div>
      
      {waypoints.length === 0 && (
        <p className="empty-message">
          No waypoints yet. Set origin first, then click on map to add waypoints.
        </p>
      )}
      
      <div className="waypoint-items">
        {waypoints.map((wp, index) => (
          <div
            key={index}
            className={`waypoint-item ${selectedIndex === index ? 'selected' : ''} ${wp.reverse ? 'reverse' : ''}`}
            onClick={() => onSelectWaypoint?.(index)}
          >
            <div className="waypoint-header">
              <span className="waypoint-number">
                {index + 1}
                {wp.reverse && <span className="reverse-badge">R</span>}
              </span>
              <div className="waypoint-actions">
                <button 
                  className="btn-icon" 
                  onClick={(e) => { e.stopPropagation(); handleMoveUp(index); }}
                  disabled={index === 0}
                  title="Move up"
                >
                  <AiOutlineUp />
                </button>
                <button 
                  className="btn-icon" 
                  onClick={(e) => { e.stopPropagation(); handleMoveDown(index); }}
                  disabled={index === waypoints.length - 1}
                  title="Move down"
                >
                  <AiOutlineDown />
                </button>
                <button 
                  className="btn-icon edit" 
                  onClick={(e) => { e.stopPropagation(); handleStartEdit(index, wp); }}
                  title="Edit"
                >
                  <AiOutlineEdit />
                </button>
                <button 
                  className="btn-icon delete" 
                  onClick={(e) => { e.stopPropagation(); onDeleteWaypoint?.(index); }}
                  title="Delete"
                >
                  <AiOutlineDelete />
                </button>
              </div>
            </div>
            
            {editingIndex === index ? (
              <div className="waypoint-edit" onClick={(e) => e.stopPropagation()}>
                <div className="edit-row">
                  <label>X:</label>
                  <input
                    type="number"
                    step="0.1"
                    value={editValues.x}
                    onChange={(e) => setEditValues({ ...editValues, x: e.target.value })}
                  />
                  <span>m</span>
                </div>
                <div className="edit-row">
                  <label>Y:</label>
                  <input
                    type="number"
                    step="0.1"
                    value={editValues.y}
                    onChange={(e) => setEditValues({ ...editValues, y: e.target.value })}
                  />
                  <span>m</span>
                </div>
                <div className="edit-row">
                  <label>θ:</label>
                  <input
                    type="number"
                    step="5"
                    value={editValues.theta_deg}
                    onChange={(e) => setEditValues({ ...editValues, theta_deg: e.target.value })}
                  />
                  <span>°</span>
                </div>
                <div className="edit-row">
                  <label>Note:</label>
                  <input
                    type="text"
                    value={editValues.note}
                    onChange={(e) => setEditValues({ ...editValues, note: e.target.value })}
                    placeholder="Optional note"
                  />
                </div>
                <div className="edit-row">
                  <label className="reverse-toggle">
                    <input
                      type="checkbox"
                      checked={editValues.reverse || false}
                      onChange={(e) => setEditValues({ ...editValues, reverse: e.target.checked })}
                    />
                    Reverse (drive backward)
                  </label>
                </div>
                <div className="edit-buttons">
                  <button className="btn-save" onClick={() => handleSaveEdit(index)}>Save</button>
                  <button className="btn-cancel" onClick={handleCancelEdit}>Cancel</button>
                </div>
              </div>
            ) : (
              <div className="waypoint-info">
                <div className="coord odom-coord">
                  <span className="coord-label">Odom:</span>
                  <span>X: {wp.x.toFixed(2)}m</span>
                  <span>Y: {wp.y.toFixed(2)}m</span>
                  <span>θ: {wp.theta_deg.toFixed(0)}°</span>
                </div>
                {showWorld && origin && (
                  <div className="coord world-coord">
                    <span className="coord-label">World:</span>
                    <span>X: {(origin.x + wp.x).toFixed(2)}m</span>
                    <span>Y: {(origin.y + wp.y).toFixed(2)}m</span>
                  </div>
                )}
                {wp.note && <div className="note">{wp.note}</div>}
              </div>
            )}
          </div>
        ))}
      </div>
    </div>
  );
}

