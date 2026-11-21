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
"""Device and backend plugin registry and factory instantiation."""

from __future__ import annotations

from collections.abc import Callable, Iterator, Mapping
import importlib.metadata
import logging
from pathlib import Path
from typing import Any

from pw_cli import plugins
from pw_daq.backend import DaqBackend
from pw_daq.common import DaqError

_LOG = logging.getLogger(__name__)

ENTRY_POINT_GROUP = "pw_daq.backends"

BackendTarget = type[DaqBackend] | Callable[..., DaqBackend]


def _builtin_backends(
    package: str | None = __package__,
) -> Mapping[str, Mapping[str, str]]:
    """Builds built-in backend definitions relative to the enclosing package."""
    pkg = package or "pw_daq"
    return {
        # Temporarily pre-register 'mock' until the 'dhalsim' backend is
        # available.
        "mock": {
            "module": f"{pkg}.backends.mock_daq.mock_backend",
            "class": "MockBackend",
        },
    }


BUILTIN_BACKENDS: Mapping[str, Mapping[str, str]] = _builtin_backends()


def _discover_entry_points(
    group: str = ENTRY_POINT_GROUP,
) -> dict[str, importlib.metadata.EntryPoint]:
    """Discovers installed DAQ backend entry points without importing them."""
    discovered: dict[str, importlib.metadata.EntryPoint] = {}
    for ep in importlib.metadata.entry_points(group=group):
        key = ep.name.lower()
        if key in discovered:
            _LOG.debug(
                "Entry point '%s' (%s) ignored; '%s' already discovered (%s).",
                ep.name,
                ep.value,
                key,
                discovered[key].value,
            )
            continue
        discovered[key] = ep
    return discovered


def _validate_backend_plugin(plugin: plugins.Plugin) -> None:
    """Validates that a registered backend plugin target is callable."""
    if not callable(plugin.target):
        raise plugins.Error(
            "DAQ backend plugin must be a DaqBackend class or factory "
            f"callable, but {plugin.target_name} is a "
            f"{type(plugin.target).__name__}"
        )
    if isinstance(plugin.target, type) and not issubclass(
        plugin.target, DaqBackend
    ):
        raise plugins.Error(
            "DAQ backend plugin class must be a DaqBackend subclass, "
            f"but {plugin.target_name} is not."
        )


class BackendRegistry(plugins.Registry):
    """Plugin registry that normalizes all backend names to lowercase."""

    def __init__(
        self,
        *,
        register_builtins: bool = True,
        discover_entry_points: bool = True,
        extra_backends: Mapping[str, BackendTarget] | None = None,
    ) -> None:
        super().__init__(validator=_validate_backend_plugin)
        self._builtins: dict[str, dict[str, str]] = (
            {k.lower(): dict(v) for k, v in BUILTIN_BACKENDS.items()}
            if register_builtins
            else {}
        )
        self._entry_points: dict[str, importlib.metadata.EntryPoint] = {}
        if discover_entry_points:
            for name, ep in _discover_entry_points().items():
                if name not in self._builtins:
                    self._entry_points[name] = ep
        if extra_backends is not None:
            for name, target in extra_backends.items():
                self.register(name, target)

    def _has_registered(self, key: str) -> bool:
        return any(existing == key for existing in super().__iter__())

    def _materialize(self, key: str) -> None:
        if self._has_registered(key):
            return
        if key in self._builtins:
            cfg = self._builtins[key]
            try:
                super().register_by_name(key, cfg["module"], cfg["class"], None)
            except plugins.Error as err:
                raise DaqError(
                    f"Failed to register DAQ backend '{key}': {err}"
                ) from err
            self._builtins.pop(key, None)
            return
        if key in self._entry_points:
            ep = self._entry_points[key]
            try:
                target = ep.load()
            except Exception as err:
                raise DaqError(
                    f"Failed to load DAQ backend entry point '{key}' "
                    f"({ep.value}): {err}"
                ) from err
            ep_source = Path(f"<entry_point:{ep.value}>")
            try:
                super()._register(plugins.Plugin(key, target, ep_source))
            except plugins.Error as err:
                raise DaqError(
                    f"Failed to register DAQ backend '{key}': {err}"
                ) from err
            self._entry_points.pop(key, None)

    def __getitem__(self, name: str) -> plugins.Plugin:
        key = name.lower()
        self._materialize(key)
        return super().__getitem__(key)

    def __contains__(self, name: object) -> bool:
        if not isinstance(name, str):
            return False
        key = name.lower()
        return (
            self._has_registered(key)
            or key in self._builtins
            or key in self._entry_points
        )

    def __iter__(self) -> Iterator[str]:
        seen: set[str] = set()
        for name in super().__iter__():
            seen.add(name)
            yield name
        for name in self._builtins:
            if name not in seen:
                seen.add(name)
                yield name
        for name in self._entry_points:
            if name not in seen:
                seen.add(name)
                yield name

    def __len__(self) -> int:
        return len(
            set(super().__iter__())
            | set(self._builtins)
            | set(self._entry_points)
        )

    def register(self, name: str, target: Any) -> plugins.Plugin | None:
        key = name.lower()
        if key in self._builtins:
            raise DaqError(
                f"Failed to register DAQ backend '{name}': "
                f"built-in backend '{key}' is already registered."
            )
        try:
            plugin = super().register(key, target)
        except plugins.Error as err:
            raise DaqError(
                f"Failed to register DAQ backend '{name}': {err}"
            ) from err
        if plugin is not None:
            self._entry_points.pop(key, None)
        return plugin

    def register_by_name(
        self,
        name: str,
        module_name: str,
        member_name: str,
        source: Path | None = None,
    ) -> plugins.Plugin | None:
        key = name.lower()
        if source is None and key in self._builtins:
            raise DaqError(
                f"Failed to register DAQ backend '{name}': "
                f"built-in backend '{key}' is already registered."
            )
        try:
            plugin = super().register_by_name(
                key, module_name, member_name, source
            )
        except plugins.Error as err:
            raise DaqError(
                f"Failed to register DAQ backend '{name}': {err}"
            ) from err
        if plugin is not None:
            self._builtins.pop(key, None)
            self._entry_points.pop(key, None)
        return plugin

    def _register(self, plugin: plugins.Plugin) -> plugins.Plugin | None:
        plugin.name = plugin.name.lower()
        return super()._register(plugin)

    def backend_names(self) -> Iterator[str]:
        return iter(self)

    def create_backend(
        self, name: str, config: Mapping[str, Any], **kwargs: Any
    ) -> DaqBackend:
        """Instantiates a registered DAQ backend by name."""
        key = name.lower()
        if key not in self:
            available = ", ".join(self.backend_names()) or "none"
            raise DaqError(
                f"Unknown DAQ backend '{name}'. Available backends: {available}"
            )
        instance = self[key].target(**kwargs)
        if not isinstance(instance, DaqBackend):
            raise DaqError(
                f"Backend '{name}' factory returned "
                f"{type(instance).__name__}, expected a DaqBackend instance."
            )
        instance.configure(config)
        return instance
