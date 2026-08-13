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
  openPath,
  closePath
} from './redux/pathsSlice';
import WaypointCanvas from './components/WaypointCanvas';
import WaypointList from './components/WaypointList';
import ExportPanel from './components/ExportPanel';
import PathManager from './components/PathManager';
import SavePathModal from './components/SavePathModal';
import { AiFillFolder, AiOutlineSave } from 'react-icons/ai';
import { loadMapRegistry, loadMapConfig, resolveActiveMapId } from './maps/loadMaps';
import './App.css';

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

  // The available maps, and the plan view plus calibration of the active one
  const [registry, setRegistry] = useState([]);
  const [activeMapId, setActiveMapId] = useState(null);
  const [mapConfig, setMapConfig] = useState(null);
  const [mapError, setMapError] = useState(null);

  // Load paths from localStorage on mount
  useEffect(() => {
    dispatch(loadPaths());
  }, [dispatch]);

  // Read the map list once and pick the one to open
  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const maps = await loadMapRegistry();
        const initialId = resolveActiveMapId(maps, window.location.search);
        if (cancelled) return;
        setRegistry(maps);
        setActiveMapId(initialId);
      } catch (error) {
        if (!cancelled) setMapError(error.message);
      }
    })();
    return () => { cancelled = true; };
  }, []);

  // Load the active map's calibration before anything converts a click to metres
  useEffect(() => {
    if (!activeMapId) return;
    let cancelled = false;
    (async () => {
      try {
        const config = await loadMapConfig(activeMapId);
        if (!cancelled) setMapConfig(config);
      } catch (error) {
        if (!cancelled) setMapError(error.message);
      }
    })();
    return () => { cancelled = true; };
  }, [activeMapId]);

  // Point the app at another map, and record it in the URL so a reload or a
  // shared link comes back to the same warehouse.
  const applyMap = useCallback((nextMapId) => {
    setActiveMapId(nextMapId);
    setMapConfig(null);
    const url = new URL(window.location.href);
    url.searchParams.set('map', nextMapId);
    window.history.replaceState({}, '', url);
  }, []);

  // Switching maps from the header discards the drawing: waypoints are metres in
  // the old scene's frame, so carrying them over would place them at coordinates
  // the user never chose.
  const handleSelectMap = useCallback((nextMapId) => {
    if (!nextMapId || nextMapId === activeMapId) return;
    if (hasUnsavedChanges &&
        !confirm('Switching maps discards the unsaved path. Continue?')) {
      return;
    }
    applyMap(nextMapId);
    setOrigin(null);
    setWaypoints([]);
    setSelectedIndex(-1);
    setMode('origin');
    dispatch(closePath());
  }, [activeMapId, applyMap, hasUnsavedChanges, dispatch]);

  // Opening a path from another map brings its map along, so the coordinates are
  // read against the calibration they were drawn with.
  const handleOpenPath = useCallback((path) => {
    if (path.mapId && path.mapId !== activeMapId) {
      applyMap(path.mapId);
    }
    dispatch(openPath(path.id));
  }, [activeMapId, applyMap, dispatch]);

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
    // A file drawn on another warehouse has coordinates that mean something else
    // here, so say so before loading it rather than after the forklift moves.
    const fileMapId = data.map?.id;
    if (fileMapId && fileMapId !== activeMapId) {
      const known = registry.find(map => map.id === fileMapId);
      if (known) {
        if (!confirm(`This path was drawn on "${known.name}". Switch to that map?`)) {
          return;
        }
        applyMap(fileMapId);
      } else if (!confirm(
        `This path was drawn on map "${fileMapId}", which is not installed. ` +
        `Its coordinates may not match the open map. Import anyway?`
      )) {
        return;
      }
    }

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
  }, [activeMapId, applyMap, registry]);

  // Clear all
  const handleClearAll = useCallback(() => {
    if (confirm('Clear all waypoints?')) {
      setWaypoints([]);
      setSelectedIndex(-1);
    }
  }, []);

  // Use default forklift start
  const handleUseDefaultOrigin = useCallback(() => {
    const defaultStart = mapConfig?.defaultForkliftStart;
    if (!defaultStart) return;
    setOrigin({ x: defaultStart.world_x, y: defaultStart.world_y });
    setMode('waypoint');
    setWaypoints([]);
    setSelectedIndex(-1);
  }, [mapConfig]);

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

  if (mapError) {
    return (
      <div className="app app-map-error">
        <h1>Cannot load map</h1>
        <pre>{mapError}</pre>
        <p>Check public/maps/maps.json and the map's config.json.</p>
      </div>
    );
  }

  if (!mapConfig) {
    return (
      <div className="app app-map-loading">
        <p>Loading map…</p>
      </div>
    );
  }

  return (
    <div className="app">
      <header className="app-header">
        <h1>Waypoint Generator</h1>
        <div className="header-info">
          <select
            className="map-select"
            value={activeMapId || ''}
            onChange={(e) => handleSelectMap(e.target.value)}
            title={`Scene: ${mapConfig.scene || 'unknown'}`}
          >
            {registry.map(map => (
              <option key={map.id} value={map.id}>{map.name}</option>
            ))}
          </select>
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
            mapConfig={mapConfig}
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
            mapConfig={mapConfig}
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
        <PathManager
          registry={registry}
          activeMapId={activeMapId}
          onOpenPath={handleOpenPath}
          onClose={() => setShowPathManager(false)}
        />
      )}
      
      {showSaveModal && (
        <SavePathModal
          mapConfig={mapConfig}
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
