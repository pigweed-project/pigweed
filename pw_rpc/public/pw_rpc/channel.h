// Copyright 2020 The Pigweed Authors
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

#include <cstdint>
#include <limits>
#include <type_traits>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_rpc/internal/config.h"
#include "pw_rpc/internal/lock.h"
#include "pw_rpc/internal/packet.h"
#include "pw_span/span.h"
#include "pw_status/status.h"

namespace pw::rpc {
namespace internal {
namespace test {

template <typename, typename, uint32_t>
class InvocationContext;  // Forward declaration for friend statement

}  // namespace test

class ChannelList;  // Forward declaration for friend statement

Status OverwriteChannelId(ByteSpan rpc_packet, uint32_t channel_id_under_128);

}  // namespace internal

/// @submodule{pw_rpc,channel}

/// Extracts the channel ID from a pw_rpc packet.
///
/// @returns @Result{the channel ID}
/// * @DATA_LOSS: The packet is corrupt and the channel ID could not be found.
Result<uint32_t> ExtractChannelId(ConstByteSpan packet);

/// Rewrites an encoded packet's channel ID in place. Both channel IDs MUST be
/// less than 128.
///
/// @returns
/// * @OK: Successfully replaced the channel ID.
/// * @DATA_LOSS: Parsing the packet failed.
/// * @OUT_OF_RANGE: The encoded packet's channel ID was 128 or larger.
template <uint32_t kNewChannelId>
Status ChangeEncodedChannelId(ByteSpan rpc_packet) {
  static_assert(kNewChannelId < 128u,
                "Channel IDs must be less than 128 to avoid needing to "
                "re-encode the packet");
  return internal::OverwriteChannelId(rpc_packet, kNewChannelId);
}

/// Version of `ChangeEncodedChannelId` with a runtime variable channel ID.
/// Prefer the template parameter version when possible to avoid a runtime check
/// on the new channel ID.
inline Status ChangeEncodedChannelId(ByteSpan rpc_packet,
                                     uint32_t new_channel_id) {
  PW_ASSERT(new_channel_id < 128);
  return internal::OverwriteChannelId(rpc_packet, new_channel_id);
}

/// Returns the maximum payload size of an RPC packet for RPC endpoints as
/// configured. This can be used when allocating response encode buffers for
/// RPC services. If the RPC encode buffer is too small to fit RPC packet
/// headers, this returns zero.
///
/// By default, this function uses `PW_RPC_ENCODING_BUFFER_SIZE_BYTES` to
/// determine the largest supported payload, even when dynamic allocation is
/// enabled.
///
/// @warning `MaxSafePayloadSize` does NOT account for the channel MTU, which
/// may be smaller. Call `MaxWriteSizeBytes()` on an RPC's call object
/// (reader/writer) to account for channel MTU.
constexpr size_t MaxSafePayloadSize(
    size_t encode_buffer_size = cfg::kEncodingBufferSizeBytes) {
  return encode_buffer_size > internal::Packet::kMinEncodedSizeWithoutPayload
             ? encode_buffer_size -
                   internal::Packet::kMinEncodedSizeWithoutPayload
             : 0;
}

class ChannelOutput {
 public:
  using Packet = internal::Packet;

  // Returned from MaximumTransmissionUnit() to indicate that this ChannelOutput
  // imposes no limits on the MTU.
  static constexpr size_t kUnlimited = std::numeric_limits<size_t>::max();

  // Creates a channel output with the provided name. The name is used for
  // logging only.
  constexpr ChannelOutput(const char* name) : name_(name) {}

  virtual ~ChannelOutput() = default;

  constexpr const char* name() const { return name_; }

  // Returns the maximum transmission unit that this ChannelOutput supports. If
  // the ChannelOutput imposes no limit on the MTU, this function returns
  // ChannelOutput::kUnlimited.
  virtual size_t MaximumTransmissionUnit() { return kUnlimited; }

#if PW_RPC_LOCKLESS_CHANNEL_SEND
#define PW_RPC_CHANNEL_OUTPUT_SEND_LOCK_REQUIREMENT
#else
#define PW_RPC_CHANNEL_OUTPUT_SEND_LOCK_REQUIREMENT \
  PW_EXCLUSIVE_LOCKS_REQUIRED(internal::rpc_lock())
#endif

  // Sends an encoded RPC packet. Returns OK if further packets may be sent,
  // even if the current packet could not be sent. Returns any other status if
  // the Channel is no longer able to send packets.
  //
  // By default, the RPC system’s internal lock is held while this function is
  // called. Avoid long-running operations, since these will delay any other
  // users of the RPC system.
  //
  // If PW_RPC_LOCKLESS_CHANNEL_SEND is enabled, the global lock is released
  // before calling Send(). In this case, implementations should still avoid
  // long-running operations as they will block channel modifications (adding,
  // moving, or deleting channels).
  //
  // !!! DANGER !!!
  //
  // No pw_rpc APIs may be accessed in this function! Implementations MUST NOT
  // access any RPC endpoints (pw::rpc::Client, pw::rpc::Server) or call objects
  // (pw::rpc::ServerReaderWriter, pw::rpc::ClientReaderWriter, etc.) inside the
  // Send() function or any descendent calls.
  //
  // When `PW_RPC_LOCKLESS_CHANNEL_SEND` is disabled, doing so will result in
  // deadlock. Even when enabled, this is still a good rule to follow.
  // RPC APIs may be used by other threads, just not within Send().
  //
  // The buffer provided in packet must NOT be accessed outside of this
  // function. It must be sent immediately or copied elsewhere before the
  // function returns.
  virtual Status Send(span<const std::byte> buffer)
      PW_RPC_CHANNEL_OUTPUT_SEND_LOCK_REQUIREMENT = 0;

  // Returns whether this ChannelOutput implements SendPacket().
  //
  // If this returns true, pw_rpc will call SendPacket() instead of encoding
  // the packet and calling Send(). This allows bypassing packet encoding if
  // the channel output does not need it (e.g. for gRPC).
  virtual bool SupportsSendPacket() const { return false; }

  // Sends an unencoded RPC packet.
  //
  // This is only called if SupportsSendPacket() returns true.
  //
  // Override this to implement custom packet sending logic that avoids
  // the overhead of encoding the packet into a buffer.
  //
  // The same lock requirements and safety rules as Send() apply to this
  // function.
  virtual Status SendPacket(const Packet&)
      PW_RPC_CHANNEL_OUTPUT_SEND_LOCK_REQUIREMENT {
    return Status::Unimplemented();
  }

 private:
  const char* name_;
};

/// @}

namespace internal {

// Base class for rpc::Channel with internal-only public methods, which are
// hidden in the public derived class.
class ChannelBase {
 public:
  static constexpr uint32_t kUnassignedChannelId = 0;

  // TODO: b/234876441 - Remove the Configure and set_channel_output functions.
  //     Users should call CloseChannel() / OpenChannel() to change a channel.
  //     This ensures calls are properly update and works consistently between
  //     static and dynamic channel allocation.

  // Manually configures a dynamically-assignable channel with a specified ID
  // and output. This is useful when a channel's parameters are not known until
  // runtime. This can only be called once per channel.
  template <typename UnusedType = void>
  constexpr void Configure(uint32_t id, ChannelOutput& output) {
    static_assert(
        !cfg::kDynamicAllocationEnabled<UnusedType>,
        "Configure() may not be used if PW_RPC_DYNAMIC_ALLOCATION is "
        "enabled. Call CloseChannel/OpenChannel on the endpoint instead.");
    PW_ASSERT(id_ == kUnassignedChannelId);
    PW_ASSERT(id != kUnassignedChannelId);
    id_ = id;
    output_ = &output;
  }

  // Configure using an enum value channel ID.
  template <typename T,
            typename = std::enable_if_t<std::is_enum_v<T>>,
            typename U = std::underlying_type_t<T>>
  constexpr void Configure(T id, ChannelOutput& output) {
    static_assert(
        !cfg::kDynamicAllocationEnabled<T>,
        "Configure() may not be used if PW_RPC_DYNAMIC_ALLOCATION is enabled. "
        "Call CloseChannel/OpenChannel on the endpoint instead.");
    static_assert(sizeof(U) <= sizeof(uint32_t));
    const U kIntId = static_cast<U>(id);
    PW_ASSERT(kIntId > 0);
    return Configure<T>(static_cast<uint32_t>(kIntId), output);
  }

  // Reconfigures a channel with a new output. Depending on the output's
  // implementatation, there might be unintended behavior if the output is in
  // use.
  template <typename UnusedType = void>
  constexpr void set_channel_output(ChannelOutput& output) {
    static_assert(
        !cfg::kDynamicAllocationEnabled<UnusedType>,
        "set_channel_output() may not be used if PW_RPC_DYNAMIC_ALLOCATION is "
        "enabled. Call CloseChannel/OpenChannel on the endpoint instead.");
    PW_ASSERT(id_ != kUnassignedChannelId);
    output_ = &output;
  }

  constexpr uint32_t id() const { return id_; }
  constexpr bool assigned() const { return id_ != kUnassignedChannelId; }

  //
  // Internal functions made private in the public Channel class.
  //

  // Invokes ChannelOutput::Send and returns its status. Any non-OK status
  // indicates that the Channel is permanently closed.
  Status Send(const Packet& packet) PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());

  constexpr void Close() {
    PW_ASSERT(id_ != kUnassignedChannelId);
    id_ = kUnassignedChannelId;
    output_ = nullptr;
  }

  // Returns the maximum payload size for this channel, factoring in the
  // ChannelOutput's MTU and the RPC system's `pw::rpc::MaxSafePayloadSize()`.
  size_t MaxWriteSizeBytes() const;

 protected:
  constexpr ChannelBase(uint32_t id, ChannelOutput* output)
      : id_(id), output_(output) {}

 private:
  uint32_t id_;
  ChannelOutput* output_;
};

}  // namespace internal

// Associates an ID with an interface for sending packets.
class Channel : public internal::ChannelBase {
 public:
  // Creates a channel with a static ID. The channel's output can also be
  // static, or it can set to null to allow dynamically opening connections
  // through the channel.
  template <uint32_t kId>
  constexpr static Channel Create(ChannelOutput* output) {
    static_assert(kId != kUnassignedChannelId, "Channel ID cannot be 0");
    return Channel(kId, output);
  }

  // Creates a channel with a static ID from an enum value.
  template <auto kId,
            typename T = decltype(kId),
            typename = std::enable_if_t<std::is_enum_v<T>>,
            typename U = std::underlying_type_t<T>>
  constexpr static Channel Create(ChannelOutput* output) {
    constexpr U kIntId = static_cast<U>(kId);
    static_assert(kIntId >= 0, "Channel ID cannot be negative");
    static_assert(kIntId <= std::numeric_limits<uint32_t>::max(),
                  "Channel ID must fit in a uint32");
    return Create<static_cast<uint32_t>(kIntId)>(output);
  }

  // Creates a dynamically assignable channel without a set ID or output.
  constexpr Channel() : internal::ChannelBase(kUnassignedChannelId, nullptr) {}

 private:
  template <typename, typename, uint32_t>
  friend class internal::test::InvocationContext;
  friend class internal::ChannelList;

  constexpr Channel(uint32_t id, ChannelOutput* output)
      : internal::ChannelBase(id, output) {
    PW_ASSERT(id != kUnassignedChannelId);
  }

 private:
  // Hide internal-only methods defined in the internal::ChannelBase.
  using internal::ChannelBase::Close;
  using internal::ChannelBase::MaxWriteSizeBytes;
  using internal::ChannelBase::Send;
};

}  // namespace pw::rpc
