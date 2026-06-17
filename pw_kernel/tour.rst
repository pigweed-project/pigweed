.. _module-pw_kernel-tour:

=================
Tour of pw_kernel
=================
.. pigweed-module-subpage::
   :name: pw_kernel

This tour provides a conceptual overview of the software architecture of
embedded systems built on top of ``pw_kernel``. It walks through the
:cs:`adventure <pw_kernel/examples/adventure>` example to keep the discussion
focused and concrete.

-----
Setup
-----
See :ref:`module-pw_kernel-quickstart` if you'd like to run the adventure
example yourself. This tour starts exactly where the quickstart left off.

--------
Overview
--------
.. To update the diagrams:
.. 1. Go to https://excalidraw.com
.. 2. Menu > Open (or Ctrl+o)
.. 3. Select //docs/sphinx/_static/svg/pw_kernel-tour-light.svg
.. 4. Make your edits in Excalidraw
.. 5. Menu > Save to current file (or Ctrl+S)
.. 6. Menu > Export image (or Ctrl+Shift+E), enable the "dark mode"
..    option, make sure that the "embed scene" option is enabled, click
..    the "SVG" button, and then replace
..    //docs/sphinx/_static/svg/pw_kernel-tour-dark.svg
..
.. TODO: b/525058207 - Enable images.
..
.. .. image:: ../_static/svg/pw_kernel-tour-light.svg
..    :class: only-light
..
.. .. image:: ../_static/svg/pw_kernel-tour-dark.svg
..    :class: only-dark

The adventure example is a multi-process system composed of three userspace
applications:

* **Supervisor**: watches other processes and restarts them when they fail.
* **UART driver**: userspace UART driver implementing the server-side of the
  streaming protocol and handles UART interrupts in userspace.
* **Game**: runs game logic and communicates with the UART driver for input/output.

-----------------------------
System generation and startup
-----------------------------
When creating a system on top of ``pw_kernel`` you define the entire system
declaratively in a file like :cs:`system.json5
<:pw_kernel/examples/adventure/targets/riscv_qemu/simulated/system.json5>`.
This includes not only the physical layout:

.. literalinclude:: ./examples/adventure/targets/riscv_qemu/simulated/system.json5
   :language: text
   :start-after: // DOCSTAG: [layout]
   :end-before: // DOCSTAG: [layout]
   :dedent:

But also logical definitions, such as how processes are allowed to communicate
with each other over IPC:

.. literalinclude:: ./examples/adventure/targets/riscv_qemu/hardware/system.json5
   :language: text
   :start-after: // DOCSTAG: [ipc]
   :end-before: // DOCSTAG: [ipc]
   :dedent:

The end-to-end system is built, linked, and optimized into a single image with a Bazel
macro such as ``system_image``.

.. literalinclude:: ./examples/adventure/targets/riscv_qemu/simulated/BUILD.bazel
   :language: text
   :start-after: # DOCSTAG: [sys]
   :end-before: # DOCSTAG: [sys]
   :dedent:

You start up the system by invoking a codegen'd ``start()`` function. See
:cs:`target.rs <:pw_kernel/examples/adventure/targets/riscv_qemu/simulated/target.rs>`
for example. At this point, ``pw_kernel`` constructs and starts the system as
defined in ``system.json5``: allocating kernel-side representations of processes,
setting up IPC channels, defining memory access permissions, etc.

----------
Supervisor
----------
The ``supervisor`` process acts as the system's orchestrator, running in its own
unprivileged process but with special permissions to monitor and control the
lifecycles of the UART driver and game processes. The supervisor uses wait groups
to wait on signals from the other processes.

.. literalinclude:: ./examples/adventure/apps/supervisor/main.rs
   :language: rs
   :start-after: // DOCSTAG: [tour]
   :end-before: // DOCSTAG: [tour]
   :dedent:

-------------
Streaming IPC
-------------
The game process communicates with the UART driver process via an IPC channel.
In ``pw_kernel`` IPC channels are synchronous and asymmetric. Only one side
of the channel (the initiator) can start transactions. The other side
(the handler) listens and acts upon read/write requests from the initiator.
In the adventure example the game process functions as the client using the
initiator side of the IPC channel, and the UART driver is the server using
the handler side of the channel. When the driver receives new data from the
UART hardware, it sets the ``USER`` signal to inform the game process that new
data is ready and therefore it should start a new IPC transaction.

.. literalinclude:: ./examples/adventure/lib/stream_protocol/server.rs
   :language: rs
   :start-after: // DOCSTAG: [tour]
   :end-before: // DOCSTAG: [tour]
   :dedent:
