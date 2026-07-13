# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# VST Sensor Manager - Python wrapper for VST Sensor Management API
#
# This module provides functions to manage sensors in VST (Video Storage Toolkit)
# for use with Isaac Sim 6.0 Actor SDG RTSP streaming.
#
# Schema requirement: cameras.yaml v6.0 (per-camera port + mount_path).
# Legacy 5.1 schema (rtsp_path + global rtsp.port) is REJECTED with a
# migration hint.
#
# Usage:
#   from vst_sensor_manager import VSTSensorManager
#
#   vst = VSTSensorManager(base_url="http://10.0.0.1:30888/vst/api")
#   vst.delete_all_sensors()
#   # 6.0 URL form: rtsp://<host>:<per-cam-port><per-cam-mount-path>
#   vst.add_sensor(url="rtsp://10.0.0.1:8554/camera", name="Camera")

import os
import json
import logging
import time
from typing import List, Dict, Optional, Any
from urllib.parse import urljoin

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
        self.perception_url = os.environ.get("PERCEPTION_BASE_URL", "http://localhost:9000")

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
        Best-effort direct DeepStream (:9000) source purge, self-verified.

        ``delete_sensor`` ignores the return value -- the load-bearing cleanup is the VST
        DELETE that stops the RTSP DESCRIBE-churn. DeepStream matches
        ``/api/v1/stream/remove`` on the EXACT ``camera_url`` used at add time (there is no
        remove-by-id or clear endpoint), and the url it holds can drift from VST's current
        proxy url after churn/redeploy -- so this remove may legitimately fail to match.
        We POST the remove, then confirm via ``get-stream-info`` whether the source
        actually left, and return the truth: a plain HTTP 200 can still carry a
        ``STREAM_REMOVE_FAIL "No record found"`` body.

        Args:
            camera_id: Camera/sensor ID (DeepStream source key)
            camera_name: Camera name
            camera_url: RTSP url DeepStream was provisioned with (VST proxy url)

        Returns:
            True only if the source is confirmed gone from DeepStream, else False.
        """
        if not camera_url:
            logger.info(
                f"No DeepStream url for {camera_name}; skipping direct purge "
                f"(the VST DELETE is the load-bearing cleanup)."
            )
            return False

        endpoint = f"{self.perception_url}/api/v1/stream/remove"
        payload = {
            "key": "sensor",
            "value": {
                "camera_id": camera_id,
                "camera_name": camera_name,
                "camera_url": camera_url,
                # nvmultiurisrcbin needs the change string at value.change; a VST-style
                # event envelope returns HTTP 400 "Sensor API change string not supported".
                "change": "camera_remove",
                "metadata": {},
            },
        }
        headers = {"Content-Type": "application/json"}

        try:
            if requests:
                resp = requests.post(
                    endpoint, headers=headers, json=payload,
                    timeout=self.timeout, verify=False,
                )
                logger.info(f"DeepStream remove {camera_name}: HTTP {resp.status_code} {resp.text.strip()}")
            else:
                import urllib.request
                req = urllib.request.Request(
                    endpoint, data=json.dumps(payload).encode("utf-8"),
                    headers=headers, method="POST",
                )
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    logger.info(f"DeepStream remove {camera_name}: {resp.read().decode('utf-8').strip()}")
        except Exception as e:
            logger.warning(f"DeepStream remove POST failed for {camera_name}: {e}")

        # Self-verify: HTTP 200 does not prove the source left (the body may be
        # STREAM_REMOVE_FAIL "No record found"). Confirm against get-stream-info.
        time.sleep(0.5)
        if self._ds_source_present(camera_id):
            logger.warning(
                f"DeepStream still lists {camera_name} ({camera_id}) after remove -- the url "
                f"likely does not match DeepStream's provisioned url (churn/port drift). This "
                f"orphan needs the exact provisioned url (host-side) or a WDM reconcile."
            )
            return False
        logger.info(f"DeepStream source purged: {camera_name} ({camera_id}).")
        return True

    def _ds_source_present(self, camera_id: str) -> bool:
        """True if DeepStream still lists a source with this camera_id."""
        endpoint = f"{self.perception_url}/api/v1/stream/get-stream-info"
        try:
            if requests:
                info = requests.get(endpoint, timeout=self.timeout, verify=False).json()
            else:
                import urllib.request
                with urllib.request.urlopen(endpoint, timeout=self.timeout) as r:
                    info = json.loads(r.read().decode("utf-8"))
            streams = (info.get("stream-info") or {}).get("stream-info") or []
            return any(s.get("camera_id") == camera_id for s in streams)
        except Exception as e:
            logger.warning(f"Could not read DeepStream stream-info to verify removal: {e}")
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
            use_workaround: If True, also best-effort purge the DeepStream source directly
                           on :9000 (the VST DELETE is the load-bearing cleanup)

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

    # ------------------------------------------------------------------
    # cameras.yaml 6.0 schema parsing — REWRITTEN for Isaac Sim 6.0
    # ------------------------------------------------------------------

    @staticmethod
    def _detect_legacy_schema(config: Dict[str, Any]) -> Optional[str]:
        """
        Detect Isaac Sim 5.1 cameras.yaml schema and return a migration hint.

        Triggers on either:
          - Top-level `rtsp.port` present (mediamtx broker — dropped in 6.0).
          - Any camera entry carrying `rtsp_path` (legacy RTSPWriter mount).

        Returns:
            Migration hint string if legacy schema detected, None otherwise.
        """
        rtsp_cfg = config.get("rtsp", {}) or {}
        cameras = config.get("cameras", []) or []

        legacy_signals: List[str] = []

        if "port" in rtsp_cfg:
            legacy_signals.append(
                f"top-level `rtsp.port={rtsp_cfg.get('port')}` (5.1 mediamtx broker)"
            )

        legacy_cams = [
            (idx, cam.get("name", f"<unnamed#{idx}>"))
            for idx, cam in enumerate(cameras)
            if isinstance(cam, dict) and "rtsp_path" in cam
        ]
        if legacy_cams:
            sample = ", ".join(f"#{i}({n})" for i, n in legacy_cams[:3])
            legacy_signals.append(
                f"{len(legacy_cams)} camera(s) with `rtsp_path` field ({sample})"
            )

        if not legacy_signals:
            return None

        return (
            "5.1 schema detected: "
            + " AND ".join(legacy_signals)
            + ". This script requires the Isaac Sim 6.0 cameras.yaml schema "
              "(per-camera `port` + `mount_path`, no global `rtsp.port`, no "
              "`rtsp_path`)."
        )

    @staticmethod
    def _validate_camera_config(camera: Dict[str, Any], idx: int) -> Optional[str]:
        """
        Validate one camera entry against the 6.0 schema.

        Required fields:
          - name        : non-empty string
          - port        : int in [1024, 65535] (TCP ephemeral / user range)
          - mount_path  : non-empty string starting with "/"

        Soft-warned (logged but not failed):
          - camera_prim : string starting with "/" (USD prim path).
                          Not used by this script — Isaac Sim consumes it.

        Returns:
            Error string if validation fails, None if camera is well-formed.
        """
        if not isinstance(camera, dict):
            return f"camera #{idx}: expected mapping, got {type(camera).__name__}"

        label = camera.get("name", f"<unnamed#{idx}>")

        # name
        name = camera.get("name")
        if not isinstance(name, str) or not name.strip():
            return f"camera #{idx} ({label}): `name` is required and must be a non-empty string"

        # port
        port = camera.get("port")
        if port is None:
            return f"camera #{idx} ({label}): `port` is required (Isaac Sim 6.0 in-process RTSP server port)"
        if not isinstance(port, int) or isinstance(port, bool):
            return f"camera #{idx} ({label}): `port` must be int, got {type(port).__name__} ({port!r})"
        if port < 1024 or port > 65535:
            return f"camera #{idx} ({label}): `port={port}` out of range [1024, 65535]"

        # mount_path
        mount_path = camera.get("mount_path")
        if not isinstance(mount_path, str) or not mount_path:
            return (
                f"camera #{idx} ({label}): `mount_path` is required and must be a non-empty string "
                f"(e.g. \"/camera\")"
            )
        if not mount_path.startswith("/"):
            return (
                f"camera #{idx} ({label}): `mount_path={mount_path!r}` must start with \"/\" "
                f"(RTSP mount path)"
            )

        # camera_prim — soft validation; this script doesn't use it but a
        # malformed value typically indicates a copy-paste error in cameras.yaml.
        camera_prim = camera.get("camera_prim")
        if camera_prim is not None:
            if not isinstance(camera_prim, str) or not camera_prim.startswith("/"):
                logger.warning(
                    f"camera #{idx} ({label}): `camera_prim={camera_prim!r}` should be a USD "
                    f"prim path starting with \"/\" — this field is not used here but Isaac Sim will reject it"
                )

        return None

    def add_sensors_from_config(
        self,
        config_path: str,
        host_ip: Optional[str] = None,
    ) -> Dict[str, Any]:
        """
        Add sensors from an Isaac Sim 6.0 cameras.yaml config file.

        Schema (6.0 only — NO 5.1 backward compat):

            cameras:
              - name: Camera
                camera_prim: /World/Cameras/Camera     # consumed by Isaac Sim
                port: 8554                              # per-camera RTSP server
                mount_path: /camera                     # URL suffix
            rtsp:
              host: ${HOST_IP}                          # only host stays global

        URL formula:
            rtsp://{rtsp.host}:{cam.port}{cam.mount_path}

        Args:
            config_path: Path to cameras.yaml config file.
            host_ip: Override HOST_IP for RTSP URLs. Precedence:
                     host_ip arg > HOST_IP env > rtsp.host in YAML > "localhost".

        Returns:
            Dict with keys:
              - config_path : str — input path
              - host        : str — resolved RTSP host
              - schema_ok   : bool — False if legacy 5.1 schema rejected
              - schema_hint : Optional[str] — migration hint if schema_ok is False
              - attempted   : int — number of cameras in config
              - added       : int — number successfully registered with VST
              - sensor_ids  : List[str] — VST sensor IDs of registered cameras
              - errors      : List[str] — per-camera or per-config error strings
        """
        import yaml

        result: Dict[str, Any] = {
            "config_path": config_path,
            "host": "",
            "schema_ok": True,
            "schema_hint": None,
            "attempted": 0,
            "added": 0,
            "sensor_ids": [],
            "errors": [],
        }

        # ---- 1. Load YAML ----------------------------------------------------
        if not os.path.exists(config_path):
            msg = f"Config file not found: {config_path}"
            logger.error(msg)
            result["errors"].append(msg)
            return result

        try:
            with open(config_path, "r") as f:
                config = yaml.safe_load(f) or {}
        except yaml.YAMLError as e:
            msg = f"Failed to parse {config_path}: {e}"
            logger.error(msg)
            result["errors"].append(msg)
            return result

        if not isinstance(config, dict):
            msg = f"Top-level YAML must be a mapping, got {type(config).__name__}"
            logger.error(msg)
            result["errors"].append(msg)
            return result

        # ---- 2. Detect legacy 5.1 schema → hard-fail with migration hint -----
        legacy_hint = self._detect_legacy_schema(config)
        if legacy_hint:
            logger.error(legacy_hint)
            result["schema_ok"] = False
            result["schema_hint"] = legacy_hint
            result["errors"].append(legacy_hint)
            return result

        # ---- 3. Resolve RTSP host (port is now per-camera) -------------------
        rtsp_cfg = config.get("rtsp", {}) or {}
        if not isinstance(rtsp_cfg, dict):
            msg = f"`rtsp` section must be a mapping, got {type(rtsp_cfg).__name__}"
            logger.error(msg)
            result["errors"].append(msg)
            return result

        # Precedence: explicit arg > env > YAML > localhost.
        yaml_host = rtsp_cfg.get("host", "localhost")
        rtsp_host = host_ip or os.environ.get("HOST_IP", yaml_host)
        # Expand ${HOST_IP} literal if it slipped through unsubstituted.
        if isinstance(rtsp_host, str) and rtsp_host.strip() in ("${HOST_IP}", "$HOST_IP"):
            env_host = os.environ.get("HOST_IP")
            if env_host:
                rtsp_host = env_host
            else:
                msg = (
                    f"`rtsp.host` is the literal {rtsp_host!r} and $HOST_IP is unset. "
                    f"Set HOST_IP env or pass --host-ip."
                )
                logger.error(msg)
                result["errors"].append(msg)
                return result
        result["host"] = rtsp_host

        # ---- 4. Per-camera validate + URL build + VST register ---------------
        cameras = config.get("cameras", []) or []
        if not isinstance(cameras, list):
            msg = f"`cameras` must be a list, got {type(cameras).__name__}"
            logger.error(msg)
            result["errors"].append(msg)
            return result

        result["attempted"] = len(cameras)
        seen_ports: Dict[int, str] = {}     # port -> first camera that used it
        seen_mounts: Dict[str, str] = {}    # mount_path -> first camera that used it

        for idx, camera in enumerate(cameras):
            err = self._validate_camera_config(camera, idx)
            if err:
                logger.error(err)
                result["errors"].append(err)
                continue

            name = camera["name"]
            port = camera["port"]
            mount_path = camera["mount_path"]

            # Duplicate-port / duplicate-mount detection: in Isaac Sim 6.0 each
            # camera spawns its own in-process RTSP server, so a port collision
            # means one camera will silently lose its stream.
            if port in seen_ports:
                err = (
                    f"camera #{idx} ({name}): port {port} already claimed by "
                    f"{seen_ports[port]!r} — each Isaac Sim 6.0 camera needs a unique port"
                )
                logger.error(err)
                result["errors"].append(err)
                continue
            if mount_path in seen_mounts:
                # Same mount on different ports is technically OK (different
                # servers), but it's almost always a copy-paste bug, so warn.
                logger.warning(
                    f"camera #{idx} ({name}): mount_path {mount_path!r} also used by "
                    f"{seen_mounts[mount_path]!r} (different port, likely OK but check cameras.yaml)"
                )
            seen_ports[port] = name
            seen_mounts.setdefault(mount_path, name)

            # 6.0 URL build: rtsp://{rtsp.host}:{cam.port}{cam.mount_path}
            # Note: no "/" between port and mount_path — mount_path already has it.
            rtsp_url = f"rtsp://{rtsp_host}:{port}{mount_path}"

            sensor_id = self.add_sensor(url=rtsp_url, name=name)
            if sensor_id:
                result["sensor_ids"].append(sensor_id)
                result["added"] += 1
            else:
                result["errors"].append(
                    f"camera #{idx} ({name}): VST add_sensor failed for {rtsp_url}"
                )

        logger.info(
            f"Added {result['added']}/{result['attempted']} camera(s) from "
            f"{config_path} (errors: {len(result['errors'])})"
        )
        return result


def main():
    """CLI for testing VST Sensor Manager."""
    import argparse
    import sys

    parser = argparse.ArgumentParser(description="VST Sensor Manager CLI (Isaac Sim 6.0 schema)")
    parser.add_argument("--base-url", help="VST API base URL")
    parser.add_argument("--list", action="store_true", help="List all sensors")
    parser.add_argument("--delete-all", action="store_true", help="Delete all sensors")
    parser.add_argument("--add-url", help="Add sensor by RTSP URL")
    parser.add_argument("--add-from-config", help="Add sensors from cameras.yaml (6.0 schema)")
    parser.add_argument("--host-ip", help="Host IP for RTSP URLs (overrides HOST_IP / rtsp.host)")
    parser.add_argument("--name", help="Sensor name (for --add-url)")

    args = parser.parse_args()

    vst = VSTSensorManager(base_url=args.base_url)

    exit_code = 0

    if args.list:
        sensors = vst.list_sensors()
        for s in sensors:
            print(f"  - {s.get('sensorId')}: {s.get('name', 'N/A')}")

    if args.delete_all:
        vst.delete_all_sensors()

    if args.add_url:
        sid = vst.add_sensor(url=args.add_url, name=args.name)
        if not sid:
            exit_code = 1

    if args.add_from_config:
        outcome = vst.add_sensors_from_config(args.add_from_config, host_ip=args.host_ip)
        print(
            f"add_from_config: host={outcome['host']!r} "
            f"added={outcome['added']}/{outcome['attempted']} "
            f"errors={len(outcome['errors'])}"
        )
        for sid in outcome["sensor_ids"]:
            print(f"  + sensor_id={sid}")
        for err in outcome["errors"]:
            print(f"  ! {err}")

        # Exit codes: 2 = legacy schema rejected, 1 = some/all cameras failed,
        # 0 = full success.
        if not outcome["schema_ok"]:
            exit_code = max(exit_code, 2)
        elif outcome["errors"] or outcome["added"] < outcome["attempted"]:
            exit_code = max(exit_code, 1)

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
