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

#include <fuchsia/bluetooth/bredr/cpp/fidl.h>

#include "lib/fidl/cpp/binding.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/channel_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/server_base.h"
#include "pw_bluetooth_sapphire/fuchsia/host/socket/socket_factory.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/server.h"
#include "pw_bluetooth_sapphire/lease.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"

namespace bthost {

// Implements the bredr::Profile FIDL interface.
class ProfileServer : public ServerBase<fuchsia::bluetooth::bredr::Profile> {
 public:
  static constexpr uint16_t kMaxUnackedSearchResults = 8;

  ProfileServer(
      bt::gap::Adapter::WeakPtr adapter,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
      uint8_t sco_offload_index,
      fidl::InterfaceRequest<fuchsia::bluetooth::bredr::Profile> request);
  ~ProfileServer() override;

  // If true, use Channel.socket. If false, use Channel.connection.
  // TODO(fxbug.dev/330590954): Remove once migration to Connection is complete.
  void set_use_sockets(bool enable) { use_sockets_ = enable; }

 private:
  class AudioDirectionExt;

  class L2capParametersExt final
      : public ServerBase<fuchsia::bluetooth::bredr::L2capParametersExt> {
   public:
    L2capParametersExt(
        fidl::InterfaceRequest<fuchsia::bluetooth::bredr::L2capParametersExt>
            request,
        bt::l2cap::Channel::WeakPtr channel)
        : ServerBase(this, std::move(request)),
          unique_id_(channel->unique_id()),
          channel_(std::move(channel)) {}

    bt::l2cap::Channel::UniqueId unique_id() const { return unique_id_; }

    void RequestParameters(fuchsia::bluetooth::ChannelParameters requested,
                           RequestParametersCallback callback) override;

    void handle_unknown_method(uint64_t ordinal,
                               bool method_has_response) override;

   private:
    bt::l2cap::Channel::UniqueId unique_id_;
    bt::l2cap::Channel::WeakPtr channel_;
  };

  class AudioOffloadExt final
      : public ServerBase<fuchsia::bluetooth::bredr::AudioOffloadExt> {
   public:
    AudioOffloadExt(
        ProfileServer& profile_server,
        fidl::InterfaceRequest<fuchsia::bluetooth::bredr::AudioOffloadExt>
            request,
        bt::l2cap::Channel::WeakPtr channel,
        bt::gap::Adapter::WeakPtr adapter)
        : ServerBase(this, std::move(request)),
          profile_server_(profile_server),
          unique_id_(channel->unique_id()),
          channel_(std::move(channel)),
          adapter_(std::move(adapter)) {}
    void GetSupportedFeatures(GetSupportedFeaturesCallback callback) override;
    void StartAudioOffload(
        fuchsia::bluetooth::bredr::AudioOffloadConfiguration
            audio_offload_configuration,
        fidl::InterfaceRequest<
            fuchsia::bluetooth::bredr::AudioOffloadController> controller)
        override;

    bt::l2cap::Channel::UniqueId unique_id() const { return unique_id_; }

    void handle_unknown_method(uint64_t ordinal,
                               bool method_has_response) override;

   private:
    std::unique_ptr<bt::l2cap::A2dpOffloadManager::Configuration>
    AudioOffloadConfigFromFidl(
        fuchsia::bluetooth::bredr::AudioOffloadConfiguration&
            audio_offload_configuration);

    ProfileServer& profile_server_;
    bt::l2cap::Channel::UniqueId unique_id_;
    bt::l2cap::Channel::WeakPtr channel_;
    bt::gap::Adapter::WeakPtr adapter_;
  };

  class AudioOffloadController
      : public ServerBase<fuchsia::bluetooth::bredr::AudioOffloadController> {
   public:
    explicit AudioOffloadController(
        fidl::InterfaceRequest<
            fuchsia::bluetooth::bredr::AudioOffloadController> request,
        bt::l2cap::Channel::WeakPtr channel)
        : ServerBase(this, std::move(request)),
          unique_id_(channel->unique_id()),
          channel_(std::move(channel)) {}

    void handle_unknown_method(uint64_t ordinal,
                               bool method_has_response) override;

    bt::l2cap::Channel::UniqueId unique_id() const { return unique_id_; }

    void Stop(StopCallback callback) override;

    void SendOnStartedEvent() { binding()->events().OnStarted(); }

    void Close(zx_status_t epitaph) { binding()->Close(epitaph); }

    WeakPtr<AudioOffloadController> GetWeakPtr() {
      return weak_self_.GetWeakPtr();
    }

   private:
    bt::l2cap::Channel::UniqueId unique_id_;
    bt::l2cap::Channel::WeakPtr channel_;
    WeakSelf<AudioOffloadController> weak_self_{this};
  };

  class ScoConnectionServer final
      : public ServerBase<fuchsia::bluetooth::bredr::ScoConnection> {
   public:
    explicit ScoConnectionServer(
        fidl::InterfaceRequest<fuchsia::bluetooth::bredr::ScoConnection>
            request,
        ProfileServer* profile_server);
    ~ScoConnectionServer() override;

    // TODO: https://fxbug.dev/447629864 - Implement this.
    void RequestDisconnect() override {}

    // Call bt::gap::ScoConnection::Activate with the appropriate callbacks. On
    // error, destroys this server.
    void Activate();

    void OnConnectedParams(
        ::fuchsia::bluetooth::bredr::ScoConnectionParameters params) {
      OnConnectionComplete(
          ::fuchsia::bluetooth::bredr::
              ScoConnectionOnConnectionCompleteRequest::WithConnectedParams(
                  std::move(params)));
    }

    // Sends the OnConnectionComplete event with `error` and shuts down the
    // server.
    void OnError(::fuchsia::bluetooth::bredr::ScoErrorCode error);

    const std::vector<fuchsia::bluetooth::bredr::ScoConnectionParameters>&
    parameters() const {
      return parameters_;
    }

    void set_parameters(
        std::vector<fuchsia::bluetooth::bredr::ScoConnectionParameters>
            params) {
      parameters_ = std::move(params);
    }

    void set_request_handle(bt::gap::Adapter::BrEdr::ScoRequestHandle handle) {
      request_handle_ = std::move(handle);
    }

    void set_connection(bt::sco::ScoConnection::WeakPtr connection) {
      connection_ = std::move(connection);
    }

    WeakPtr<ScoConnectionServer> GetWeakPtr() {
      return weak_self_.GetWeakPtr();
    }

   private:
    // ScoConnection overrides:
    void Read(ReadCallback callback) override;
    void Write(::fuchsia::bluetooth::bredr::ScoConnectionWriteRequest request,
               WriteCallback callback) override;
    void handle_unknown_method(uint64_t ordinal,
                               bool method_has_response) override;

    void TryRead();

    // Closes the server with `epitaph` and destroys the server.
    void Close(zx_status_t epitaph);

    void OnConnectionComplete(
        ::fuchsia::bluetooth::bredr::ScoConnectionOnConnectionCompleteRequest
            request) {
      binding()->events().OnConnectionComplete(std::move(request));
    }

    std::optional<bt::gap::Adapter::BrEdr::ScoRequestHandle> request_handle_;
    std::vector<fuchsia::bluetooth::bredr::ScoConnectionParameters> parameters_;

    bt::sco::ScoConnection::WeakPtr connection_;
    // Non-null when a read request is waiting for an inbound packet.
    fit::callback<void(::fuchsia::bluetooth::bredr::ScoConnection_Read_Result)>
        read_cb_;

    ProfileServer* profile_server_;

    // Keep this as the last member to make sure that all weak pointers are
    // invalidated before other members get destroyed.
    WeakSelf<ScoConnectionServer> weak_self_;
  };

  using ConnectionReceiverVariant = std::variant<
      fidl::InterfacePtr<fuchsia::bluetooth::bredr::ConnectionReceiver>,
      fidl::InterfacePtr<fuchsia::bluetooth::bredr::ConnectionReceiver2>>;

  // fuchsia::bluetooth::bredr::Profile overrides:
  void Advertise(fuchsia::bluetooth::bredr::ProfileAdvertiseRequest request,
                 AdvertiseCallback callback) override;
  void Search(
      ::fuchsia::bluetooth::bredr::ProfileSearchRequest request) override;
  void Connect(fuchsia::bluetooth::PeerId peer_id,
               fuchsia::bluetooth::bredr::ConnectParameters connection,
               ConnectCallback callback) override;
  void ConnectSco(
      ::fuchsia::bluetooth::bredr::ProfileConnectScoRequest request) override;
  void handle_unknown_method(uint64_t ordinal,
                             bool method_has_response) override;

  // Callback when clients close or revoke their connection targets
  void OnConnectionReceiverClosed(uint64_t ad_id);

  // Callback when clients close their search results
  void OnSearchResultError(uint64_t search_id, zx_status_t status);

  // Callback for incoming connections
  void OnChannelConnected(uint64_t ad_id,
                          bt::l2cap::Channel::WeakPtr channel,
                          const bt::sdp::DataElement& protocol_list);

  // Callback for services found on connected device
  void OnServiceFound(
      uint64_t search_id,
      bt::PeerId peer_id,
      const std::map<bt::sdp::AttributeId, bt::sdp::DataElement>& attributes);
  void OnServiceFoundComplete(
      bt::gap::BrEdrConnectionManager::SearchId search_id);

  // Callback for SCO connections requests.
  static void OnScoConnectionResult(
      WeakPtr<ScoConnectionServer>& server,
      bt::sco::ScoConnectionManager::AcceptConnectionResult result);

  // Callback when clients close their audio direction extension.
  void OnAudioDirectionExtError(AudioDirectionExt* ext_server,
                                zx_status_t status);

  // Create an AudioDirectionExt server for the given channel and set up
  // callbacks. Returns the client end of the channel.
  fidl::InterfaceHandle<fuchsia::bluetooth::bredr::AudioDirectionExt>
  BindAudioDirectionExtServer(bt::l2cap::Channel::WeakPtr channel);

  // Callback when clients close their l2cap parameters extension.
  void OnL2capParametersExtError(L2capParametersExt* ext_server,
                                 zx_status_t status);

  // Create an L2capParametersExt server for the given channel and set up
  // callbacks. Returns the client end of the channel.
  fidl::InterfaceHandle<fuchsia::bluetooth::bredr::L2capParametersExt>
  BindL2capParametersExtServer(bt::l2cap::Channel::WeakPtr channel);

  // Callback when clients close their audio offload extension.
  void OnAudioOffloadExtError(AudioOffloadExt* ext_server, zx_status_t status);

  // Create an AudioOffloadExt server for the given channel and set up
  // callbacks. Returns the client end of the channel.
  fidl::InterfaceHandle<fuchsia::bluetooth::bredr::AudioOffloadExt>
  BindAudioOffloadExtServer(bt::l2cap::Channel::WeakPtr channel);

  // Create an Connection server for the given channel and set up callbacks.
  // Returns the client end of the channel, or null on failure.
  std::optional<fidl::InterfaceHandle<fuchsia::bluetooth::Channel>>
  BindChannelServer(bt::l2cap::Channel::WeakPtr channel,
                    fit::callback<void()> closed_callback);

  // Create a FIDL Channel from an l2cap::Channel. A Connection relay is created
  // from |channel| and returned in the FIDL Channel.
  std::optional<fuchsia::bluetooth::bredr::Channel> ChannelToFidl(
      bt::l2cap::Channel::WeakPtr channel);

  const bt::gap::Adapter::WeakPtr& adapter() const { return adapter_; }

  // Advertised Services
  struct AdvertisedService {
    AdvertisedService(ConnectionReceiverVariant receiver,
                      bt::sdp::Server::RegistrationHandle registration_handle)
        : receiver(std::move(receiver)),
          registration_handle(registration_handle) {}
    ConnectionReceiverVariant receiver;
    bt::sdp::Server::RegistrationHandle registration_handle;
  };

  uint64_t advertised_total_;
  std::map<uint64_t, AdvertisedService> current_advertised_;

  // Search results pending
  struct PendingSearchResult {
    PendingSearchResult(
        fuchsia::bluetooth::PeerId peer_id,
        fidl::VectorPtr<fuchsia::bluetooth::bredr::ProtocolDescriptor>
            descriptor_list,
        std::vector<fuchsia::bluetooth::bredr::Attribute> attributes)
        : peer_id(peer_id),
          descriptor_list(std::move(descriptor_list)),
          attributes(std::move(attributes)) {}

    fuchsia::bluetooth::PeerId peer_id;
    fidl::VectorPtr<fuchsia::bluetooth::bredr::ProtocolDescriptor>
        descriptor_list;
    std::vector<fuchsia::bluetooth::bredr::Attribute> attributes;
  };

  // Searches registered
  struct RegisteredSearch {
    RegisteredSearch(
        fidl::InterfacePtr<fuchsia::bluetooth::bredr::SearchResults> results,
        bt::gap::BrEdrConnectionManager::SearchId search_id)
        : results(std::move(results)), search_id(search_id) {}
    fidl::InterfacePtr<fuchsia::bluetooth::bredr::SearchResults> results;
    bt::gap::BrEdrConnectionManager::SearchId search_id;
    uint16_t unacknowledged_search_results_count = 0;
    std::optional<pw::bluetooth_sapphire::Lease> wake_lease;
    std::deque<PendingSearchResult> pending_search_results;
  };

  // Send or queue for sending a service found.
  void SendServiceFound(RegisteredSearch& search, PendingSearchResult result);

  uint64_t searches_total_;
  std::map<uint64_t, RegisteredSearch> searches_;

  class AudioDirectionExt final
      : public ServerBase<fuchsia::bluetooth::bredr::AudioDirectionExt> {
   public:
    // Calls to SetPriority() are forwarded to |priority_cb|.
    AudioDirectionExt(fidl::InterfaceRequest<
                          fuchsia::bluetooth::bredr::AudioDirectionExt> request,
                      bt::l2cap::Channel::WeakPtr channel)
        : ServerBase(this, std::move(request)),
          unique_id_(channel->unique_id()),
          channel_(std::move(channel)) {}

    bt::l2cap::Channel::UniqueId unique_id() const { return unique_id_; }

    // fuchsia::bluetooth::bredr::AudioDirectionExt overrides:
    void SetPriority(fuchsia::bluetooth::bredr::A2dpDirectionPriority priority,
                     SetPriorityCallback callback) override;

    void handle_unknown_method(uint64_t ordinal,
                               bool method_has_response) override;

   private:
    bt::l2cap::Channel::UniqueId unique_id_;
    bt::l2cap::Channel::WeakPtr channel_;
  };
  std::unordered_map<bt::l2cap::Channel::UniqueId,
                     std::unique_ptr<L2capParametersExt>>
      l2cap_parameters_ext_servers_;

  std::unordered_map<bt::l2cap::Channel::UniqueId,
                     std::unique_ptr<AudioDirectionExt>>
      audio_direction_ext_servers_;

  std::unordered_map<bt::l2cap::Channel::UniqueId,
                     std::unique_ptr<AudioOffloadExt>>
      audio_offload_ext_servers_;

  std::unique_ptr<AudioOffloadController> audio_offload_controller_server_;

  bt::gap::Adapter::WeakPtr adapter_;

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;

  // The index to use when SCO is set to the OFFLOAD data path.
  uint8_t sco_offload_index_;

  // TODO(fxbug.dev/542275364): delete all the socket code once we're
  // convinced the Connection configuration is stable.
  //
  // If true, use Channel.socket. If false, use Channel.connection.
  bool use_sockets_ = false;

  // Creates sockets that bridge L2CAP channels to profile processes.
  bt::socket::SocketFactory<bt::l2cap::Channel> l2cap_socket_factory_;

  std::unordered_map<bt::l2cap::Channel::UniqueId,
                     std::unique_ptr<ChannelServer>>
      channel_servers_;

  std::unordered_map<ScoConnectionServer*, std::unique_ptr<ScoConnectionServer>>
      sco_connection_servers_;

  // Keep this as the last member to make sure that all weak pointers are
  // invalidated before other members get destroyed.
  WeakSelf<ProfileServer> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ProfileServer);
};

}  // namespace bthost
