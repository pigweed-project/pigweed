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

#include <string_view>

#include "pw_bytes/span.h"
#include "pw_perf_test/perf_test.h"
#include "pw_protobuf/buffer_encoder.h"
#include "pw_protobuf/encoder.h"
#include "pw_protobuf_test_protos/full_test.pwpb.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_stream/memory_stream.h"

namespace pw::protobuf {
namespace {

namespace Pigweed = test::pwpb::Pigweed;

template <typename T>
inline void DoNotOptimize(const T& value) {
#if defined(__clang__) || defined(__GNUC__)
  asm volatile("" : : "r,m"(value) : "memory");
#else
  // Fallback if not GCC/Clang
  static_cast<void>(value);
#endif
}

void BasicIntegerPerformance(pw::perf_test::State& state, uint32_t value) {
  std::byte encode_buffer[30];

  while (state.KeepRunning()) {
    MemoryEncoder encoder(encode_buffer);
    encoder.WriteUint32(1, value).IgnoreError();
    DoNotOptimize(encode_buffer);
  }
}

PW_PERF_TEST(SmallIntegerEncoding, BasicIntegerPerformance, 1);
PW_PERF_TEST(LargerIntegerEncoding, BasicIntegerPerformance, 4000000000);

// Tiny message benchmarks (~20 bytes)
void StreamEncodeNestedTiny(pw::perf_test::State& state) {
  std::byte encode_buffer[1024];
  while (state.KeepRunning()) {
    Pigweed::MemoryEncoder pigweed(encode_buffer);
    pigweed.WriteMagicNumber(42).IgnoreError();
    {
      auto proto = pigweed.GetProtoEncoder();
      {
        auto meta = proto.GetMetaEncoder();
        meta.WriteFileName("test.proto").IgnoreError();
        meta.WriteStatus(Pigweed::Protobuf::Compiler::Status::OK).IgnoreError();
      }
    }
    pigweed.status().IgnoreError();
    DoNotOptimize(encode_buffer);
  }
}
PW_PERF_TEST(StreamEncodeNestedMessageTiny, StreamEncodeNestedTiny);

void BufferEncodeNestedCheckedTiny(pw::perf_test::State& state) {
  std::byte encode_buffer[1024];
  while (state.KeepRunning()) {
    Pigweed::BufferEncoder pigweed(encode_buffer);
    pigweed.WriteMagicNumber(42).IgnoreError();
    {
      auto proto = pigweed.GetProtoEncoder();
      {
        auto meta = proto.GetMetaEncoder();
        meta.WriteFileName("test.proto").IgnoreError();
        meta.WriteStatus(Pigweed::Protobuf::Compiler::Status::OK).IgnoreError();
      }
    }
    pigweed.status().IgnoreError();
    DoNotOptimize(encode_buffer);
  }
}
PW_PERF_TEST(BufferEncodeNestedMessageCheckedTiny,
             BufferEncodeNestedCheckedTiny);

void BufferEncodeNestedUncheckedTiny(pw::perf_test::State& state) {
  std::byte encode_buffer[1024];
  while (state.KeepRunning()) {
    Pigweed::BufferEncoder pigweed(encode_buffer);
    if (pigweed.EnsureSpace(100)) {
      pigweed.UncheckedWriteMagicNumber(42);
      {
        auto proto = pigweed.UncheckedGetProtoEncoder();
        {
          auto meta = proto.UncheckedGetMetaEncoder();
          meta.UncheckedWriteFileName("test.proto");
          meta.UncheckedWriteStatus(Pigweed::Protobuf::Compiler::Status::OK);
        }
      }
    }
    pigweed.status().IgnoreError();
    DoNotOptimize(encode_buffer);
  }
}
PW_PERF_TEST(BufferEncodeNestedMessageUncheckedTiny,
             BufferEncodeNestedUncheckedTiny);

// Large message benchmarks (~1KB)
constexpr std::string_view kLargeString =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaa";

void StreamEncodeNestedLarge(pw::perf_test::State& state) {
  std::byte encode_buffer[2048];
  while (state.KeepRunning()) {
    Pigweed::MemoryEncoder pigweed(encode_buffer);
    pigweed.WriteMagicNumber(42).IgnoreError();
    {
      auto proto = pigweed.GetProtoEncoder();
      {
        auto meta = proto.GetMetaEncoder();
        meta.WriteFileName(kLargeString).IgnoreError();
        meta.WriteStatus(Pigweed::Protobuf::Compiler::Status::OK).IgnoreError();
      }
    }
    pigweed.status().IgnoreError();
    DoNotOptimize(encode_buffer);
  }
}
PW_PERF_TEST(StreamEncodeNestedMessageLarge, StreamEncodeNestedLarge);

void BufferEncodeNestedCheckedLarge(pw::perf_test::State& state) {
  std::byte encode_buffer[2048];
  while (state.KeepRunning()) {
    Pigweed::BufferEncoder pigweed(encode_buffer);
    pigweed.WriteMagicNumber(42).IgnoreError();
    {
      auto proto = pigweed.UncheckedGetProtoEncoder();
      {
        auto meta = proto.UncheckedGetMetaEncoder();
        meta.WriteFileName(kLargeString).IgnoreError();
        meta.WriteStatus(Pigweed::Protobuf::Compiler::Status::OK).IgnoreError();
      }
    }
    pigweed.status().IgnoreError();
    DoNotOptimize(encode_buffer);
  }
}
PW_PERF_TEST(BufferEncodeNestedMessageCheckedLarge,
             BufferEncodeNestedCheckedLarge);

void BufferEncodeNestedUncheckedLarge(pw::perf_test::State& state) {
  std::byte encode_buffer[2048];
  while (state.KeepRunning()) {
    Pigweed::BufferEncoder pigweed(encode_buffer);
    if (pigweed.EnsureSpace(1100)) {
      pigweed.UncheckedWriteMagicNumber(42);
      {
        auto proto = pigweed.UncheckedGetProtoEncoder();
        {
          auto meta = proto.UncheckedGetMetaEncoder();
          meta.UncheckedWriteFileName(kLargeString);
          meta.UncheckedWriteStatus(Pigweed::Protobuf::Compiler::Status::OK);
        }
      }
    }
    pigweed.status().IgnoreError();
    DoNotOptimize(encode_buffer);
  }
}
PW_PERF_TEST(BufferEncodeNestedMessageUncheckedLarge,
             BufferEncodeNestedUncheckedLarge);

}  // namespace
}  // namespace pw::protobuf
