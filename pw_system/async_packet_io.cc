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

#include "pw_system/internal/async_packet_io.h"

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_system/config.h"
#include "pw_thread/detached_thread.h"

// Normal logging is not possible here. This code processes log messages, so
// must not produce logs for each log.
#define PACKET_IO_DEBUG_LOG(msg, ...)                          \
  if (false) /* Set to true to enable printf debug logging. */ \
  printf("DEBUG LOG: " msg "\n" __VA_OPT__(, ) __VA_ARGS__)

namespace pw::system::internal {

using ::pw::async2::Context;
using ::pw::async2::Dispatcher;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::multibuf::MultiBuf;

// With atomic head/tail reads/writes, this type of queue interaction could be
// lockless in single producer, single consumer scenarios.

// TODO: b/349398108 - MultiBuf directly out of (and into) the ring buffer.
Poll<InlineVarLenEntryQueue<>::const_value_type>
RpcChannelOutputQueue::PendOutgoingDatagram(Context& cx) {
  // The head pointer will not change until Pop is called.
  std::lock_guard lock(mutex_);
  if (queue_.empty()) {
    PW_ASYNC_STORE_WAKER(
        cx,
        packet_ready_,
        "RpcChannel is waiting for outgoing RPC datagrams to be enqueued");
    return Pending();
  }
  return Ready(queue_.front());
}

Status RpcChannelOutputQueue::Send(ConstByteSpan datagram) {
  PACKET_IO_DEBUG_LOG("Pushing %zu B packet into outbound queue",
                      datagram.size());
  mutex_.lock();
  if (datagram.size() <= queue_.max_size_bytes() && queue_.try_push(datagram)) {
    packet_ready_.Wake();
  } else {
    dropped_packets_ += 1;
  }
  mutex_.unlock();
  return OkStatus();
}

RpcServerThread::RpcServerThread(Allocator& allocator, rpc::Server& server)
    : allocator_(allocator), rpc_server_(server) {
  PW_CHECK_OK(rpc_server_.OpenChannel(1, rpc_packet_queue_));
}

void RpcServerThread::PushPacket(MultiBuf&& packet) {
  PACKET_IO_DEBUG_LOG("Received %zu B RPC packet", packet.size());
  std::lock_guard lock(mutex_);
  ready_for_packet_ = false;
  packet_multibuf_ = std::move(packet);
  new_packet_available_.release();
}

void RpcServerThread::RunOnce() {
  new_packet_available_.acquire();

  std::optional<ConstByteSpan> span = packet_multibuf_.ContiguousSpan();
  if (span.has_value()) {
    rpc_server_.ProcessPacket(*span).IgnoreError();
  } else {
    // Copy the packet into a contiguous buffer.
    // TODO: b/349440355 - Consider a global buffer instead of repeated allocs.
    const size_t packet_size = packet_multibuf_.size();
    std::byte* buffer = static_cast<std::byte*>(
        allocator_.Allocate({packet_size, alignof(std::byte)}));

    auto copy_result = packet_multibuf_.CopyTo({buffer, packet_size});
    PW_DCHECK_OK(copy_result.status());
    rpc_server_.ProcessPacket({buffer, packet_size}).IgnoreError();

    allocator_.Deallocate(buffer);
  }

  packet_multibuf_.Release();
  std::lock_guard lock(mutex_);
  ready_for_packet_ = true;
  ready_to_receive_packet_.Wake();
}

PacketIO::PacketIO(channel::ByteReaderWriter& io_channel,
                   ByteSpan buffer,
                   Allocator& allocator,
                   rpc::Server& rpc_server)
    : allocator_(allocator),
      mb_allocator_1_(mb_allocator_buffer_1_, allocator_),
      mb_allocator_2_(mb_allocator_buffer_2_, allocator_),
      channels_(mb_allocator_1_, mb_allocator_2_),
      router_(io_channel, buffer),
      rpc_server_thread_(allocator_, rpc_server),
      packet_reader_(*this),
      packet_writer_(*this) {
  PW_CHECK_OK(router_.AddChannel(channels_.second(),
                                 PW_SYSTEM_DEFAULT_RPC_HDLC_ADDRESS,
                                 PW_SYSTEM_DEFAULT_RPC_HDLC_ADDRESS));
}

void PacketIO::Start(Dispatcher& dispatcher,
                     const thread::Options& thread_options) {
  dispatcher.Post(packet_reader_);
  dispatcher.Post(packet_writer_);

  thread::DetachedThread(thread_options, [this] {
    while (true) {
      rpc_server_thread_.RunOnce();
    }
  });
}

Poll<> PacketIO::PacketReader::DoPend(Context& cx) {
  // Let the router do its work.
  if (io_.router_.Pend(cx).IsReady()) {
    return Ready();  // channel is closed, we're done here
  }

  // If the dispatcher isn't ready for another packet, wait.
  if (io_.rpc_server_thread_.PendReadyForPacket(cx).IsPending()) {
    return Pending();  // Nothing else to do for now
  }

  // Read a packet from the router and provide it to the RPC thread.
  auto read = io_.channel().PendRead(cx);
  if (read.IsPending()) {
    return Pending();  // Nothing else to do for now
  }
  if (!read->ok()) {
    PW_LOG_ERROR("Channel::PendRead() returned status %s",
                 read->status().str());
    return Ready();  // Channel is broken
  }
  // Push the packet into the RPC thread.
  io_.rpc_server_thread_.PushPacket(*std::move(*read));
  return Pending();  // Nothing else to do for now
}

Poll<> PacketIO::PacketWriter::DoPend(Context& cx) {
  // Do the work of writing any existing packets.
  // We ignore Pending because we want to continue trying to make
  // progress sending new packets regardless of whether the write was
  // able to complete.
  Poll<Status> write_status = io_.channel().PendWrite(cx);
  if (write_status.IsReady() && !write_status->ok()) {
    PW_LOG_ERROR("Channel::PendWrite() returned non-OK status %s",
                 write_status->str());
    return Ready();
  }

  // Get the next packet to send, if any.
  if (outbound_packet_.IsPending()) {
    outbound_packet_ = io_.rpc_server_thread_.PendOutgoingDatagram(cx);
  }

  if (outbound_packet_.IsPending()) {
    return Pending();
  }

  PACKET_IO_DEBUG_LOG("Sending %u B outbound packet",
                      static_cast<unsigned>(outbound_packet_->size()));

  // There is a packet -- check if we can write.
  auto writable = io_.channel().PendReadyToWrite(cx);
  if (writable.IsPending()) {
    return Pending();
  }

  if (!writable->ok()) {
    PW_LOG_ERROR("Channel::PendReadyToWrite() returned status %s",
                 writable->str());
    return Ready();
  }

  // Allocate a multibuf to send the packet.
  // TODO: b/349398108 - Instead, get a MultiBuf that refers to the queue entry.
  auto mb = io_.channel().PendAllocateWriteBuffer(cx, outbound_packet_->size());
  if (mb.IsPending()) {
    return Pending();
  }

  if (!mb->has_value()) {
    PW_LOG_ERROR("Async MultiBuf allocation of %u B failed",
                 static_cast<unsigned>(outbound_packet_->size()));
    return Ready();  // Could not allocate mb
  }

  // Copy the packet into the multibuf.
  auto [first, second] = outbound_packet_->contiguous_data();
  PW_CHECK_OK((*mb)->CopyFrom(first).status());
  PW_CHECK_OK((*mb)->CopyFromAndTruncate(second, first.size()).status());
  io_.rpc_server_thread_.PopOutboundPacket();

  PACKET_IO_DEBUG_LOG("Writing %zu B outbound packet", (**mb).size());
  auto write_result = io_.channel().StageWrite(**std::move(mb));
  if (!write_result.ok()) {
    return Ready();  // Write failed, but should not have
  }

  // Write was accepted, so set up for the next packet
  outbound_packet_ = Pending();

  // Sent one packet, let other tasks run.
  cx.ReEnqueue();
  return Pending();
}

}  // namespace pw::system::internal
