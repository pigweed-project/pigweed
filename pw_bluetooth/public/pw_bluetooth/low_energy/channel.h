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
#pragma once

#include "pw_async2/dispatcher.h"
#include "pw_async2/value_future.h"
#include "pw_bluetooth/internal/raii_ptr.h"
#include "pw_channel/channel.h"
#include "pw_result/result.h"

namespace pw::bluetooth::low_energy {

/// @module{pw_bluetooth}

/// An identifier for a service that accepts connection-oriented channel
/// connections. Referred to as a (simplified) protocol/service multiplexer
/// in the Bluetooth specification.
enum class Psm : uint16_t {};

/// The authentication an encryption requirements for a channel.
struct SecurityRequirements {
  /// If true, the link must be authenticated with on-path attacker protection.
  /// If false, authentication is not required.
  bool authentication_required;

  /// If true, the link must be encrypted with a Secure Connections key.
  bool secure_connections_required;
};

/// A duplex datagram channel that models the lifetime of a connection-oriented
/// channel. Closing or destroying `Channel` will close the underlying channel.
class Channel : public pw::channel::ReliableDatagramReaderWriter {
 public:
  virtual ~Channel() = default;

  /// Maximum payload size (SDU) that the peer supports receiving.
  virtual uint16_t max_transmit_size() = 0;

  /// Maximum payload size (SDU) that this channel supports receiving.
  virtual uint16_t max_receive_size() = 0;

 private:
  virtual void Release() = 0;

 public:
  using Ptr = internal::RaiiPtr<Channel, &Channel::Release>;
};

/// Represents a service or protocol that accepts incoming channels for a PSM.
/// Destroying this object will cease accepting any incoming channels, but
/// existing established channels will not be affected. Additionally, once
/// this object is destroyed the implementation is free to reuse the PSM that
/// was previously assigned for this instance.
class ChannelListener {
 public:
  virtual ~ChannelListener() = default;

  /// Poll to receive incoming channels.
  virtual async2::Poll<Channel::Ptr> PendChannel(async2::Waker&& waker) = 0;

  /// The protocol/service multiplexer for this listener.
  virtual Psm psm() = 0;

 private:
  /// Custom deleter called when `ChannelListener::Ptr` is destroyed. The
  /// implementation should free or clean up the memory used by this
  /// object. This enables the use of smart pointer semantics while leaving
  /// memory management up to the implementation. Calling the virtual destructor
  /// or not is up to the implementation.
  virtual void Release() = 0;

 public:
  using Ptr = internal::RaiiPtr<ChannelListener, &ChannelListener::Release>;
};

class ChannelListenerRegistry {
 public:
  /// The parameters to use for incoming channels.
  struct ListenParameters {
    /// Maximum supported payload size (SDU) for receiving.
    uint16_t max_receive_size;
    /// The security requirements that must be met before data is exchanged on
    /// the channel. If the requirements cannot be met, channel establishment
    /// will fail.
    SecurityRequirements security_requirements;
  };

  virtual ~ChannelListenerRegistry() = default;

  /// Register a listener for incoming channels. The registry will assign a
  /// protocol/service multiplexer value that is unique for the local device, as
  /// well as create a `ChannelListener` for accepting incoming channels. In the
  /// unlikely event that all PSMs have been assigned, this call will fail with
  /// `RESOURCE_EXHAUSTED`.
  ///
  /// Note that the method of service discovery or advertising is defined by
  /// the service or protocol, so it is the responsibility of the caller to
  /// update the GATT database or other service discovery mechanism.
  ///
  /// @param parameters Parameters for the local side of the channel.
  /// @return The result of starting the listener. On success, contains a
  /// `ChannelListener` that can be used to receive new channels.
  virtual async2::ValueFuture<pw::Result<ChannelListener::Ptr>> ListenL2cap(
      ListenParameters parameters) = 0;
};

}  // namespace pw::bluetooth::low_energy
