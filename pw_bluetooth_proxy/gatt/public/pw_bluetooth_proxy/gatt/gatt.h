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

#include "pw_allocator/allocator.h"
#include "pw_bluetooth_proxy/channel_proxy.h"
#include "pw_bluetooth_proxy/connection_handle.h"
#include "pw_bluetooth_proxy/l2cap_channel_manager_interface.h"
#include "pw_containers/dynamic_map.h"
#include "pw_containers/dynamic_vector.h"
#include "pw_multibuf/allocator.h"
#include "pw_multibuf/multibuf.h"
#include "pw_span/span.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"

namespace pw::bluetooth::proxy::gatt {

enum class AttributeHandle : uint16_t {};

namespace internal {
enum class ClientId : uint16_t {};
enum class ServerId : uint16_t {};
}  // namespace internal

/// New values may be added.
enum class Error : uint8_t {
  kDisconnection,
  kReset,
  kClosedByClient,
};

struct CharacteristicInfo {
  AttributeHandle value_handle;
};

class Gatt;

/// Client represents the client role of a GATT connection to a remote device.
/// Many Clients can exist per connection.
///
/// This class is NOT thread-safe and requires external synchronization if used
/// by multiple threads.
class Client final {
 public:
  /// This delegate uses the NVI pattern.
  /// The `Delegate` must live until a call to `HandleError`. Closing the
  /// `Client` will send the error `kClosedByClient`.
  class Delegate {
   public:
    virtual ~Delegate() = default;

    void HandleNotification(ConnectionHandle connection_handle,
                            AttributeHandle value_handle,
                            multibuf::MultiBuf&& value);

    void HandleError(Error error, ConnectionHandle connection_handle);

   private:
    /// Called when an ATT_HANDLE_VALUE_NTF PDU is received for an intercepted
    /// attribute handle `value_handle`. Most operational APIs (e.g.
    /// InterceptNotification, CancelInterceptNotification, etc.) may be called
    /// safely within this method without deadlock. However, calling Close() or
    /// unregistering any Client or Server belonging to the same Gatt instance
    /// from within this callback will result in a deadlock.
    virtual void DoHandleNotification(ConnectionHandle connection_handle,
                                      AttributeHandle value_handle,
                                      multibuf::MultiBuf&& value) = 0;

    /// Called when a fatal error occurs, invalidating this delegate and Client.
    /// The Client may be destroyed/closed inside of this function.
    /// @param error The GATT error that occurred.
    /// @param connection_handle The connection handle of the Client this
    /// delegate corresponds to.
    virtual void DoHandleError(Error error,
                               ConnectionHandle connection_handle) = 0;
  };

  Client();
  ~Client();
  Client(Client& other) = delete;
  Client& operator=(Client& other) = delete;
  Client(Client&& other);
  Client& operator=(Client&& other);

  /// Unregisters the client and delegate.
  /// Alternatively, the client may be destroyed.
  /// The delegate will be notified with a kClosedByClient error.
  void Close();

  /// Starts intercepting notifications for handle `value_handle`. Notifications
  /// will be delivered to Delegate::DoHandleNotification.
  ///
  /// @returns
  /// * @OK: The attribute is now being intercepted.
  /// * @NOT_FOUND: The connection or attribute does not exist or is invalid.
  /// * @ALREADY_EXISTS: The notification is already being intercepted.
  /// * @UNAVAILABLE: Allocation failed.
  Status InterceptNotification(AttributeHandle value_handle);

  /// Stop intercepting the notification identified by `value_handle`.
  ///
  /// @returns
  /// * @OK: The notification is no longer being intercepted.
  /// * @NOT_FOUND: The interception does not exist.
  Status CancelInterceptNotification(AttributeHandle value_handle);

 private:
  friend class Gatt;

  Client(internal::ClientId client_id,
         ConnectionHandle connection_handle,
         Gatt& gatt);

  void Move(Client&& other);

  internal::ClientId client_id_;
  ConnectionHandle connection_handle_;

  // nullptr when the client is closed, default constructed, or moved from.
  Gatt* gatt_ = nullptr;
};

/// Server represents the server role of a GATT connection to a remote device.
/// Many Servers can exist per connection.
///
/// This class is NOT thread-safe and requires external synchronization if used
/// by multiple threads. However, it is thread safe with respect to other
/// Server/Client objects and the Gatt API.
class Server {
 public:
  /// This delegate uses the NVI pattern.
  /// The `Delegate` must live until a call to `HandleError`. Closing the
  /// `Server` will send the error `kClosedByClient`.
  class Delegate {
   public:
    virtual ~Delegate() = default;
    void HandleWriteWithoutResponse(ConnectionHandle connection_handle,
                                    AttributeHandle value_handle,
                                    multibuf::MultiBuf&& value);

    void HandleWriteAvailable(ConnectionHandle connection_handle);

    void HandleError(Error error, ConnectionHandle connection_handle);

   private:
    /// Called when a Write Without Response (ATT_WRITE_CMD) PDU is received for
    /// an offloaded characteristic with value handle `value_handle`. Most
    /// operational APIs (e.g. SendNotification, AddCharacteristic, etc.) may be
    /// called safely within this method without deadlock. However, calling
    /// Close() or unregistering any Client or Server belonging to the same Gatt
    /// instance from within this callback will result in a deadlock.
    virtual void DoHandleWriteWithoutResponse(
        ConnectionHandle connection_handle,
        AttributeHandle value_handle,
        multibuf::MultiBuf&& value) = 0;

    /// Called when queue space for TX packets is available. Most operational
    /// APIs (e.g. SendNotification, AddCharacteristic, etc.) may be called
    /// safely within this method without deadlock. However, calling Close() or
    /// unregistering any Client or Server belonging to the same Gatt instance
    /// from within this callback will result in a deadlock.
    virtual void DoHandleWriteAvailable(ConnectionHandle connection_handle) = 0;

    /// Called when a fatal error occurs (e.g. the connection closed or
    /// controller reset). The delegate will not be called again after an error
    /// has been reported, so it is safe to free it.
    virtual void DoHandleError(Error error,
                               ConnectionHandle connection_handle) = 0;
  };

  Server() = default;
  ~Server();
  Server(Server& other) = delete;
  Server& operator=(Server& other) = delete;
  Server(Server&& other);
  Server& operator=(Server&& other);

  /// Unregisters the Server and delegate.
  /// Alternatively, the server may be destroyed.
  /// The delegate will be notified with a kClosedByClient error.
  void Close();

  /// Add a characteristic to this Server's set of offloaded characteristics.
  ///
  /// @returns
  /// * @OK: Success.
  /// * @ALREADY_EXISTS: The characteristic is already being handled by a
  /// Server.
  /// * @RESOURCE_EXHAUSTED: A memory allocation failed.
  /// * @FAILED_PRECONDITION: The connection or Server is no longer registered.
  Status AddCharacteristic(CharacteristicInfo characteristic);

  /// Remove a characteristic from this Server's set of offloaded
  /// characteristics.
  ///
  /// @returns
  /// * @OK: Success.
  /// * @FAILED_PRECONDITION: The connection or Server is no longer registered.
  /// * @NOT_FOUND: The characteristic was already not being offloaded by this
  /// Server.
  Status RemoveCharacteristic(CharacteristicInfo characteristic);

  /// Send a GATT notification for `value_handle` with payload `value` to the
  /// remote device. On failure (e.g. queue full), returns an error status and
  /// the `value`.
  ///
  /// @returns
  /// * @OK: The notification has been queued for transmission.
  /// * @FAILED_PRECONDITION: The connection or Server has already been closed.
  /// * @INVALID_ARGUMENT: `value_handle` is not being offloaded by this Server.
  /// * @RESOURCE_EXHAUSTED: An allocation failed.
  [[nodiscard]] StatusWithMultiBuf SendNotification(
      AttributeHandle value_handle, multibuf::MultiBuf&& value);

 private:
  friend class Gatt;

  Server(internal::ServerId server_id,
         ConnectionHandle connection_handle,
         Gatt& gatt);

  void Move(Server&& other);

  internal::ServerId server_id_{0};
  ConnectionHandle connection_handle_{0};
  Gatt* gatt_ = nullptr;
};

/// The Gatt class multiplexes the ATT fixed channels for each connection. Gatt
/// provides an API for multiple clients to perform GATT operations.
/// This class is thread-safe.
class Gatt {
 public:
  /// @param l2cap The L2CAP interface to be used to obtain ATT channels. This
  /// is normally ProxyHost/L2capChannelManager, but a fake can be used for
  /// tests.
  /// @param allocator The allocator to use for internal state.
  /// @param multibuf_allocator The allocator to use for allocating buffers.
  Gatt(L2capChannelManagerInterface& l2cap,
       Allocator& allocator,
       multibuf::MultiBufAllocator& multibuf_allocator);

  Gatt(const Gatt&) = delete;
  Gatt(Gatt&&) = delete;
  Gatt& operator=(const Gatt&) = delete;
  Gatt& operator=(Gatt&&) = delete;

  ~Gatt();

  /// Create a GATT Client object corresponding to a specific connection.
  /// @param connection_handle The handle of the connection the client should
  /// use.
  /// @param delegate The Delegate must be valid until Delegate::HandleError()
  /// is called or the Client is closed/destroyed.
  ///
  /// @returns
  /// * @OK: Success. A Client is returned.
  /// * @RESOURCE_EXHAUSTED: An internal ID could not be allocated for this
  /// client.
  /// * @UNAVAILABLE: A memory allocation failed or the L2CAP channel could not
  /// be acquired.
  Result<Client> CreateClient(ConnectionHandle connection_handle,
                              Client::Delegate& delegate);

  /// Create a GATT server for a specific connection only.
  /// Characteristics can only be owned by 1 Server at a time.
  /// @param delegate Must be valid until an error is reported.
  ///
  /// @returns
  /// * @OK: Success. A Server is returned.
  /// * @ALREADY_EXISTS: One or more of the characteristics is already being
  /// handled by a Server.
  /// * @RESOURCE_EXHAUSTED: An internal ID could not be allocated for this
  /// server or a memory allocation failed.
  /// * @UNAVAILABLE: The L2CAP channel could not be acquired.
  Result<Server> CreateServer(ConnectionHandle connection_handle,
                              span<const CharacteristicInfo> characteristics,
                              Server::Delegate& delegate);

 private:
  friend class Client;
  friend class Server;

  using ClientMap =
      DynamicMap<std::underlying_type_t<internal::ClientId>, Client::Delegate*>;
  using ServerMap =
      DynamicMap<std::underlying_type_t<internal::ServerId>, Server::Delegate*>;
  using CharacteristicMap =
      DynamicMap<std::underlying_type_t<AttributeHandle>, internal::ServerId>;

  struct Connection final {
    explicit Connection(Allocator& allocator)
        : intercepted_notifications_(allocator),
          clients(allocator),
          servers(allocator),
          characteristics(allocator) {}

    UniquePtr<ChannelProxy> att_channel;
    DynamicVector<std::pair<AttributeHandle, internal::ClientId>>
        intercepted_notifications_;
    ClientMap clients;
    ServerMap servers;
    CharacteristicMap characteristics;
  };

  struct QueuedWriteAvailable {
    internal::ServerId server_id;
    Server::Delegate* delegate;
    ConnectionHandle connection_handle;
  };

  struct PendingNotification {
    internal::ClientId client_id;
    Client::Delegate* delegate;
    ConnectionHandle connection_handle;
    AttributeHandle att_handle;
    multibuf::MultiBuf value;
  };

  struct PendingWriteCommand {
    internal::ServerId server_id;
    Server::Delegate* delegate;
    ConnectionHandle connection_handle;
    AttributeHandle att_handle;
    multibuf::MultiBuf value;
  };

  using ConnectionMap =
      DynamicMap<std::underlying_type_t<ConnectionHandle>, Connection>;

  void UnregisterClient(internal::ClientId client_id,
                        ConnectionHandle connection_handle)
      PW_LOCKS_EXCLUDED(delegate_mutex_, mutex_);

  void UnregisterServer(internal::ServerId server_id,
                        ConnectionHandle connection_handle)
      PW_LOCKS_EXCLUDED(delegate_mutex_, mutex_);

  Status InterceptNotification(internal::ClientId client,
                               ConnectionHandle connection_handle,
                               AttributeHandle value_handle);

  Status CancelInterceptNotification(internal::ClientId client,
                                     ConnectionHandle connection_handle,
                                     AttributeHandle value_handle);

  Result<UniquePtr<ChannelProxy>> InterceptAttChannel(
      ConnectionHandle connection_handle);

  bool OnSpanReceivedFromController(ConstByteSpan payload,
                                    ConnectionHandle connection_handle,
                                    uint16_t local_channel_id,
                                    uint16_t remote_channel_id)
      PW_LOCKS_EXCLUDED(delegate_mutex_, mutex_);

  bool OnSpanReceivedFromHost(ConstByteSpan payload,
                              ConnectionHandle connection_handle,
                              uint16_t local_channel_id,
                              uint16_t remote_channel_id);

  void OnL2capEvent(L2capChannelEvent event,
                    ConnectionHandle connection_handle);

  void OnChannelClosedEvent(ConnectionHandle connection_handle)
      PW_LOCKS_EXCLUDED(delegate_mutex_, mutex_);

  void OnWriteAvailable(ConnectionHandle connection_handle)
      PW_LOCKS_EXCLUDED(delegate_mutex_, mutex_);

  void ResetConnections() PW_LOCKS_EXCLUDED(delegate_mutex_, mutex_);

  StatusWithMultiBuf SendNotification(internal::ServerId server_id,
                                      ConnectionHandle connection_handle,
                                      AttributeHandle value_handle,
                                      multibuf::MultiBuf&& value);

  ConnectionMap::iterator FindOrInterceptAttChannel(
      ConnectionHandle connection_handle) PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  bool OnAttHandleValueNtfFromController(ConstByteSpan payload,
                                         ConnectionMap::iterator conn_iter)
      PW_EXCLUSIVE_LOCKS_REQUIRED(delegate_mutex_, mutex_);

  bool OnAttWriteCmdFromController(ConstByteSpan payload,
                                   ConnectionMap::iterator conn_iter)
      PW_EXCLUSIVE_LOCKS_REQUIRED(delegate_mutex_, mutex_);

  Status AddCharacteristic(internal::ServerId server_id,
                           ConnectionHandle connection_handle,
                           CharacteristicInfo characteristic);

  Status RemoveCharacteristic(internal::ServerId server_id,
                              ConnectionHandle connection_handle,
                              CharacteristicInfo characteristic);

  sync::Mutex delegate_mutex_;
  sync::Mutex mutex_ PW_ACQUIRED_AFTER(delegate_mutex_);

  L2capChannelManagerInterface& l2cap_;
  Allocator& allocator_;
  multibuf::MultiBufAllocator& multibuf_allocator_;

  // Entries are allocated with allocator_.
  ConnectionMap connections_ PW_GUARDED_BY(mutex_);

  uint16_t next_id_ PW_GUARDED_BY(mutex_) = 0;

  // In order to prevent deadlock when delegates call back into Gatt APIs,
  // delegate callbacks are queued while mutex_ is locked and then invoked
  // when only delegate_mutex_ is locked.
  DynamicVector<QueuedWriteAvailable> write_available_queue_
      PW_GUARDED_BY(delegate_mutex_){allocator_};
  DynamicVector<PendingNotification> pending_notifications_
      PW_GUARDED_BY(delegate_mutex_){allocator_};
  DynamicVector<PendingWriteCommand> pending_write_commands_
      PW_GUARDED_BY(delegate_mutex_){allocator_};
};

}  // namespace pw::bluetooth::proxy::gatt
