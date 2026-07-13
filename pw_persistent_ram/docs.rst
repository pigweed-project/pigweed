.. _module-pw_persistent_ram:

=================
pw_persistent_ram
=================
.. pigweed-module::
   :name: pw_persistent_ram

This module provides utilities and containers for storing data (usually crash
dumps in the form of snapshots) into RAM that persists across reboots. For many
common MCUs, RAM is retained during a soft reboot, but this is not guaranteed.
The containers therefore include integrity checks to detect corruption.

.. note::

  Not all hardware supports this; see the :ref:`Hardware Requirements
  <module-pw_persistent_ram-hw_requirements>` section.

------------------------
Persistent RAM Lifecycle
------------------------
Understanding the lifecycle of persistent RAM is crucial for its effective use.
Key considerations include when to initialize, clear, and access the data
stored in persistent RAM. The following diagram illustrates a common pattern
for managing persistent RAM, particularly in scenarios involving crash data
capture and retrieval:

.. raw:: html

   <style>
     #lifecycle > svg { height: 1000px; }
   </style>

.. mermaid::
   :name: lifecycle

   graph TD
       DB[Device Boot]

       DB --> PreMainInit[Pre-Main<br>Initialization];
       PreMainInit --> AppStart[Application Starts];

       %% After application starts, check for crash data from a previous boot
       AppStart --> CheckCrashData{Check Persistent RAM<br>for Crash Data};

       %% Path 1: No crash data found / Leads to normal operation
       CheckCrashData -- No Data --> NormalOp[Normal Device<br>Operation];

       %% Normal Operation can lead to a clean reboot or a crash
       NormalOp -- Clean Reboot --> DB;
       NormalOp -- Crash Event --> CrashHdlr[Crash Handler<br>Executes];

       %% Crash Handling Path (occurs during NormalOp, leads to reboot)
       CrashHdlr --> CaptureToPersistentRAM[Capture Crash Data<br>to Persistent RAM];
       CaptureToPersistentRAM -- Crash Reboot --> DB;

       %% Path 2: Crash data found after a reboot (from CheckCrashData)
       CheckCrashData -- Data Found --> ProcessAndClearCrashData[Log, Transmit, and<br>Clear Crash Data];
       ProcessAndClearCrashData --> NormalOp;

The key aspect of this flow is that the crash exception handler stores crash
data into persistent RAM. Then, after rebooting into a known-good state, the
crash data is read from persistent RAM to be stored or transmitted.

The lifecycle flow above highlights several stages:

- **Device Boot & Pre-Main Initialization**: Standard boot procedures,
  including setting up BSS/data sections and running C++ static constructors.
  Special care must be taken (e.g., via the linker script) to avoid clearing
  persistent RAM at this stage.
- **Application Start**: The main application begins execution.
- **Crash Data Check**: Upon starting, the application checks persistent RAM
  for any crash data logged from a previous, unexpected reboot. If crash data
  is found and its checksum indicates it is not corrupt, product-specific
  logic to transmit or store the data is executed at this point. Finally,
  the persistent RAM is cleared.
- **Normal Operation**: If no crash data is found, or after crash data has
  been processed, the device proceeds with its normal operations. Persistent
  RAM can be used by the application as needed during this phase.
- **Crash Handling**: If a crash occurs during normal operation, a dedicated
  crash handler captures relevant system state and writes it to persistent
  RAM before the system reboots.

The management of persistent RAM is tightly coupled to the device's boot
sequence and application logic.

-----------------------------------
Guidelines for Persistent RAM Usage
-----------------------------------
While Pigweed cannot provide a one-size-fits-all solution for persistent RAM
management due to its hardware-dependent nature, we recommend following these
guidelines:

1. **Only use persistent containers in the persistent RAM region** -
   Arbitrary objects in the persistent RAM section are unsafe since their
   integrity is not guarded by checksums. All types in persistent memory
   regions should be wrapped in persistent RAM containers to detect data loss
   and prevent operations on corrupt data.
2. **Do not use persistent containers as members in other objects** - Due to
   the placement restrictions for persistent RAM containers, it is not
   generally possible to make these containers members of other objects.
   Instead, create them separately and use dependency injection to connect
   other objects like crash handling infrastructure. This also facilitates unit
   testing.
3. **Erase persistent RAM in your update flow** - If persistent RAM
   addresses shift between software versions, there will be a skew between
   the addresses on the current boot (with the old code) and those on the
   post-update boot (with the new code). The shifted addresses can cause
   arbitrary memory from the old boot to become the contents of the new
   persistent RAM. This results in a checksum mismatch and will be reported as
   corrupted persistent memory by your crash storage/transmit code, even though
   the underlying cause is innocuous. This is avoidable by fixing the size and
   address of your persistent RAM containers between software updates.
   Alternatively, zeroing out the persistent memory can reduce the chance of
   spurious corruption.
4. **Zero persistent RAM on cold boot** - To ensure deterministic cold boots,
   clear persistent RAM when booting from a power-off state. This requires
   detecting a cold boot, which not all systems support.
5. **Provide a mechanism to manually clear persistent RAM** - Add a reliable
   hook or request flag that can be set (e.g., before a reboot) to zero all
   persistent RAM on the next boot. This is useful for emulating persistent
   memory loss in a thread-safe manner for testing, and provides a recovery
   path if persistent RAM data causes boot loops or other unexpected behavior.
6. **Watch out for boot loops** - In rare cases, bugs in handling the
   persistent RAM transmit or storage phase can lead to boot loops.

.. _module-pw_persistent_ram-hw_requirements:

---------------------
Hardware Requirements
---------------------
The use of persistent RAM is dependent on specific hardware and system
configurations. It requires memory regions that retain their state across
reboots without being cleared by bootloaders or hardware initialization
routines. Many common MCUs operate this way; for example, many common STM32 and
NXP MCUs retain their memory when rebooting.

------------------------
Persistent RAM Placement
------------------------
Persistent RAM is typically provided through specially carved-out linker script
sections and/or memory ranges located such that bootloaders and application
boot code do not clobber them.

1. If persistent linker sections are provided, use the ``PW_PLACE_IN_SECTION()``
   macro to assign variables to that memory region. For example, if the
   persistent memory section name is ``.noinit``, you could instantiate an
   object as follows:

   .. code-block:: cpp

      #include "pw_persistent_ram/persistent.h"
      #include "pw_preprocessor/compiler.h"

      using pw::persistent_ram::Persistent;

      PW_PLACE_IN_SECTION(".noinit") Persistent<bool> persistent_bool;

2. If persistent memory ranges are provided, you can use a struct to wrap
   the different persisted objects. This makes it possible to ensure that the
   data fits in the provided memory range. This must be done via a runtime check
   against variables provided through the linker script since the addresses
   of linker script symbols aren't available at compile time.

   .. code-block:: cpp

      #include "pw_assert/check.h"
      #include "pw_persistent_ram/persistent.h"

      // Provided for example through a linker script.
      extern "C" uint8_t __noinit_begin;
      extern "C" uint8_t __noinit_end;

      struct PersistentData {
        Persistent<bool> persistent_bool;
      };
      PersistentData& persistent_data =
          *reinterpret_cast<PersistentData*>(&__noinit_begin);

      void CheckPersistentDataSize() {
        PW_DCHECK_UINT_LE(sizeof(PersistentData),
                          __noinit_end - __noinit_begin,
                          "PersistentData overflowed the noinit memory range");
      }

---------------------------------
pw::persistent_ram::Persistent<T>
---------------------------------
The ``Persistent<T>`` class is a simple container for holding its templated
value ``T`` with CRC16 integrity checking. Note that a ``Persistent<T>``
object's contents will be lost if a write/set operation is interrupted or
otherwise not completed, as it is not double-buffered.

The default constructor does nothing, meaning it will result in either invalid
state initially or a valid persisted value from a previous session.

The destructor does nothing; therefore, it is okay if it is not executed during
shutdown.

Example: Storing an integer
===========================
A common use case for persistent data is to track boot counts, effectively
measuring how often the device has rebooted. This can be useful for monitoring
how many times the device has rebooted and/or crashed. This can be easily
accomplished using the ``Persistent<T>`` container.

.. code-block:: cpp

   #include "pw_persistent_ram/persistent.h"
   #include "pw_preprocessor/compiler.h"

   using pw::persistent_ram::Persistent;

   class BootCount {
    public:
     explicit BootCount(Persistent<uint16_t>& persistent_boot_count)
         : persistent_(persistent_boot_count) {
       if (!persistent_.has_value()) {
         persistent_ = 0;
       } else {
         persistent_ = persistent_.value() + 1;
       }
       boot_count_ = persistent_.value();
     }

     uint16_t GetBootCount() { return boot_count_; }

    private:
     Persistent<uint16_t>& persistent_;
     uint16_t boot_count_;
   };

   PW_PLACE_IN_SECTION(".noinit") Persistent<uint16_t> persistent_boot_count;

   int main() {
     BootCount boot_count(persistent_boot_count);
     // Example usage: boot_count.GetBootCount();
     // ... rest of main
   }

Example: Storing larger objects
===============================
Larger objects may be inefficient to copy back and forth due to the need for
a working copy. To work around this, you can get a ``Mutator`` handle that
provides direct access to the underlying object. As long as the ``Mutator`` is
in scope, it is invalid to access the underlying ``Persistent<T>`` object
directly, but you'll be able to modify the object in place. Once the
``Mutator`` goes out of scope, the ``Persistent<T>`` object's checksum is
updated to reflect the changes.

.. code-block:: cpp

   #include "pw_persistent_ram/persistent.h"
   #include "pw_preprocessor/compiler.h"

   using pw::persistent_ram::Persistent;

   constexpr size_t kMaxReasonLength = 256;

   struct LastCrashInfo {
     uint32_t uptime_ms;
     uint32_t boot_id;
     char reason[kMaxReasonLength];
   };

   PW_PLACE_IN_SECTION(".noinit") Persistent<LastCrashInfo> persistent_crash_info;

   void HandleCrash(const char* fmt, va_list args) {
     // Once this scope ends, we know the persistent object has been updated
     // to reflect changes.
     {
       auto mutable_crash_info =
           persistent_crash_info.mutator(GetterAction::kReset);
       vsnprintf(mutable_crash_info->reason,
                 sizeof(mutable_crash_info->reason),
                 fmt,
                 args);
       mutable_crash_info->uptime_ms = system::GetUptimeMs();
       mutable_crash_info->boot_id = system::GetBootId();
     }
     // ...
   }

   int main() {
     if (persistent_crash_info.has_value()) {
       LogLastCrashInfo(persistent_crash_info.value());
       // Clear crash info once it has been dumped.
       persistent_crash_info.Invalidate();
     }

     // ... rest of main
   }

.. _module-pw_persistent_ram-persistent_buffer:

------------------------------------
pw::persistent_ram::PersistentBuffer
------------------------------------
The ``PersistentBuffer`` is a persistent storage container for variable-length
serialized data. Rather than allowing direct access to the underlying buffer for
random-access mutations, the ``PersistentBuffer`` is mutable through a
``PersistentBufferWriter`` that implements the ``pw::stream::Writer``
interface. This removes the potential for logical errors due to RAII or
``open()``/``close()`` semantics, as both the ``PersistentBuffer`` and
``PersistentBufferWriter`` can be used validly as long as their access is
serialized.

An example use case is emitting crash handler logs to a buffer so they are
available after the device reboots. Once the device reboots, the logs would be
emitted by the logging system. While this isn't always practical for plaintext
logs, tokenized logs are small enough for this to be useful.

Example: Logging to a persistent buffer
=======================================
An example use case is emitting crash handler logs to a buffer for them to be
available after a the device reboots. Once the device reboots, the logs would be
emitted by the logging system. While this isn't always practical for plaintext
logs, tokenized logs are small enough for this to be useful.

.. code-block:: cpp

   #include "pw_bytes/span.h"  // For pw::ConstByteSpan
   #include "pw_persistent_ram/persistent_buffer.h"
   #include "pw_preprocessor/compiler.h"

   using pw::persistent_ram::PersistentBuffer;
   using pw::persistent_ram::PersistentBufferWriter;

   PW_PLACE_IN_SECTION(".noinit") PersistentBuffer<2048> crash_logs;
   void CheckForCrashLogs() {
     if (crash_logs.has_value()) {
       // A function that dumps sequentially serialized logs using pw_log.
       // Assumes DumpRawLogs can take data() and size() or a ConstByteSpan.
       DumpRawLogs(pw::ConstByteSpan(crash_logs.data(), crash_logs.size()));
       crash_logs.clear();
     }
   }

   void HandleCrash(CrashInfo* crash_info) {
     // Clear previous logs before getting a new writer.
     crash_logs.clear();
     PersistentBufferWriter crash_log_writer = crash_logs.GetWriter();
     // Sets the pw::stream::Writer that pw_log should dump logs to.
     SetLogSink(crash_log_writer);
     // Handle crash, calling PW_LOG to log useful info.
   }

   int main() {
     CheckForCrashLogs();
     // ... rest of main
   }

Size Report
-----------
The following size report showcases the overhead for using ``Persistent<T>``.
Note that this templates ``Persistent<T>`` only on a ``uint32_t``; therefore,
the cost without ``pw_checksum``'s CRC16 is the approximate cost per type.

.. include:: persistent_size

Compatibility
-------------
* C++17

Dependencies
------------
* ``pw_checksum``
