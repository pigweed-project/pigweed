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

#include <string>

#include "pw_status/try.h"
#include "pw_stream/stream.h"

namespace pw::elf::internal {

// TODO(jrreinhart): Move to pw_stream
template <class T>
Result<T> ReadObject(stream::Reader& stream) {
  static_assert(std::is_trivially_copyable_v<T>);

  std::array<std::byte, sizeof(T)> buffer;
  PW_TRY(stream.ReadExact(buffer));

  T object;
  std::memcpy(&object, buffer.data(), buffer.size());
  return object;
}

// TODO(jrreinhart): Move to pw_stream
Result<std::string> ReadNullTermString(stream::Reader& stream);

}  // namespace pw::elf::internal
