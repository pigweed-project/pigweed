// Copyright 2022 The Pigweed Authors
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

// pw_protobuf performance tests.
//
// These tests cover operations that any protobuf library supports, so they can
// be mirrored to compare against other libraries. pw_protobuf-only APIs are
// benchmarked in pwpb_specific_perf_test.cc.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

#include "pw_assert/check.h"
#include "pw_bytes/span.h"
#include "pw_perf_test/perf_test.h"
#include "pw_protobuf/encoder.h"
#include "pw_protobuf/stream_decoder.h"
#include "pw_protobuf_test_protos/perf_test.pwpb.h"
#include "pw_protobuf_test_protos/test_data_perf_test.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_status/try.h"
#include "pw_stream/memory_stream.h"
#include "pw_stream/null_stream.h"

namespace pw::protobuf {
namespace {

using test::DoNotOptimize;
using test::MakeOpaque;
using test::Opaque;
namespace ItemInfo = test::pwpb::ItemInfo;
namespace Location = test::pwpb::Location;
namespace NestedLeaf = test::pwpb::NestedLeaf;
namespace NestedMid = test::pwpb::NestedMid;
namespace NestedRoot = test::pwpb::NestedRoot;
namespace ResponseInfo = test::pwpb::ResponseInfo;
namespace SensorLog = test::pwpb::SensorLog;
namespace SensorReport = test::pwpb::SensorReport;

// ============================================================================
// 0. Basic Integer Encoding
// ============================================================================

void BasicIntegerPerformance(perf_test::State& state, uint32_t value) {
  const uint32_t field_number = Opaque(test::kIntegerFieldNumber);
  value = Opaque(value);
  std::byte encode_buffer[30];

  while (state.KeepRunning()) {
    MemoryEncoder encoder(encode_buffer);
    PW_CHECK_OK(encoder.WriteUint32(field_number, value));
    DoNotOptimize(ConstByteSpan(encoder));
  }
}

PW_PERF_TEST(SmallIntegerEncoding,
             BasicIntegerPerformance,
             test::kSmallInteger);
PW_PERF_TEST(LargerIntegerEncoding,
             BasicIntegerPerformance,
             test::kLargerInteger);

// ============================================================================
// 1. Simple Scalar/Enum Message: ItemInfo (3 fields, 11-byte wire payload)
// ============================================================================

void ItemInfoStructEncode(perf_test::State& state) {
  ItemInfo::Message msg{};
  msg.offset = test::kItemOffset;
  msg.size = test::kItemSize;
  msg.access_level = ItemInfo::Access::READ_AND_WRITE;
  MakeOpaque(msg);
  std::byte buf[ItemInfo::kMaxEncodedSizeBytes];

  while (state.KeepRunning()) {
    ItemInfo::MemoryEncoder encoder(buf);
    PW_CHECK_OK(encoder.Write(msg));
    DoNotOptimize(ConstByteSpan(encoder));
  }
}
PW_PERF_TEST(ItemInfoStructEncode, ItemInfoStructEncode);

void ItemInfoMemoryEncoder(perf_test::State& state) {
  const uint64_t offset = Opaque(test::kItemOffset);
  const uint32_t size = Opaque(test::kItemSize);
  const ItemInfo::Access access_level =
      Opaque(ItemInfo::Access::READ_AND_WRITE);
  std::byte buf[ItemInfo::kMaxEncodedSizeBytes];

  while (state.KeepRunning()) {
    ItemInfo::MemoryEncoder encoder(buf);
    PW_CHECK_OK(encoder.WriteOffset(offset));
    PW_CHECK_OK(encoder.WriteSize(size));
    PW_CHECK_OK(encoder.WriteAccessLevel(access_level));
    DoNotOptimize(ConstByteSpan(encoder));
  }
}
PW_PERF_TEST(ItemInfoMemoryEncoder, ItemInfoMemoryEncoder);

void ItemInfoStructDecode(perf_test::State& state) {
  const ConstByteSpan encoded = Opaque(ConstByteSpan(test::kEncodedItemInfo));
  while (state.KeepRunning()) {
    ItemInfo::Message msg{};
    stream::MemoryReader reader(encoded);
    ItemInfo::StreamDecoder decoder(reader);
    PW_CHECK_OK(decoder.Read(msg));
    DoNotOptimize(msg);
  }
}
PW_PERF_TEST(ItemInfoStructDecode, ItemInfoStructDecode);

void ItemInfoStreamDecoder(perf_test::State& state) {
  const ConstByteSpan encoded = Opaque(ConstByteSpan(test::kEncodedItemInfo));
  while (state.KeepRunning()) {
    stream::MemoryReader reader(encoded);
    ItemInfo::StreamDecoder decoder(reader);
    uint64_t offset = 0;
    uint32_t size = 0;
    ItemInfo::Access access = ItemInfo::Access::NONE;
    Status status;
    while ((status = decoder.Next()).ok()) {
      switch (*decoder.Field()) {
        case ItemInfo::Fields::kOffset:
          offset = decoder.ReadOffset().value();
          break;
        case ItemInfo::Fields::kSize:
          size = decoder.ReadSize().value();
          break;
        case ItemInfo::Fields::kAccessLevel:
          access = decoder.ReadAccessLevel().value();
          break;
      }
    }
    PW_CHECK(status.IsOutOfRange());
    DoNotOptimize(offset);
    DoNotOptimize(size);
    DoNotOptimize(access);
  }
}
PW_PERF_TEST(ItemInfoStreamDecoder, ItemInfoStreamDecoder);

// ============================================================================
// 2. Realistic Composite Message: SensorReport (80-byte wire payload)
// ============================================================================

SensorReport::Message MakeSensorReport() {
  SensorReport::Message msg{};
  msg.device_id = test::kSensorDeviceId;
  msg.active = test::kSensorActive;
  msg.temperature_mc = test::kSensorTemperatureMc;
  msg.label = test::kSensorLabel;
  msg.payload.assign(std::begin(test::kSensorPayload),
                     std::end(test::kSensorPayload));
  for (int32_t s : test::kSensorSamples) {
    msg.samples.push_back(s);
  }
  msg.location.x = test::kLocationX;
  msg.location.y = test::kLocationY;
  return msg;
}

void SensorReportStructEncode(perf_test::State& state) {
  SensorReport::Message msg = MakeSensorReport();
  MakeOpaque(msg);
  std::byte buf[SensorReport::kMaxEncodedSizeBytes];
  while (state.KeepRunning()) {
    SensorReport::MemoryEncoder encoder(buf);
    PW_CHECK_OK(encoder.Write(msg));
    DoNotOptimize(ConstByteSpan(encoder));
  }
}
PW_PERF_TEST(SensorReportStructEncode, SensorReportStructEncode);

// Encodes through the generic pw::stream::Writer interface rather than
// directly to a buffer.
void SensorReportStructEncodeToWriter(perf_test::State& state) {
  SensorReport::Message msg = MakeSensorReport();
  MakeOpaque(msg);
  std::byte buf[SensorReport::kMaxEncodedSizeBytes];
  std::byte scratch[SensorReport::kScratchBufferSizeBytes];
  while (state.KeepRunning()) {
    stream::MemoryWriter writer(buf);
    SensorReport::StreamEncoder encoder(writer, scratch);
    PW_CHECK_OK(encoder.Write(msg));
    DoNotOptimize(writer.WrittenData());
  }
}
PW_PERF_TEST(SensorReportStructEncodeToWriter,
             SensorReportStructEncodeToWriter);

void SensorReportMemoryEncoder(perf_test::State& state) {
  const uint32_t device_id = Opaque(test::kSensorDeviceId);
  const bool active = Opaque(test::kSensorActive);
  const int32_t temperature_mc = Opaque(test::kSensorTemperatureMc);
  const std::string_view label = Opaque(test::kSensorLabel);
  const ConstByteSpan payload = Opaque(ConstByteSpan(test::kSensorPayload));
  const span<const int32_t> samples =
      Opaque(span<const int32_t>(test::kSensorSamples));
  const int32_t loc_x = Opaque(test::kLocationX);
  const int32_t loc_y = Opaque(test::kLocationY);
  std::byte buf[SensorReport::kMaxEncodedSizeBytes];

  while (state.KeepRunning()) {
    SensorReport::MemoryEncoder encoder(buf);
    PW_CHECK_OK(encoder.WriteDeviceId(device_id));
    PW_CHECK_OK(encoder.WriteActive(active));
    PW_CHECK_OK(encoder.WriteTemperatureMc(temperature_mc));
    PW_CHECK_OK(encoder.WriteLabel(label));
    PW_CHECK_OK(encoder.WritePayload(payload));
    PW_CHECK_OK(encoder.WriteSamples(samples));
    PW_CHECK_OK(encoder.WriteLocationMessage(
        [loc_x, loc_y](Location::StreamEncoder& loc) {
          PW_TRY(loc.WriteX(loc_x));
          return loc.WriteY(loc_y);
        }));
    PW_CHECK_OK(encoder.status());
    DoNotOptimize(ConstByteSpan(encoder));
  }
}
PW_PERF_TEST(SensorReportMemoryEncoder, SensorReportMemoryEncoder);

void SensorReportEncodedSize(perf_test::State& state) {
  SensorReport::Message msg = MakeSensorReport();
  MakeOpaque(msg);
  while (state.KeepRunning()) {
    stream::CountingNullStream counter;
    SensorReport::StreamEncoder encoder(counter);
    PW_CHECK_OK(encoder.Write(msg));
    size_t size = counter.bytes_written();
    DoNotOptimize(size);
  }
}
PW_PERF_TEST(SensorReportEncodedSize, SensorReportEncodedSize);

void SensorReportStructDecode(perf_test::State& state) {
  const ConstByteSpan encoded =
      Opaque(ConstByteSpan(test::kEncodedSensorReport));
  while (state.KeepRunning()) {
    SensorReport::Message msg{};
    stream::MemoryReader reader(encoded);
    SensorReport::StreamDecoder decoder(reader);
    PW_CHECK_OK(decoder.Read(msg));
    DoNotOptimize(msg);
  }
}
PW_PERF_TEST(SensorReportStructDecode, SensorReportStructDecode);

// Decodes through the generic pw::stream::Reader interface (identical to
// SensorReportStructDecode in pwpb because StreamDecoder always reads from a
// Reader, included so libraries with direct buffer decode can compare both).
void SensorReportStructDecodeFromReader(perf_test::State& state) {
  const ConstByteSpan encoded =
      Opaque(ConstByteSpan(test::kEncodedSensorReport));
  while (state.KeepRunning()) {
    SensorReport::Message msg{};
    stream::MemoryReader reader(encoded);
    SensorReport::StreamDecoder decoder(reader);
    PW_CHECK_OK(decoder.Read(msg));
    DoNotOptimize(msg);
  }
}
PW_PERF_TEST(SensorReportStructDecodeFromReader,
             SensorReportStructDecodeFromReader);

void SensorReportStreamDecoder(perf_test::State& state) {
  const ConstByteSpan encoded =
      Opaque(ConstByteSpan(test::kEncodedSensorReport));
  while (state.KeepRunning()) {
    stream::MemoryReader reader(encoded);
    SensorReport::StreamDecoder decoder(reader);
    uint32_t device_id = 0;
    bool active = false;
    int32_t temp = 0;
    char label[SensorReport::kLabelMaxSize];
    size_t label_size = 0;
    std::byte data[SensorReport::kPayloadMaxSize];
    size_t data_size = 0;
    int32_t samples[SensorReport::kSamplesMaxSize];
    size_t samples_count = 0;
    int32_t loc_x = 0;
    int32_t loc_y = 0;
    Status status;
    while ((status = decoder.Next()).ok()) {
      switch (*decoder.Field()) {
        case SensorReport::Fields::kDeviceId:
          device_id = decoder.ReadDeviceId().value();
          break;
        case SensorReport::Fields::kActive:
          active = decoder.ReadActive().value();
          break;
        case SensorReport::Fields::kTemperatureMc:
          temp = decoder.ReadTemperatureMc().value();
          break;
        case SensorReport::Fields::kLabel: {
          const StatusWithSize sws = decoder.ReadLabel(label);
          PW_CHECK_OK(sws.status());
          label_size = sws.size();
          break;
        }
        case SensorReport::Fields::kPayload: {
          const StatusWithSize sws = decoder.ReadPayload(data);
          PW_CHECK_OK(sws.status());
          data_size = sws.size();
          break;
        }
        case SensorReport::Fields::kSamples: {
          const StatusWithSize sws = decoder.ReadSamples(samples);
          PW_CHECK_OK(sws.status());
          samples_count = sws.size();
          break;
        }
        case SensorReport::Fields::kLocation: {
          Location::StreamDecoder loc = decoder.GetLocationDecoder();
          Status loc_status;
          while ((loc_status = loc.Next()).ok()) {
            switch (*loc.Field()) {
              case Location::Fields::kX:
                loc_x = loc.ReadX().value();
                break;
              case Location::Fields::kY:
                loc_y = loc.ReadY().value();
                break;
            }
          }
          PW_CHECK(loc_status.IsOutOfRange());
          break;
        }
      }
    }
    PW_CHECK(status.IsOutOfRange());
    DoNotOptimize(device_id);
    DoNotOptimize(active);
    DoNotOptimize(temp);
    DoNotOptimize(span<const char>(label, label_size));
    DoNotOptimize(label_size);
    DoNotOptimize(span<const std::byte>(data, data_size));
    DoNotOptimize(data_size);
    DoNotOptimize(span<const int32_t>(samples, samples_count));
    DoNotOptimize(samples_count);
    DoNotOptimize(loc_x);
    DoNotOptimize(loc_y);
  }
}
PW_PERF_TEST(SensorReportStreamDecoder, SensorReportStreamDecoder);

// ============================================================================
// 3. Oneof + Optional + Submessage: ResponseInfo (35-byte wire payload)
// ============================================================================

struct ResponseInfoInputs {
  std::string_view key_string = test::kResponseKeyString;
  int64_t timestamp = test::kResponseTimestamp;
  bool has_value = test::kResponseHasValue;
  uint64_t item_offset = test::kItemOffset;
  uint32_t item_size = test::kItemSize;
  ItemInfo::Access item_access = ItemInfo::Access::READ;
};

ResponseInfo::Message MakeResponseInfo(const ResponseInfoInputs& inputs) {
  ResponseInfo::Message msg{};
  msg.timestamp = inputs.timestamp;
  msg.has_value = inputs.has_value;
  msg.item_info.offset = inputs.item_offset;
  msg.item_info.size = inputs.item_size;
  msg.item_info.access_level = inputs.item_access;
  msg.key.SetEncoder([&inputs](ResponseInfo::StreamEncoder& encoder) {
    return encoder.WriteKeyString(inputs.key_string);
  });
  return msg;
}

// pw_protobuf represents oneofs as callbacks (pw::Function).
// ResponseInfoOneofEncode populates ResponseInfo::Message inside the loop to
// measure per-message struct and oneof callback setup alongside encoding,
// whereas ResponseInfoPrepopulatedEncode populates it before the loop to
// isolate encoding cost when the Message and callback can be reused.
void ResponseInfoOneofEncode(perf_test::State& state) {
  ResponseInfoInputs inputs;
  MakeOpaque(inputs);
  std::byte buf[128];
  while (state.KeepRunning()) {
    ResponseInfo::Message msg = MakeResponseInfo(inputs);
    ResponseInfo::MemoryEncoder encoder(buf);
    PW_CHECK_OK(encoder.Write(msg));
    DoNotOptimize(ConstByteSpan(encoder));
  }
}
PW_PERF_TEST(ResponseInfoOneofEncode, ResponseInfoOneofEncode);

void ResponseInfoPrepopulatedEncode(perf_test::State& state) {
  ResponseInfoInputs inputs;
  MakeOpaque(inputs);
  ResponseInfo::Message msg = MakeResponseInfo(inputs);
  std::byte buf[128];
  while (state.KeepRunning()) {
    ResponseInfo::MemoryEncoder encoder(buf);
    PW_CHECK_OK(encoder.Write(msg));
    DoNotOptimize(ConstByteSpan(encoder));
  }
}
PW_PERF_TEST(ResponseInfoPrepopulatedEncode, ResponseInfoPrepopulatedEncode);

void ResponseInfoOneofDecode(perf_test::State& state) {
  const ConstByteSpan encoded =
      Opaque(ConstByteSpan(test::kEncodedResponseInfo));
  while (state.KeepRunning()) {
    ResponseInfo::Message msg{};
    struct {
      char buf[16];
      size_t size = 0;
    } decoded_key;
    msg.key.SetDecoder([&decoded_key](ResponseInfo::Fields field,
                                      ResponseInfo::StreamDecoder& dec) {
      if (field == ResponseInfo::Fields::kKeyString) {
        const StatusWithSize sws = dec.ReadKeyString(decoded_key.buf);
        if (sws.ok()) {
          decoded_key.size = sws.size();
        }
        return sws.status();
      }
      return Status::InvalidArgument();
    });
    stream::MemoryReader reader(encoded);
    ResponseInfo::StreamDecoder decoder(reader);
    PW_CHECK_OK(decoder.Read(msg));
    DoNotOptimize(msg);
    DoNotOptimize(span<const char>(decoded_key.buf, decoded_key.size));
    DoNotOptimize(decoded_key.size);
  }
}
PW_PERF_TEST(ResponseInfoOneofDecode, ResponseInfoOneofDecode);

// ============================================================================
// 4. Unbounded Callback Fields: SensorLog (47-byte wire payload)
// ============================================================================

void SensorLogCallbackEncode(perf_test::State& state) {
  const std::string_view tag = Opaque(test::kSensorLogTag);
  const span<const int32_t> readings =
      Opaque(span<const int32_t>(test::kSensorSamples));
  std::byte buf[128];
  while (state.KeepRunning()) {
    SensorLog::Message msg{};
    msg.tag.SetEncoder(
        [&tag](SensorLog::StreamEncoder& enc) { return enc.WriteTag(tag); });
    msg.readings.SetEncoder([&readings](SensorLog::StreamEncoder& enc) {
      return enc.WriteReadings(readings);
    });
    SensorLog::MemoryEncoder encoder(buf);
    PW_CHECK_OK(encoder.Write(msg));
    DoNotOptimize(ConstByteSpan(encoder));
  }
}
PW_PERF_TEST(SensorLogCallbackEncode, SensorLogCallbackEncode);

void SensorLogCallbackDecode(perf_test::State& state) {
  const ConstByteSpan encoded = Opaque(ConstByteSpan(test::kEncodedSensorLog));
  while (state.KeepRunning()) {
    SensorLog::Message msg{};
    struct {
      char buf[32];
      size_t size = 0;
    } tag;
    struct {
      int32_t buf[8];
      size_t count = 0;
    } readings;
    auto decode_tag = [&tag](SensorLog::StreamDecoder& dec) {
      const StatusWithSize sws = dec.ReadTag(tag.buf);
      if (sws.ok()) {
        tag.size = sws.size();
      }
      return sws.status();
    };
    auto decode_readings = [&readings](SensorLog::StreamDecoder& dec) {
      const StatusWithSize sws = dec.ReadReadings(readings.buf);
      if (sws.ok()) {
        readings.count = sws.size();
      }
      return sws.status();
    };
    msg.tag.SetDecoder(decode_tag);
    msg.readings.SetDecoder(decode_readings);
    stream::MemoryReader reader(encoded);
    SensorLog::StreamDecoder decoder(reader);
    PW_CHECK_OK(decoder.Read(msg));
    DoNotOptimize(span<const char>(tag.buf, tag.size));
    DoNotOptimize(tag.size);
    DoNotOptimize(span<const int32_t>(readings.buf, readings.count));
    DoNotOptimize(readings.count);
  }
}
PW_PERF_TEST(SensorLogCallbackDecode, SensorLogCallbackDecode);

// ============================================================================
// 5. 3-Level Nested Message: NestedRoot (Tiny: 4B leaf, Large: 1000B leaf)
// ============================================================================

std::byte nested_encode_buf[NestedRoot::kMaxEncodedSizeBytes];
NestedRoot::Message nested_msg;

void NestedMemoryEncoder(perf_test::State& state, ConstByteSpan data) {
  data = Opaque(data);
  const uint64_t timestamp = Opaque(test::kNestedTimestamp);
  const uint32_t kind = Opaque(test::kNestedKind);
  const uint64_t leaf_id = Opaque(test::kNestedLeafId);
  const uint32_t leaf_count = Opaque(test::kNestedLeafCount);
  const uint32_t flags = Opaque(test::kNestedFlags);
  const uint32_t status = Opaque(test::kNestedStatus);

  while (state.KeepRunning()) {
    NestedRoot::MemoryEncoder root(nested_encode_buf);
    PW_CHECK_OK(root.WriteTimestamp(timestamp));
    PW_CHECK_OK(root.WriteMidMessage([&](NestedMid::StreamEncoder& mid) {
      PW_TRY(mid.WriteKind(kind));
      PW_TRY(mid.WriteLeafMessage([&](NestedLeaf::StreamEncoder& leaf) {
        PW_TRY(leaf.WriteId(leaf_id));
        PW_TRY(leaf.WriteCount(leaf_count));
        return leaf.WriteData(data);
      }));
      return mid.WriteFlags(flags);
    }));
    PW_CHECK_OK(root.WriteStatus(status));
    PW_CHECK_OK(root.status());
    DoNotOptimize(ConstByteSpan(root));
  }
}
PW_PERF_TEST(NestedTinyMemoryEncoder,
             NestedMemoryEncoder,
             test::kNestedTinyData);
PW_PERF_TEST(NestedLargeMemoryEncoder,
             NestedMemoryEncoder,
             test::kNestedLargeData);

void ResetNestedMessage(NestedRoot::Message& msg) {
  msg.timestamp = 0;
  msg.mid.kind = 0;
  msg.mid.leaf.id = 0;
  msg.mid.leaf.count = 0;
  msg.mid.leaf.data.clear();
  msg.mid.flags = 0;
  msg.status = 0;
}

void NestedStructEncode(perf_test::State& state, ConstByteSpan data) {
  ResetNestedMessage(nested_msg);
  nested_msg.timestamp = test::kNestedTimestamp;
  nested_msg.mid.kind = test::kNestedKind;
  nested_msg.mid.leaf.id = test::kNestedLeafId;
  nested_msg.mid.leaf.count = test::kNestedLeafCount;
  nested_msg.mid.leaf.data.assign(data.begin(), data.end());
  nested_msg.mid.flags = test::kNestedFlags;
  nested_msg.status = test::kNestedStatus;
  MakeOpaque(nested_msg);

  while (state.KeepRunning()) {
    NestedRoot::MemoryEncoder encoder(nested_encode_buf);
    PW_CHECK_OK(encoder.Write(nested_msg));
    DoNotOptimize(ConstByteSpan(encoder));
  }
}
PW_PERF_TEST(NestedTinyStructEncode, NestedStructEncode, test::kNestedTinyData);
PW_PERF_TEST(NestedLargeStructEncode,
             NestedStructEncode,
             test::kNestedLargeData);

void NestedStructDecode(perf_test::State& state, ConstByteSpan payload) {
  payload = Opaque(payload);
  while (state.KeepRunning()) {
    // Reset in-place per iteration since pwpb's Read() merges into the
    // destination struct (avoiding a ~1 KB stack temporary).
    ResetNestedMessage(nested_msg);
    stream::MemoryReader reader(payload);
    NestedRoot::StreamDecoder decoder(reader);
    PW_CHECK_OK(decoder.Read(nested_msg));
    DoNotOptimize(nested_msg);
  }
}
PW_PERF_TEST(NestedTinyStructDecode,
             NestedStructDecode,
             test::kEncodedNestedTiny);
PW_PERF_TEST(NestedLargeStructDecode,
             NestedStructDecode,
             test::kEncodedNestedLarge);

struct NestedValues {
  uint64_t timestamp = 0;
  uint32_t kind = 0;
  uint64_t leaf_id = 0;
  uint32_t leaf_count = 0;
  std::byte leaf_data[NestedLeaf::kDataMaxSize];
  size_t leaf_data_size = 0;
  uint32_t flags = 0;
  uint32_t status = 0;

  void ResetScalars() {
    timestamp = 0;
    kind = 0;
    leaf_id = 0;
    leaf_count = 0;
    leaf_data_size = 0;
    flags = 0;
    status = 0;
  }
};

NestedValues nested_values;

void ReadNestedLeaf(NestedLeaf::StreamDecoder& leaf, NestedValues& values) {
  Status status;
  while ((status = leaf.Next()).ok()) {
    switch (*leaf.Field()) {
      case NestedLeaf::Fields::kId:
        values.leaf_id = leaf.ReadId().value();
        break;
      case NestedLeaf::Fields::kCount:
        values.leaf_count = leaf.ReadCount().value();
        break;
      case NestedLeaf::Fields::kData: {
        const StatusWithSize sws = leaf.ReadData(values.leaf_data);
        PW_CHECK_OK(sws.status());
        values.leaf_data_size = sws.size();
        break;
      }
    }
  }
  PW_CHECK(status.IsOutOfRange());
}

void ReadNestedMid(NestedMid::StreamDecoder& mid, NestedValues& values) {
  Status status;
  while ((status = mid.Next()).ok()) {
    switch (*mid.Field()) {
      case NestedMid::Fields::kKind:
        values.kind = mid.ReadKind().value();
        break;
      case NestedMid::Fields::kLeaf: {
        NestedLeaf::StreamDecoder leaf = mid.GetLeafDecoder();
        ReadNestedLeaf(leaf, values);
        break;
      }
      case NestedMid::Fields::kFlags:
        values.flags = mid.ReadFlags().value();
        break;
    }
  }
  PW_CHECK(status.IsOutOfRange());
}

void NestedStreamDecoder(perf_test::State& state, ConstByteSpan payload) {
  payload = Opaque(payload);
  while (state.KeepRunning()) {
    nested_values.ResetScalars();
    stream::MemoryReader reader(payload);
    NestedRoot::StreamDecoder decoder(reader);
    Status status;
    while ((status = decoder.Next()).ok()) {
      switch (*decoder.Field()) {
        case NestedRoot::Fields::kTimestamp:
          nested_values.timestamp = decoder.ReadTimestamp().value();
          break;
        case NestedRoot::Fields::kMid: {
          NestedMid::StreamDecoder mid = decoder.GetMidDecoder();
          ReadNestedMid(mid, nested_values);
          break;
        }
        case NestedRoot::Fields::kStatus:
          nested_values.status = decoder.ReadStatus().value();
          break;
      }
    }
    PW_CHECK(status.IsOutOfRange());
    DoNotOptimize(nested_values);
  }
}
PW_PERF_TEST(NestedTinyStreamDecoder,
             NestedStreamDecoder,
             test::kEncodedNestedTiny);
PW_PERF_TEST(NestedLargeStreamDecoder,
             NestedStreamDecoder,
             test::kEncodedNestedLarge);

}  // namespace
}  // namespace pw::protobuf
