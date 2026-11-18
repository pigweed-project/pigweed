# Copyright 2026 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""pw_daq YAML configuration loading, validation, and option management."""

from __future__ import annotations

from collections.abc import Mapping
import copy
import logging
import os
from pathlib import Path
from typing import Any, overload

import jsonschema  # type: ignore[import-untyped]
import jsonschema.exceptions  # type: ignore[import-untyped]
from pw_config_loader.yaml_config_loader_mixin import (
    MissingConfigTitle,
    Stage,
    YamlConfigLoaderMixin,
)
from pw_daq.backend_registry import BackendRegistry
from pw_daq.common import DaqError
import yaml

_LOG = logging.getLogger(__name__)

_CONFIG_SECTION_TITLE = "pw_daq"
_PROJECT_CONFIG_FILE = Path("$PW_PROJECT_ROOT/.pw_daq.yaml")
_PROJECT_USER_CONFIG_FILE = Path("$PW_PROJECT_ROOT/.pw_daq.user.yaml")
_USER_CONFIG_FILE = Path("~/.pw_daq.yaml")
_CONFIG_ENV_VAR = "PW_DAQ_CONFIG"
_OPTIONAL_DEFAULT_STAGES = frozenset(
    {
        Stage.PROJECT_FILE,
        Stage.USER_PROJECT_FILE,
        Stage.USER_FILE,
    }
)
_CONFIG_SCHEMA = {
    "$schema": "http://json-schema.org/draft-07/schema#",
    "title": "pw_daq configuration",
    "description": "Configuration schema for pw_daq and daq-cli.",
    "type": "object",
    "properties": {
        "config_title": {
            "type": "string",
            "const": "pw_daq",
            "description": "Configuration section title for pw_config_loader.",
        },
        "options": {
            "type": "object",
            "description": "General pw_daq configuration options.",
            "properties": {
                "default_device": {
                    "type": "string",
                    "minLength": 1,
                    "description": (
                        "Default configured device name used when no device "
                        "is explicitly specified."
                    ),
                }
            },
            "additionalProperties": False,
        },
        "devices": {
            "type": "object",
            "description": (
                "Mapping of device names to device configuration objects."
            ),
            "additionalProperties": {
                "type": "object",
                "properties": {
                    "backend": {
                        "type": "string",
                        "minLength": 1,
                        "description": (
                            "Name of the backend used by this device."
                        ),
                    },
                    "dev": {
                        "type": "object",
                        "description": "Device connection configuration.",
                        "properties": {
                            "type": {
                                "type": "string",
                                "minLength": 1,
                                "description": (
                                    "Connection type (e.g. 'serial')."
                                ),
                            },
                            "path": {
                                "type": "string",
                                "minLength": 1,
                                "description": (
                                    "Device path "
                                    "(e.g. '/dev/cu.usbmodem1113301')."
                                ),
                            },
                            "baudrate": {
                                "type": "integer",
                                "minimum": 1,
                                "description": "Communication baud rate.",
                            },
                        },
                        "additionalProperties": True,
                    },
                    "capabilities": {
                        "type": "object",
                        "description": (
                            "Inline mapping describing device capabilities, "
                            "hardware resources, virtual resource mappings, "
                            "and backend-defined configuration options."
                        ),
                    },
                },
                "required": ["backend", "capabilities"],
                "additionalProperties": True,
            },
        },
    },
    "required": ["options", "devices"],
    "additionalProperties": False,
}


def _ensure_project_root_env() -> None:
    if "PW_PROJECT_ROOT" not in os.environ:
        for var in ("BUILD_WORKSPACE_DIRECTORY", "PW_ROOT"):
            if val := os.environ.get(var):
                os.environ["PW_PROJECT_ROOT"] = val
                break


@overload
def _deep_merge(
    v1: Mapping[Any, Any], v2: Mapping[Any, Any]
) -> dict[Any, Any]: ...


@overload
def _deep_merge(v1: list[Any], v2: list[Any]) -> list[Any]: ...


def _deep_merge(
    v1: Mapping[Any, Any] | list[Any],
    v2: Mapping[Any, Any] | list[Any],
) -> dict[Any, Any] | list[Any]:
    if isinstance(v1, Mapping) and isinstance(v2, Mapping):
        merged = copy.deepcopy(dict(v1))
        for key, value in v2.items():
            if (
                key in merged
                and isinstance(merged[key], Mapping)
                and isinstance(value, Mapping)
            ):
                # If the key exists in v1 and both are mappings, deep merge
                merged[key] = _deep_merge(merged[key], value)
            elif (
                key in merged
                and isinstance(merged[key], list)
                and isinstance(value, list)
            ):
                # If the key exists in v1 and both are lists, merge the lists
                merged[key] = _deep_merge(merged[key], value)
            elif (
                key in merged
                and isinstance(merged[key], (Mapping, list))
                and value is None
            ):
                # Preserve existing mapping or list when YAML key has no value
                continue
            else:
                # If either the key doesn't exist in v1 or the items are not
                # both mappings or both lists, override with the v2 value
                merged[key] = copy.deepcopy(value)
        return merged

    if isinstance(v1, list) and isinstance(v2, list):
        merged_list: list[Any] = []
        for item in [*v1, *v2]:
            if not any(
                isinstance(existing, bool) == isinstance(item, bool)
                and existing == item
                for existing in merged_list
            ):
                merged_list.append(copy.deepcopy(item))
        return merged_list

    raise TypeError(
        f"Cannot deep merge {type(v1).__name__} and {type(v2).__name__}"
    )


class DaqConfig(YamlConfigLoaderMixin):
    """Storage container and loader for a daq-cli configuration."""

    def __init__(
        self,
        *,
        config_file: Path | str | None = None,
        project_file: Path | None = _PROJECT_CONFIG_FILE,
        project_user_file: Path | None = _PROJECT_USER_CONFIG_FILE,
        user_file: Path | None = _USER_CONFIG_FILE,
        environment_var: str | None = _CONFIG_ENV_VAR,
    ) -> None:
        self._config: dict[str, Any] = {}

        _ensure_project_root_env()
        default_cfg: dict[str, Any] = {
            "config_title": _CONFIG_SECTION_TITLE,
            "options": {},
            "devices": {},
        }
        if config_file is not None:
            self.config_init(
                config_section_title=_CONFIG_SECTION_TITLE,
                default_config=default_cfg,
                skip_files_without_sections=False,
            )
            self._load_explicit_config_file(config_file)
            return

        try:
            self.config_init(
                config_section_title=_CONFIG_SECTION_TITLE,
                project_file=project_file,
                project_user_file=project_user_file,
                user_file=user_file,
                default_config=default_cfg,
                environment_var=None,
                skip_files_without_sections=False,
            )
        except (FileNotFoundError, OSError, RuntimeError) as err:
            _LOG.warning("Failed to load pw_daq configuration: %s", err)

        if environment_var and (env_cfg := os.environ.get(environment_var)):
            self.reset_config()
            self._load_explicit_config_file(
                env_cfg, stage=Stage.ENVIRONMENT_VAR_FILE
            )

    @property
    def options(self) -> dict[str, Any]:
        """Returns the configuration options mapping."""
        opts = self._config.setdefault("options", {})
        return opts

    @options.setter
    def options(self, value: Mapping[str, Any]) -> None:
        """Sets the configuration options mapping."""
        self._config["options"] = dict(value)

    def load_config_file(
        self,
        file_path: Path,
        skip_files_without_sections: bool = False,
        stage: Stage = Stage.OUT_OF_BAND,
    ) -> None:
        """Loads a YAML config file, raising DaqError on explicit overrides."""
        is_optional = stage in _OPTIONAL_DEFAULT_STAGES
        if not file_path.is_file():
            if is_optional:
                _LOG.info(
                    "Configuration file not found, skipping: %s", file_path
                )
                return
            raise DaqError(f"Configuration file not found: {file_path}")

        try:
            contents = file_path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as err:
            if is_optional:
                _LOG.warning(
                    "Failed to load configuration file '%s': %s",
                    file_path,
                    err,
                )
                return
            raise DaqError(
                f"Failed to load configuration file '{file_path}': {err}"
            ) from err

        if not contents.strip():
            if is_optional:
                _LOG.warning("Configuration file is empty: %s", file_path)
                return
            raise DaqError(f"Configuration file is empty: {file_path}")

        saved_config = copy.deepcopy(self._config)
        try:
            cfgs = self._load_config_from_string(contents)
            if not cfgs or all(doc is None for doc in cfgs):
                raise DaqError(f"Configuration file is empty: {file_path}")
            super().load_config_file(
                file_path,
                skip_files_without_sections=skip_files_without_sections,
                stage=stage,
            )
        except (
            OSError,
            UnicodeDecodeError,
            yaml.YAMLError,
            MissingConfigTitle,
            AttributeError,
            TypeError,
            DaqError,
        ) as err:
            self._config = saved_config
            if is_optional:
                _LOG.warning(
                    "Failed to load configuration file '%s': %s",
                    file_path,
                    err,
                )
                return
            if isinstance(err, DaqError):
                raise
            raise DaqError(
                f"Failed to load configuration file '{file_path}': {err}"
            ) from err

    def _load_explicit_config_file(
        self,
        file_path: Path | str,
        stage: Stage = Stage.OUT_OF_BAND,
    ) -> None:
        raw_path = str(file_path)
        if not raw_path.strip():
            raise DaqError("Configuration file path cannot be empty.")

        try:
            expanded = Path(raw_path).expanduser()
        except RuntimeError as err:
            raise DaqError(
                f"Failed to expand configuration file path '{raw_path}': {err}"
            ) from err
        resolved = Path(os.path.expandvars(str(expanded)))
        if not resolved.is_absolute() and not resolved.is_file():
            if build_wd := os.environ.get("BUILD_WORKING_DIRECTORY"):
                candidate = Path(build_wd) / resolved
                if candidate.is_file():
                    resolved = candidate

        if not resolved.is_file():
            raise DaqError(f"Configuration file not found: {resolved}")

        self.load_config_file(
            resolved,
            skip_files_without_sections=False,
            stage=stage,
        )

    @property
    def devices(self) -> dict[str, Any]:
        """Returns the merged mapping of configured and registered devices."""
        d = self._config.setdefault("devices", {})
        return d

    @devices.setter
    def devices(self, value: Mapping[str, Any]) -> None:
        """Sets the devices configuration mapping."""
        self._config["devices"] = dict(value)

    @property
    def config(self) -> dict[str, Any]:
        """Returns the full merged configuration dictionary."""
        return self._config

    def reset_config(self) -> None:
        self._config = copy.deepcopy(self.default_config)

    def get_device_config(
        self,
        device_name: str,
        registry: BackendRegistry | None = None,
    ) -> dict[str, Any]:
        """Returns the configuration for `device_name`, validating it exists."""
        device: dict[str, Any] | None = self.devices.get(device_name)
        if device is None:
            raise DaqError(
                f"Missing device configuration for selected device: "
                f"{device_name}"
            )
        backend_name: str | None = device.get("backend")
        active_registry = (
            registry if registry is not None else BackendRegistry()
        )
        if (
            not isinstance(backend_name, str)
            or backend_name not in active_registry
        ):
            available = ", ".join(active_registry.backend_names()) or "none"
            raise DaqError(
                f"Unknown DAQ backend '{backend_name}' configured for "
                f"device '{device_name}'. Available backends: {available}"
            )
        return device

    def validate(self, registry: BackendRegistry | None = None) -> None:
        """Validates this configuration against the pw_daq JSON schema."""
        try:
            jsonschema.validate(
                instance=dict(self.config), schema=_CONFIG_SCHEMA
            )
        except jsonschema.exceptions.ValidationError as err:
            raise DaqError(
                f"Invalid pw_daq configuration: {err.message}"
            ) from err

        # Verify that if "default_device" is specified, it is also configured
        default_device: str | None = self.options.get("default_device")
        if default_device is not None:
            self.get_device_config(default_device, registry=registry)

    def handle_overloaded_value(
        self,
        key: str,
        stage: Stage,
        original_value: Any,
        overriding_value: Any,
    ) -> Any:
        del key, stage  # Unused
        if isinstance(original_value, (Mapping, list)) and (
            overriding_value is None
        ):
            return copy.deepcopy(original_value)
        if isinstance(original_value, Mapping) and isinstance(
            overriding_value, Mapping
        ):
            # Deep merge dictionaries (merge sub-dictionaries)
            return _deep_merge(original_value, overriding_value)
        if isinstance(original_value, list) and isinstance(
            overriding_value, list
        ):
            # Merge lists
            return _deep_merge(original_value, overriding_value)

        # Otherwise, always prefer the newer value
        return overriding_value
