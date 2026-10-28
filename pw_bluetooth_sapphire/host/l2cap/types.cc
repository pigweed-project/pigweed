// Copyright 2023 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"

#include <pw_assert/check.h>

#include <cinttypes>

namespace bt::l2cap {
namespace {
// Size for a string/buffer that ensures that pw::ToString<AnyChannelMode> will
// succeed.
constexpr size_t kAnyChannelModeMaxStringSize = 40;
}  // namespace

std::string AnyChannelModeToString(const AnyChannelMode& mode) {
  std::string buffer(kAnyChannelModeMaxStringSize, 0);
  pw::StatusWithSize result = pw::ToString(mode, buffer);
  PW_CHECK(result.ok());
  buffer.resize(result.size());
  return buffer;
}

pw::StatusWithSize AnyChannelModeToPwString(const AnyChannelMode& mode,
                                            pw::span<char> buffer) {
  return std::visit(
      [&](auto content) -> pw::StatusWithSize {
        if constexpr (std::is_same_v<decltype(content),
                                     RetransmissionAndFlowControlMode>) {
          static_assert(
              std::string_view("(RetransmissionAndFlowControlMode) 0x00")
                  .size() < kAnyChannelModeMaxStringSize);
          return pw::string::Format(
              buffer,
              "(RetransmissionAndFlowControlMode) %#.2" PRIx8,
              static_cast<uint8_t>(content));
        } else if constexpr (std::is_same_v<decltype(content),
                                            CreditBasedFlowControlMode>) {
          static_assert(
              std::string_view("(CreditBasedFlowControlMode) 0x00").size() <
              kAnyChannelModeMaxStringSize);
          return pw::string::Format(buffer,
                                    "(CreditBasedFlowControlMode) %#.2" PRIx8,
                                    static_cast<uint8_t>(content));
        }
      },
      mode);
}
}  // namespace bt::l2cap

namespace pw {
template <>
StatusWithSize ToString(const bt::l2cap::AnyChannelMode& value,
                        span<char> buffer) {
  return bt::l2cap::AnyChannelModeToPwString(value, buffer);
}
}  // namespace pw
