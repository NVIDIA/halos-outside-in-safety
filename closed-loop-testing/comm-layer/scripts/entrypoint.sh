#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set -e

# Source ROS2 (Jazzy for Isaac Sim 5.1 compatibility)
source /opt/ros/jazzy/setup.bash

echo "============================================================"
echo "     SIL Communication & Control Container                  "
echo "     UDP -> OPC UA -> ROS2 Bridge                          "
echo "============================================================"
echo ""
echo "Configuration:"
echo "  UDP Port:        $UDP_PORT"
echo "  OPC UA Endpoint: $OPCUA_ENDPOINT"
echo "  ROS Topic:       $ROS_TOPIC_PREFIX"
echo "  ROS Domain ID:   $ROS_DOMAIN_ID"
echo ""

# Create log files
touch /app/logs/opc_server.log
touch /app/logs/ros_bridge.log

# Function to handle shutdown
cleanup() {
    echo ""
    echo "Shutting down SIL services..."
    kill $OPC_PID $ROS_PID 2>/dev/null || true
    wait 2>/dev/null || true
    echo "SIL services stopped."
    exit 0
}
trap cleanup SIGTERM SIGINT

# Start OPC UA Server (includes UDP receiver)
echo "[1/2] Starting OPC UA Server + UDP Receiver..."
cd /app/comm_layer
python3 scripts/run_opc_server.py \
    -p $UDP_PORT \
    -e $OPCUA_ENDPOINT \
    2>&1 | tee /app/logs/opc_server.log &

OPC_PID=$!
echo "      OPC UA Server started (PID: $OPC_PID)"

# Wait for OPC UA server to be ready
echo "      Waiting for OPC UA server to be ready..."
sleep 3

# Verify OPC UA is running
for i in {1..10}; do
    if kill -0 $OPC_PID 2>/dev/null; then
        # Try to connect
        if python3 -c "from asyncua.sync import Client; c=Client('$OPCUA_ENDPOINT'); c.connect(); c.disconnect()" 2>/dev/null; then
            echo "      OPC UA Server is ready!"
            break
        fi
    else
        echo "ERROR: OPC UA Server process died!"
        exit 1
    fi
    
    if [ $i -eq 10 ]; then
        echo "WARNING: OPC UA Server might not be fully ready, continuing anyway..."
    fi
    sleep 1
done

# Start ROS2 Bridge
echo "[2/2] Starting ROS2 Bridge..."
cd /app/ros_bridge
python3 scripts/run_ros_bridge.py \
    --opcua $OPCUA_ENDPOINT \
    --topic-prefix $ROS_TOPIC_PREFIX \
    --rate 10.0 \
    2>&1 | tee /app/logs/ros_bridge.log &

ROS_PID=$!
echo "      ROS2 Bridge started (PID: $ROS_PID)"

# Wait a moment and verify
sleep 2
if ! kill -0 $ROS_PID 2>/dev/null; then
    echo "ERROR: ROS2 Bridge failed to start!"
    cat /app/logs/ros_bridge.log
    exit 1
fi

echo ""
echo "============================================================"
echo "SIL Communication & Control is running"
echo ""
echo "Services:"
echo "   - OPC UA Server: $OPCUA_ENDPOINT"
echo "   - UDP Receiver:  port $UDP_PORT"
echo "   - ROS2 Topics:   $ROS_TOPIC_PREFIX/*"
echo ""
echo "Logs:"
echo "   - OPC UA:  /app/logs/opc_server.log"
echo "   - ROS2:    /app/logs/ros_bridge.log"
echo ""
echo "Test commands:"
echo "   ros2 topic list | grep $ROS_TOPIC_PREFIX"
echo "   ros2 topic echo $ROS_TOPIC_PREFIX/command"
echo "============================================================"

# Wait for any process to exit
wait -n

# If we get here, something failed
echo ""
echo "ERROR: A service exited unexpectedly!"
echo "OPC Server log tail:"
tail -20 /app/logs/opc_server.log
echo ""
echo "ROS Bridge log tail:"
tail -20 /app/logs/ros_bridge.log
exit 1
