.. _module-pw_bytes-guide:

===================
Quickstart & guides
===================
.. pigweed-module-subpage::
   :name: pw_bytes

.. _module-pw_bytes-quickstart:

Quickstart
==========
.. tab-set::

   .. tab-item:: Bazel

      Add ``@pigweed//pw_bytes`` to the ``deps`` list in your Bazel target:

      .. code-block::

         cc_library("...") {
           # ...
           deps = [
             # ...
             "@pigweed//pw_bytes",
             # ...
           ]
         }

      If only one part of the module is needed, depend only on it; for example
      ``@pigweed//pw_bytes:array``.

   .. tab-item:: GN

      Add ``$dir_pw_bytes`` to the ``deps`` list in your ``pw_executable()``
      build target:

      .. code-block::

         pw_executable("...") {
           # ...
           deps = [
             # ...
             "$dir_pw_bytes",
             # ...
           ]
         }

   .. tab-item:: CMake

      Add ``pw_bytes`` to your ``pw_add_library`` or similar CMake target:

      .. code-block::

         pw_add_library(my_library STATIC
           HEADERS
             ...
           PRIVATE_DEPS
             # ...
             pw_bytes
             # ...
         )

      For a narrower dependency, depend on subtargets like
      ``pw_bytes.array``, etc.

   .. tab-item:: Zephyr

      There are two ways to use ``pw_bytes`` from a Zephyr project:

      #. Depend on ``pw_bytes`` in your CMake target (see CMake tab). This is
         Pigweed Team's suggested approach since it enables precise CMake
         dependency analysis.

      #. Add ``CONFIG_PIGWEED_BYTES=y`` to the Zephyr project's configuration,
         which causes ``pw_bytes`` to become a global dependency and have the
         includes exposed to all targets. Pigweed team does not recommend this
         approach, though it is the typical Zephyr solution.

Byte units
==========
Define byte sizes clearly using IEC standard units.

Using function calls:

.. code-block:: cpp

   #include "pw_bytes/units.h"

   constexpr size_t kSmallBuffer = pw::bytes::KiB(2);  // 2048 bytes
   constexpr size_t kLargeBuffer = pw::bytes::MiB(1);  // 1048576 bytes
   constexpr size_t kTotalMemory = pw::bytes::GiB(4);  // 4 gibibytes

Using user-defined literals (requires ``using namespace``):

.. code-block:: cpp

   #include "pw_bytes/units.h"

   using namespace pw::bytes::unit_literals;

   constexpr size_t kNetworkPacketSize = 1500_B;
   constexpr size_t kFlashSectorSize = 4_KiB;
   constexpr size_t kRamDiskSize = 16_MiB;

Compile-time byte arrays
========================
Construct ``std::array<std::byte, N>`` at compile time.

From individual byte values or integers (little-endian for multi-byte integers):

.. code-block:: cpp

   #include "pw_bytes/array.h"

   // Creates std::array<std::byte, 4>
   constexpr auto kMagicWord = pw::bytes::Array<'M', 'A', 'G', 'K'>;

   // Creates std::array<std::byte, 2> {0x34, 0x12}
   constexpr auto kValue16 = pw::bytes::Array<std::byte>(0x1234);

   // Creates std::array<uint8_t, 4> {0x78, 0x56, 0x34, 0x12}
   constexpr auto kValue32_u8 = pw::bytes::Array<uint8_t>(0x12345678);

From string literals (without null terminator):

.. code-block:: cpp

   #include "pw_bytes/array.h"

   // Creates std::array<std::byte, 5> {'H', 'e', 'l', 'l', 'o'}
   constexpr auto kGreeting = pw::bytes::String("Hello");

   // Creates std::array<std::byte, 0>
   constexpr auto kEmpty = pw::bytes::String("");

Concatenate multiple sources:

.. code-block:: cpp

   #include "pw_bytes/array.h"
   #include "pw_bytes/endian.h"  // For CopyInOrder

   constexpr auto kPart1 = pw::bytes::String("ID:");
   constexpr uint16_t kDeviceId = 0xABCD;

   // Creates std::array<std::byte, 5> {'I', 'D', ':', 0xCD, 0xAB} (kDeviceId as
   // little-endian)
   constexpr auto kFullId_LE = pw::bytes::Concat(kPart1, kDeviceId);

   // Creates std::array<std::byte, 5> {'I', 'D', ':', 0xAB, 0xCD} (kDeviceId as
   // big-endian)
   constexpr auto kFullId_BE = pw::bytes::Concat(
       kPart1, pw::bytes::CopyInOrder(pw::endian::big, kDeviceId));

Dynamically constructed byte sequences in fixed-size buffers
============================================================
.. code-block:: cpp

   #include <cstring>  // For strlen (example)

   #include "pw_bytes/byte_builder.h"
   #include "pw_bytes/span.h"  // For pw::ByteSpan
   #include "pw_log/log.h"     // For PW_LOG_ERROR (example)

   // Example functions
   void ProcessPacket(pw::ConstByteSpan packet_data) { /* ... */ }
   void SendData(const std::byte* data, size_t size) { /* ... */ }

   void BuildPacket(pw::ByteSpan buffer) {
     pw::ByteBuilder builder(buffer);

     builder.PutUint8(0x01);                      // Packet type
     builder.PutUint16(0x1234, pw::endian::big);  // Length in big-endian

     const char* payload_str = "DATA";
     builder.append(payload_str, strlen(payload_str));

     if (!builder.ok()) {
       // Handle error, e.g., buffer too small
       PW_LOG_ERROR("Failed to build packet: %s", builder.status().str());
       return;
     }

     // Use builder.data() and builder.size()
     ProcessPacket(pw::ConstByteSpan(builder.data(), builder.size()));
   }

   void ExampleWithByteBuffer() {
     pw::ByteBuffer<64> local_buffer;  // Buffer is part of the ByteBuffer object

     local_buffer.PutUint32(0xAABBCCDD, pw::endian::little);
     // ... build more data ...

     if (local_buffer.ok()) {
       SendData(local_buffer.data(), local_buffer.size());
     }
   }

Endianness conversion
=====================
Convert integers to and from specific byte orders.

.. code-block:: cpp

   #include <array>
   #include <cstdint>

   #include "pw_bytes/endian.h"

   // Convert a value to a specific byte order for storage/transmission
   uint32_t native_value = 0x12345678;
   uint32_t big_endian_value =
       pw::bytes::ConvertOrderTo(pw::endian::big, native_value);
   // On a little-endian system, big_endian_value is now 0x78563412
   // On a big-endian system, big_endian_value is still 0x12345678

   // Copy value into a buffer with a specific order
   std::array<std::byte, 4> buffer;
   // pw::bytes::CopyInOrder returns a std::array<std::byte, sizeof(T)>
   auto ordered_bytes = pw::bytes::CopyInOrder(pw::endian::little, native_value);
   std::memcpy(buffer.data(), ordered_bytes.data(), ordered_bytes.size());
   // buffer now contains {0x78, 0x56, 0x34, 0x12} on a little-endian system if
   // native_value was 0x12345678

   // Read a value from a buffer with a specific order
   uint32_t read_value =
       pw::bytes::ReadInOrder<uint32_t>(pw::endian::little, buffer.data());
   // read_value is 0x12345678 (assuming buffer was filled as above)

Alignment utilities
===================
Ensure data is correctly aligned in memory.

.. code-block:: cpp

   #include <cstdint>  // For std::byte, uintptr_t
   #include <cstdlib>  // For malloc, free (example)

   #include "pw_bytes/alignment.h"

   // Example functions
   void* GetMemory() { return std::malloc(100); }
   size_t GetMemorySize() { return 100; }
   void FreeMemory(void* ptr) { std::free(ptr); }

   void AlignmentExample() {
     void* some_memory_block = GetMemory();
     if (!some_memory_block)
       return;
     size_t memory_size = GetMemorySize();

     // Align pointer up to the next uint32_t boundary
     uint32_t* aligned_ptr_u32 =
         pw::AlignUp(static_cast<uint32_t*>(some_memory_block), alignof(uint32_t));

     // Check if a pointer is aligned
     if (pw::IsAlignedAs<double>(aligned_ptr_u32)) {
       // This block might not be reached if alignof(uint32_t) < alignof(double)
       // and aligned_ptr_u32 didn't happen to align for double.
     }

     // Get the largest aligned subspan
     pw::ByteSpan original_span =
         pw::AsWritableBytes(some_memory_block, memory_size);
     pw::ByteSpan aligned_subspan =
         pw::GetAlignedSubspan(original_span, alignof(uint64_t));
     // aligned_subspan can now be safely used to store uint64_t values,
     // provided aligned_subspan.size() is sufficient.

     FreeMemory(some_memory_block);
   }

Packed pointers
===============
Store extra data in the unused bits of aligned pointers.

.. code-block:: cpp

   #include <cstdint>

   #include "pw_bytes/packed_ptr.h"

   struct alignas(4) MyNode {  // Aligned to 4 bytes, so 2 LSBs are available
     int data;
     // MyNode* next;
     // bool flag1;
     // bool flag2;
     // Instead of separate flags, pack them into the pointer:
     pw::PackedPtr<MyNode> next_and_flags;
   };

   MyNode node1, node2;

   MyNode list_head;
   list_head.next_and_flags.set(&node1);
   // Store two flags (e.g., flag1=1, flag2=0). Max value for 2 bits is 0b11 (3).
   list_head.next_and_flags.set_packed_value(0b01);

   MyNode* next_node = list_head.next_and_flags.get();
   uintptr_t flags = list_head.next_and_flags.packed_value();
   // bool flag1_val = flags & 0b01;
   // bool flag2_val = (flags & 0b10) >> 1;

Bit manipulation
================
Perform low-level bit operations.

.. code-block:: cpp

   #include <cstdint>

   #include "pw_bytes/bit.h"

   // Sign extend a 12-bit signed value to int16_t
   uint16_t raw_adc_value =
       0x0F00;  // Represents -256 in 12-bit two's complement (0b111100000000)
   int16_t signed_adc_value = pw::bytes::SignExtend<12>(raw_adc_value);
   // signed_adc_value is now 0xFF00 (which is -256 in int16_t)

   // Extract a bitfield
   uint32_t register_value = 0xAABBCCDD;
   // Extract bits 15 down to 8 (inclusive)
   uint8_t byte_value = pw::bytes::ExtractBits<uint8_t, 15, 8>(register_value);
   // byte_value is 0xCC

Syntactic sugar for std::byte literals
======================================
Conveniently create ``std::byte`` literals.

.. code-block:: cpp

   #include <cstddef>  // For std::byte

   #include "pw_bytes/suffix.h"

   // Required to bring the operator into scope
   using ::pw::operator""_b;

   constexpr std::byte kMyByteValue = 128_b;
   constexpr std::byte kAnotherByte = 0xFF_b;

   void ProcessByte(std::byte b);

   // ProcessByte(42_b); // Example call
