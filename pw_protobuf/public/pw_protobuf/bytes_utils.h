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
#pragma once

#include "pw_bytes/endian.h"
#include "pw_bytes/span.h"
#include "pw_protobuf/decoder.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace pw::protobuf {

/// @module{pw_protobuf}

/// Decodes a proto message bytes field to a `uint32_t` value.
///
/// @warning The caller must advance the decoder using `Next()` and verify the
/// field number using `FieldNumber()` prior to calling this function;
/// otherwise, behavior is undefined.
///
/// @code
///   protobuf::Decoder decoder(request);
///   if (!decoder.Next().ok()) {
///     // Handle error.
///   }
///   if (static_cast<MyProtoMessage::Fields>(decoder.FieldNumber()) !=
///       MyProtoMessage::Fields::kMyFields) {
///     // Handle error.
///   }
///   Result<uint32_t> result = DecodeBytesToUint32(decoder);
///   if (result.ok()) {
///     // Do something with result.value().
///   }
/// @endcode
///
/// @param[in,out] decoder The decoder currently positioned at the bytes field.
///
/// @returns @Result{the decoded `uint32_t` value}
/// * @DATA_LOSS: Invalid protobuf data.
/// * @INVALID_ARGUMENT: Not able to read exactly 4 bytes from the field.
/// * @FAILED_PRECONDITION: No bytes were read.
inline Result<uint32_t> DecodeBytesToUint32(Decoder& decoder) {
  ConstByteSpan bytes_read;
  PW_TRY(decoder.ReadBytes(&bytes_read));
  if (bytes_read.size() != sizeof(uint32_t)) {
    return Status::InvalidArgument();
  }
  uint32_t value;
  if (!bytes::ReadInOrder(endian::little, bytes_read, value)) {
    return Status::Internal();
  }
  return value;
}

/// @endmodule

}  // namespace pw::protobuf
