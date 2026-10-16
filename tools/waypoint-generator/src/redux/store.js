// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

import { configureStore } from '@reduxjs/toolkit';
import pathsReducer, { PATHS_STORAGE_KEY } from './pathsSlice.js';

export function makeStore() {
  const store = configureStore({
    reducer: {
      paths: pathsReducer,
    },
    middleware: (getDefaultMiddleware) =>
      getDefaultMiddleware({
        serializableCheck: false,
      }),
  });

  // Every change to the saved paths is written, whichever action made it: a
  // reload rebuilds the list from this key alone. The reducers keep the array's
  // identity unless a saved path changed, so editing the open drawing writes
  // nothing.
  let persistedPaths = store.getState().paths.paths;
  store.subscribe(() => {
    const { paths } = store.getState().paths;
    if (paths === persistedPaths) return;
    persistedPaths = paths;
    try {
      localStorage.setItem(PATHS_STORAGE_KEY, JSON.stringify(paths));
    } catch (error) {
      console.error('Failed to persist paths:', error);
    }
  });

  return store;
}

export default makeStore();
