// Copyright 2023 The Pigweed Authors
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
#include <lib/fit/function.h>

#include <functional>
#include <list>
#include <optional>
#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/common/lru_cache.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/scoped_channel.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/pdu.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/sdp.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/service_record.h"

namespace bt::sdp {

// The SDP server object owns the Service Database and all Service Records.
// Only one server is expected to exist per host.
// This object is not thread-safe.
// TODO(jamuraa): make calls thread-safe or ensure single-threadedness
class Server final {
 public:
  static constexpr const char* kInspectNodeName = "sdp_server";

  // A placeholder value for a dynamic PSM.
  // Note: This is not a valid PSM value itself. It is used to request a
  // randomly generated dynamic PSM.
  static constexpr uint16_t kDynamicPsm = 0xffff;

  // The maximum number of recently generated SDP responses to store in the LRU
  // cache across all connected channels. This mitigates CPU amplification from
  // repeated queries while curbing unbounded memory usage.
  static constexpr size_t kMaxCachedResponses = 10;

  // A new SDP server, which starts with just a ServiceDiscoveryService record.
  // Registers itself with |l2cap| when created.
  explicit Server(l2cap::ChannelManager* l2cap);
  ~Server();

  // Attach SDP server inspect node as a child node of |parent|.
  void AttachInspect(inspect::Node& parent,
                     std::string name = kInspectNodeName);

  // Initialize a new SDP profile connection with |peer_id| on |channel|.
  // Returns false if the channel cannot be activated.
  bool AddConnection(l2cap::Channel::WeakPtr channel);

  // An identifier for a set of services that have been registered at the same
  // time.
  using RegistrationHandle = uint32_t;

  const RegistrationHandle kNotRegistered = 0x00000000;

  // Given incomplete ServiceRecords, register services that will be made
  // available over SDP. Takes ownership of |records|. Channels created for this
  // service will be configured using the preferred parameters in |chan_params|.
  //
  // A non-zero RegistrationHandle will be returned if the service was
  // successfully registered.
  //
  // If any record in |records| fails registration checks, none of the services
  // will be registered.
  //
  // |conn_cb| will be called for any connections made to any of the services in
  // |records| with a connected channel and the descriptor list for the endpoint
  // which was connected.
  using ConnectCallback =
      fit::function<void(l2cap::Channel::WeakPtr, const DataElement&)>;
  RegistrationHandle RegisterService(std::vector<ServiceRecord> records,
                                     l2cap::ChannelParameters chan_params,
                                     ConnectCallback conn_cb);

  // Unregister services previously registered with RegisterService. Idempotent.
  // Returns |true| if any records were removed.
  bool UnregisterService(RegistrationHandle handle);

  // Returns a list of records for the provided |handle|.
  // The returned list may be empty if |handle| is not registered.
  std::vector<ServiceRecord> GetRegisteredServices(
      RegistrationHandle handle) const;

  // Define the ServiceDiscoveryService record for the SDP server object.
  // This method is public for testing purposes.
  static ServiceRecord MakeServiceDiscoveryService();

  // Construct a response based on input packet |sdu| and max size
  // |max_tx_sdu_size|. Note that this function can both be called by means of
  // connecting an l2cap::Channel and directly querying its database. As long
  // as the database does not change between requests, both of these approaches
  // are compatible.
  // This function will drop the packet if the PDU is too short, and it will
  // handle most errors by returning a valid packet with an ErrorResponse.
  std::optional<ByteBufferPtr> HandleRequest(ByteBufferPtr sdu,
                                             uint16_t max_tx_sdu_size);

  // Returns the set of allocated L2CAP PSMs in the SDP server.
  // TESTONLY hook and should not be used otherwise.
  std::set<l2cap::Psm> AllocatedPsmsForTest() const;

 private:
  struct RequestKey {
    uint8_t pdu_id;
    std::vector<uint8_t> parameters;

    bool operator==(const RequestKey& other) const {
      return pdu_id == other.pdu_id && parameters == other.parameters;
    }
    bool operator!=(const RequestKey& other) const { return !(*this == other); }
  };

  struct RequestKeyHasher {
    std::size_t operator()(const RequestKey& key) const {
      std::size_t hash = std::hash<uint8_t>{}(key.pdu_id);
      for (uint8_t byte : key.parameters) {
        hash = ((hash << 5) + hash) ^ byte;
      }
      return hash;
    }
  };

  // Returns the next unused Service Handle, or 0 if none are available.
  ServiceHandle GetNextHandle();

  // Performs a Service Search, returning any service record that contains
  // all UUID from the |search_pattern|
  ServiceSearchResponse SearchServices(
      const std::unordered_set<UUID>& pattern) const;

  // Gets Service Attributes in the |attribute_ranges| from the service record
  // with |handle|.
  ServiceAttributeResponse GetServiceAttributes(
      ServiceHandle handle, const std::list<AttributeRange>& ranges) const;

  // Retrieves Service Attributes in the |attribute_ranges|, using the pattern
  // to search for the services that contain all UUIDs from the |search_pattern|
  ServiceSearchAttributeResponse SearchAllServiceAttributes(
      const std::unordered_set<UUID>& search_pattern,
      const std::list<AttributeRange>& attribute_ranges) const;

  // An array of PSM to ServiceHandle assignments that are used to represent
  // the services that need to be registered in Server::QueueService.
  using ProtocolQueue = std::vector<std::pair<l2cap::Psm, ServiceHandle>>;

  /// Returns true if the |psm| is allocated in the SDP server.
  bool IsAllocated(l2cap::Psm psm) const { return psm_to_service_.count(psm); }

  // Attempts to add the |psm| to the queue of protocols to be registered.
  // Returns true if the PSM was successfully added to the queue, false
  // otherwise.
  bool AddPsmToProtocol(ProtocolQueue* protocols_to_register,
                        l2cap::Psm psm,
                        ServiceHandle handle) const;

  // Returns the next available dynamic PSM. A PSM is considered available if it
  // has not been allocated already nor reserved in |queued_psms|. Returns
  // |kInvalidPsm| if no PSM is available.
  l2cap::Psm GetDynamicPsm(const ProtocolQueue* queued_psms) const;

  // Given a complete ServiceRecord, extracts the PSM, ProtocolDescriptorList,
  // and any AdditionalProtocolDescriptorList information. Allocates any dynamic
  // PSMs that were requested in the aforementioned protocol lists.
  // Inserts the extracted info into |psm_to_register|.
  //
  // Returns |true| if the protocols are successfully validated and queued,
  // |false| otherwise.
  bool QueueService(ServiceRecord* record,
                    ProtocolQueue* protocols_to_register);

  // l2cap::Channel callbacks
  void OnChannelClosed(l2cap::Channel::UniqueId channel_id);

  // Updates the property values associated with the |sdp_server_node_|.
  void UpdateInspectProperties();

  // Send |bytes| over the channel associated with the connection handle
  // |conn|. Logs an error if channel not found.
  void Send(l2cap::Channel::UniqueId channel_id, ByteBufferPtr bytes);

  // Constructs a RequestKey by stripping the continuation state and its
  // preceding length byte from |payload_data|, isolating the immutable request
  // parameters for a given |pdu_id|. Returns std::nullopt if |payload_data| is
  // malformed or too small to contain the provided |cont_state|.
  static std::optional<RequestKey> GetRequestKey(uint8_t pdu_id,
                                                 const BufferView& payload_data,
                                                 const BufferView& cont_state);

  struct InspectProperties {
    // Inspect hierarchy node representing the sdp server.
    inspect::Node sdp_server_node;

    // Each ServiceRecord has it's record and nodes associated wth the
    // registered PSMs.
    struct InspectServiceRecordProperties {
      InspectServiceRecordProperties(std::string record,
                                     std::unordered_set<l2cap::Psm> psms);
      void AttachInspect(inspect::Node& parent, std::string name);
      inspect::Node node;
      // The record description.
      const std::string record;
      inspect::StringProperty record_property;
      // The node for the registered PSMs.
      inspect::Node psms_node;
      // The currently registered PSMs.
      const std::unordered_set<l2cap::Psm> psms;
      std::vector<std::pair<inspect::Node, inspect::StringProperty>> psm_nodes;
    };

    // The currently registered ServiceRecords.
    std::vector<InspectServiceRecordProperties> svc_record_properties;
  };

  // Used to register callbacks for the channels of services registered.
  l2cap::ChannelManager* l2cap_;
  InspectProperties inspect_properties_;

  // Channels currently connected to this server.
  std::unordered_map<l2cap::Channel::UniqueId, l2cap::ScopedChannel> channels_;

  // LRU cache of recently generated responses (up to kMaxCachedResponses),
  // shared across channels to avoid repeating computation on duplicate
  // requests.
  LruCache<RequestKey, std::unique_ptr<Response>, RequestKeyHasher>
      cached_responses_{kMaxCachedResponses};

  // The map of ServiceHandles that are associated with ServiceRecords.
  // This is a 1:1 mapping.
  std::unordered_map<ServiceHandle, ServiceRecord> records_;

  // Which PSMs are registered to services. Multiple ServiceHandles can be
  // registered to a single PSM.
  std::unordered_map<l2cap::Psm, std::unordered_set<ServiceHandle>>
      psm_to_service_;
  // The set of PSMs that are registered to a service.
  std::unordered_map<ServiceHandle, std::unordered_set<l2cap::Psm>>
      service_to_psms_;

  // The next available ServiceHandle.
  ServiceHandle next_handle_;

  // The set of ServiceHandles that are registered together, identified by a
  // RegistrationHandle.
  std::unordered_map<RegistrationHandle, std::set<ServiceHandle>>
      reg_to_service_;

  // The service database state tracker.
  uint32_t db_state_ [[maybe_unused]];

  WeakSelf<Server> weak_ptr_factory_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Server);
};

}  // namespace bt::sdp
