// Copyright 2021 The Pigweed Authors
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
//
// The header provides a set of helper utils for protobuf related operations.
// The APIs may not be finalized yet.

#pragma once

#include <cstddef>
#include <string_view>

#include "pw_assert/check.h"
#include "pw_protobuf/stream_decoder.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_stream/stream.h"

namespace pw::protobuf {

/// @module{pw_protobuf}

/// Writes an entry for the proto `map<string, bytes>` field type.
///
/// Since all length-delimited fields can be treated as `bytes`, this function
/// can be used to write any string-keyed map entry with length-delimited
/// values, such as `map<string, message>` or `map<string, bytes>`.
///
/// @param[in] field_number The field number for the map.
/// @param[in,out] key Stream reader for the string key payload.
/// @param[in] key_size Number of bytes in the key.
/// @param[in,out] value Stream reader for the value payload.
/// @param[in] value_size Number of bytes in the value.
/// @param[in] stream_pipe_buffer A non-zero sized buffer used for reading data
/// from the readers and staging it to the writer.
/// @param[in,out] writer The output writer to write serialized map entry data
/// to.
///
/// @returns
/// * @OK: Entry successfully written.
/// * @RESOURCE_EXHAUSTED: Entry would exceed the writer limit.
/// * @INVALID_ARGUMENT: Field number is invalid
/// (`!ValidFieldNumber(field_number)`).
Status WriteProtoStringToBytesMapEntry(uint32_t field_number,
                                       stream::Reader& key,
                                       size_t key_size,
                                       stream::Reader& value,
                                       size_t value_size,
                                       ByteSpan stream_pipe_buffer,
                                       stream::Writer& writer);

/// @endmodule

}  // namespace pw::protobuf
