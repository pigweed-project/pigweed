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

#include "pw_allocator/async_pool.h"
#include "pw_allocator/bump_allocator.h"
#include "pw_allocator/chunk_pool.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf/size_report/transfer.h"

namespace pw::multibuf::size_report {

using MultiBufType = v2::MultiBuf::Instance;

class FrameHandlerV2 : public virtual size_report::FrameHandler<MultiBufType> {
 public:
  static constexpr allocator::Layout kLayout =
      allocator::Layout(examples::kMaxDemoLinkFrameLength);

 protected:
  FrameHandlerV2(allocator::ChunkPool& pool,
                 allocator::BumpAllocator& metadata_allocator)
      : pool_(pool), metadata_allocator_(metadata_allocator) {}

 private:
  MultiBufType DoAllocateFrame() override {
    auto bytes = pool_.MakeUnique<std::byte[]>();
    PW_ASSERT(bytes != nullptr);
    MultiBufType instance(metadata_allocator_);
    instance->PushBack(std::move(bytes));
    PW_ASSERT(instance->AddLayer(0));
    return instance;
  }

  void DoTruncate(MultiBufType& mb, size_t length) override {
    mb->TruncateTopLayer(length);
  }

  void DoNarrow(MultiBufType& mb, size_t offset, size_t length) override {
    PW_ASSERT(mb->AddLayer(offset, length));
  }

  void DoWiden(MultiBufType& mb,
               size_t prefix_len,
               size_t suffix_len) override {
    size_t payload_len = mb->size();
    mb->PopLayer();
    // Shrink the MultiBuf to account for a reduced payload size.
    mb->TruncateTopLayer(prefix_len + payload_len + suffix_len);
  }

  const_iterator GetBegin(const MultiBufType& mb) const override {
    return mb->begin();
  }

  const_iterator GetEnd(const MultiBufType& mb) const override {
    return mb->end();
  }

  void DoPushBack(MultiBufType& mb, MultiBufType&& chunk) override {
    mb->PushBack(std::move(*chunk));
  }

  allocator::ChunkPool& pool_;
  allocator::BumpAllocator& metadata_allocator_;
};

class SenderV2 : public FrameHandlerV2,
                 public size_report::Sender<MultiBufType> {
 public:
  SenderV2(allocator::ChunkPool& pool,
           allocator::BumpAllocator& metadata_allocator,
           InlineAsyncQueue<MultiBufType>& queue)
      : size_report::FrameHandler<MultiBufType>(),
        FrameHandlerV2(pool, metadata_allocator),
        size_report::Sender<MultiBufType>(queue) {}
};

class ReceiverV2 : public FrameHandlerV2,
                   public size_report::Receiver<MultiBufType> {
 public:
  ReceiverV2(allocator::ChunkPool& pool,
             allocator::BumpAllocator& metadata_allocator,
             InlineAsyncQueue<MultiBufType>& queue)
      : size_report::FrameHandler<MultiBufType>(),
        FrameHandlerV2(pool, metadata_allocator),
        size_report::Receiver<MultiBufType>(queue, metadata_allocator) {}
};

constexpr size_t kMultiBufRegionSize = 8192;
constexpr size_t kMetadataRegionSize = 1024;
std::array<std::byte, kMultiBufRegionSize> multibuf_region;
std::array<std::byte, kMetadataRegionSize> metadata_region;

void TransferMessage() {
  pw::allocator::ChunkPool chunk_pool(multibuf_region, FrameHandlerV2::kLayout);
  allocator::BumpAllocator metadata_allocator;
  metadata_allocator.Init(metadata_region);
  InlineAsyncQueue<MultiBufType, 3> queue;
  SenderV2 sender(chunk_pool, metadata_allocator, queue);
  ReceiverV2 receiver(chunk_pool, metadata_allocator, queue);
  size_report::TransferMessage(sender, receiver);
}

}  // namespace pw::multibuf::size_report

int main() {
  pw::multibuf::size_report::TransferMessage();
  return 0;
}
