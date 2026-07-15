// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

import { useState, useCallback, useEffect, useRef } from 'react';
import { useAppDispatch } from './redux/hooks';
import { 
  useCurrentPath, 
  useCurrentPathId, 
  useHasUnsavedChanges 
} from './redux/hooks';
import { 
  updateCurrentPath, 
  loadPaths, 
  persistPaths,
  openPath 
} from './redux/pathsSlice';
import WaypointCanvas from './components/WaypointCanvas';
import WaypointList from './components/WaypointList';
import ExportPanel from './components/ExportPanel';
import PathManager from './components/PathManager';
import SavePathModal from './components/SavePathModal';
import { AiFillFolder, AiOutlineSave } from 'react-icons/ai';
import { getCalibration } from './utils/coordinates';
import './App.css';

const calibration = getCalibration();

function App() {
  const dispatch = useAppDispatch();
  const currentPath = useCurrentPath();
  const currentPathId = useCurrentPathId();
  const hasUnsavedChanges = useHasUnsavedChanges();

  // Mode: 'origin' | 'waypoint' | 'view'
  const [mode, setMode] = useState('origin');
  
  // Origin point in world coordinates
  const [origin, setOrigin] = useState(null);
  
  // Waypoints in odom coordinates (relative to origin)
  const [waypoints, setWaypoints] = useState([]);
  
  // Selected waypoint index
  const [selectedIndex, setSelectedIndex] = useState(-1);
  
  // Preview heading for new waypoint
  const [previewHeading, setPreviewHeading] = useState(180);

  // Modal states
  const [showPathManager, setShowPathManager] = useState(false);
  const [showSaveModal, setShowSaveModal] = useState(false);

  // Flag to prevent update loop when loading path from Redux
  const isLoadingFromRedux = useRef(false);

  // Load paths from localStorage on mount
  useEffect(() => {
    dispatch(loadPaths());
  }, [dispatch]);

  // Load current path when it changes
  useEffect(() => {
    if (currentPath) {
      // Set flag to prevent update loop
      isLoadingFromRedux.current = true;
      
      setOrigin(currentPath.origin);
      setWaypoints(currentPath.waypoints || []);
      setMode('waypoint');
      setSelectedIndex(-1);
      
      // Reset flag after state updates have been queued
      setTimeout(() => {
        isLoadingFromRedux.current = false;
      }, 0);
    }
  }, [currentPath]);

  // Update Redux when waypoints or origin change
  useEffect(() => {
    // Skip if we're currently loading from Redux to prevent infinite loop
    if (isLoadingFromRedux.current) {
      return;
    }
    
    if (currentPathId && (origin || waypoints.length > 0)) {
      dispatch(updateCurrentPath({ origin, waypoints }));
    }
  }, [origin, waypoints, currentPathId, dispatch]);

  // Set origin point
  const handleSetOrigin = useCallback((point) => {
    setOrigin(point);
    setMode('waypoint');
    // Clear waypoints when origin changes
    setWaypoints([]);
    setSelectedIndex(-1);
  }, []);

  // Add waypoint (normal forward)
  const handleAddWaypoint = useCallback((wp) => {
    setWaypoints(prev => [...prev, { ...wp, note: '', reverse: false }]);
    setSelectedIndex(waypoints.length);
  }, [waypoints.length]);

  // Add reverse waypoint (forklift drives backward to this point)
  const handleAddReverseWaypoint = useCallback((wp) => {
    setWaypoints(prev => [...prev, { ...wp, note: '', reverse: true }]);
    setSelectedIndex(waypoints.length);
  }, [waypoints.length]);

  // Select waypoint
  const handleSelectWaypoint = useCallback((index) => {
    setSelectedIndex(index);
  }, []);

  // Update waypoint
  const handleUpdateWaypoint = useCallback((index, updates) => {
    setWaypoints(prev => prev.map((wp, i) => 
      i === index ? { ...wp, ...updates } : wp
    ));
  }, []);

  // Delete waypoint
  const handleDeleteWaypoint = useCallback((index) => {
    setWaypoints(prev => prev.filter((_, i) => i !== index));
    if (selectedIndex >= index) {
      setSelectedIndex(prev => Math.max(-1, prev - 1));
    }
  }, [selectedIndex]);

  // Move waypoint position
  const handleMoveWaypoint = useCallback((index, newPos) => {
    setWaypoints(prev => prev.map((wp, i) => 
      i === index ? { ...wp, x: newPos.x, y: newPos.y } : wp
    ));
  }, []);

  // Reorder waypoint
  const handleReorderWaypoint = useCallback((fromIndex, toIndex) => {
    setWaypoints(prev => {
      const newWaypoints = [...prev];
      const [moved] = newWaypoints.splice(fromIndex, 1);
      newWaypoints.splice(toIndex, 0, moved);
      return newWaypoints;
    });
    setSelectedIndex(toIndex);
  }, []);

  // Import waypoints
  const handleImport = useCallback((data) => {
    if (data.origin) {
      setOrigin({ x: data.origin.world_x, y: data.origin.world_y });
    }
    if (data.waypoints) {
      setWaypoints(data.waypoints.map(wp => ({
        x: wp.x,
        y: wp.y,
        theta_deg: wp.theta_deg,
        note: wp.note || '',
        velocity: wp.velocity,
        reverse: wp.reverse || false,
      })));
    }
    setMode('waypoint');
  }, []);

  // Clear all
  const handleClearAll = useCallback(() => {
    if (confirm('Clear all waypoints?')) {
      setWaypoints([]);
      setSelectedIndex(-1);
    }
  }, []);

  // Use default forklift start
  const handleUseDefaultOrigin = useCallback(() => {
    const defaultStart = calibration.defaultForkliftStart;
    setOrigin({ x: defaultStart.world_x, y: defaultStart.world_y });
    setMode('waypoint');
    setWaypoints([]);
    setSelectedIndex(-1);
  }, []);

  // Handle save
  const handleSave = useCallback(() => {
    if (!origin) {
      alert('Please set an origin point first');
      return;
    }
    setShowSaveModal(true);
  }, [origin]);

  // Handle save completed
  const handleSaveCompleted = useCallback(() => {
    // Persist to localStorage
    dispatch(persistPaths());
  }, [dispatch]);

  return (
    <div className="app">
      <header className="app-header">
        <h1>Waypoint Generator</h1>
        <div className="header-info">
          {currentPath && (
            <span className="current-path-name">
              {currentPath.name}
              {hasUnsavedChanges && <span className="unsaved-indicator">●</span>}
            </span>
          )}
          {origin && (
            <span className="origin-info">
              Origin: ({origin.x.toFixed(2)}, {origin.y.toFixed(2)})
            </span>
          )}
        </div>
        <div className="header-actions">
          <button 
            className="header-btn"
            onClick={() => setShowPathManager(true)}
            title="Manage paths"
          >
            <AiFillFolder /> Paths
          </button>
          <button 
            className="header-btn save-btn"
            onClick={handleSave}
            disabled={!origin && waypoints.length === 0}
            title="Save current path"
          >
            <AiOutlineSave /> Save
          </button>
        </div>
      </header>
      
      <div className="app-body">
        <aside className="sidebar">
          <div className="mode-selector">
            <h3>Mode</h3>
            <div className="mode-buttons">
              <button
                className={`mode-btn ${mode === 'origin' ? 'active' : ''}`}
                onClick={() => setMode('origin')}
              >
                Set Origin
              </button>
              <button
                className={`mode-btn ${mode === 'waypoint' ? 'active' : ''}`}
                onClick={() => setMode('waypoint')}
                disabled={!origin}
              >
                Add Waypoint
              </button>
              <button
                className={`mode-btn ${mode === 'view' ? 'active' : ''}`}
                onClick={() => setMode('view')}
              >
                View
              </button>
            </div>
            
            {!origin && (
              <div className="origin-hint">
                <p>Click on map to set origin, or:</p>
                <button className="btn-default-origin" onClick={handleUseDefaultOrigin}>
                  Use Default Forklift Start
                </button>
              </div>
            )}
            
            {mode === 'waypoint' && origin && (
              <div className="heading-control">
                <label>Preview Heading: {previewHeading}°</label>
                <input
                  type="range"
                  min="-180"
                  max="180"
                  step="5"
                  value={previewHeading}
                  onChange={(e) => setPreviewHeading(parseInt(e.target.value))}
                />
                <p className="hint">Shift+Scroll to adjust on canvas</p>
                <p className="hint">Right-click: Add reverse waypoint</p>
              </div>
            )}
          </div>
          
          <WaypointList
            waypoints={waypoints}
            origin={origin}
            selectedIndex={selectedIndex}
            onSelectWaypoint={handleSelectWaypoint}
            onUpdateWaypoint={handleUpdateWaypoint}
            onDeleteWaypoint={handleDeleteWaypoint}
            onReorderWaypoint={handleReorderWaypoint}
          />
          
          {waypoints.length > 0 && (
            <button className="btn-clear" onClick={handleClearAll}>
              Clear All
            </button>
          )}
        </aside>
        
        <main className="canvas-container">
          <WaypointCanvas
            waypoints={waypoints}
            origin={origin}
            selectedIndex={selectedIndex}
            previewHeading={previewHeading}
            mode={mode}
            onAddWaypoint={handleAddWaypoint}
            onAddReverseWaypoint={handleAddReverseWaypoint}
            onSelectWaypoint={handleSelectWaypoint}
            onSetOrigin={handleSetOrigin}
            onUpdatePreviewHeading={setPreviewHeading}
            onMoveWaypoint={handleMoveWaypoint}
          />
        </main>
        
        <aside className="sidebar-right">
          <ExportPanel
            waypoints={waypoints}
            origin={origin}
            onImport={handleImport}
          />
          
          <div className="help-section">
            <h4>Controls</h4>
            <ul>
              <li><b>Click</b>: Add waypoint / Select</li>
              <li><b>Drag</b>: Move waypoint</li>
              <li><b>Alt+Drag / Middle</b>: Pan view</li>
              <li><b>Scroll</b>: Zoom</li>
              <li><b>Shift+Scroll</b>: Adjust heading</li>
            </ul>
          </div>
        </aside>
      </div>

      {/* Modals */}
      {showPathManager && (
        <PathManager onClose={() => setShowPathManager(false)} />
      )}
      
      {showSaveModal && (
        <SavePathModal
          origin={origin}
          waypoints={waypoints}
          onClose={() => setShowSaveModal(false)}
          onSaved={handleSaveCompleted}
        />
      )}
    </div>
  );
}

export default App;
