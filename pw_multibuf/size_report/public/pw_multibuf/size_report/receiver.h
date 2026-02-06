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
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "pw_async2/channel.h"
#include "pw_async2/context.h"
#include "pw_async2/task.h"
#include "pw_async2/try.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/examples/protocol.h"
#include "pw_multibuf/size_report/handler.h"

namespace pw::multibuf::size_report {

class BasicReceiver {
 protected:
  constexpr size_t remaining() const { return remaining_; }
  constexpr size_t offset() const { return offset_; }

  template <typename T>
  ByteSpan AsWritableBytes(T& t) {
    return as_writable_bytes(span(&t, 1));
  }

  void CheckDemoLinkHeader(const examples::DemoLinkHeader& demo_link_header);

  void CheckDemoLinkFooter(const examples::DemoLinkFooter& demo_link_footer,
                           uint32_t checksum);

  void CheckDemoNetworkHeader(
      const examples::DemoNetworkHeader& demo_network_header,
      size_t payload_len);

  void CheckDemoTransportFirstHeader(
      const examples::DemoTransportFirstHeader& demo_transport_header);

  void CheckDemoTransportHeader(
      const examples::DemoTransportHeader& demo_transport_header);

 private:
  uint64_t segment_id_;
  size_t offset_ = 0;
  size_t remaining_ = 0;
};

template <typename MultiBufType>
class Receiver : public virtual FrameHandler<MultiBufType>,
                 public BasicReceiver {
 public:
  template <typename... MultiBufArgs>
  Receiver(async2::Receiver<MultiBufType>&& receiver, MultiBufArgs&&... args)
      : receiver_(std::move(receiver)),
        received_(std::forward<MultiBufArgs>(args)...) {}

  std::optional<MultiBufType> TakeReceived();

 private:
  async2::Poll<> DoPend(async2::Context& context) override;
  void HandleDemoLinkFrame(MultiBufType& frame);
  void HandleDemoNetworkPacket(MultiBufType& packet);
  void HandleDemoTransportFirstSegment(MultiBufType& segment);
  void HandleDemoTransportSegment(MultiBufType& segment);

  async2::Receiver<MultiBufType> receiver_;
  std::optional<async2::ReceiveFuture<MultiBufType>> receive_future_;
  MultiBufType received_;
};

// Template method implementations.

template <typename MultiBufType>
std::optional<MultiBufType> Receiver<MultiBufType>::TakeReceived() {
  if (remaining() != 0) {
    return std::nullopt;
  }
  MultiBufType received = std::move(received_);
  return received;
}

template <typename MultiBufType>
async2::Poll<> Receiver<MultiBufType>::DoPend(async2::Context& context) {
  while (remaining() != 0 || internal::GetMultiBuf(received_).empty()) {
    if (!receive_future_.has_value()) {
      receive_future_ = receiver_.Receive();
    }
    PW_TRY_READY_ASSIGN(auto result, receive_future_->Pend(context));
    receive_future_.reset();
    if (!result.has_value()) {
      break;
    }
    MultiBufType mb = std::move(*result);
    HandleDemoLinkFrame(mb);
    FrameHandler<MultiBufType>::PushBack(received_, std::move(mb));
  }

  return async2::Ready();
}

template <typename MultiBufType>
void Receiver<MultiBufType>::HandleDemoLinkFrame(MultiBufType& frame) {
  auto& mb = internal::GetMultiBuf(frame);
  examples::DemoLinkHeader demo_link_header;
  examples::DemoLinkFooter demo_link_footer;
  std::ignore = mb.CopyTo(AsWritableBytes(demo_link_header));
  size_t frame_len = sizeof(examples::DemoLinkHeader) + demo_link_header.length;
  std::ignore = mb.CopyTo(AsWritableBytes(demo_link_footer), frame_len);

  CheckDemoLinkHeader(demo_link_header);
  CheckDemoLinkFooter(demo_link_footer,
                      FrameHandler<MultiBufType>::CalculateChecksum(frame));

  FrameHandler<MultiBufType>::Narrow(
      frame, sizeof(examples::DemoLinkHeader), demo_link_header.length);
  HandleDemoNetworkPacket(frame);
}

template <typename MultiBufType>
void Receiver<MultiBufType>::HandleDemoNetworkPacket(MultiBufType& packet) {
  auto& mb = internal::GetMultiBuf(packet);
  examples::DemoNetworkHeader demo_network_header;
  std::ignore = mb.CopyTo(AsWritableBytes(demo_network_header));
  FrameHandler<MultiBufType>::Narrow(packet,
                                     sizeof(examples::DemoNetworkHeader));
  CheckDemoNetworkHeader(demo_network_header, mb.size());
  if (offset() == 0) {
    HandleDemoTransportFirstSegment(packet);
  } else {
    HandleDemoTransportSegment(packet);
  }
}

template <typename MultiBufType>
void Receiver<MultiBufType>::HandleDemoTransportFirstSegment(
    MultiBufType& segment) {
  examples::DemoTransportFirstHeader demo_transport_header;
  std::ignore = internal::GetMultiBuf(segment).CopyTo(
      AsWritableBytes(demo_transport_header));
  CheckDemoTransportFirstHeader(demo_transport_header);
  FrameHandler<MultiBufType>::Narrow(
      segment, sizeof(examples::DemoTransportFirstHeader));
}

template <typename MultiBufType>
void Receiver<MultiBufType>::HandleDemoTransportSegment(MultiBufType& segment) {
  examples::DemoTransportHeader demo_transport_header;
  std::ignore = internal::GetMultiBuf(segment).CopyTo(
      AsWritableBytes(demo_transport_header));
  CheckDemoTransportHeader(demo_transport_header);
  FrameHandler<MultiBufType>::Narrow(segment,
                                     sizeof(examples::DemoTransportHeader));
}

}  // namespace pw::multibuf::size_report
