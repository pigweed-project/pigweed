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

#include "pw_multibuf/v1/stream.h"

#include <algorithm>
#include <cstring>

#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_status/try.h"
#include "pw_stream/seek.h"

namespace pw::multibuf::v1 {

Status Stream::DoWrite(ConstByteSpan data) {
  while (!data.empty()) {
    if (iterator_ == multibuf_.end()) {
      return Status::OutOfRange();
    }

    Chunk* current_chunk = iterator_.chunk();
    const size_t remaining_in_chunk =
        current_chunk->size() - iterator_.byte_index();

    const size_t bytes_to_copy = std::min(data.size(), remaining_in_chunk);

    std::memcpy(current_chunk->data() + iterator_.byte_index(),
                data.data(),
                bytes_to_copy);

    data = data.subspan(bytes_to_copy);
    multibuf_offset_ += bytes_to_copy;
    iterator_ += bytes_to_copy;
  }

  return OkStatus();
}

StatusWithSize Stream::DoRead(ByteSpan destination) {
  size_t bytes_written = 0;

  while (!destination.empty()) {
    if (iterator_ == multibuf_.end()) {
      if (bytes_written > 0) {
        return StatusWithSize(bytes_written);
      }
      return StatusWithSize::OutOfRange();
    }

    Chunk* current_chunk = iterator_.chunk();
    const size_t remaining_in_chunk =
        current_chunk->size() - iterator_.byte_index();

    const size_t bytes_to_copy =
        std::min(remaining_in_chunk, destination.size());
    std::memcpy(destination.data(),
                current_chunk->data() + iterator_.byte_index(),
                bytes_to_copy);

    destination = destination.subspan(bytes_to_copy);
    bytes_written += bytes_to_copy;
    multibuf_offset_ += bytes_to_copy;
    iterator_ += bytes_to_copy;
  }

  return StatusWithSize(bytes_written);
}

Status Stream::DoSeek(ptrdiff_t offset, Whence origin) {
  size_t new_offset = multibuf_offset_;
  PW_TRY(stream::CalculateSeek(offset, origin, multibuf_.size(), new_offset));

  if (new_offset < multibuf_offset_) {
    return Status::OutOfRange();
  }

  iterator_ += new_offset - multibuf_offset_;
  multibuf_offset_ = new_offset;
  return iterator_ == multibuf_.end() ? Status::OutOfRange() : OkStatus();
}

}  // namespace pw::multibuf::v1
