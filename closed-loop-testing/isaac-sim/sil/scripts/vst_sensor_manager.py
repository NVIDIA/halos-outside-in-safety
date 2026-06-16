# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# VST Sensor Manager - Python wrapper for VST Sensor Management API
#
# This module provides functions to manage sensors in VST (Video Storage Toolkit)
# for use with Isaac Sim Actor SDG RTSP streaming.
#
# Usage:
#   from vst_sensor_manager import VSTSensorManager
#   
#   vst = VSTSensorManager(base_url="http://10.0.0.1:30888/vst/api")
#   vst.delete_all_sensors()
#   vst.add_sensor(url="rtsp://10.0.0.1:8553/cam1", name="Camera 1")

import os
import json
import logging
import time
from typing import List, Dict, Optional, Any
from urllib.parse import urljoin
from datetime import datetime

try:
    import requests
except ImportError:
    # Fallback for environments without requests
    import urllib.request
    import urllib.error
    requests = None

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


class VSTSensorManager:
    """
    Manager for VST Sensor Management API.
    
    Provides methods to list, add, and delete sensors from VST.
    """
    
    def __init__(
        self,
        base_url: Optional[str] = None,
        auth_token: Optional[str] = None,
        timeout: int = 30,
    ):
        """
        Initialize VST Sensor Manager.
        
        Args:
            base_url: VST API base URL (e.g., http://10.0.0.1:30888/vst/api)
                     Defaults to VST_BASE_URL environment variable
            auth_token: Bearer token for authentication (optional)
            timeout: Request timeout in seconds
        """
        self.base_url = base_url or os.environ.get("VST_BASE_URL", "http://localhost:30888/vst/api")
        self.auth_token = auth_token or os.environ.get("VST_AUTH_TOKEN", "")
        self.timeout = timeout
        self.perception_url = "http://localhost:9000"
        
        # Ensure base_url ends without trailing slash for consistent joining
        self.base_url = self.base_url.rstrip("/")
        
        logger.info(f"VST Sensor Manager initialized with base URL: {self.base_url}")
        logger.info(f"Perception-2D API URL: {self.perception_url}")
    
    def _get_headers(self) -> Dict[str, str]:
        """Get HTTP headers for API requests."""
        headers = {
            "Accept": "application/json",
            "Content-Type": "application/json",
        }
        if self.auth_token:
            headers["Authorization"] = f"Bearer {self.auth_token}"
        return headers
    
    def _request(
        self,
        method: str,
        endpoint: str,
        data: Optional[Dict] = None,
    ) -> Dict[str, Any]:
        """
        Make HTTP request to VST API.
        
        Args:
            method: HTTP method (GET, POST, DELETE)
            endpoint: API endpoint (e.g., /v1/sensor/list)
            data: Request body for POST requests
            
        Returns:
            Response JSON as dictionary
            
        Raises:
            Exception: If request fails
        """
        url = f"{self.base_url}{endpoint}"
        headers = self._get_headers()
        
        if requests:
            # Use requests library if available
            try:
                if method == "GET":
                    response = requests.get(url, headers=headers, timeout=self.timeout, verify=False)
                elif method == "POST":
                    response = requests.post(url, headers=headers, json=data, timeout=self.timeout, verify=False)
                elif method == "DELETE":
                    response = requests.delete(url, headers=headers, timeout=self.timeout, verify=False)
                else:
                    raise ValueError(f"Unsupported HTTP method: {method}")
                
                response.raise_for_status()
                
                if response.text:
                    return response.json()
                return {}
                
            except requests.exceptions.RequestException as e:
                logger.error(f"VST API request failed: {e}")
                raise
        else:
            # Fallback to urllib
            try:
                req = urllib.request.Request(url, headers=headers, method=method)
                if data:
                    req.data = json.dumps(data).encode("utf-8")
                
                with urllib.request.urlopen(req, timeout=self.timeout) as response:
                    body = response.read().decode("utf-8")
                    if body:
                        return json.loads(body)
                    return {}
                    
            except urllib.error.URLError as e:
                logger.error(f"VST API request failed: {e}")
                raise
    
    def list_sensors(self) -> List[Dict[str, Any]]:
        """
        List all sensors registered in VST.
        
        Returns:
            List of sensor dictionaries with keys like:
            - sensorId: Unique sensor ID
            - name: Sensor name
            - sensorIp: Sensor IP address
            - state: Current state
        """
        try:
            result = self._request("GET", "/v1/sensor/list")
            sensors = result if isinstance(result, list) else []
            logger.info(f"Found {len(sensors)} sensor(s) in VST")
            return sensors
        except Exception as e:
            logger.error(f"Failed to list sensors: {e}")
            return []
    
    def get_sensor_streams(self, sensor_id: str) -> Optional[Dict[str, Any]]:
        """
        Get stream information for a specific sensor.
        
        Args:
            sensor_id: ID of the sensor
            
        Returns:
            Stream information dictionary or None
        """
        try:
            result = self._request("GET", "/v1/sensor/streams")
            
            # VST returns streams as: [{'sensor_id': [stream_data, ...]}, ...]
            if isinstance(result, list):
                for stream_dict in result:
                    if isinstance(stream_dict, dict) and sensor_id in stream_dict:
                        stream_list = stream_dict[sensor_id]
                        if isinstance(stream_list, list) and len(stream_list) > 0:
                            # Return first stream (main stream)
                            return stream_list[0]
            
            return None
        except Exception as e:
            logger.error(f"Failed to get streams for sensor {sensor_id}: {e}")
            return None
    
    def _remove_from_perception(
        self,
        camera_id: str,
        camera_name: str,
        camera_url: str,
    ) -> bool:
        """
        Direct API call to perception-2d to remove camera.
        
        This is needed because VST sometimes sends empty camera_url in the remove event.
        We bypass VST and call perception-2d directly with the correct camera_url.
        
        Args:
            camera_id: Camera/sensor ID
            camera_name: Camera name
            camera_url: RTSP URL of the camera
            
        Returns:
            True if successful, False otherwise
        """
        try:
            perception_endpoint = f"{self.perception_url}/api/v1/stream/remove"
            
            payload = {
                "alert_type": "camera_status_change",
                "created_at": datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"),
                "event": {
                    "camera_id": camera_id,
                    "camera_name": camera_name,
                    "camera_url": camera_url,
                    "change": "camera_remove",
                    "tags": ""
                },
                "source": "vst"
            }
            
            headers = {
                "Content-Type": "application/json",
            }
            
            # print(f"Calling perception-2d directly to remove camera: {camera_name}")
            # print(f"URL: {perception_endpoint}")
            # print(f"Payload: {json.dumps(payload, indent=2)}")
            
            if requests:
                response = requests.post(
                    perception_endpoint,
                    headers=headers,
                    json=payload,
                    timeout=self.timeout,
                    verify=False
                )
                
                print(f"Response status: {response.status_code}")
                print(f"Response body: {response.text}")
                
                if response.status_code == 200:
                    print(f"Successfully removed camera")
                    return True
                else:
                    print(f"Returned status {response.status_code}")
                    return False
            else:
                # Fallback to urllib
                import urllib.request
                req = urllib.request.Request(
                    perception_endpoint,
                    data=json.dumps(payload).encode("utf-8"),
                    headers=headers,
                    method="POST"
                )
                
                with urllib.request.urlopen(req, timeout=self.timeout) as response:
                    body = response.read().decode("utf-8")
                    print(f"Response: {body}")
                    print(f"Successfully removed camera from perception-2d")
                    return True
                    
        except Exception as e:
            print(f"Failed to remove camera from perception-2d: {e}")
            return False
    
    def add_sensor(
        self,
        url: Optional[str] = None,
        ip: Optional[str] = None,
        username: str = "",
        password: str = "",
        name: Optional[str] = None,
        location: Optional[str] = None,
    ) -> Optional[str]:
        """
        Add a sensor to VST.
        
        Args:
            url: RTSP URL of the sensor (for RTSP streams)
            ip: IP address of the sensor (for ONVIF cameras)
            username: Sensor username (required for IP-based sensors)
            password: Sensor password (required for IP-based sensors)
            name: Display name for the sensor
            location: Location description
            
        Returns:
            Sensor ID if successful, None otherwise
        """
        if not url and not ip:
            logger.error("Either 'url' or 'ip' must be provided")
            return None
        
        payload = {
            "username": username,
            "password": password,
        }
        
        if url:
            payload["sensorUrl"] = url
        elif ip:
            payload["sensorIp"] = ip
            
        if name:
            payload["name"] = name
        if location:
            payload["location"] = location
        
        try:
            result = self._request("POST", "/v1/sensor/add", data=payload)
            sensor_id = result.get("sensorId", "unknown")
            logger.info(f"✓ Added sensor: {name or url or ip} (ID: {sensor_id})")
            return sensor_id
        except Exception as e:
            logger.error(f"✗ Failed to add sensor {name or url or ip}: {e}")
            return None
    
    def delete_sensor(self, sensor_id: str, use_workaround: bool = True) -> bool:
        """
        Delete a sensor from VST.
        
        Args:
            sensor_id: ID of the sensor to delete
            use_workaround: If True, also call perception-2d directly to ensure removal
                           (workaround for VST sending empty camera_url)
            
        Returns:
            True if successful (or if VST returns 501 which often still deletes), False otherwise
        """
        # Step 1: Get sensor details BEFORE deletion (for workaround)
        sensor_info = None
        camera_url = None
        
        if use_workaround:
            # print(f"Getting sensor info before deletion for sensor: {sensor_id}")
            
            # Get sensor list to find name and state
            sensors = self.list_sensors()
            for sensor in sensors:
                if sensor.get("sensorId") == sensor_id:
                    sensor_info = sensor
                    sensor_state = sensor.get("state", "")
                    
                    # Skip if sensor is already removed
                    if sensor_state == "removed":
                        # print(f"Skipping sensor {sensor.get('name')} - already removed (state: {sensor_state})")
                        return True  # Return success, no need to delete again
                    
                    print(f"Found sensor: {sensor.get('name')} (state: {sensor_state})")
                    break
            
            # If sensor not found or already removed, return success
            if not sensor_info:
                print(f"Sensor {sensor_id} not found in list, assuming already deleted")
                return True
            
            # Try to get URL from streams API
            stream_info = self.get_sensor_streams(sensor_id)
            if stream_info:
                camera_url = stream_info.get("url", "")
                if camera_url:
                    print(f"Got camera URL: {camera_url}")
        
        # Step 2: Delete from VST
        try:
            self._request("DELETE", f"/v1/sensor/{sensor_id}")
            print(f"Deleted sensor from VST: {sensor_id}")
            vst_success = True
        except Exception as e:
            error_str = str(e)
            # VST API bug: returns 501 but still deletes the sensor
            if "501" in error_str:
                print(f"VST returned 501 for sensor {sensor_id} (may still be deleted - VST API quirk)")
                vst_success = True  # Treat as success since VST usually deletes despite 501
            else:
                print(f"Failed to delete sensor {sensor_id}: {e}")
                vst_success = False
        
        # Step 3: Wait for VST to send notification to perception-2d
        if use_workaround and vst_success and sensor_info and camera_url:
            print(f"Waiting 3 seconds for VST to send remove notification...")
            time.sleep(3)
        
        # Step 4: WORKAROUND - Call perception-2d directly if we have camera URL
        if use_workaround and vst_success and sensor_info and camera_url:
            camera_name = sensor_info.get("name", "Unknown")
            # print(f"Calling perception-2d directly")
            self._remove_from_perception(
                camera_id=sensor_id,
                camera_name=camera_name,
                camera_url=camera_url,
            )
        elif use_workaround and vst_success and not camera_url:
            print(f"No camera URL found, workaround not applied (VST should handle it)")
        
        return vst_success
    
    def delete_all_sensors(self, use_workaround: bool = True) -> int:
        """
        Delete all sensors from VST.
        
        Args:
            use_workaround: If True, also call perception-2d directly for each deletion
        
        Returns:
            Number of sensors successfully deleted
        """
        sensors = self.list_sensors()
        if not sensors:
            logger.info("No sensors to delete")
            return 0
        
        deleted = 0
        for sensor in sensors:
            sensor_id = sensor.get("sensorId")
            if sensor_id and self.delete_sensor(sensor_id, use_workaround=use_workaround):
                deleted += 1
        
        logger.info(f"Deleted {deleted}/{len(sensors)} sensor(s)")
        return deleted
    
    def add_sensors_from_config(
        self,
        config_path: str,
        host_ip: Optional[str] = None,
    ) -> List[str]:
        """
        Add sensors from a YAML config file.
        
        Args:
            config_path: Path to cameras.yaml config file
            host_ip: Override HOST_IP for RTSP URLs
            
        Returns:
            List of sensor IDs that were successfully added
        """
        import yaml
        
        if not os.path.exists(config_path):
            logger.error(f"Config file not found: {config_path}")
            return []
        
        with open(config_path, "r") as f:
            config = yaml.safe_load(f)
        
        cameras = config.get("cameras", [])
        rtsp_config = config.get("rtsp", {})
        
        # Determine RTSP host
        rtsp_host = host_ip or os.environ.get("HOST_IP", rtsp_config.get("host", "localhost"))
        rtsp_port = rtsp_config.get("port", 8553)
        
        sensor_ids = []
        for camera in cameras:
            name = camera.get("name", "Unknown")
            rtsp_path = camera.get("rtsp_path", "")
            
            if not rtsp_path:
                logger.warning(f"Skipping camera {name}: no rtsp_path defined")
                continue
            
            # Construct full RTSP URL
            rtsp_url = f"rtsp://{rtsp_host}:{rtsp_port}/{rtsp_path}"
            
            sensor_id = self.add_sensor(
                url=rtsp_url,
                name=name,
            )
            
            if sensor_id:
                sensor_ids.append(sensor_id)
        
        logger.info(f"Added {len(sensor_ids)}/{len(cameras)} camera(s) from config")
        return sensor_ids


def main():
    """CLI for testing VST Sensor Manager."""
    import argparse
    
    parser = argparse.ArgumentParser(description="VST Sensor Manager CLI")
    parser.add_argument("--base-url", help="VST API base URL")
    parser.add_argument("--list", action="store_true", help="List all sensors")
    parser.add_argument("--delete-all", action="store_true", help="Delete all sensors")
    parser.add_argument("--add-url", help="Add sensor by RTSP URL")
    parser.add_argument("--add-from-config", help="Add sensors from config file")
    parser.add_argument("--host-ip", help="Host IP for RTSP URLs")
    parser.add_argument("--name", help="Sensor name (for --add-url)")
    
    args = parser.parse_args()
    
    vst = VSTSensorManager(base_url=args.base_url)
    
    if args.list:
        sensors = vst.list_sensors()
        for s in sensors:
            print(f"  - {s.get('sensorId')}: {s.get('name', 'N/A')}")
    
    if args.delete_all:
        vst.delete_all_sensors()
    
    if args.add_url:
        vst.add_sensor(url=args.add_url, name=args.name)
    
    if args.add_from_config:
        vst.add_sensors_from_config(args.add_from_config, host_ip=args.host_ip)


if __name__ == "__main__":
    main()

