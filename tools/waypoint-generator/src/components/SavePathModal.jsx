// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

import { useState, useEffect } from 'react';
import { useAppDispatch } from '../redux/hooks';
import { useCurrentPath, useCurrentPathId } from '../redux/hooks';
import { createPath, savePath, persistPaths } from '../redux/pathsSlice';
import { AiOutlineClose } from 'react-icons/ai';
import './SavePathModal.css';

function SavePathModal({ origin, waypoints, onClose, onSaved }) {
  const dispatch = useAppDispatch();
  const currentPath = useCurrentPath();
  const currentPathId = useCurrentPathId();
  
  const [pathName, setPathName] = useState(currentPath?.name || '');
  const [saveMode, setSaveMode] = useState(currentPathId ? 'update' : 'new');

  useEffect(() => {
    if (currentPath) {
      setPathName(currentPath.name);
    }
  }, [currentPath]);

  const handleSave = () => {
    if (!pathName.trim()) {
      alert('Please enter a path name');
      return;
    }

    if (!origin) {
      alert('Please set an origin point first');
      return;
    }

    if (saveMode === 'new' || !currentPathId) {
      // Create new path
      dispatch(createPath({
        name: pathName.trim(),
        origin,
        waypoints,
      }));
    } else {
      // Update existing path
      dispatch(savePath({
        origin,
        waypoints,
      }));
    }

    // Persist to localStorage
    setTimeout(() => {
      dispatch(persistPaths());
    }, 100);

    onSaved?.();
    onClose();
  };

  return (
    <div className="save-path-overlay" onClick={onClose}>
      <div className="save-path-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <h3>{currentPathId ? 'Save Path' : 'Create New Path'}</h3>
          <button className="close-btn" onClick={onClose}>
            <AiOutlineClose />
          </button>
        </div>

        <div className="modal-body">
          {currentPathId && (
            <div className="save-mode-selector">
              <label>
                <input
                  type="radio"
                  value="update"
                  checked={saveMode === 'update'}
                  onChange={(e) => setSaveMode(e.target.value)}
                />
                Update existing path "{currentPath?.name}"
              </label>
              <label>
                <input
                  type="radio"
                  value="new"
                  checked={saveMode === 'new'}
                  onChange={(e) => setSaveMode(e.target.value)}
                />
                Save as new path
              </label>
            </div>
          )}

          {(saveMode === 'new' || !currentPathId) && (
            <div className="form-group">
              <label htmlFor="pathName">Path Name</label>
              <input
                id="pathName"
                type="text"
                value={pathName}
                onChange={(e) => setPathName(e.target.value)}
                placeholder="Enter path name..."
                autoFocus
                onKeyDown={(e) => {
                  if (e.key === 'Enter') handleSave();
                  if (e.key === 'Escape') onClose();
                }}
              />
            </div>
          )}

          <div className="path-summary">
            <h4>Summary</h4>
            <div className="summary-item">
              <span>Origin:</span>
              <span>({origin?.x.toFixed(2)}, {origin?.y.toFixed(2)})</span>
            </div>
            <div className="summary-item">
              <span>Waypoints:</span>
              <span>{waypoints.length}</span>
            </div>
          </div>
        </div>

        <div className="modal-footer">
          <button className="cancel-btn" onClick={onClose}>
            Cancel
          </button>
          <button className="save-btn" onClick={handleSave}>
            {saveMode === 'update' && currentPathId ? 'Update' : 'Create'}
          </button>
        </div>
      </div>
    </div>
  );
}

export default SavePathModal;

