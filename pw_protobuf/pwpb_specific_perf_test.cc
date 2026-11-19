// Copyright 2026 The Pigweed Authors
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

// Performance tests for pw_protobuf-specific APIs.
//
// Tests for operations that any protobuf library supports belong in
// pwpb_perf_test.cc. Test names here use the same <Message><Operation> scheme
// so results can be compared against those tests.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "pw_assert/check.h"
#include "pw_bytes/span.h"
#include "pw_perf_test/perf_test.h"
#include "pw_protobuf/buffer_encoder.h"
#include "pw_protobuf/decoder.h"
#include "pw_protobuf/encoder.h"
#include "pw_protobuf_private/test_data_perf_test.h"
#include "pw_protobuf_test_protos/perf_test.pwpb.h"
#include "pw_span/span.h"
#include "pw_status/status.h"

namespace pw::protobuf {
namespace {

using test::DoNotOptimize;
using test::Opaque;
namespace ItemInfo = test::pwpb::ItemInfo;
namespace Location = test::pwpb::Location;
namespace NestedLeaf = test::pwpb::NestedLeaf;
namespace NestedMid = test::pwpb::NestedMid;
namespace NestedRoot = test::pwpb::NestedRoot;
namespace SensorReport = test::pwpb::SensorReport;

// ============================================================================
// 1. Simple Scalar/Enum Message: ItemInfo (3 fields, 11-byte wire payload)
// ============================================================================

void ItemInfoBufferEncoder(perf_test::State& state) {
  const uint64_t offset = Opaque(test::kItemOffset);
  const uint32_t size = Opaque(test::kItemSize);
  const ItemInfo::Access access_level =
      Opaque(ItemInfo::Access::READ_AND_WRITE);
  std::byte buf[ItemInfo::kMaxEncodedSizeBytes];

  while (state.KeepRunning()) {
    ItemInfo::BufferEncoder encoder(buf);
    PW_CHECK_OK(encoder.WriteOffset(offset));
    PW_CHECK_OK(encoder.WriteSize(size));
    PW_CHECK_OK(encoder.WriteAccessLevel(access_level));
    PW_CHECK_OK(encoder.status());
    DoNotOptimize(ConstByteSpan(encoder));
  }
}
PW_PERF_TEST(ItemInfoBufferEncoder, ItemInfoBufferEncoder);

// Untyped, span-based wire decoder (pw::protobuf::Decoder).
void ItemInfoWireDecoder(perf_test::State& state) {
  const ConstByteSpan encoded = Opaque(ConstByteSpan(test::kEncodedItemInfo));
  while (state.KeepRunning()) {
    Decoder decoder(encoded);
    uint64_t offset = 0;
    uint32_t size = 0;
    uint32_t access = 0;
    Status status;
    while ((status = decoder.Next()).ok()) {
      switch (decoder.FieldNumber()) {
        case static_cast<uint32_t>(ItemInfo::Fields::kOffset):
          PW_CHECK_OK(decoder.ReadUint64(&offset));
          break;
        case static_cast<uint32_t>(ItemInfo::Fields::kSize):
          PW_CHECK_OK(decoder.ReadUint32(&size));
          break;
        case static_cast<uint32_t>(ItemInfo::Fields::kAccessLevel):
          PW_CHECK_OK(decoder.ReadUint32(&access));
          break;
      }
    }
    PW_CHECK(status.IsOutOfRange());
    DoNotOptimize(offset);
    DoNotOptimize(size);
    DoNotOptimize(access);
  }
}
PW_PERF_TEST(ItemInfoWireDecoder, ItemInfoWireDecoder);

// ============================================================================
// 2. Realistic Composite Message: SensorReport (80-byte wire payload)
// ============================================================================

// Same as SensorReportMemoryEncoder in pwpb_perf_test.cc, but writes the
// submessage with a scoped Get<Field>Encoder() instead of a callback.
void SensorReportMemoryEncoderGetNested(perf_test::State& state) {
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
    {
      Location::StreamEncoder loc = encoder.GetLocationEncoder();
      PW_CHECK_OK(loc.WriteX(loc_x));
      PW_CHECK_OK(loc.WriteY(loc_y));
    }
    PW_CHECK_OK(encoder.status());
    DoNotOptimize(ConstByteSpan(encoder));
  }
}
PW_PERF_TEST(SensorReportMemoryEncoderGetNested,
             SensorReportMemoryEncoderGetNested);

void SensorReportBufferEncoder(perf_test::State& state) {
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
    SensorReport::BufferEncoder encoder(buf);
    PW_CHECK_OK(encoder.WriteDeviceId(device_id));
    PW_CHECK_OK(encoder.WriteActive(active));
    PW_CHECK_OK(encoder.WriteTemperatureMc(temperature_mc));
    PW_CHECK_OK(encoder.WriteLabel(label));
    PW_CHECK_OK(encoder.WritePayload(payload));
    PW_CHECK_OK(encoder.WriteSamples(samples));
    {
      Location::BufferEncoder loc = encoder.GetLocationEncoder();
      PW_CHECK_OK(loc.WriteX(loc_x));
      PW_CHECK_OK(loc.WriteY(loc_y));
    }
    PW_CHECK_OK(encoder.status());
    DoNotOptimize(ConstByteSpan(encoder));
  }
}
PW_PERF_TEST(SensorReportBufferEncoder, SensorReportBufferEncoder);

// ============================================================================
// 5. 3-Level Nested Message: NestedRoot (Tiny: 4B leaf, Large: 1000B leaf)
// ============================================================================

std::byte nested_encode_buf[NestedRoot::kMaxEncodedSizeBytes];

// Same as NestedMemoryEncoder in pwpb_perf_test.cc, but writes submessages
// with scoped Get<Field>Encoder() calls instead of callbacks.
void NestedMemoryEncoderGetNested(perf_test::State& state, ConstByteSpan data) {
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
    {
      NestedMid::StreamEncoder mid = root.GetMidEncoder();
      PW_CHECK_OK(mid.WriteKind(kind));
      {
        NestedLeaf::StreamEncoder leaf = mid.GetLeafEncoder();
        PW_CHECK_OK(leaf.WriteId(leaf_id));
        PW_CHECK_OK(leaf.WriteCount(leaf_count));
        PW_CHECK_OK(leaf.WriteData(data));
      }
      PW_CHECK_OK(mid.WriteFlags(flags));
    }
    PW_CHECK_OK(root.WriteStatus(status));
    PW_CHECK_OK(root.status());
    DoNotOptimize(ConstByteSpan(root));
  }
}
PW_PERF_TEST(NestedTinyMemoryEncoderGetNested,
             NestedMemoryEncoderGetNested,
             test::kNestedTinyData);
PW_PERF_TEST(NestedLargeMemoryEncoderGetNested,
             NestedMemoryEncoderGetNested,
             test::kNestedLargeData);

void NestedBufferEncoder(perf_test::State& state, ConstByteSpan data) {
  data = Opaque(data);
  const uint64_t timestamp = Opaque(test::kNestedTimestamp);
  const uint32_t kind = Opaque(test::kNestedKind);
  const uint64_t leaf_id = Opaque(test::kNestedLeafId);
  const uint32_t leaf_count = Opaque(test::kNestedLeafCount);
  const uint32_t flags = Opaque(test::kNestedFlags);
  const uint32_t status = Opaque(test::kNestedStatus);

  while (state.KeepRunning()) {
    NestedRoot::BufferEncoder root(nested_encode_buf);
    PW_CHECK_OK(root.WriteTimestamp(timestamp));
    {
      NestedMid::BufferEncoder mid = root.GetMidEncoder();
      PW_CHECK_OK(mid.WriteKind(kind));
      {
        NestedLeaf::BufferEncoder leaf = mid.GetLeafEncoder();
        PW_CHECK_OK(leaf.WriteId(leaf_id));
        PW_CHECK_OK(leaf.WriteCount(leaf_count));
        PW_CHECK_OK(leaf.WriteData(data));
      }
      PW_CHECK_OK(mid.WriteFlags(flags));
    }
    PW_CHECK_OK(root.WriteStatus(status));
    PW_CHECK_OK(root.status());
    DoNotOptimize(ConstByteSpan(root));
  }
}
PW_PERF_TEST(NestedTinyBufferEncoder,
             NestedBufferEncoder,
             test::kNestedTinyData);
PW_PERF_TEST(NestedLargeBufferEncoder,
             NestedBufferEncoder,
             test::kNestedLargeData);

void NestedBufferEncoderUnchecked(perf_test::State& state, ConstByteSpan data) {
  data = Opaque(data);
  const uint64_t timestamp = Opaque(test::kNestedTimestamp);
  const uint32_t kind = Opaque(test::kNestedKind);
  const uint64_t leaf_id = Opaque(test::kNestedLeafId);
  const uint32_t leaf_count = Opaque(test::kNestedLeafCount);
  const uint32_t flags = Opaque(test::kNestedFlags);
  const uint32_t status = Opaque(test::kNestedStatus);

  while (state.KeepRunning()) {
    NestedRoot::BufferEncoder root(nested_encode_buf);
    if (root.EnsureSpace(NestedRoot::kMaxEncodedSizeBytes)) {
      root.UncheckedWriteTimestamp(timestamp);
      {
        NestedMid::BufferEncoder mid = root.UncheckedGetMidEncoder();
        mid.UncheckedWriteKind(kind);
        {
          NestedLeaf::BufferEncoder leaf = mid.UncheckedGetLeafEncoder();
          leaf.UncheckedWriteId(leaf_id);
          leaf.UncheckedWriteCount(leaf_count);
          leaf.UncheckedWriteData(data);
        }
        mid.UncheckedWriteFlags(flags);
      }
      root.UncheckedWriteStatus(status);
    }
    PW_CHECK_OK(root.status());
    DoNotOptimize(ConstByteSpan(root));
  }
}
PW_PERF_TEST(NestedTinyBufferEncoderUnchecked,
             NestedBufferEncoderUnchecked,
             test::kNestedTinyData);
PW_PERF_TEST(NestedLargeBufferEncoderUnchecked,
             NestedBufferEncoderUnchecked,
             test::kNestedLargeData);

}  // namespace
}  // namespace pw::protobuf
