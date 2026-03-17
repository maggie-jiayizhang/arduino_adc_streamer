"""
Sensor configuration helpers and persistence for editable 5-channel layouts.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Dict, List, Tuple

from config_constants import (
    DEFAULT_SENSOR_CONFIGURATION,
    DEFAULT_SENSOR_CONFIGURATION_NAME,
    SENSOR_LOCATION_CODES,
)

SENSOR_POSITION_ORDER = ["T", "R", "C", "L", "B"]
SENSOR_POSITION_LABELS = {
    "T": "Top",
    "R": "Right",
    "C": "Center",
    "L": "Left",
    "B": "Bottom",
}


def default_sensor_configuration() -> Dict[str, object]:
    return {
        "name": str(DEFAULT_SENSOR_CONFIGURATION.get("name", DEFAULT_SENSOR_CONFIGURATION_NAME)),
        "channel_sensor_map": list(DEFAULT_SENSOR_CONFIGURATION.get("channel_sensor_map", [])),
        "is_bundled": False,
    }


def normalize_channel_sensor_map(channel_sensor_map) -> List[str] | None:
    if not isinstance(channel_sensor_map, list) or len(channel_sensor_map) != 5:
        return None

    normalized = [str(value).strip().upper() for value in channel_sensor_map]
    if sorted(normalized) != sorted(SENSOR_LOCATION_CODES):
        return None
    return normalized


def normalize_sensor_config(config: Dict[str, object]) -> Dict[str, object] | None:
    if not isinstance(config, dict):
        return None

    name = str(config.get("name", "")).strip()
    channel_sensor_map = normalize_channel_sensor_map(config.get("channel_sensor_map"))
    if not name or channel_sensor_map is None:
        return None

    return {
        "name": name,
        "channel_sensor_map": channel_sensor_map,
    }


def _repo_root() -> Path:
    return Path(__file__).resolve().parent


def _read_sensor_configs_file(file_path: Path) -> List[Dict[str, object]]:
    if not file_path.exists():
        return []

    with file_path.open("r", encoding="utf-8") as handle:
        payload = json.load(handle)

    raw_configs = payload.get("configurations", payload if isinstance(payload, list) else [])
    configs: List[Dict[str, object]] = []
    used_names = set()
    for raw_config in raw_configs:
        normalized = normalize_sensor_config(raw_config)
        if normalized is None:
            continue
        if normalized["name"] in used_names:
            continue
        used_names.add(str(normalized["name"]))
        configs.append(normalized)
    return configs


def mapping_to_position_channels(channel_sensor_map: List[str]) -> Dict[str, int]:
    return {
        sensor_label: channel_sensor_map.index(sensor_label) + 1
        for sensor_label in SENSOR_POSITION_ORDER
    }


def position_channels_to_mapping(position_channels: Dict[str, int]) -> List[str]:
    mapping = [None] * 5
    for sensor_label in SENSOR_POSITION_ORDER:
        channel_number = int(position_channels[sensor_label])
        if channel_number < 1 or channel_number > 5:
            raise ValueError(f"Channel number out of range for {sensor_label}: {channel_number}")
        if mapping[channel_number - 1] is not None:
            raise ValueError(f"Duplicate channel assignment for channel {channel_number}")
        mapping[channel_number - 1] = sensor_label

    if any(value is None for value in mapping):
        raise ValueError("Incomplete sensor mapping")
    return mapping


class SensorConfigStore:
    def __init__(self, file_path: Path | None = None, bundled_file_path: Path | None = None):
        self.file_path = file_path or (Path.home() / ".adc_streamer" / "sensors" / "sensor_configurations.json")
        self.bundled_file_path = bundled_file_path or (_repo_root() / "sensors.json")

    def load(self) -> Tuple[List[Dict[str, object]], str]:
        default_config = normalize_sensor_config(default_sensor_configuration()) or default_sensor_configuration()
        bundled_configs = _read_sensor_configs_file(self.bundled_file_path)
        if not bundled_configs:
            bundled_configs = [dict(default_config)]

        local_payload = {}
        if self.file_path.exists():
            with self.file_path.open("r", encoding="utf-8") as handle:
                local_payload = json.load(handle)

        deleted_names = {
            str(name).strip()
            for name in local_payload.get("deleted_names", [])
            if str(name).strip()
        }
        local_configs = _read_sensor_configs_file(self.file_path)

        configs_by_name: Dict[str, Dict[str, object]] = {}
        for config in bundled_configs:
            if config["name"] in deleted_names:
                continue
            configs_by_name[str(config["name"])] = {
                "name": config["name"],
                "channel_sensor_map": list(config["channel_sensor_map"]),
                "is_bundled": True,
            }

        for config in local_configs:
            configs_by_name[str(config["name"])] = {
                "name": config["name"],
                "channel_sensor_map": list(config["channel_sensor_map"]),
                "is_bundled": False,
            }

        configs = list(configs_by_name.values())
        if not configs:
            configs = [dict(default_config)]

        selected_name = str(local_payload.get("selected_name", "")).strip()
        if selected_name not in {config["name"] for config in configs}:
            selected_name = str(default_config["name"])
        if selected_name not in {config["name"] for config in configs}:
            selected_name = str(configs[0]["name"])

        return configs, selected_name

    def save(self, configs: List[Dict[str, object]], selected_name: str) -> None:
        bundled_configs = {
            config["name"]: config
            for config in _read_sensor_configs_file(self.bundled_file_path)
        }

        normalized_local_configs = []
        current_names = set()
        for config in configs:
            normalized = normalize_sensor_config(config)
            if normalized is None:
                continue
            name = str(normalized["name"])
            current_names.add(name)
            is_bundled = bool(config.get("is_bundled", False))
            bundled_match = bundled_configs.get(name)
            if is_bundled and bundled_match == normalized:
                continue
            normalized_local_configs.append(normalized)

        deleted_names = sorted(
            name
            for name in bundled_configs
            if name not in current_names
        )

        all_names = {
            config["name"] for config in normalized_local_configs
        } | {
            name for name in bundled_configs if name not in deleted_names
        }
        if not all_names:
            default_config = default_sensor_configuration()
            normalized_local_configs = [normalize_sensor_config(default_config) or default_config]
            selected_name = str(default_config["name"])

        if selected_name not in all_names:
            selected_name = str(next(iter(sorted(all_names))))

        self.file_path.parent.mkdir(parents=True, exist_ok=True)
        with self.file_path.open("w", encoding="utf-8") as handle:
            json.dump(
                {
                    "version": 1,
                    "selected_name": selected_name,
                    "deleted_names": deleted_names,
                    "configurations": normalized_local_configs,
                },
                handle,
                indent=2,
            )
