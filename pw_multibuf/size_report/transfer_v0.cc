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
#include "pw_multibuf/size_report/transfer.h"

namespace pw::multibuf::size_report {

class FakeMultiBuf {
 public:
  using iterator = ByteSpan::iterator;
  using const_iterator = ConstByteSpan::iterator;

  constexpr FakeMultiBuf() {}

  constexpr FakeMultiBuf(void* data, size_t size)
      : buffer_(static_cast<std::byte*>(data), size),
        view_(static_cast<std::byte*>(data), size) {}

  constexpr std::byte* data() { return view_.data(); }
  constexpr const std::byte* data() const { return view_.data(); }

  constexpr size_t size() const { return view_.size_bytes(); }

  constexpr bool empty() const { return view_.empty(); }

  constexpr iterator begin() const { return view_.begin(); }
  constexpr iterator end() const { return view_.end(); }

  size_t CopyFrom(ConstByteSpan src, size_t offset = 0) {
    if (offset >= size()) {
      return 0;
    }
    size_t copy_size = std::min(src.size(), size() - offset);
    std::memcpy(data() + offset, src.data(), copy_size);
    return copy_size;
  }

  size_t CopyTo(ByteSpan dst, size_t offset = 0) const {
    if (offset >= size()) {
      return 0;
    }
    size_t copy_size = std::min(dst.size(), size() - offset);
    std::memcpy(dst.data(), data() + offset, copy_size);
    return copy_size;
  }

  void PushBack(Allocator& allocator, ConstByteSpan chunk) {
    const size_t new_size = size() + chunk.size();

    // BumpAllocator doesn't support Reallocate() so do it manually.
    std::byte* ptr = static_cast<std::byte*>(
        allocator.Allocate(allocator::Layout(new_size)));
    PW_ASSERT(ptr != nullptr);

    std::memcpy(ptr, buffer_.data(), buffer_.size());
    std::memcpy(ptr + size(), chunk.data(), chunk.size());
    allocator.Deallocate(buffer_.data());
    buffer_ = ByteSpan(static_cast<std::byte*>(ptr), new_size);
    view_ = buffer_;
  }

  void set_view(std::byte* data, size_t size) {
    PW_ASSERT(data >= buffer_.data());
    PW_ASSERT(data + size <= buffer_.data() + buffer_.size());
    view_ = ByteSpan(data, size);
  }

 private:
  ByteSpan buffer_;
  ByteSpan view_;
};

class FrameHandlerV0 : public virtual size_report::FrameHandler<FakeMultiBuf> {
 protected:
  FrameHandlerV0(allocator::BumpAllocator& allocator) : allocator_(allocator) {}

 private:
  FakeMultiBuf DoAllocateFrame() override {
    void* ptr = allocator_.Allocate(
        allocator::Layout(examples::kMaxDemoLinkFrameLength));
    PW_ASSERT(ptr != nullptr);
    return FakeMultiBuf(ptr, examples::kMaxDemoLinkFrameLength);
  }

  void DoTruncate(FakeMultiBuf& mb, size_t length) override {
    mb.set_view(mb.data(), length);
  }

  void DoNarrow(FakeMultiBuf& mb, size_t offset, size_t length) override {
    if (length == dynamic_extent) {
      length = mb.size() - offset;
    }
    mb.set_view(mb.data() + offset, length);
  }

  void DoWiden(FakeMultiBuf& mb,
               size_t prefix_len,
               size_t suffix_len) override {
    mb.set_view(mb.data() - prefix_len, mb.size() + prefix_len + suffix_len);
  }

  void DoPushBack(FakeMultiBuf& mb, FakeMultiBuf&& chunk) override {
    if (chunk.empty()) {
      return;
    }
    if (mb.empty()) {
      mb = std::move(chunk);
      return;
    }
    mb.PushBack(allocator_, ConstByteSpan(chunk));
  }

  FakeMultiBuf::const_iterator GetBegin(const FakeMultiBuf& mb) const final {
    return mb.begin();
  }

  FakeMultiBuf::const_iterator GetEnd(const FakeMultiBuf& mb) const final {
    return mb.end();
  }

  allocator::BumpAllocator& allocator_;
};

class SenderV0 : public FrameHandlerV0,
                 public size_report::Sender<FakeMultiBuf> {
 public:
  SenderV0(allocator::BumpAllocator& allocator,
           async2::Sender<FakeMultiBuf>&& tx)
      : size_report::FrameHandler<FakeMultiBuf>(),
        FrameHandlerV0(allocator),
        size_report::Sender<FakeMultiBuf>(std::move(tx)) {}
};

class ReceiverV0 : public FrameHandlerV0,
                   public size_report::Receiver<FakeMultiBuf> {
 public:
  ReceiverV0(allocator::BumpAllocator& allocator,
             async2::Receiver<FakeMultiBuf>&& rx)
      : size_report::FrameHandler<FakeMultiBuf>(),
        FrameHandlerV0(allocator),
        size_report::Receiver<FakeMultiBuf>(std::move(rx)) {}
};

constexpr size_t kMultiBufRegionSize = 8192;
std::array<std::byte, kMultiBufRegionSize> multibuf_region;

void TransferMessage() {
  allocator::BumpAllocator allocator;
  allocator.Init(multibuf_region);
  async2::ChannelStorage<FakeMultiBuf, 3> storage;
  [[maybe_unused]] auto [h, tx, rx] =
      async2::CreateSpscChannel<FakeMultiBuf>(storage);
  SenderV0 sender(allocator, std::move(tx));
  ReceiverV0 receiver(allocator, std::move(rx));
  size_report::TransferMessage(sender, receiver);
}

}  // namespace pw::multibuf::size_report

int main() {
  pw::multibuf::size_report::TransferMessage();
  return 0;
}
