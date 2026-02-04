// Copyright 2024 The Pigweed Authors
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

#include "pw_multibuf/config.h"
#include "pw_multibuf/v1/multibuf.h"
#include "pw_stream/stream.h"

namespace pw::multibuf::v1 {

/// @submodule{pw_multibuf,v1}

/// A readable, writable, and seekable ``Stream`` implementation backed by a
/// ``MultiBuf``.
class PW_MULTIBUF_DEPRECATED Stream : public stream::SeekableReaderWriter {
 public:
  Stream(MultiBuf& multibuf)
      : multibuf_(multibuf),
        iterator_(multibuf_.begin()),
        multibuf_offset_(0) {}

  /// Returns the ``MultiBuf`` backing this stream.
  constexpr const MultiBuf& multibuf() const { return multibuf_; }

 private:
  Status DoWrite(ConstByteSpan data) override;

  StatusWithSize DoRead(ByteSpan destination) override;

  /// Seeks the writer's cursor position within the multibuf. Only forward
  /// seeking is permitted; attempting to seek backwards will result in an
  /// `OUT_OF_RANGE`. This operation is `O(multibuf().Chunks().size())`.
  Status DoSeek(ptrdiff_t offset, Whence origin) override;

  size_t DoTell() override { return multibuf_offset_; }

  size_t ConservativeLimit(LimitType) const override {
    return multibuf_.size() - multibuf_offset_;
  }

  MultiBuf& multibuf_;
  MultiBuf::iterator iterator_;
  size_t multibuf_offset_;
};

/// @}

}  // namespace pw::multibuf::v1
