// Copyright 2025 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#define PW_LOG_MODULE_NAME "EXAMPLES_TOP_DOWN_FORWARD"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string_view>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_assert/check.h"
#include "pw_bytes/span.h"
#include "pw_hex_dump/log_bytes.h"
#include "pw_log/levels.h"
#include "pw_log/log.h"
#include "pw_multibuf/examples/protocol.emb.h"
#include "pw_multibuf/examples/protocol.h"
#include "pw_multibuf/multibuf.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"
#include "runtime/cpp/emboss_memory_util.h"

// About this example
// ------------------
//
// This example demonstrates how to parse the contents of a Multibuf
// (bottom-up, or from outer to inner), and then turn around and rewrap that
// payload (top-down, or from inner to outer) with headers to generate an output
// packet.
//
// It also demonstrates hwo to use Google Emboss
// (https://github.com/google/emboss) to read and write the bytes, to pack and
// unpack the header structures.
//
// Structure of the packets (input and output)
// -------------------------------------------
//
// +--------------------------------------------------------------+
// |         +------------------------------------------+         |
// |  Link   |          +----------------------------+  |   Link  |
// | Header  | Network  | Transport +-------------+  |  |  Footer |
// |         | Header   | Header    |  Payload    |  |  |         |
// |         |          | 16 or     | (variable)  |  |  |         |
// |         | 20 bytes | 20 bytes  +-------------+  |  |         |
// | 6 bytes |          +----------------------------+  |  4 byte |
// |         +------------------------------------------+   CRC   |
// +--------------------------------------------------------------+
// |<----------One Packet: Max Total Size 1024 bytes------------->|
//
// The protocol assumes that the payload is fragmented across multiple packets,
// if needed, with the transport header containing the metadata necessary for
// reassembly. However this example assumes no fragmentation for simplicity.

namespace pw::multibuf::examples {

constexpr size_t kProtocolMaxPacketChunks = 5;

// Serialization and deserialization functions, to go between the
// (unpacked) definitions in "protocol.h" to the (packed) definitions in
// "protocol.emb.h"

// DOCSTAG: [pw_multibuf-examples-top_down_forward-demo_link_header_serialize]
DemoLinkHeader Deserialize(emboss::DemoLinkHeaderView view) {
  return {
      .src_addr = view.src_addr().Read(),
      .dst_addr = view.dst_addr().Read(),
      .length = view.length().Read(),
  };
}

void Serialize(const DemoLinkHeader& value,
               emboss::DemoLinkHeaderWriter& writer) {
  writer.src_addr().Write(value.src_addr);
  writer.dst_addr().Write(value.dst_addr);
  writer.length().Write(value.length);
}
// DOCSTAG: [pw_multibuf-examples-top_down_forward-demo_link_header_serialize]

DemoLinkFooter Deserialize(emboss::DemoLinkFooterView view) {
  return {
      .crc32 = view.crc32().Read(),
  };
}

void Serialize(const DemoLinkFooter& value,
               emboss::DemoLinkFooterWriter& writer) {
  writer.crc32().Write(value.crc32);
}

DemoNetworkHeader Deserialize(emboss::DemoNetworkHeaderView view) {
  return {
      .src_addr = view.src_addr().Read(),
      .dst_addr = view.dst_addr().Read(),
      .length = view.length().Read(),
  };
}

void Serialize(const DemoNetworkHeader& value,
               emboss::DemoNetworkHeaderWriter& writer) {
  writer.src_addr().Write(value.src_addr);
  writer.dst_addr().Write(value.dst_addr);
  writer.length().Write(value.length);
}

DemoTransportHeader Deserialize(emboss::DemoTransportHeaderView view) {
  return {
      .segment_id = view.segment_id().Read(),
      .offset = view.offset().Read(),
      .length = view.length().Read(),
  };
}

void Serialize(const DemoTransportHeader& value,
               emboss::DemoTransportHeaderWriter& writer) {
  writer.segment_id().Write(value.segment_id);
  writer.offset().Write(value.offset);
  writer.length().Write(value.length);
}

DemoTransportFirstHeader Deserialize(
    emboss::DemoTransportFirstHeaderView view) {
  // Because it DemoTransportFirstHeader is defined using inheritance in
  // protocol.h, returning a value is a bit more verbose.
  DemoTransportFirstHeader result{};
  result.segment_id = view.header().segment_id().Read();
  result.offset = view.header().offset().Read();
  result.length = view.header().length().Read();
  result.total_length = view.total_length().Read();
  return result;
}

void Serialize(const DemoTransportFirstHeader& value,
               emboss::DemoTransportFirstHeaderWriter& writer) {
  writer.header().segment_id().Write(value.segment_id);
  writer.header().offset().Write(value.offset);
  writer.header().length().Write(value.length);
  writer.total_length().Write(value.total_length);
}

/// Helper function to dump out all the chunks in a Multibuf
///
/// @param  name  The name to use when dumping out the bytes.
/// @param  mb    The MultiBuf to dump.
void LogHexdumpForMultibufChunks(const char* name, const ConstMultiBuf& mb) {
  PW_LOG_DEBUG("%s: Multibuf has %hu fragments, %hu layers, %zu size",
               name,
               mb.NumFragments(),
               mb.NumLayers(),
               mb.size());

  size_t i = 0;
  for (const ConstByteSpan chunk : mb.ConstChunks()) {
    PW_LOG_DEBUG("[[chunk %zu]]", i);
    dump::LogBytes(PW_LOG_LEVEL_DEBUG, chunk);
    ++i;
  }
}

/// Helper for working with Emboss views (reader or writer)
///
/// Returns the maximum size of the structure represented by the view.
/// The assumption is that there is some reasonable and fixed upper bound.
template <typename View>
[[nodiscard]] constexpr size_t GetMaxSizeForEmbossView() {
  static_assert(View::MaxSizeInBytes().Ok(), "No constant maximum size!");
  return View::MaxSizeInBytes().UncheckedRead();
}

/// Helper for working with Emboss views (reader or writer)
///
/// Returns a std::array<std::byte, N> where N is the maximum size of the
/// structure represented by the view.
template <typename View>
[[nodiscard]] constexpr auto GetLocalBufferForEmbossView() {
  return std::array<std::byte, GetMaxSizeForEmbossView<View>()>{};
}

/// Helper for working with Emboss views (reader or writer)
///
/// Returns a UniquePtr<byte[]> allocation with N bytes, where N is the maximum
/// size of the structure represented by the view.
template <typename View, size_t... kCannot, typename S = size_t>
[[nodiscard]] UniquePtr<std::byte[]> AllocateBufferForEmbossView(
    Allocator& alloc, S size = GetMaxSizeForEmbossView<View>()) {
  return alloc.MakeUnique<std::byte[]>(size);
}

/// Helper for working with Emboss views (reader or writer)
///
/// Returns an instantiation of the view for a byte span (constant or
/// non-constant), which requires using an intermediary Emboss-defined wrapper.
template <typename View, typename Byte, size_t Extent>
[[nodiscard]] View InstantiateView(span<Byte, Extent> buffer) {
  constexpr size_t kAlignment = 1;
  constexpr size_t kOffset = 0;
  using ContiguousBuffer =
      ::emboss::support::ContiguousBuffer<Byte, kAlignment, kOffset>;
  return View{ContiguousBuffer{buffer.data(), buffer.size()}};
}

/// Helper for writing to a buffer with an Emboss writer.
///
/// Serializes the provided header structure to the provided buffer, and returns
/// the count of bytes actually written.
///
/// The given buffer is assumed to be big enough to contain the bytes, otherwise
/// this function will assert.
template <typename EmbossWriter, typename HeaderType>
[[nodiscard]] size_t SerializeTo(ByteSpan buffer, const HeaderType& header) {
  // Construct an instance of the writer, pointing it to the allocation.
  auto writer = InstantiateView<EmbossWriter>(buffer);

  // Serialize using the writer instance.
  Serialize(header, writer);

  // Ensure we have a valid header, and that the size is known.
  PW_CHECK(writer.Ok());

  // Determine the actual size of the header, in case it isn't constant.
  const size_t actual_size = writer.SizeInBytes();

  return actual_size;
}

/// Helper for serializing an Emboss writer view to a Multibuf.
///
/// This function allocates a buffer for the maximum number of bytes that the
/// EmbossWriter can output, instantiates the EmbossWriter as a writable view
/// of those bytes, and then invokes a function to actually write the header.
///
/// It returns true if the chunk was successfully added to the Multibuf.
///
/// @param  mb               The multibuf to insert into.
/// @param  insert_position  An iterator into the multibuf to indicate the
//                           insert position. Typically begin() or end().
/// @param  allocation       The byte allocation to insert
/// @param  offset           The starting offset into the allocation which
///                          should become part of the multibuf view.
/// @param  size             The count of bytes after 'offset' which should
///                          become part fo the multibuf view.
/// @param  name             A debug name to use when logging.
[[nodiscard]] bool InsertAllocatedChunk(
    ConstMultiBuf& mb,
    ConstMultiBuf::const_iterator insert_position,
    UniquePtr<std::byte[]> allocation,
    size_t offset,
    size_t size,
    const char* name) {
  PW_CHECK(offset < allocation.size());
  PW_CHECK(offset + size <= allocation.size());

  // For the purposes of this example, log the bytes we are about to insert.
  PW_LOG_DEBUG("Inserting bytes for %s", name);
  dump::LogBytes(PW_LOG_LEVEL_DEBUG,
                 ConstByteSpan({allocation.get(), allocation.size()})
                     .subspan(offset, size));

  // Insert the bytes into the Multibuf, transferring ownership of the
  // allocation.
  if (!mb.TryReserveForInsert(insert_position)) {
    return false;
  }

  mb.Insert(std::move(insert_position), std::move(allocation), offset, size);
  return true;
}

/// A helper function for prepending a header structure to the MultiBuf.
///
/// In this case we append using an allocated buffer, which means the MultiBuf
/// ends up owning the bytes.
///
/// @tparam EmbossWriter  The Emboss writer type to use to serialize the header.
/// @param  alloc         The allocator to use.
/// @param  mb            The multibuf to insert into.
/// @param  header        The CPU friendly header data to insert.
/// @param  name          A debug name for logging.
template <typename EmbossWriter, typename HeaderType>
[[nodiscard]] bool PrependHeader(Allocator& alloc,
                                 ConstMultiBuf& mb,
                                 const HeaderType& header,
                                 const char* name) {
  auto allocation = AllocateBufferForEmbossView<EmbossWriter>(alloc);
  const size_t size = SerializeTo<EmbossWriter>(
      ByteSpan{allocation.get(), allocation.size()}, header);
  return InsertAllocatedChunk(
      mb, mb.begin(), std::move(allocation), 0, size, name);
}

/// A helper function for appending a header structure to the MultiBuf.
///
/// In this case we append using an allocated buffer, which means the MultiBuf
/// ends up owning the bytes.
///
/// @tparam EmbossWriter  The Emboss writer type to use to serialize the header.
/// @param  alloc         The allocator to use.
/// @param  mb            The multibuf to insert into.
/// @param  header        The CPU friendly header data to insert.
/// @param  name          A debug name for logging.
template <typename EmbossWriter, typename HeaderType>
[[nodiscard]] bool AppendHeader(Allocator& alloc,
                                ConstMultiBuf& mb,
                                const HeaderType& header,
                                const char* name) {
  auto allocation = AllocateBufferForEmbossView<EmbossWriter>(alloc);
  const size_t size = SerializeTo<EmbossWriter>(
      ByteSpan{allocation.get(), allocation.size()}, header);
  return InsertAllocatedChunk(
      mb, mb.end(), std::move(allocation), 0, size, name);
}

/// Simulate sending a packet.
///
/// Grabs a contiguous copy of the content of the multibuf, and logs it.
///
/// @param  packet  The Multibuf contents to "send".
void SimulateSend(const ConstMultiBuf& packet) {
  PW_LOG_INFO("Simulating sending a final packet (%zu bytes)", packet.size());

  // Allocate a chunk of memory on the stack for grabbing a copy of the
  // Multibuf content.
  std::array<std::byte, kMaxDemoLinkFrameLength> buffer{};
  if (packet.size() > buffer.size()) {
    PW_LOG_ERROR("Packet unexpectedly large (%zu). Can't make a copy.",
                 packet.size());
    return;
  }

  // Copy the Multibuf content to the buffer.
  const size_t copied_size = packet.CopyTo(ByteSpan(buffer));

  // Dump out the bytes that are being "sent".
  dump::LogBytes(PW_LOG_LEVEL_INFO,
                 ConstByteSpan(buffer).subspan(0, copied_size));
}

/// Prepends a canned header for the purpose of the example.
[[nodiscard]] bool PrependCannedTransportFirstHeader(Allocator& alloc,
                                                     ConstMultiBuf& output) {
  DemoTransportFirstHeader first_header{};
  first_header.segment_id = 0;
  first_header.offset = 0;
  first_header.length = static_cast<uint32_t>(output.size());
  first_header.total_length = static_cast<uint32_t>(output.size());
  if (!PrependHeader<emboss::DemoTransportFirstHeaderWriter>(
          alloc, output, first_header, "DemoTransportFirstHeader")) {
    PW_LOG_ERROR("Failed to prepend the first transport header.");
    return false;
  }
  return true;
}

/// Prepends a canned header for the purpose of the example.
[[nodiscard]] bool PrependCannedNetworkHeader(Allocator& alloc,
                                              ConstMultiBuf& output) {
  // Chunk 3: Prepend a DemoNetworkHeader
  // (Uses a fake src_addr / dst_addr)
  const DemoNetworkHeader canned_network_header = {
      .src_addr = 0x1234'1111'2222'3333,
      .dst_addr = 0x4567'4444'5555'6666,
      .length = static_cast<uint32_t>(output.size()),
  };
  if (!PrependHeader<emboss::DemoNetworkHeaderWriter>(
          alloc, output, canned_network_header, "DemoNetworkHeader")) {
    PW_LOG_ERROR("Failed to prepend the network header.");
    return false;
  }
  return true;
}

[[nodiscard]] bool PrependCannedLinkHeader(Allocator& alloc,
                                           ConstMultiBuf& output) {
  // (Uses a fake src_addr / dst_addr)
  const DemoLinkHeader canned_link_header = {
      .src_addr = 0x4321,
      .dst_addr = 0xFEDC,
      .length = static_cast<uint16_t>(output.size()),
  };

  if (!PrependHeader<emboss::DemoLinkHeaderWriter>(
          alloc, output, canned_link_header, "DemoLinkHeader")) {
    PW_LOG_ERROR("Failed to prepend the link header.");
    return false;
  }
  return true;
}

[[nodiscard]] bool AppendCannedLinkFooter(Allocator& alloc,
                                          ConstMultiBuf& output) {
  // Chunk 5: Prepend a DemoLinkFooter
  // (Uses a fake CRC)
  const DemoLinkFooter canned_link_footer = {
      .crc32 = 0xdeadbeef,
  };
  if (!AppendHeader<emboss::DemoLinkFooterWriter>(
          alloc, output, canned_link_footer, "DemoLinkFooter")) {
    PW_LOG_ERROR("Failed to append the link footer.");
    return false;
  }

  return true;
}

// Note this also adjusts the Multibuf to removed the deserialized bytes.
template <
    typename EmbossView,
    typename UnpackedType = decltype(Deserialize(std::declval<EmbossView>()))>
[[nodiscard]] constexpr auto DeserializeFromMultibufBegin(ConstMultiBuf& mb)
    -> UnpackedType {
  // `alternate_buffer` is here in case the Multibuf is internally fragmented,
  // fragmented, and the header bytes span multiple chunks. If the header we
  // read does not span multiple chunks, it is not actually used!
  //
  // Note that in this example we know the source Multibuf will always be a
  // single chunk, but for safety we have this
  auto alternate_buffer = GetLocalBufferForEmbossView<EmbossView>();

  // Instantiate a view, either pointing to a continuous chunk in the
  // MultiBuf, or if necessary using `alternative_buffer` as the continuous
  // memory.
  auto view = InstantiateView<EmbossView>(mb.Get(alternate_buffer));

  // Ensure the structure defined by the view is complete.
  PW_CHECK(view.Ok());

  // Adding a layer narrows the view provided by the Multibuf
  // In this case we narrow in a way that removes the header bytes from the
  // view.
  PW_CHECK(mb.AddLayer(view.SizeInBytes()));

  // Invoke the deserialization function which handles the details of reading
  // from the view.
  return Deserialize(std::move(view));
}

// Note this also adjusts the Multibuf to removed the deserialized bytes.
template <
    typename EmbossView,
    typename UnpackedType = decltype(Deserialize(std::declval<EmbossView>()))>
[[nodiscard]] constexpr auto DeserializeFromMultibufEnd(ConstMultiBuf& mb,
                                                        size_t offset)
    -> UnpackedType {
  // `alternate_buffer` is here in case the Multibuf is internally fragmented,
  // fragmented, and the header bytes span multiple chunks. If the header we
  // read does not span multiple chunks, it is not actually used!
  //
  // Note that in this example we know the source Multibuf will always be a
  // single chunk, but for safety we have this
  auto alternate_buffer = GetLocalBufferForEmbossView<EmbossView>();

  // Instantiate a view, either pointing to a continuous chunk in the
  // MultiBuf, or if necessary using `alternative_buffer` as the continuous
  // memory.
  auto view = InstantiateView<EmbossView>(mb.Get(alternate_buffer, offset));

  // Ensure the structure defined by the view is complete.
  PW_CHECK(view.Ok());

  // Adding a layer narrows the view provided by the Multibuf.
  // In this case we narrow in a way that removes the footer bytes from the
  // view.
  PW_CHECK(mb.AddLayer(0, mb.size() - view.SizeInBytes()));

  // Invoke the deserialization function which handles the details of reading
  // from the view.
  return Deserialize(std::move(view));
}

// Unwraps the payload from the innermost DemoTransportHeader.
[[nodiscard]] bool InPlaceDecodeTransportFirstHeader(ConstMultiBuf& remaining) {
  // Read the header via an Emboss view.
  const DemoTransportFirstHeader header =
      DeserializeFromMultibufBegin<emboss::DemoTransportFirstHeaderView>(
          remaining);

  // Confirm the canned values encoded by the sample packet bytes.
  constexpr uint32_t kExpectedSegmentId = 0;
  constexpr uint32_t kExpectedOffset = 0;
  constexpr uint32_t kExpectedLength = 12;
  constexpr uint32_t kExpectedTotalLength = 12;
  if (header.segment_id != kExpectedSegmentId ||
      header.offset != kExpectedOffset || header.length != kExpectedLength ||
      header.total_length != kExpectedTotalLength) {
    PW_LOG_ERROR("Unexpected transport header values for canned example");
    return false;
  }

  if (header.length > remaining.size()) {
    PW_LOG_ERROR("Transport header length (%" PRIu32
                 ") indicates a size larger than the remaining size (%zu).",
                 header.length,
                 remaining.size());
    return false;
  }

  return true;
}

[[nodiscard]] bool InPlaceDecodeNetworkHeader(ConstMultiBuf& remaining) {
  // Read the header via an Emboss view.
  const DemoNetworkHeader header =
      DeserializeFromMultibufBegin<emboss::DemoNetworkHeaderView>(remaining);

  // Confirm the canned values encoded by the sample packet bytes.
  constexpr uint64_t kExpectedSrcAddr = 0x4567'4444'5555'6666;
  constexpr uint64_t kExpectedDstAddr = 0x1234'1111'2222'3333;
  constexpr uint32_t kExpectedLength = 32;
  if (header.src_addr != kExpectedSrcAddr &&
      header.dst_addr != kExpectedDstAddr && header.length == kExpectedLength) {
    PW_LOG_ERROR("Unexpected network header values for canned example");
    return false;
  }

  if (header.length > remaining.size()) {
    PW_LOG_ERROR("Network header length (%" PRIu32
                 ") indicates a size larger than the remaining size (%zu).",
                 header.length,
                 remaining.size());
    return false;
  }

  return true;
}

[[nodiscard]] bool InPlaceDecodeLinkHeaderAndFooter(ConstMultiBuf& remaining) {
  // Read the header via an Emboss view.
  const DemoLinkHeader header =
      DeserializeFromMultibufBegin<emboss::DemoLinkHeaderView>(remaining);

  // Confirm the canned values encoded by the sample bytes.
  constexpr uint16_t kExpectedSrcAddr = 0xeeee;
  constexpr uint16_t kExpectedDstAddr = 0xffff;
  constexpr uint16_t kExpectedLength = 52;
  if (header.src_addr != kExpectedSrcAddr ||
      header.dst_addr != kExpectedDstAddr || header.length != kExpectedLength) {
    PW_LOG_ERROR("Unexpected link header values for canned example");
    return false;
  }

  if (header.length > remaining.size() - sizeof(uint32_t)) {
    PW_LOG_ERROR("Link header length (%" PRIu16
                 ") indicates a size larger than the remaining size (%zu), not "
                 "including the CRC.",
                 header.length,
                 remaining.size());
    return false;
  }

  // Now that we decoded the link header, we know how big the inner content
  // is, and therefore where the footer is after that content.
  const DemoLinkFooter footer =
      DeserializeFromMultibufEnd<emboss::DemoLinkFooterView>(remaining,
                                                             header.length);

  // For this example, we don't actually compute a CRC, but look for a magic
  // value.
  constexpr uint32_t kExpectedCrc32 = 0xdeadbeef;
  if (footer.crc32 != kExpectedCrc32) {
    PW_LOG_ERROR("Unexpected link footer/crc value for canned example");
    return false;
  }

  return true;
}

using namespace std::string_view_literals;

// We use a hard-coded input packet, which we proceed to decode.
constexpr auto kExampleInputPacketData =
    // DemoLinkHeader
    "\xee\xee"
    "\xff\xff"
    "\x00\x34"

    // DemoNetworkHeader
    "\x45\x67\x44\x44\x55\x55\x66\x66"
    "\x12\x34\x11\x11\x22\x22\x33\x33"
    "\x00\x00\x00\x20"

    // DemoTransportFirstHeader
    "\x00\x00\x00\x00\x00\x00\x00\000"
    "\x00\x00\x00\x00"
    "\x00\x00\x00\x0c"
    "\x00\x00\x00\x0c"

    // Payload ("Hello, World")
    "Hello, World"

    // DemoLinkFooter
    "\xde\xad\xbe\xef"sv;

int main() {
  Allocator& alloc = allocator::GetLibCAllocator();

  // DOCSTAG: [pw_multibuf-examples-top_down_forward-main]
  // Create an read-only Multibuf containing the example input packet bytes.
  //
  // Note that providing bytes this way works only as long as the lifetime of
  // those bytes exceeds that of their use in the multibuf.
  ConstMultiBuf::Instance received_packet_instance(alloc);
  received_packet_instance->PushBack(as_bytes(span(kExampleInputPacketData)));

  // Decode the three layers of the packet. This uses `MultiBuf::AddLayer()`
  // to deconstruct each layer from the outside in.
  ConstMultiBuf& decoded = received_packet_instance;
  if (!InPlaceDecodeLinkHeaderAndFooter(decoded) ||
      !InPlaceDecodeNetworkHeader(decoded) ||
      !InPlaceDecodeTransportFirstHeader(decoded)) {
    return 1;
  }

  // At this point `decoded` has been modified to be a view of just the
  // payload. Simulate a forwarding operation by passing it to be used in
  // generating a forward packet, where we wrap the received payload with a new
  // set of headers.
  //
  // Note that this does not requiring copying the payload bytes
  // unless the hardware needs a contiguous byte array for sending.

  // Set up a new Multibuf to hold the output packet.
  ConstMultiBuf::Instance output_packet_instance(alloc);

  // Attempt to reserve room to hold the metadata for the five total chunks
  // that will be in the final packet:
  if (!output_packet_instance->TryReserveChunks(kProtocolMaxPacketChunks)) {
    PW_LOG_ERROR("Failed to reserve chunks up front.");
    return 1;
  }

  ConstMultiBuf& output_packet = output_packet_instance;

  // Chunk 1: Insert the Multibuf containing the payload into the new
  // Multibuf.
  //
  // Note that in doing this the new multibuf takes over the old multibufs
  // contents, including ownership of any owner chunks.
  //
  // hHowever in this example the first multibuf at the top of the function is
  // populated with bytes it doesn't own, so the new multibuf here will not own
  // them either.
  output_packet.Insert(output_packet.begin(), std::move(decoded));

  // Each of the headers we add below is added by allocating chunks of memory
  // using the allocator, and inserting them into the output multibuf. The
  // output multibuf will own that memory, and will deallocate those additional
  // chunks on destruction.
  //
  // We could perhaps be more efficient in this example by allocating a single
  // larger chunk for all the headers, and a single chunk for the final CRC
  // (footer), but for now we assume we might want them each individually.

  // Chunks 2 - 5: Allocate and add each of the headers/footers needed.
  if (!PrependCannedTransportFirstHeader(alloc, output_packet) ||
      !PrependCannedNetworkHeader(alloc, output_packet) ||
      !PrependCannedLinkHeader(alloc, output_packet) ||
      !AppendCannedLinkFooter(alloc, output_packet)) {
    return 1;
  }

  // Dump out the contents of the multibuf. This shows all five chunks and
  // their hexdumps, and is a way of seeing how the multibuf is stored.
  LogHexdumpForMultibufChunks("Final output multibuf contents", output_packet);

  SimulateSend(output_packet);
  // DOCSTAG: [pw_multibuf-examples-top_down_forward-main]

  return 0;
}

}  // namespace pw::multibuf::examples

namespace {

TEST(ExampleTests, TopDownMultibuf) {
  using namespace ::pw::multibuf::examples;

  // Ensure the example main() works and returns zero.
  EXPECT_EQ(0, main());

  ::pw::Allocator& alloc = ::pw::allocator::GetLibCAllocator();

  ::pw::ConstMultiBuf::Instance received_packet_instance(alloc);
  received_packet_instance->PushBack(
      as_bytes(::pw::span(kExampleInputPacketData)));

  ::pw::ConstMultiBuf& decoded = received_packet_instance;
  ASSERT_TRUE(InPlaceDecodeLinkHeaderAndFooter(decoded));
  ASSERT_TRUE(InPlaceDecodeNetworkHeader(decoded));
  ASSERT_TRUE(InPlaceDecodeTransportFirstHeader(decoded));

  pw::ConstMultiBuf::Instance output_packet_instance(alloc);
  pw::ConstMultiBuf& output_packet = output_packet_instance;

  ASSERT_TRUE(output_packet.TryReserveChunks(kProtocolMaxPacketChunks));
  output_packet.Insert(output_packet.begin(), std::move(decoded));
  ASSERT_TRUE(PrependCannedTransportFirstHeader(alloc, output_packet));
  ASSERT_TRUE(PrependCannedNetworkHeader(alloc, output_packet));
  ASSERT_TRUE(PrependCannedLinkHeader(alloc, output_packet));
  ASSERT_TRUE(AppendCannedLinkFooter(alloc, output_packet));

  std::array<std::byte, kMaxDemoLinkFrameLength> output_packet_buffer{};
  const size_t output_packet_buffer_used =
      output_packet.CopyTo(output_packet_buffer);

  using namespace std::string_view_literals;

  constexpr auto kExpectedOutputData =
      // DemoLinkHeader
      "\x43\x21"
      "\xfe\xdc"
      "\x00\x34"

      // DemoNetworkHeader
      "\x12\x34\x11\x11\x22\x22\x33\x33"
      "\x45\x67\x44\x44\x55\x55\x66\x66"
      "\x00\x00\x00\x20"

      // DemoTransportFirstHeader
      "\x00\x00\x00\x00\x00\x00\x00\x00"
      "\x00\x00\x00\x00"
      "\x00\x00\x00\x0c"
      "\x00\x00\x00\x0c"

      // Payload ("Hello, World")
      "Hello, World"

      // DemoLinkFooter
      "\xde\xad\xbe\xef"sv;

  const ::pw::span kExpectedOutputBytes =
      ::pw::as_bytes(::pw::span(kExpectedOutputData));

  ASSERT_EQ(output_packet_buffer_used, kExpectedOutputData.size());
  EXPECT_TRUE(std::equal(kExpectedOutputBytes.begin(),
                         kExpectedOutputBytes.end(),
                         output_packet_buffer.begin()));
}

}  // namespace
