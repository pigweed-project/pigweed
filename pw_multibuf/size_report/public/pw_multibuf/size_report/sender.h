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
#include <optional>

#include "pw_async2/channel.h"
#include "pw_async2/context.h"
#include "pw_async2/task.h"
#include "pw_async2/try.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/examples/protocol.h"
#include "pw_multibuf/size_report/common.h"
#include "pw_multibuf/size_report/handler.h"

namespace pw::multibuf::size_report {

class BasicSender {
 public:
  void Send(const char* message);

 protected:
  constexpr BasicSender()
      : demo_link_header_({.src_addr = kDemoLinkSender,
                           .dst_addr = kDemoLinkReceiver,
                           .length = 0}),
        demo_link_footer_({.crc32 = 0}),
        demo_network_header_({.src_addr = kDemoNetworkSender,
                              .dst_addr = kDemoNetworkReceiver,
                              .length = 0}),
        demo_transport_header_({}) {}

  constexpr size_t offset() const { return demo_transport_header_.offset; }

  constexpr size_t remaining() const {
    return demo_transport_header_.total_length - demo_transport_header_.offset;
  }

  constexpr size_t GetFrameSize() {
    size_t overhead = sizeof(examples::DemoLinkHeader);
    overhead += sizeof(examples::DemoLinkFooter);
    overhead += sizeof(examples::DemoNetworkHeader);
    if (offset() == 0) {
      overhead += sizeof(examples::DemoTransportFirstHeader);
    } else {
      overhead += sizeof(examples::DemoTransportHeader);
    }
    size_t payload_len =
        std::min(examples::kMaxDemoLinkFrameLength - overhead, remaining());
    return overhead + payload_len;
  }

  ConstByteSpan GetDemoLinkHeader(size_t payload_len);

  ConstByteSpan GetDemoLinkFooter(uint32_t checksum);

  ConstByteSpan GetDemoNetworkHeader(size_t payload_len);

  ConstByteSpan GetDemoTransportHeader(size_t payload_len);

  ConstByteSpan GetMessageFragment(size_t segment_size);
  void AdvanceOffset(size_t off);

  async2::Waker waker_;
  ConstByteSpan message_;
  examples::DemoLinkHeader demo_link_header_;
  examples::DemoLinkFooter demo_link_footer_;
  examples::DemoNetworkHeader demo_network_header_;
  examples::DemoTransportFirstHeader demo_transport_header_;
};

template <typename MultiBufType>
class Sender : public virtual FrameHandler<MultiBufType>, public BasicSender {
 protected:
  Sender(async2::Sender<MultiBufType>&& sender) : sender_(std::move(sender)) {}

 private:
  async2::Poll<> DoPend(async2::Context& cx) override;

  void MakeDemoLinkFrame(MultiBufType& frame);

  void MakeDemoNetworkPacket(MultiBufType& packet);

  void MakeDemoTransportSegment(MultiBufType& segment);

  async2::Sender<MultiBufType> sender_;
  std::optional<async2::ReserveSendFuture<MultiBufType>> send_future_;
};

// Template method implementations.

template <typename MultiBufType>
async2::Poll<> Sender<MultiBufType>::DoPend(async2::Context& cx) {
  while (remaining() != 0) {
    if (!send_future_.has_value()) {
      send_future_ = sender_.ReserveSend();
    }
    PW_TRY_READY_ASSIGN(auto reservation, send_future_->Pend(cx));
    send_future_.reset();

    if (!reservation.has_value()) {
      break;
    }

    MultiBufType frame = FrameHandler<MultiBufType>::AllocateFrame();
    MakeDemoLinkFrame(frame);
    reservation->Commit(std::move(frame));
  }

  return async2::Ready();
}

template <typename MultiBufType>
void Sender<MultiBufType>::MakeDemoLinkFrame(MultiBufType& frame) {
  auto& mb = internal::GetMultiBuf(frame);
  size_t payload_len = mb.size() - sizeof(examples::DemoLinkHeader) -
                       sizeof(examples::DemoLinkFooter);
  FrameHandler<MultiBufType>::Narrow(
      frame, sizeof(examples::DemoLinkHeader), payload_len);
  MakeDemoNetworkPacket(frame);
  payload_len = mb.size();

  FrameHandler<MultiBufType>::Widen(frame,
                                    sizeof(examples::DemoLinkHeader),
                                    sizeof(examples::DemoLinkFooter));
  std::ignore = mb.CopyFrom(GetDemoLinkHeader(payload_len));
  uint32_t checksum = FrameHandler<MultiBufType>::CalculateChecksum(frame);
  size_t footer_offset = mb.size() - sizeof(examples::DemoLinkFooter);
  std::ignore = mb.CopyFrom(GetDemoLinkFooter(checksum), footer_offset);
}

template <typename MultiBufType>
void Sender<MultiBufType>::MakeDemoNetworkPacket(MultiBufType& packet) {
  auto& mb = internal::GetMultiBuf(packet);
  FrameHandler<MultiBufType>::Narrow(packet,
                                     sizeof(examples::DemoNetworkHeader));
  MakeDemoTransportSegment(packet);
  size_t payload_len = mb.size();

  FrameHandler<MultiBufType>::Widen(packet,
                                    sizeof(examples::DemoNetworkHeader));
  std::ignore = mb.CopyFrom(GetDemoNetworkHeader(payload_len));
}

template <typename MultiBufType>
void Sender<MultiBufType>::MakeDemoTransportSegment(MultiBufType& segment) {
  auto& mb = internal::GetMultiBuf(segment);
  const size_t original_size = mb.size();
  const size_t header_size = offset() == 0
                                 ? sizeof(examples::DemoTransportFirstHeader)
                                 : sizeof(examples::DemoTransportHeader);
  const size_t payload_len = mb.size() - header_size;

  ConstByteSpan fragment = GetMessageFragment(payload_len);
  std::ignore = mb.CopyFrom(fragment, header_size);
  std::ignore = mb.CopyFrom(GetDemoTransportHeader(fragment.size()));
  AdvanceOffset(fragment.size());

  const size_t actual_size = header_size + fragment.size();
  if (original_size != actual_size) {
    FrameHandler<MultiBufType>::Truncate(segment, actual_size);
  }
}

}  // namespace pw::multibuf::size_report
