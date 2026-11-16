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
"""Abstract DAQ backend interface."""

from __future__ import annotations

import abc
from collections.abc import Iterable, Mapping
from types import TracebackType
from typing import Any

from pw_daq.common import (
    ActionRef,
    ActionResult,
    AttributeRef,
    BlockRef,
    EventRef,
    EventStream,
    SignalRef,
    Value,
)


class DaqBackend(abc.ABC):
    """Abstract base class for DAQ hardware backends."""

    @abc.abstractmethod
    def name(self) -> str:
        """Returns the name of the backend."""

    def configure(
        self,
        config: Mapping[str, Any] | None = None,
    ) -> None:
        """Configures the backend with the selected device configuration.

        Backends that require device configuration must be configured before
        being passed to `pw_daq.Daq` or opened via `open()`.
        """

    def open(self) -> None:
        """Opens and initializes underlying transport or hardware resources."""

    def close(self) -> None:
        """Releases any underlying transport or hardware resources."""

    @abc.abstractmethod
    def create(
        self,
        block_ref: BlockRef,
        **attributes: Any,
    ) -> BlockRef:
        """Creates a block (e.g. 'uart0') and returns its BlockRef."""

    @abc.abstractmethod
    def list(self) -> Iterable[BlockRef]:
        """Lists all currently created blocks on the DAQ device."""

    @abc.abstractmethod
    def destroy(self, block_ref: BlockRef) -> None:
        """Tears down the specified block on the DAQ device."""

    @abc.abstractmethod
    def set(
        self,
        attribute_ref: AttributeRef,
        value: Value,
    ) -> None:
        """Sets an attribute on the specified block."""

    @abc.abstractmethod
    def get(
        self,
        attribute_ref: AttributeRef,
    ) -> Value:
        """Gets the current value of an attribute from the specified block."""

    @abc.abstractmethod
    def on(
        self,
        signal_ref: SignalRef,
        action_ref: ActionRef,
        action_arg: Value = None,
    ) -> None:
        """Registers an action to be invoked when a signal emits an event."""

    @abc.abstractmethod
    def listen(
        self,
        signal_ref: SignalRef,
        count: int | None = None,
        timeout: float | None = None,
    ) -> EventStream[EventRef[Any]]:
        """Streams new EventRef instances from the specified SignalRef."""

    @abc.abstractmethod
    def do(
        self,
        action_ref: ActionRef,
        action_arg: Value = None,
    ) -> ActionResult:
        """Invokes the Action endpoint and returns its ActionResult."""

    @abc.abstractmethod
    def dump(self) -> Iterable[EventRef[Any]]:
        """Returns all logged EventRef instances from the DAQ device."""

    @abc.abstractmethod
    def clear(self) -> None:
        """Clears all EventRef logs from the DAQ device."""

    @abc.abstractmethod
    def info(self) -> Mapping[str, Any]:
        """Returns information about the connected DAQ device or backend."""

    @abc.abstractmethod
    def capabilities(self) -> Mapping[str, Any]:
        """Returns capabilities of the DAQ device.

        Must not require an active connection to the DAQ device (`open()`),
        though it may require the backend to be configured (`configure()`).
        """

    @abc.abstractmethod
    def reset(self) -> None:
        """Resets the DAQ device configuration to a power-on state."""

    def __enter__(self) -> DaqBackend:
        self.open()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None:
        self.close()
