// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

import { createSlice } from '@reduxjs/toolkit';

const initialState = {
  // List of all saved paths
  paths: [],
  
  // Currently active path
  currentPath: null,
  
  // Current path ID
  currentPathId: null,
  
  // Whether there are unsaved changes
  hasUnsavedChanges: false,
};

export const pathsSlice = createSlice({
  name: 'paths',
  initialState,
  reducers: {
    // Create a new path
    createPath: (state, action) => {
      const { name, origin, waypoints, mapId } = action.payload;
      const newPath = {
        id: Date.now().toString(),
        name,
        // Which map the coordinates were drawn against. Waypoints are metres in
        // one scene's world frame, so the same numbers land somewhere else
        // entirely on another map — the path is meaningless without this.
        mapId: mapId || null,
        origin,
        waypoints: waypoints || [],
        createdAt: new Date().toISOString(),
        updatedAt: new Date().toISOString(),
      };
      state.paths.push(newPath);
      state.currentPath = newPath;
      state.currentPathId = newPath.id;
      state.hasUnsavedChanges = false;
    },

    // Open an existing path
    openPath: (state, action) => {
      const pathId = action.payload;
      const path = state.paths.find(p => p.id === pathId);
      if (path) {
        state.currentPath = path;
        state.currentPathId = pathId;
        state.hasUnsavedChanges = false;
      }
    },

    // Save current path changes
    savePath: (state, action) => {
      const { origin, waypoints, mapId } = action.payload;
      if (state.currentPathId) {
        const pathIndex = state.paths.findIndex(p => p.id === state.currentPathId);
        if (pathIndex !== -1) {
          state.paths[pathIndex] = {
            ...state.paths[pathIndex],
            // Saving is also how a path predating map tracking gets labelled
            mapId: mapId || state.paths[pathIndex].mapId || null,
            origin,
            waypoints,
            updatedAt: new Date().toISOString(),
          };
          state.currentPath = state.paths[pathIndex];
          state.hasUnsavedChanges = false;
        }
      }
    },

    // Update current path without saving to list (marks as unsaved)
    updateCurrentPath: (state, action) => {
      const { origin, waypoints } = action.payload;
      if (state.currentPath) {
        state.currentPath = {
          ...state.currentPath,
          origin,
          waypoints,
        };
        state.hasUnsavedChanges = true;
      }
    },

    // Delete a path
    deletePath: (state, action) => {
      const pathId = action.payload;
      state.paths = state.paths.filter(p => p.id !== pathId);
      
      // If deleted path was current, clear current path
      if (state.currentPathId === pathId) {
        state.currentPath = null;
        state.currentPathId = null;
        state.hasUnsavedChanges = false;
      }
    },

    // Rename a path
    renamePath: (state, action) => {
      const { pathId, newName } = action.payload;
      const pathIndex = state.paths.findIndex(p => p.id === pathId);
      if (pathIndex !== -1) {
        state.paths[pathIndex].name = newName;
        state.paths[pathIndex].updatedAt = new Date().toISOString();
        
        // Update current path if it's the one being renamed
        if (state.currentPathId === pathId) {
          state.currentPath = state.paths[pathIndex];
        }
      }
    },

    // Create new empty path
    newPath: (state) => {
      state.currentPath = null;
      state.currentPathId = null;
      state.hasUnsavedChanges = false;
    },

    // Close current path
    closePath: (state) => {
      state.currentPath = null;
      state.currentPathId = null;
      state.hasUnsavedChanges = false;
    },

    // Import paths from file
    importPaths: (state, action) => {
      const importedPaths = action.payload;
      // Merge imported paths, avoiding duplicates by name
      importedPaths.forEach(imported => {
        const exists = state.paths.find(p => p.name === imported.name);
        if (!exists) {
          state.paths.push({
            ...imported,
            id: Date.now().toString() + Math.random(),
            importedAt: new Date().toISOString(),
          });
        }
      });
    },

    // Save to localStorage
    persistPaths: (state) => {
      try {
        localStorage.setItem('waypoint-paths', JSON.stringify(state.paths));
      } catch (error) {
        console.error('Failed to persist paths:', error);
      }
    },

    // Load from localStorage
    loadPaths: (state) => {
      try {
        const savedPaths = localStorage.getItem('waypoint-paths');
        if (savedPaths) {
          // Paths saved before maps were tracked get mapId null rather than a
          // guess. The tool was retargeted from the 20x20 scene to the 40x20 one
          // in place, so a stored path could belong to either and nothing in it
          // says which. Null means "unlabelled", and the UI says so instead of
          // silently drawing it on whichever map happens to be open.
          state.paths = JSON.parse(savedPaths).map(path => ({
            ...path,
            mapId: path.mapId ?? null,
          }));
        }
      } catch (error) {
        console.error('Failed to load paths:', error);
      }
    },
  },
});

export const {
  createPath,
  openPath,
  savePath,
  updateCurrentPath,
  deletePath,
  renamePath,
  newPath,
  closePath,
  importPaths,
  persistPaths,
  loadPaths,
} = pathsSlice.actions;

export default pathsSlice.reducer;

