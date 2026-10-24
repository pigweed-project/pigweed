.. _module-pw_daq-backends:

===============
pw_daq backends
===============
Backends are implementations which support a family of DAQ devices. They
implement the abstract backend interface necessary to provide the functionality
of ``pw_daq``.

--------------
Backend design
--------------
Each ``pw_daq`` backend translates abstract DAQ operations—creating and
destroying blocks, getting and setting block attributes, connecting events to
actions, triggering actions, streaming events, and managing event logs—into
device-specific transport and protocol operations.

Flags and environment variables
===============================
Backends may add their own top level flags and environment variables, but flags
must be prefixed with the backend name, e.g. ``--dhalsim-flag``, and
environment variables must be prefixed with the backend module name, e.g.
``PW_DAQ_DHALSIM_VARIABLE``.

.. warning::

   Flags will override environment variables.

Capability support
==================
.. note::

   Not all backends will support every capability of the ``daq-cli`` interface.
   In cases where the backend does not support a command, the process will
   return ``1`` (error) and print details to stderr.

Registration and discovery
==========================
Backends are managed through the ``pw_daq.backend.backend_registry`` plugin
registry:

* **Built-in backends**: Built-in backends (such as ``dhalsim`` and ``mock``)
  are registered automatically when ``pw_daq`` initializes.
* **JSON configuration files**: Custom backends can be registered via a JSON
  configuration file passed with ``-c`` / ``--config`` or in ``pigweed.json``
  under ``pw.pw_daq.backends``:

  .. code-block:: json

     {
       "pw": {
         "pw_daq": {
           "backends": {
             "my_backend": {
               "module": "my_package.my_backend",
               "function": "MyBackend"
             }
           }
         }
       }
     }

* **Plugin registry file**: A ``PW_DAQ_PLUGINS`` file in ``PW_PROJECT_ROOT``
  can register backends using standard ``pw_cli.plugins`` syntax
  (``<name> <module> <target>``).
* **Programmatic registration**: Python code can register backends directly
  via ``register_backend()`` or ``register_backend_by_name()``.

-----------
Backend API
-----------
Backends subclass ``pw_daq.backend.DaqBackend`` and raise
``pw_daq.backend.DaqError`` on configuration, validation, or transport errors.

Lifecycle methods
=================
* ``add_arguments(parser: argparse.ArgumentParser) -> None``: Registers
  backend-specific CLI arguments on the top-level parser. Every flag registered
  by a backend must start with ``--<backend>-``.
* ``configure(args: argparse.Namespace) -> None``: Configures the backend
  instance using the parsed CLI arguments and environment variables before a
  command is executed.
* ``close() -> None``: Releases any underlying transport or hardware resources.
  ``DaqBackend`` also implements the context manager protocol (``__enter__``
  and ``__exit__``), which invokes ``close()`` on exit.

Command methods
===============
Each ``daq-cli`` command corresponds to an abstract method on ``DaqBackend``.
If a backend does not support a command, it should raise ``DaqError``:

* ``create(block_ref: BlockRef, *, force: bool = False, **attributes: Any) -> None``:
  Creates and configures ``block_ref`` using the supplied block attributes as
  keyword arguments.
* ``list() -> Iterable[BlockRef]``: Returns all currently created blocks on the
  DAQ device.
* ``destroy(block_ref: BlockRef) -> None``: Tears down the specified block on
  the DAQ device.
* ``set(block_ref: BlockRef, attribute: str, value: Any) -> None``: Sets an
  attribute on the specified block.
* ``get(block_ref: BlockRef, attribute: str) -> Any``: Gets the current value
  of an attribute from the specified block.
* ``on(event_ref: EndpointRef, action_ref: EndpointRef, action_arg: EventValue = None) -> None``:
  Registers a connection or handler so that ``action_ref`` is invoked whenever
  ``event_ref`` triggers.
* ``listen(event_ref: EndpointRef, count: int | None = None) -> Iterator[Event]``:
  Yields new ``Event`` instances from ``event_ref`` as they occur.
* ``do(action_ref: EndpointRef, action_arg: EventValue = None) -> EventValue``:
  Immediately invokes the specified Action endpoint with ``action_arg`` and
  returns any result.
* ``dump() -> Iterable[Event]``: Returns all logged ``Event`` instances from
  the DAQ device.
* ``clear() -> None``: Clears all ``Event`` logs from the DAQ device.
* ``usage() -> LogUsage | Mapping[str, Any]``: Returns ``Event`` log usage and
  capacity statistics.
* ``info() -> Mapping[str, Any]``: Returns information about the connected DAQ
  device or backend.
* ``capabilities() -> Mapping[str, Any]``: Returns the capabilities of the DAQ
  device.
* ``reset() -> None``: Resets the DAQ device configuration to a power-on state.

Data models
===========
* ``BlockRef``: Identifies a block type (``BlockType``) and non-negative
  peripheral index (e.g. ``digital0``, ``uart1``).
* ``AttributeRef``: Identifies a block and attribute name (e.g.
  ``digital1.pull``, ``uart0.baudrate``) and provides attribute specifications
  (``AttributeSpec``).
* ``EndpointRef``: Identifies a block and endpoint name (e.g.
  ``digital1.change``, ``uart0.tx``) and provides endpoint type specifications
  (``EndpointSpec``).
* ``Event``: Represents an event occurrence with ``timestamp`` (nanoseconds
  since the DAQ device's epoch), ``endpoint`` (``EndpointRef``), ``value``, and
  ``value_type`` (``ValueType``).
* ``LogUsage``: Represents event log usage (``used``) and total ``capacity``.

------------------
Supported backends
------------------
* :ref:`module-pw_daq-backend-dhalsim`
* :ref:`module-pw_daq-backend-mock`

.. toctree::
   :hidden:
   :maxdepth: 1

   dhalsim/docs
   mock/docs
