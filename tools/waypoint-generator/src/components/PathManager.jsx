// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

import { useState } from 'react';
import { useAppDispatch } from '../redux/hooks';
import { 
  usePaths, 
  useCurrentPathId, 
  useHasUnsavedChanges 
} from '../redux/hooks';
import { 
  deletePath, 
  renamePath, 
  newPath 
} from '../redux/pathsSlice';
import PathPreview from './PathPreview';
import { AiFillFolderOpen, AiOutlineEdit, AiOutlineDelete, AiOutlinePlus, AiOutlineClose } from 'react-icons/ai';
import './PathManager.css';

function PathManager({ registry = [], activeMapId, onOpenPath, onClose }) {
  const dispatch = useAppDispatch();
  const paths = usePaths();
  const currentPathId = useCurrentPathId();
  const hasUnsavedChanges = useHasUnsavedChanges();
  
  const [editingId, setEditingId] = useState(null);
  const [editingName, setEditingName] = useState('');
  const [searchTerm, setSearchTerm] = useState('');
  const [showOtherMaps, setShowOtherMaps] = useState(false);

  const mapNameOf = (mapId) =>
    registry.find(map => map.id === mapId)?.name || mapId;

  const handleOpenPath = (path) => {
    if (hasUnsavedChanges) {
      if (!confirm('You have unsaved changes. Do you want to discard them?')) {
        return;
      }
    }
    onOpenPath?.(path);
    onClose?.();
  };

  const handleDeletePath = (pathId, pathName) => {
    if (confirm(`Are you sure you want to delete "${pathName}"?`)) {
      dispatch(deletePath(pathId));
    }
  };

  const handleStartRename = (pathId, currentName) => {
    setEditingId(pathId);
    setEditingName(currentName);
  };

  const handleSaveRename = (pathId) => {
    if (editingName.trim() && editingName !== paths.find(p => p.id === pathId)?.name) {
      dispatch(renamePath({ pathId, newName: editingName.trim() }));
    }
    setEditingId(null);
    setEditingName('');
  };

  const handleCancelRename = () => {
    setEditingId(null);
    setEditingName('');
  };

  const handleNewPath = () => {
    if (hasUnsavedChanges) {
      if (!confirm('You have unsaved changes. Do you want to discard them?')) {
        return;
      }
    }
    dispatch(newPath());
    onClose?.();
  };

  // A path belongs to the map it was drawn on. Paths with no recorded map are
  // always listed: they predate map tracking, so they may well belong here, and
  // hiding them would look like they had been lost.
  const belongsHere = (path) => !path.mapId || path.mapId === activeMapId;
  const otherMapCount = paths.filter(path => !belongsHere(path)).length;

  const filteredPaths = paths.filter(path =>
    path.name.toLowerCase().includes(searchTerm.toLowerCase()) &&
    (showOtherMaps || belongsHere(path))
  );

  const sortedPaths = [...filteredPaths].sort((a, b) => 
    new Date(b.updatedAt) - new Date(a.updatedAt)
  );

  return (
    <div className="path-manager-overlay" onClick={onClose}>
      <div className="path-manager" onClick={(e) => e.stopPropagation()}>
        <div className="path-manager-header">
          <h2>Path Manager</h2>
          <button className="close-btn" onClick={onClose}>
            <AiOutlineClose />
          </button>
        </div>

        <div className="path-manager-actions">
          <input
            type="text"
            placeholder="Search paths..."
            value={searchTerm}
            onChange={(e) => setSearchTerm(e.target.value)}
            className="search-input"
          />
          <button className="new-path-btn" onClick={handleNewPath}>
            <AiOutlinePlus /> New Path
          </button>
        </div>

        {otherMapCount > 0 && (
          <label className="show-other-maps">
            <input
              type="checkbox"
              checked={showOtherMaps}
              onChange={(e) => setShowOtherMaps(e.target.checked)}
            />
            Show {otherMapCount} path(s) from other maps
          </label>
        )}

        <div className="paths-list">
          {sortedPaths.length === 0 ? (
            <div className="empty-state">
              <p>No paths found.</p>
              <p className="hint">Create a new path to get started!</p>
            </div>
          ) : (
            sortedPaths.map(path => (
              <div 
                key={path.id} 
                className={`path-item ${currentPathId === path.id ? 'active' : ''}`}
              >
                <div className="path-preview-container">
                  <PathPreview 
                    origin={path.origin} 
                    waypoints={path.waypoints || []} 
                  />
                </div>

                <div className="path-info">
                  {editingId === path.id ? (
                    <input
                      type="text"
                      value={editingName}
                      onChange={(e) => setEditingName(e.target.value)}
                      onKeyDown={(e) => {
                        if (e.key === 'Enter') handleSaveRename(path.id);
                        if (e.key === 'Escape') handleCancelRename();
                      }}
                      onBlur={() => handleSaveRename(path.id)}
                      autoFocus
                      className="rename-input"
                    />
                  ) : (
                    <>
                      <h3 
                        className="path-name"
                        onDoubleClick={() => handleStartRename(path.id, path.name)}
                      >
                        {path.name}
                        {currentPathId === path.id && <span className="current-badge">Current</span>}
                      </h3>
                      <div className="path-meta">
                        <span>{path.waypoints?.length || 0} waypoints</span>
                        <span>•</span>
                        <span>Updated: {new Date(path.updatedAt).toLocaleDateString()}</span>
                        {!path.mapId ? (
                          <>
                            <span>•</span>
                            <span
                              className="map-unknown"
                              title="Saved before the tool tracked maps, so which warehouse it was drawn on is unknown. Save it again to label it."
                            >
                              map not recorded
                            </span>
                          </>
                        ) : path.mapId !== activeMapId && (
                          <>
                            <span>•</span>
                            <span className="map-foreign">{mapNameOf(path.mapId)}</span>
                          </>
                        )}
                      </div>
                    </>
                  )}
                </div>
                
                <div className="path-actions">
                  {editingId !== path.id && (
                    <>
                      <button 
                        className="action-btn open-btn"
                        onClick={() => handleOpenPath(path)}
                        title={path.mapId && path.mapId !== activeMapId
                          ? `Open, switching to ${mapNameOf(path.mapId)}`
                          : 'Open path'}
                      >
                        <AiFillFolderOpen /> Open
                      </button>
                      <button 
                        className="action-btn rename-btn"
                        onClick={() => handleStartRename(path.id, path.name)}
                        title="Rename path"
                      >
                        <AiOutlineEdit />
                      </button>
                      <button 
                        className="action-btn delete-btn"
                        onClick={() => handleDeletePath(path.id, path.name)}
                        title="Delete path"
                      >
                        <AiOutlineDelete />
                      </button>
                    </>
                  )}
                </div>
              </div>
            ))
          )}
        </div>

        <div className="path-manager-footer">
          <p className="total-count">Total: {paths.length} path(s)</p>
        </div>
      </div>
    </div>
  );
}

export default PathManager;

