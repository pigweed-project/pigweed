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

#include "pw_bluetooth/snoop.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "pw_assert/check.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/hci_events.emb.h"
#include "pw_bluetooth/snoop.emb.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bytes/span.h"
#include "pw_chrono/system_clock.h"
#include "pw_containers/inline_var_len_entry_queue.h"
#include "pw_hex_dump/hex_dump.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_stream/stream.h"
#include "pw_string/string.h"
#include "pw_string/string_builder.h"
#include "pw_sync/mutex.h"
#include "pw_trace/trace.h"

namespace pw::bluetooth {

namespace {

/// Copies as many items from src into dest as possible,
/// returning the number of items copied.
/// Note: The spans must not overlap.
template <typename T>
size_t CopyMax(span<const T> src, span<T> dest) {
  size_t n = std::min(src.size(), dest.size());
  std::copy_n(src.begin(), n, dest.begin());
  return n;
}

constexpr uint32_t kEmbossFileVersion = 1;
constexpr size_t kHeaderSize = 16;

// Generates the snoop log file header
//
// @returns the file header
Result<std::array<uint8_t, kHeaderSize>> GetSnoopLogFileHeader() {
  std::array<uint8_t, kHeaderSize> file_header_data;
  pw::Result<emboss::snoop_log::FileHeaderWriter> result =
      MakeEmbossWriter<emboss::snoop_log::FileHeaderWriter>(file_header_data);
  if (!result.ok()) {
    return result.status();
  }
  emboss::snoop_log::FileHeaderWriter writer = result.value();
  constexpr std::array<uint8_t, 8> kBtSnoopIdentificationPatternData = {
      0x62, 0x74, 0x73, 0x6E, 0x6F, 0x6F, 0x70, 0x00};

  auto identification_pattern_storage =
      writer.identification_pattern().BackingStorage();
  PW_CHECK(kBtSnoopIdentificationPatternData.size() ==
           identification_pattern_storage.SizeInBytes());
  std::copy(kBtSnoopIdentificationPatternData.begin(),
            kBtSnoopIdentificationPatternData.end(),
            identification_pattern_storage.begin());
  writer.version_number().Write(kEmbossFileVersion);
  writer.datalink_type().Write(emboss::snoop_log::DataLinkType::HCI_UART_H4);
  return file_header_data;
}

void AddTrace(emboss::snoop_log::PacketFlags emboss_packet_flag,
              const proxy::H4PacketInterface& hci_packet) {
  pw::InlineString<32> trace_tag;
  pw::StringBuilder trace_tag_builder(trace_tag);

  uint8_t type = static_cast<uint8_t>(hci_packet.GetH4Type());
  // Direction uses 0 for incoming, 1 for outcomming packet to match android
  // BTSL traces.
  uint8_t direction;
  switch (emboss_packet_flag) {
    case emboss::snoop_log::PacketFlags::RECEIVED:
      direction = 0;
      break;
    case emboss::snoop_log::PacketFlags::SENT:
      direction = 1;
      break;
    default:
      PW_UNREACHABLE;
      direction = -1;
  }
  size_t size = hci_packet.GetHciSpan().size();
  trace_tag_builder << type << "/" << direction << "/" << size;
  switch (hci_packet.GetH4Type()) {
    case emboss::H4PacketType::EVENT: {
      const auto header =
          emboss::MakeEventHeaderView(hci_packet.GetHciSpan().data(),
                                      emboss::EventHeaderView::SizeInBytes());
      if (!header.IsComplete()) {
        break;
      }
      trace_tag_builder.Format("/%02x", header.event_code_uint().Read());
      const emboss::EventCode event_code = header.event_code().Read();
      if (event_code == emboss::EventCode::LE_META_EVENT) {
        const auto le_meta_event =
            emboss::MakeLEMetaEventView(hci_packet.GetHciSpan().data(),
                                        emboss::LEMetaEventView::SizeInBytes());
        if (!le_meta_event.IsComplete()) {
          break;
        }
        trace_tag_builder.Format("/%02x", le_meta_event.subevent_code().Read());
      } else if (event_code == emboss::EventCode::VENDOR_DEBUG) {
        const auto vendor_debug_event = emboss::MakeVendorDebugEventView(
            hci_packet.GetHciSpan().data(),
            emboss::VendorDebugEventView::SizeInBytes());
        if (!vendor_debug_event.IsComplete()) {
          break;
        }
        trace_tag_builder.Format("/%02x",
                                 vendor_debug_event.subevent_code().Read());
      }
    } break;
    case emboss::H4PacketType::COMMAND: {
      const auto header = emboss::MakeCommandHeaderView(
          hci_packet.GetHciSpan().data(),
          emboss::CommandHeaderView::SizeInBytes());
      if (!header.IsComplete()) {
        break;
      }
      trace_tag_builder.Format("/%02x",
                               static_cast<uint8_t>(header.opcode().Read()));
    } break;
    case emboss::H4PacketType::ACL_DATA: {
      const auto header = emboss::MakeAclDataFrameHeaderView(
          hci_packet.GetHciSpan().data(),
          emboss::AclDataFrameHeaderView::SizeInBytes());
      if (!header.IsComplete()) {
        break;
      }
      trace_tag_builder.Format("/%03x/", header.handle().Read());
      trace_tag_builder << header.packet_boundary_flag().Read();
    } break;
    case emboss::H4PacketType::UNKNOWN:
    case emboss::H4PacketType::SYNC_DATA:
    case emboss::H4PacketType::ISO_DATA:
      break;
  }
  PW_TRACE_INSTANT_DATA("BTSL",
                        "Bluetooth",
                        "@pw_arg_label=%s",
                        trace_tag_builder.c_str(),
                        trace_tag_builder.size());
}

}  // namespace

Snoop::Reader::Reader(Snoop& snoop)
    : snoop_(&snoop),
      entry_iterator_(snoop_->queue_.begin()),
      offset_in_entry_(0),
      header_offset_(0),
      was_enabled_(snoop_->Disable()) {}

Snoop::Reader::Reader(Snoop::Reader&& other)
    : snoop_(other.snoop_),
      entry_iterator_(std::move(other.entry_iterator_)),
      offset_in_entry_(other.offset_in_entry_),
      header_offset_(other.header_offset_),
      was_enabled_(other.was_enabled_) {
  other.snoop_ = nullptr;
}

Snoop::Reader& Snoop::Reader::operator=(Snoop::Reader&& other) {
  if (this == &other) {
    return *this;
  }

  // Not supported due to one Reader policy.
  PW_CHECK(!snoop_);

  snoop_ = other.snoop_;
  other.snoop_ = nullptr;
  entry_iterator_ = std::move(other.entry_iterator_);
  offset_in_entry_ = other.offset_in_entry_;
  header_offset_ = other.header_offset_;
  was_enabled_ = other.was_enabled_;
  return *this;
}

Snoop::Reader::~Reader() {
  if (snoop_ && was_enabled_) {
    snoop_->ClearReader();
    snoop_->Enable();
  }
}

StatusWithSize Snoop::Reader::DoRead(ByteSpan dest) {
  size_t bytes_written = 0;

  // Header

  if (header_offset_ < kHeaderSize) {
    Result<std::array<uint8_t, kHeaderSize>> header = GetSnoopLogFileHeader();
    if (!header.ok()) {
      return StatusWithSize::Internal();
    }

    const size_t num_copied =
        CopyMax(as_bytes(span(*header)).subspan(header_offset_), dest);
    header_offset_ += num_copied;
    bytes_written += num_copied;
    dest = dest.subspan(num_copied);
  }

  if (dest.empty()) {
    return StatusWithSize(bytes_written);
  }

  std::lock_guard lock(snoop_->queue_lock_);

  // Queue
  while (!dest.empty() && entry_iterator_ != snoop_->queue_.end()) {
    const auto& entry = *entry_iterator_;
    const auto data_parts = entry.contiguous_data();

    ConstByteSpan src;
    if (offset_in_entry_ < data_parts.first.size()) {
      src = data_parts.first.subspan(offset_in_entry_);
    } else {
      const size_t offset_in_part2 = offset_in_entry_ - data_parts.first.size();
      src = data_parts.second.subspan(offset_in_part2);
    }

    const size_t num_copied = CopyMax(src, dest);
    offset_in_entry_ += num_copied;
    bytes_written += num_copied;
    dest = dest.subspan(num_copied);

    if (offset_in_entry_ == entry.size()) {
      ++entry_iterator_;
      offset_in_entry_ = 0;
    }
  }

  if (bytes_written == 0) {
    return StatusWithSize::OutOfRange();
  } else {
    return StatusWithSize(bytes_written);
  }
}

Result<Snoop::Reader> Snoop::GetReader() {
  {
    std::lock_guard lock(queue_lock_);
    if (reader_active_) {
      return Status::FailedPrecondition();
    }
    reader_active_ = true;
  }

  return Snoop::Reader(*this);
}

void Snoop::ClearReader() {
  std::lock_guard lock(queue_lock_);
  reader_active_ = false;
}

// Dump the snoop log to the log as a hex string
Status Snoop::DumpToLog() {
  PW_LOG_INFO("Snoop Log Start");
  PW_LOG_INFO("Step 1: Copy and paste the hex data into a text file");
  PW_LOG_INFO("Step 2: Remove any extra text (e.g. file, timestamp, etc)");
  PW_LOG_INFO("Step 3: $ xxd -r -p input.hex output.snoop");
  PW_LOG_INFO("Step 4: $ wireshark output.snoop");
  Status status = Dump([](ConstByteSpan data) {
    std::array<char, 80> temp;
    pw::dump::FormattedHexDumper hex_dumper(temp);
    hex_dumper.flags.prefix_mode =
        pw::dump::FormattedHexDumper::AddressMode::kDisabled;
    hex_dumper.flags.show_ascii = false;
    hex_dumper.flags.bytes_per_line = 32;
    hex_dumper.flags.group_every = 32;
    hex_dumper.flags.show_header = false;
    Status hex_dumper_status = hex_dumper.BeginDump(data);
    if (!hex_dumper_status.ok()) {
      return hex_dumper_status;
    }
    while (hex_dumper.DumpLine().ok()) {
      PW_LOG_INFO("%s", temp.data());
    }
    return pw::OkStatus();
  });
  PW_LOG_INFO("Snoop Log End");
  return status;
}

/// Dump the snoop log via callback without locking
///
/// The callback will be invoked multiple times as the circular buffer is
/// traversed.
///
/// Note: this function does NOT lock the snoop log. Do not invoke it unless
/// the snoop log is not being used. For example, use this API to read the
/// snoop log in a crash handler where mutexes are not allowed to be taken.
///
/// @param callback callback to invoke
Status Snoop::DumpUnlocked(
    const Function<Status(ConstByteSpan data)>& callback) {
  Result<std::array<uint8_t, 16>> result = GetSnoopLogFileHeader();
  if (!result.ok()) {
    return result.status();
  }
  Status status = callback(as_bytes(span(result.value())));
  if (!status.ok()) {
    return status;
  }

  for (const auto& entry : queue_) {
    std::pair<ConstByteSpan, ConstByteSpan> data = entry.contiguous_data();
    status = callback(data.first);
    if (!status.ok()) {
      return status;
    }
    if (!data.second.empty()) {
      status = callback(data.second);
      if (!status.ok()) {
        return status;
      }
    }
  }
  return OkStatus();
}

void Snoop::AddEntry(emboss::snoop_log::PacketFlags emboss_packet_flag,
                     const proxy::H4PacketInterface& hci_packet) {
  std::lock_guard lock(queue_lock_);
  if (!is_enabled_) {
    return;
  }

  if (PW_TRACE_ENABLE) {
    AddTrace(emboss_packet_flag, hci_packet);
  }

  size_t hci_packet_length_to_include = std::min(
      hci_packet.GetHciSpan().size(),
      static_cast<size_t>(scratch_buffer_.size() -
                          emboss::snoop_log::EntryHeader::MaxSizeInBytes() -
                          /* hci type*/ 1));
  size_t total_entry_size = hci_packet_length_to_include + /* hci type*/ 1 +
                            emboss::snoop_log::EntryHeader::MaxSizeInBytes();
  // Ensure the scratch buffer can fit the entire entry
  PW_CHECK_INT_GE(scratch_buffer_.size(), total_entry_size);

  pw::Result<emboss::snoop_log::EntryWriter> result =
      MakeEmbossWriter<emboss::snoop_log::EntryWriter>(scratch_buffer_);
  PW_CHECK_OK(result);
  emboss::snoop_log::EntryWriter writer = result.value();
  writer.header().original_length().Write(hci_packet.GetHciSpan().size() +
                                          /* hci type*/ 1);
  writer.header().included_length().Write(hci_packet_length_to_include +
                                          /* hci type*/ 1);
  writer.header().packet_flags().Write(emboss_packet_flag);
  writer.header().cumulative_drops().Write(0);
  writer.header().timestamp_us().Write(static_cast<int64_t>(
      std::chrono::time_point_cast<std::chrono::microseconds>(
          system_clock_.now())
          .time_since_epoch()
          .count()));

  // write h4 type
  writer.packet_h4_type().Write(static_cast<uint8_t>(hci_packet.GetH4Type()));

  // write hci packet
  pw::span<const uint8_t> hci_packet_trimmed =
      hci_packet.GetHciSpan().first(hci_packet_length_to_include);
  PW_CHECK(TryToCopyToEmbossStruct(/*emboss_dest=*/writer.packet_hci_data(),
                                   /*src=*/hci_packet_trimmed));

  // save the entry!
  queue_.push_overwrite(
      as_bytes(span{scratch_buffer_.data(), total_entry_size}));
}

}  // namespace pw::bluetooth
