/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "NvPSSSpatialCorrelation.hpp"


float TrajectoryCorrelator::calculateMaxDistance(const std::vector<Point>& traj1,
                                                 const std::vector<Point>& traj2) const
{
    if (traj1.empty() || traj2.empty())
    {
        return 1.0f; // Default normalization factor
    }

    // Find bounding box of both trajectories
    float min_x = std::min(traj1[0].x, traj2[0].x);
    float max_x = std::max(traj1[0].x, traj2[0].x);
    float min_y = std::min(traj1[0].y, traj2[0].y);
    float max_y = std::max(traj1[0].y, traj2[0].y);

    for (const auto& point : traj1)
    {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }

    for (const auto& point : traj2)
    {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }

    // Return diagonal of bounding rectangle
    float width = max_x - min_x;
    float height = max_y - min_y;
    return std::sqrt(width * width + height * height);
}

float TrajectoryCorrelator::computeDiscreteFrechetDistance(const std::vector<Point>& traj1,
                                                           const std::vector<Point>& traj2) const
{
    const size_t n = traj1.size();
    const size_t m = traj2.size();

    if (n == 0 || m == 0)
    {
        return std::numeric_limits<float>::infinity();
    }

    // Use space-optimized approach: only store current and previous row
    std::vector<float> prev_row(m, std::numeric_limits<float>::infinity());
    std::vector<float> curr_row(m, std::numeric_limits<float>::infinity());

    // Initialize first row
    prev_row[0] = traj1[0].distance(traj2[0]);
    for (size_t j = 1; j < m; ++j)
    {
        prev_row[j] = std::max(traj1[0].distance(traj2[j]), prev_row[j-1]);
    }

    // Fill the dynamic programming table row by row
    for (size_t i = 1; i < n; ++i)
    {
        // Initialize first column of current row
        curr_row[0] = std::max(traj1[i].distance(traj2[0]), prev_row[0]);

        // Fill remaining columns
        for (size_t j = 1; j < m; ++j)
        {
            float point_distance = traj1[i].distance(traj2[j]);
            float min_prev = std::min({prev_row[j],      // from above
                                      curr_row[j-1],     // from left
                                      prev_row[j-1]});   // from diagonal
            curr_row[j] = std::max(point_distance, min_prev);
        }

        // Swap rows for next iteration
        std::swap(prev_row, curr_row);
    }

    return prev_row[m-1];
}

float TrajectoryCorrelator::calculateSpatialCorrelation(const std::vector<Point>& traj1,
                                                        const std::vector<Point>& traj2) const
{
    // Validate input
    assert(!traj1.empty() && !traj2.empty() &&
               "Trajectories must not be empty");

    // Handle identical trajectories
    if (traj1.size() == traj2.size())
    {
        bool identical = true;
        for (size_t i = 0; i < traj1.size() && identical; ++i)
        {
            if (traj1[i].distance(traj2[i]) > EPSILON)
            {
                identical = false;
            }
        }
        if (identical)
        {
            return 1.0f;
        }
    }

    // Calculate discrete Fréchet distance
    float frechet_distance = computeDiscreteFrechetDistance(traj1, traj2);

    // Handle infinite distance (completely incomparable trajectories)
    if (std::isinf(frechet_distance))
    {
        return 0.0f;
    }

    // Normalize using maximum possible distance
    float max_distance = calculateMaxDistance(traj1, traj2);

    // Avoid division by zero
    if (max_distance < EPSILON)
    {
        // Points are essentially at the same location
        return 1.0f;
    }

    // Calculate similarity weight (0 = no correlation, 1 = perfect correlation)
    float similarity = 1.0f - std::min(1.0f, frechet_distance / max_distance);

    return std::max(0.0f, similarity); // Ensure non-negative result
}


std::vector<Point> TrajectoryCorrelator::createTrajectory(
                                             const float* x_coords,
                                             const float* y_coords,
                                             size_t size)
{
    std::vector<Point> trajectory;
    trajectory.reserve(size);

    for (size_t i = 0; i < size; ++i)
    {
        trajectory.emplace_back(x_coords[i], y_coords[i]);
    }

    return trajectory;
}
