.. _module-pw_bluetooth_proxy-crash_recovery:
.. _module-pw_bluetooth_proxy-recovery:

================
Crash recovery
================
This guide details how a platform container integrates ``pw_bluetooth_proxy``
crash recovery to restore proxy state across co-processor restarts without
resetting the Bluetooth Controller or dropping active connections.

Container responsibilities are modular: containers only configure, persist
state for, and restore the specific subsystems used by their deployment.

.. _module-pw_bluetooth_proxy-crash_recovery-configuration:

-------------
Configuration
-------------
Crash recovery features are configured at compile time via the options in
:cs:`pw_bluetooth_proxy/public/pw_bluetooth_proxy/config.h`.

.. list-table::
   :header-rows: 1

   * - ``PW_BLUETOOTH_PROXY_CONFIG_*``
     - Default
     - Description
   * - ``ENABLE_RECOVERY``
     - ``0``
     - Enables snapshot persistence, state restoration, and state update
       callbacks across proxy subsystems. When set to ``0``, all recovery
       code is eliminated at compile time.
   * - ``ENABLE_CREDIT_SNAPSHOT_UPDATES``
     - ``1``
     - Controls whether proxy subsystems emit state update callbacks for
       dynamic flow control credit changes across HCI commands, ACL transport,
       L2CAP CoC, and RFCOMM. Setting to ``0`` reduces CPU and IPC overhead
       during high-throughput streaming by persisting only connection and
       channel lifecycle events, with credits resetting to default initial
       values upon recovery.
   * - ``MAX_SNAPSHOT_CONNECTIONS``
     - ``10``
     - Maximum number of concurrent ACL and Sniff connections that can be
       tracked in persistent storage.
   * - ``MAX_SNAPSHOT_L2CAP_CHANNELS``
     - ``16``
     - Maximum number of concurrent L2CAP channels that can be tracked in
       persistent storage.
   * - ``MAX_SNAPSHOT_RFCOMM_CHANNELS``
     - ``8``
     - Maximum number of concurrent RFCOMM channels that can be tracked in
       persistent storage.

Capacity limits and Subsystem Restarts (SSR)
============================================
If any active subsystem exceeds its configured capacity during snapshot capture,
that subsystem sets its snapshot record's ``snapshot_incomplete`` field to
``true``. During subsequent restoration, the corresponding recovery methods
(``ProxyHost::RecoverFromSnapshot``,
``hci::SniffOffloadManager::RecoverFromSnapshot``, or
``rfcomm::RfcommManager::RecoverFromSnapshot``) detect
this flag and synchronously return ``pw::Status::DataLoss()``.

When the Platform Container receives ``pw::Status::DataLoss()`` from any
subsystem, it is contractually required to abort crash recovery and trigger a
full Subsystem Restart (SSR) to reset the Bluetooth controller and host stack
cleanly.

-----------------------
Container orchestration
-----------------------
When the co-processor reboots, the Platform Container orchestrates state
restoration across the active subsystems using a deterministic 7-step sequence
before returning the proxy to steady-state operation. Subsystems not used in the
deployment are omitted from each step.

.. list-table::
   :header-rows: 1

   * - Step
     - Phase
     - Primary action
   * - 1
     - Pause Traffic
     - Pause hardware transport traffic and host packet dispatch.
   * - 2
     - Reconstruct Subsystems
     - Create fresh subsystem instances with their callbacks.
   * - 3
     - RecoverFromSnapshot
     - Restore saved snapshots into each active subsystem.
   * - 4
     - Re-Registration Window
     - Re-apply event filters and re-open active channels.
   * - 5
     - CompleteRecovery
     - Close unrecovered channels and notify clients.
   * - 6
     - Resume Traffic
     - Resume hardware transport traffic and host packet dispatch.
   * - 7
     - Post-Resumption Dispatch
     - Send credit refunds to host and resync hardware state.

Step 1: Pause traffic
=====================
Upon detecting a co-processor reboot, the Platform Container must immediately
pause all inbound and outbound transport traffic (e.g., UART Tx/Rx) and suspend
Host application packet dispatch. No packets or events may enter the proxy
until Step 6.

Step 2: Subsystem reconstruction
================================
The Container instantiates fresh instances of the active subsystem managers
required by the application using the constructor-injected delegate pattern:

* ``ProxyHost``: Mandatory core proxy (internally
  instantiates ``AclDataChannel`` and ``L2capChannelManager``).
* ``hci::CommandMultiplexer``: Instantiated only if the
  platform multiplexes local HCI commands.
* ``hci::SniffOffloadManager``: Instantiated only if
  offloading link-layer Sniff mode transitions.
* ``rfcomm::RfcommManager``: Instantiated only if
  multiplexing RFCOMM channels over L2CAP.

State update callbacks passed to constructors must remain valid for the lifetime
of the subsystems.

Step 3: RecoverFromSnapshot
===========================
For each active subsystem, the Container invokes ``RecoverFromSnapshot()``
respecting relative bottom-up dependency order:

1. **CommandMultiplexer (if used):**
   ``hci::CommandMultiplexer::RecoverFromSnapshot``
   restores controller command credit tracking.
2. **ProxyHost (core):**
   ``ProxyHost::RecoverFromSnapshot``
   restores connection records and transport state, preparing channels for
   re-acquisition in Step 4. (HCI event filters are not restored
   automatically and must be re-applied by clients in Step 4).
3. **SniffOffloadManager (if used):**
   ``hci::SniffOffloadManager::RecoverFromSnapshot``
   restores connection tracking for offloaded links, baselining them to
   active mode (hardware commands are deferred until Step 7).
4. **RfcommManager (if used):**
   ``rfcomm::RfcommManager::RecoverFromSnapshot``
   restores RFCOMM session state, preparing channels for re-acquisition in
   Step 4.

If an optional subsystem is not used in the deployment, its corresponding step
is simply omitted. However, the relative ordering between present subsystems
must always be preserved (for example, ``ProxyHost`` must always be restored
before ``RfcommManager``).

.. important::
   * **Snapshot Lifetime & Pointer Semantics:**
     ``ProxyHost::RecoverFromSnapshot`` and
     ``rfcomm::RfcommManager::RecoverFromSnapshot`` accept raw pointers to
     snapshot objects (``const ProxyHostSnapshot*`` and
     ``const RfcommSnapshot*``). The caller must ensure that the pointed-to
     snapshot objects remain valid and in scope until Step 5
     (``CompleteRecovery()``) completes.
   * **Decouple Baseline from Live Storage:** The snapshot pointers provided
     to ``RecoverFromSnapshot()`` represent an immutable, read-only pre-crash
     baseline. If the container applies live state updates (via
     ``ProxyHostStateUpdateCallback`` or ``RfcommStateUpdateCallback``) directly
     to persistent storage, it **must not** pass the address of that mutable
     storage directly to ``RecoverFromSnapshot()``. Doing so leads to iterator
     invalidation when sweeping abandoned channels in Step 5, as well as
     baseline snapshot corruption if new channels are acquired in Step 4.
     Instead, provide a local copy of the baseline snapshot that remains frozen
     until Step 5 completes.
   * **Read-Only Invariant:** Subsystems do not emit state update callbacks
     during Step 3.
   * **Error Handling:** If any active subsystem returns
     ``pw::Status::DataLoss()``, the Container must abort recovery and initiate
     a full Subsystem Restart.

Step 4: Re-registration window
==============================
Clients and upper-layer services re-bind their dynamic conduits and re-apply
runtime event filtering for the protocols they utilize:

* **HCI Event Filtering:** Re-apply dynamic filter rules via
  ``ProxyHost::SetEventBlocked`` and
  ``ProxyHost::SetLeSubeventBlocked``. Event filters are
  client-driven and are not automatically re-applied by ``ProxyHost``.
* **L2CAP Channels:** Re-acquire active L2CAP channels via
  ``ProxyHost::AcquireL2capCoc``,
  ``ProxyHost::AcquireBasicL2capChannel``, or
  ``ProxyHost::InterceptBasicL2capChannel``.
* **RFCOMM Channels (if using RfcommManager):** Re-acquire active RFCOMM
  channels via
  ``rfcomm::RfcommManager::AcquireRfcommChannel`` or
  ``rfcomm::RfcommManager::InterceptRfcommChannel``.
* **GATT Services (if using GATT):** Re-create ``Gatt::Client`` and
  ``Gatt::Server`` objects and re-register delegates. (GATT is stateless in the
  proxy and requires no snapshot hydration).

Data loss
---------
If an L2CAP channel experienced data loss during co-processor downtime:

* **Loss-tolerant protocols** (``allow_data_loss = true``, such as RFCOMM):
  Channel acquisition succeeds, leaving it to upper layers to handle any
  missing data.
* **Loss-sensitive protocols** (``allow_data_loss = false``, the default):
  Channel acquisition synchronously fails with ``pw::Status::Cancelled()``. The
  proxy swallows subsequent traffic on that CID.

When channel acquisition is rejected with ``pw::Status::Cancelled()``, the
client is responsible for initiating a protocol-level channel teardown with the
remote peer (such as sending an L2CAP Disconnection Request) once transport
resumes in Step 6.

.. warning::

   In dynamic credit sharing mode, all data packets are queued in the proxy
   before being forwarded to the controller. If a crash occurs, queued packets
   on **non-offloaded channels** are lost without notifying the host. While
   offloaded channels recognize and handle data loss during Step 4
   re-acquisition, non-offloaded channels have no such mechanism: synthetic
   ``HCI_Number_Of_Completed_Packets`` events dispatched in Step 7 restore host
   credit accounting, but provide no indication that packet payloads were
   dropped.

Step 5: CompleteRecovery
========================
The Container invokes ``CompleteRecovery()`` in top-down dependency order across
active managers to prune channels that were present in snapshots but never
re-acquired in Step 4:

1. **RfcommManager (if used):**
   ``rfcomm::RfcommManager::CompleteRecovery`` sweeps
   unacquired DLCIs, emits an
   ``rfcomm::RfcommChannelRemoved`` callback for each
   abandoned channel, and releases its snapshot reference.
2. **ProxyHost (core):**
   ``ProxyHost::CompleteRecovery`` sweeps unacquired
   L2CAP channels, emits an
   ``L2capChannelRemoved`` callback for each abandoned
   channel, and releases its snapshot reference.

If ``RfcommManager`` is not part of the deployment, the Container only invokes
``ProxyHost::CompleteRecovery()``.

Step 6: Resume traffic
======================
The Container unpauses transport interfaces (UART Tx/Rx) and resumes Host
application packet dispatch. Steady-state traffic resumes through the proxy.

Step 7: Post-resumption dispatch resynchronization
==================================================
Finally, with transport pipelines active and responsive, the Container triggers
deferred resynchronization on active subsystems:

1. **ProxyHost:**
   ``ProxyHost::InitiateAclCreditResynchronization``
   dispatches synthetic ``HCI_Number_Of_Completed_Packets`` events to the host
   to replenish controller credits for host packets that were queued and dropped
   during the reboot.
2. **SniffOffloadManager (if used):**
   ``hci::SniffOffloadManager::InitiateHardwareResynchronization``
   dispatches proactive ``HCI_Exit_Sniff_Mode`` commands to the controller to
   align hardware state to the ``ConnectionMode::kActive`` baseline.

If ``SniffOffloadManager`` is not used, only ACL credit resynchronization is
performed.

.. warning::
   **Re-entrancy & Deadlock Hazard:** Resynchronization calls must **never** be
   executed while traffic pipelines are paused. Emitting synthetic events or
   hardware commands while the transport is suspended will cause pipe buffer
   exhaustion, backpressure deadlocks, and missed completions.

--------------------------------------------------
Container responsibilities: State update callbacks
--------------------------------------------------
The Platform Container is only required to provide persistent storage and
implement state update callbacks for the subsystems included in its deployment.

Callback types and payload variants
===================================
Each subsystem defines its own strongly typed callback and payload variant:

.. list-table::
   :header-rows: 1

   * - Subsystem module
     - Callback signature
     - Payload variants
   * - CommandMultiplexer
     - ``hci::CommandMultiplexerStateUpdateCallback``
     - ``hci::CommandMultiplexerSnapshot``
   * - ProxyHost (ACL)
     - ``ProxyHostStateUpdateCallback``
     - ``AclConnectionSnapshot``,
       ``AclConnectionRemoved``
   * - ProxyHost (L2CAP)
     - ``ProxyHostStateUpdateCallback``
     - ``L2capSignalingStateSnapshot``,
       ``L2capChannelSnapshot``,
       ``L2capChannelRemoved``
   * - SniffOffloadManager
     - ``hci::SniffStateUpdateCallback``
     - ``hci::SniffSnapshot``,
       ``hci::SniffConnectionSnapshot``
   * - RfcommManager
     - ``rfcomm::RfcommStateUpdateCallback``
     - ``rfcomm::RfcommChannelSnapshot``,
       ``rfcomm::RfcommChannelRemoved``

Re-entrancy safety rules
========================
State update callbacks are invoked synchronously from within proxy packet
processing loops while internal mutexes are held.

* **Never call proxy APIs from within a callback.** Attempting to call methods on
  ``ProxyHost``, ``AclDataChannel``, or ``L2capChannelManager`` from inside a
  state update delegate will result in a recursive mutex deadlock.
* **Keep callbacks lightweight.** Copy or move the payload into a lock-free queue
  or notify a background persistence worker thread rather than executing
  blocking I/O or flash writes directly inside the callback.

Available utilities and snapshot helpers
========================================
Each subsystem provides its own standalone snapshot structure with in-place
mutation helpers, allowing the Container to store snapshots in separate memory
regions or together in a unified structure:

* ``ProxyHostSnapshot::ApplyStateUpdate``:
  Takes a ``ProxyHostStateUpdate`` and routes it
  directly to the corresponding ACL or L2CAP snapshot entry.
* ``AclSnapshot::ApplyStateUpdate``:
  Updates an existing ``AclConnectionSnapshot`` or
  erases the entry if an ``AclConnectionRemoved``
  is received.
* ``L2capSnapshot::ApplyStateUpdate``:
  Updates signaling records, updates channel entries, or erases channels upon
  receiving ``L2capChannelRemoved``.
* ``hci::SniffSnapshot::ApplyStateUpdate``:
  Updates or overwrites active sniff connection records.
* ``rfcomm::RfcommSnapshot::ApplyStateUpdate``:
  Updates channel credits/parameters or removes records on
  ``rfcomm::RfcommChannelRemoved``.
* ``hci::CommandMultiplexerSnapshot::ApplyStateUpdate``:
  Updates command credit counts directly from the update payload.
* Individual record methods ``Update()`` and ``MatchesKey()`` allow custom
  searching and in-place updates when maintaining custom persistent layouts.

----------------------
Implementation example
----------------------
The following example demonstrates how a Platform Container wires state update
delegates and executes the 7-step recovery sequence upon reboot.

While this example demonstrates a full-featured system integrating all available
subsystems, applications that only use a subset (such as ``ProxyHost`` alone)
simply omit the unused managers, callbacks, and snapshots from their
implementation.

.. literalinclude:: examples/crash_recovery.cc
   :language: cpp
   :linenos:
   :start-after: [pw_bluetooth_proxy-examples-crash-recovery]
   :end-before: [pw_bluetooth_proxy-examples-crash-recovery]
