// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

import { useDispatch, useSelector } from 'react-redux';

// Custom hooks for easier usage
export const useAppDispatch = () => useDispatch();
export const useAppSelector = useSelector;

// Selector hooks for paths
export const usePaths = () => useAppSelector((state) => state.paths.paths);
export const useCurrentPath = () => useAppSelector((state) => state.paths.currentPath);
export const useCurrentPathId = () => useAppSelector((state) => state.paths.currentPathId);
export const useHasUnsavedChanges = () => useAppSelector((state) => state.paths.hasUnsavedChanges);

