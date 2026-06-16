#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Latency Distribution Analyzer
# Analyzes message latency from log files and generates histogram
#
# Usage:
#   ./analyze_latency.sh <logfile>
#   cat logfile | ./analyze_latency.sh
#

# Input from file or stdin
if [ -n "$1" ]; then
    INPUT="$1"
    if [ ! -f "$INPUT" ]; then
        echo "Error: File not found: $INPUT"
        exit 1
    fi
    DATA=$(cat "$INPUT")
else
    DATA=$(cat)
fi

# Extract latency values (assumes format like "latency: 123ms" or "123 ms")
LATENCIES=$(echo "$DATA" | grep -oE '[0-9]+\.?[0-9]* ?ms' | grep -oE '[0-9]+\.?[0-9]*')

if [ -z "$LATENCIES" ]; then
    echo "No latency data found in input"
    exit 1
fi

# Calculate statistics using awk
STATS=$(echo "$LATENCIES" | awk '
BEGIN {
    count = 0
    sum = 0
    min = 999999
    max = 0
}
{
    count++
    sum += $1
    if ($1 < min) min = $1
    if ($1 > max) max = $1
    latencies[count] = $1
}
END {
    avg = sum / count
    
    # Distribution buckets
    bucket_0_100 = 0
    bucket_100_200 = 0
    bucket_200_300 = 0
    bucket_300_400 = 0
    bucket_400_500 = 0
    bucket_500_600 = 0
    bucket_600_700 = 0
    bucket_700_800 = 0
    bucket_800_900 = 0
    bucket_900_1000 = 0
    bucket_1000_1500 = 0
    bucket_1500_2000 = 0
    bucket_2000_2500 = 0
    bucket_over_2500 = 0
    
    for (i = 1; i <= count; i++) {
        val = latencies[i]
        if (val < 100) bucket_0_100++
        else if (val < 200) bucket_100_200++
        else if (val < 300) bucket_200_300++
        else if (val < 400) bucket_300_400++
        else if (val < 500) bucket_400_500++
        else if (val < 600) bucket_500_600++
        else if (val < 700) bucket_600_700++
        else if (val < 800) bucket_700_800++
        else if (val < 900) bucket_800_900++
        else if (val < 1000) bucket_900_1000++
        else if (val < 1500) bucket_1000_1500++
        else if (val < 2000) bucket_1500_2000++
        else if (val < 2500) bucket_2000_2500++
        else bucket_over_2500++
    }
    
    # Print statistics
    print "================================================================"
    print ""
    print " OVERVIEW:"
    printf "   Total messages: %d\n", count
    printf "   Average latency: %.2f ms\n", avg
    printf "   Min: %.0f ms  |  Max: %.0f ms\n", min, max
    print ""
    print "================================================================"
    print "Segment (ms)      | Count    | Ratio   | Chart"
    print "================================================================"
    
    # Helper to create bar chart (inline, no function)
    # Print distribution with inline bar generation
    pct = bucket_0_100/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "0 - 100", bucket_0_100, pct, bar
    
    pct = bucket_100_200/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "100 - 200", bucket_100_200, pct, bar
    
    pct = bucket_200_300/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "200 - 300", bucket_200_300, pct, bar
    
    pct = bucket_300_400/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "300 - 400", bucket_300_400, pct, bar
    
    pct = bucket_400_500/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "400 - 500", bucket_400_500, pct, bar
    
    pct = bucket_500_600/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "500 - 600", bucket_500_600, pct, bar
    
    pct = bucket_600_700/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "600 - 700", bucket_600_700, pct, bar
    
    pct = bucket_700_800/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "700 - 800", bucket_700_800, pct, bar
    
    pct = bucket_800_900/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "800 - 900", bucket_800_900, pct, bar
    
    pct = bucket_900_1000/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "900 - 1000", bucket_900_1000, pct, bar
    
    pct = bucket_1000_1500/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "1000 - 1500", bucket_1000_1500, pct, bar
    
    pct = bucket_1500_2000/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "1500 - 2000", bucket_1500_2000, pct, bar
    
    pct = bucket_2000_2500/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "2000 - 2500", bucket_2000_2500, pct, bar
    
    pct = bucket_over_2500/count*100; bars = int(pct/2); bar = ""; for (i=0; i<bars; i++) bar = bar "█"
    printf "%-17s | %8d | %6.2f%% | %s\n", "> 2500", bucket_over_2500, pct, bar
    
    print "================================================================"
}
')

echo "$STATS"

# Percentile analysis
PERCENTILES=$(echo "$LATENCIES" | sort -n | awk '
{
    latencies[NR] = $1
    count = NR
}
END {
    p50 = latencies[int(count * 0.50)]
    p90 = latencies[int(count * 0.90)]
    p95 = latencies[int(count * 0.95)]
    p99 = latencies[int(count * 0.99)]
    
    print ""
    print "   PERCENTILES:"
    printf "   P50 (median): %.0f ms\n", p50
    printf "   P90: %.0f ms\n", p90
    printf "   P95: %.0f ms\n", p95
    printf "   P99: %.0f ms\n", p99
    print "================================================================"
}
')

echo "$PERCENTILES"

