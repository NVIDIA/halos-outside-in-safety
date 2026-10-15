# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""cameras.yaml validation in VSTSensorManager.add_sensors_from_config, with no VST.

Run it from `sil/scripts/`:

    python3 -m pytest test_vst_sensor_manager_config.py
"""

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from vst_sensor_manager import VSTSensorManager  # noqa: E402

CAMERA = "cameras:\n  - {name: Camera, port: 8554, mount_path: /camera}\n"


@pytest.fixture
def manager(monkeypatch):
    vst = VSTSensorManager(base_url="http://vst.invalid/vst/api")
    calls = []
    monkeypatch.setattr(vst, "add_sensor", lambda *a, **k: calls.append((a, k)))
    vst.registration_calls = calls
    return vst


def load(manager, tmp_path, text):
    path = tmp_path / "cameras.yaml"
    path.write_text(text)
    return manager.add_sensors_from_config(str(path), host_ip="127.0.0.1")


@pytest.mark.parametrize("rtsp", ["7", "true", "[a, b]"])
def test_non_mapping_rtsp_is_a_validation_error(manager, tmp_path, rtsp):
    result = load(manager, tmp_path, CAMERA + f"rtsp: {rtsp}\n")
    assert result["added"] == 0
    assert any("`rtsp` section must be a mapping" in e for e in result["errors"])
    assert manager.registration_calls == []


def test_non_list_cameras_is_a_validation_error(manager, tmp_path):
    result = load(manager, tmp_path, "cameras: 5\n")
    assert result["added"] == 0
    assert any("`cameras` must be a list" in e for e in result["errors"])
    assert manager.registration_calls == []


def test_legacy_rtsp_port_still_rejected(manager, tmp_path):
    result = load(manager, tmp_path, CAMERA + "rtsp: {port: 8554}\n")
    assert result["schema_ok"] is False
    assert manager.registration_calls == []
