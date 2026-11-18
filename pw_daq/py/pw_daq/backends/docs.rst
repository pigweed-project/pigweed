.. _module-pw_daq-backends:

===============
pw_daq backends
===============
.. pigweed-module-subpage::
   :name: pw_daq

Backends are implementations which support a family of DAQ devices. They
implement the abstract backend interface necessary to provide the functionality
of ``pw_daq``.

--------------
Backend design
--------------
Each ``pw_daq`` backend translates abstract DAQ operations—creating and
destroying blocks, getting and setting block attributes, connecting signals to
actions, triggering actions, streaming events, and managing event logs—into
device-specific transport and protocol operations.

Capability support & resource mapping
=====================================
Backends receive device-specific :ref:`capabilities
<module-pw_daq-design-capabilities>` as part of the device configuration
dictionary passed to ``configure()``, rather than hardcoding board layouts or
resource assignments in the backend driver.

While the exact schema within ``capabilities`` is backend-specific, every
backend's ``capabilities`` definition should focus on:

* Which virtual DAQ blocks the device supports.
* Which real hardware resources exist on the concrete DAQ device.
* How virtual DAQ blocks and virtual resources (such as virtual pins like
  ``p0``) map to real hardware resources.
* Any device-specific Backend Defined (``BED``) configuration values or
  constraints.

Backends use these mappings to instantiate and control blocks on the physical
device and to prevent conflicting blocks from claiming the same underlying
hardware resources simultaneously.

.. note::

   Not all backends will support every capability of the ``daq-cli`` interface.
   In cases where the backend does not support a command, the process will
   return ``1`` (error) and print details to stderr.

Configuration and registration
==============================
Backends may define their own configuration fields in a device config. The
selected device's configuration dictionary (including ``capabilities`` and, when
present, ``dev`` connection settings) is passed to the backend via the
``configure`` API.

* **Device configuration (YAML)**: Devices are configured declaratively via
  ``$PW_PROJECT_ROOT/.pw_daq.yaml``, ``$PW_PROJECT_ROOT/.pw_daq.user.yaml``,
  ``~/.pw_daq.yaml``, ``PW_DAQ_CONFIG``, or ``-c`` / ``--config-file`` (see
  :ref:`module-pw_daq-daq-cli-configuration`), referencing a registered backend
  by name (for example, ``backend: dhalsim``).
* **Built-in backends**: Upstream backends are pre-registered in
  ``BackendRegistry`` and imported lazily when the selected device requests
  them.
* **Custom wrapper binaries (Bazel)**: Downstream projects can register custom
  ``DaqBackend`` subclasses or factories in Python by passing
  ``extra_backends`` (or a custom ``BackendRegistry``) to ``pw_daq.cli.main()``
  or ``pw_daq.cli.run_cli()`` in their own ``pw_py_binary`` target:

  .. code-block:: python

     import sys
     from my_package.my_backend import MyBackend
     from pw_daq import cli

     if __name__ == "__main__":
         sys.exit(cli.main(extra_backends={"my_backend": MyBackend}))

* **Python package entry points**: Installed Python distributions (via ``pip``
  or GN virtual environments) can advertise backends under the
  ``pw_daq.backends`` entry-point group, which ``BackendRegistry`` discovers
  automatically and loads lazily on demand:

  .. code-block:: ini

     [options.entry_points]
     pw_daq.backends =
         my_backend = my_package.my_backend:MyBackend

-----------
Backend API
-----------
Backends subclass ``pw_daq.backend.DaqBackend`` and raise
``pw_daq.common.DaqError`` on configuration, validation, or transport errors.

Lifecycle methods
=================
* ``name() -> str``: Returns the name of the backend.
* ``configure(config: Mapping[str, Any] | None = None) -> None``: Configures
  the backend instance with the selected device's configuration mapping
  (including inline ``capabilities``) before a command is executed. ``Daq``
  requires a pre-configured ``DaqBackend`` instance, so ``configure()`` must be
  called on the backend before passing it to ``Daq(backend)``.
* ``open() -> None``: Optional lifecycle hook to open and initialize any
  underlying transport or hardware resources. ``DaqBackend`` also implements
  the context manager protocol (``__enter__`` and ``__exit__``), which invokes
  ``open()`` on entry and ``close()`` on exit.
* ``close() -> None``: Optional lifecycle hook to release any underlying
  transport or hardware resources.

Command methods
===============
Each ``daq-cli`` command corresponds to an abstract method on ``DaqBackend``.
If a backend does not support a command, it should raise ``DaqError``:

* ``create(block_ref: BlockRef, **attributes: Any) -> BlockRef``:
  Creates and configures ``block_ref`` using the supplied block attributes as
  keyword arguments, returning its ``BlockRef``.
* ``list() -> Iterable[BlockRef]``: Returns all currently created blocks on the
  DAQ device.
* ``destroy(block_ref: BlockRef) -> None``: Tears down the specified block
  on the DAQ device.
* ``set(attribute_ref: AttributeRef, value: Value) -> None``: Sets an
  attribute on the specified block.
* ``get(attribute_ref: AttributeRef) -> Value``: Gets the current
  value of an attribute from the specified block.
* ``on(signal_ref: SignalRef, action_ref: ActionRef, action_arg: Value = None) -> None``:
  Registers a connection or handler so that ``action_ref`` is invoked whenever
  ``signal_ref`` emits an event.
* ``listen(signal_ref: SignalRef, count: int | None = None, timeout: float | None = None) -> EventStream[EventRef[Any]]``:
  Returns a context-manageable ``EventStream[EventRef]`` yielding new
  ``EventRef`` instances from ``signal_ref`` as they occur, stopping after
  ``count`` events or ``timeout`` seconds if specified.
* ``do(action_ref: ActionRef, action_arg: Value = None) -> ActionResult``:
  Immediately invokes the specified Action endpoint with ``action_arg`` and
  returns an ``ActionResult`` indicating success or failure along with any
  optional return value.
* ``dump() -> Iterable[EventRef[Any]]``: Returns all logged ``EventRef``
  instances from the DAQ device.
* ``clear() -> None``: Clears all ``EventRef`` logs from the DAQ device.
* ``info() -> Mapping[str, Any]``: Returns information about the connected DAQ
  device or backend, including log usage and capacity if available.
* ``capabilities() -> Mapping[str, Any]``: Returns the configured capabilities
  mapping of the DAQ device (describing supported virtual blocks, hardware
  resources, virtual-to-real mappings, and ``BED`` options). This method must
  not require a connection to the DAQ device (``open()``), though it may require
  the backend to be configured (``configure()``). In other words,
  ``capabilities()`` can be called after ``configure()`` and before ``open()``.
* ``reset() -> None``: Resets the DAQ device configuration to a power-on state.

Data models
===========
* ``BlockRef``: Minimal reference identifying a block type (``BlockType``) and
  non-negative peripheral index (e.g. ``digital0``, ``uart1``).
* ``AttributeRef``: Minimal reference identifying a block and attribute name
  (e.g. ``digital1.pull``, ``uart0.baudrate``).
* ``SignalRef``: Minimal reference identifying a block and signal name (e.g.
  ``digital1.change``).
* ``ActionRef``: Minimal reference identifying a block and action name (e.g.
  ``uart0.tx``).
* ``ActionResult``: Represents the result of invoking an ``ActionRef`` with
  ``success`` (``bool``), optional ``value``, and ``value_type``
  (``ValueType``).
* ``EventRef``: Minimal reference representing an emitted signal event with
  ``timestamp_ns`` (nanoseconds since the DAQ device's epoch), ``endpoint``
  (``SignalRef``), ``value``, and ``value_type`` (``ValueType``).
* ``EventStream``: Context-manageable ``Iterator`` (e.g.
  ``EventStream[EventRef]``) returned by ``listen()`` that closes the
  underlying stream and releases any resources on ``close()`` or when exiting a
  ``with`` block.
* ``DaqTimeoutError``: Subclass of ``DaqError`` raised when an operation or
  event wait times out.

------------------
Supported backends
------------------
* :ref:`module-pw_daq-backend-dhalsim`
* :ref:`module-pw_daq-backend-mock`

.. toctree::
   :hidden:
   :maxdepth: 1

   dhalsim_daq/docs
   mock_daq/docs
