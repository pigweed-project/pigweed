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

#include "pw_allocator/bump_allocator.h"
#include "pw_async2/channel.h"
#include "pw_multibuf/allocator.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf/simple_allocator.h"
#include "pw_multibuf/size_report/transfer.h"

namespace pw::multibuf::size_report {

using MultiBufType = v1::MultiBuf;

class FrameHandlerV1 : public virtual size_report::FrameHandler<MultiBufType> {
 protected:
  FrameHandlerV1(MultiBufAllocator& mb_allocator)
      : mb_allocator_(mb_allocator) {}

 private:
  MultiBufType DoAllocateFrame() override {
    auto frame =
        mb_allocator_.AllocateContiguous(examples::kMaxDemoLinkFrameLength);
    PW_ASSERT(frame.has_value());
    return std::move(*frame);
  }

  void DoTruncate(MultiBufType& mb, size_t length) override {
    mb.Truncate(length);
  }

  void DoNarrow(MultiBufType& mb, size_t offset, size_t length) override {
    mb.DiscardPrefix(offset);
    if (length != dynamic_extent) {
      mb.Truncate(length);
    }
  }

  void DoWiden(MultiBufType& mb,
               size_t prefix_len,
               size_t suffix_len) override {
    if (prefix_len != 0) {
      PW_ASSERT(mb.ClaimPrefix(prefix_len));
    }
    if (suffix_len != 0) {
      PW_ASSERT(mb.ClaimSuffix(suffix_len));
    }
  }

  void DoPushBack(MultiBufType& mb, MultiBufType&& chunk) override {
    mb.PushSuffix(std::move(chunk));
  }

  MultiBufType::const_iterator GetBegin(const MultiBufType& mb) const override {
    return mb.begin();
  }

  MultiBufType::const_iterator GetEnd(const MultiBufType& mb) const override {
    return mb.end();
  }

  MultiBufAllocator& mb_allocator_;
};

class SenderV1 : public FrameHandlerV1,
                 public size_report::Sender<MultiBufType> {
 public:
  SenderV1(MultiBufAllocator& mb_allocator, async2::Sender<MultiBufType>&& tx)
      : FrameHandlerV1(mb_allocator),
        size_report::Sender<MultiBufType>(std::move(tx)) {}
};

class ReceiverV1 : public FrameHandlerV1,
                   public size_report::Receiver<MultiBufType> {
 public:
  ReceiverV1(MultiBufAllocator& mb_allocator,
             async2::Receiver<MultiBufType>&& rx)
      : FrameHandlerV1(mb_allocator),
        size_report::Receiver<MultiBufType>(std::move(rx)) {}
};

constexpr size_t kMultiBufTypeRegionSize = 8192;
constexpr size_t kMetadataRegionSize = 1024;
std::array<std::byte, kMultiBufTypeRegionSize> multibuf_region;
std::array<std::byte, kMetadataRegionSize> metadata_region;

void TransferMessage() {
  allocator::BumpAllocator metadata_allocator(metadata_region);
  SimpleAllocator mb_allocator(multibuf_region, metadata_allocator);
  async2::ChannelStorage<MultiBufType, 3> storage;
  [[maybe_unused]] auto [h, tx, rx] =
      async2::CreateSpscChannel<MultiBufType>(storage);
  SenderV1 sender(mb_allocator, std::move(tx));
  ReceiverV1 receiver(mb_allocator, std::move(rx));
  size_report::TransferMessage(sender, receiver);
}

}  // namespace pw::multibuf::size_report

int main() {
  pw::multibuf::size_report::TransferMessage();
  return 0;
}
