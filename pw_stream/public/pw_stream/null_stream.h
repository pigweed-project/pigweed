// Copyright 2020 The Pigweed Authors
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
#pragma once

#include <cstddef>

#include "pw_bytes/span.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_stream/stream.h"

namespace pw::stream {

/// @submodule{pw_stream,concrete}

/// No-op stream implementation, similar to `/dev/null`. Writes are always
/// dropped. Reads always return `OUT_OF_RANGE`. Seeks have no effect.
class NullStream final : public SeekableReaderWriter {
 public:
  // Gives access to a global NullStream instance. It is not necessary to have
  // multiple NullStream instances since they have no state and do nothing.
  static NullStream& Instance() {
    PW_CONSTINIT static NullStream stream;
    return stream;
  }

 private:
  Status DoWrite(ConstByteSpan) final { return OkStatus(); }
  StatusWithSize DoRead(ByteSpan) final { return StatusWithSize::OutOfRange(); }
  Status DoSeek(ptrdiff_t, Whence) final { return OkStatus(); }
};

/// Same as `pw::stream::NullStream`, but tracks the number of bytes written.
class CountingNullStream final : public SeekableReaderWriter {
 public:
  constexpr CountingNullStream() : bytes_written_(0) {}

  /// @returns The number of bytes provided to previous `Write` or `Advance`
  /// calls.
  size_t bytes_written() const { return bytes_written_; }

  /// Advances the internal count without writing any data.
  void Advance(size_t count) { bytes_written_ += count; }

 private:
  Status DoWrite(ConstByteSpan data) final {
    Advance(data.size());
    return OkStatus();
  }

  StatusWithSize DoRead(ByteSpan) final { return StatusWithSize::OutOfRange(); }
  Status DoSeek(ptrdiff_t, Whence) final { return OkStatus(); }

  size_t bytes_written_;
};

/// @}

}  // namespace pw::stream
