// Copyright 2025 The Pigweed Authors
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

#include "pw_bluetooth_proxy/comms2/h4_packet.h"

#include "pw_assert/check.h"
#include "pw_multibuf/v2/multibuf.h"
#include "pw_status/try.h"

namespace pw::bluetooth::proxy {

using ::pw::multibuf::v2::MultiBuf;

bool H4Packet::CanConstructFrom(const MultiBuf& buffer) {
  return buffer.size() >= kMinSize && buffer.NumLayers() >= kLayer;
}

Status H4Packet::Prepare(MultiBuf& buffer) {
  if (!CanConstructFrom(buffer)) {
    return Status::InvalidArgument();
  }

  return multibuf().TryReserveForPushBack(buffer) ? OkStatus()
                                                  : Status::ResourceExhausted();
}

void H4Packet::Assign(MultiBuf&& buffer) {
  PW_CHECK(CanConstructFrom(buffer),
           "Buffer does not contain a valid H4 packet");

  while (buffer.NumLayers() > H4Packet::kLayer) {
    buffer.PopLayer();
  }

  multibuf().PushBack(std::move(buffer));

  type_ = static_cast<H4Packet::Type>((*this)[0]);
  AddLayer(sizeof(Type));
}

}  // namespace pw::bluetooth::proxy
