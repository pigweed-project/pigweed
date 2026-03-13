# bt-host

## Test

`$ fx test $dir_pw_bluetooth_sapphire/host`

### Fuzz Testing

bt-host contains fuzz tests for several libraries. Make sure to include the desired fuzzing target
in your `fx set`. For example, to include all bt-host fuzzing targets, use:

```
fx set core.x64 --fuzz-with asan --with $dir_pw_bluetooth_sapphire/host:fuzzers
```

Before running the test, ensure QEMU is running.

Run `fx fuzz list` to see the full list of available fuzzing targets. To run a specific fuzz test,
do `fx fuzz $package/$fuzzer` where `$package` and `$fuzzer` match those reported by `fx fuzz list`.



See the [fuzzing documentation](https://fuchsia.dev/fuchsia-src/development/testing/fuzzing/overview?hl=en)
for a more in depth guide.

## Inspect

`bt-host` uses the [standard driver processes](https://fuchsia.googlesource.com/fuchsia/+/57edce1df72b148c33e8f219bddbd038cdbb861b/zircon/system/ulib/inspect/) to expose its inspect hierarchy
to the Fuchsia system.

### Usage

To query the current state of the `bt-host` Inspect hierarchy through `ffx` tooling, run

`ffx inspect show core/bluetooth-core/bt-host-collection`

### Hierarchy
```
adapter:
    adapter_id
    hci_version
    bredr_max_num_packets
    bredr_max_data_length
    le_max_num_packets
    le_max_data_length
    sco_max_num_packets
    sco_max_data_length
    lmp_features
    le_features
    hci:
        command_channel:
            allowed_command_packets
            next_event_handler_id
            transactions:
                transaction_1:
                    opcode
                    complete_event_code
                    state
        acl_data_channel:
            bredr:
                num_sent_packets
            le:
                num_sent_packets
                independent_from_bredr
    low_energy_discovery_manager:
        state
        paused
        failed_count
        scan_interval_ms
        scan_window_ms
    bredr_discovery_manager:
        discoverable_sessions
        pending_discoverable
        discoverable_sessions_count
        last_discoverable_length_sec
        discovery_sessions
        inquiry_sessions_count
        last_inquiry_length_sec
    metrics:
        bredr:
            open_l2cap_channel_requests
            outgoing_connection_requests
            pair_requests
            request_discoverable_events
            request_discovery_events
            set_connectable_false_events
            set_connectable_true_events
        le:
            open_l2cap_channel_requests
            outgoing_connection_requests
            pair_requests
            start_advertising_events
            start_discovery_events
            stop_advertising_events
    l2cap:
        logical_links:
          logical_link_0x0:
            handle
            link_type
            flush_timeout_ms
            channels:
              channel_0x0:
                local_id
                remote_id
                psm
        services:
          service_0x0:
            psm
    peer_cache:
        metrics:
            bredr:
                bond_failure_events
                bond_success_events
                connection_events
                disconnection_events
            le:
                bond_failure_events
                bond_success_events
                connection_events
                disconnection_events
        peer_0x0:
            peer_id
            technology
            address
            connectable
            temporary
            features
            hci_version
            manufacturer
            bredr_data:
                connection_state
                services
                link_key:
                    security_properties:
                        encrypted
                        secure_connections
                        authenticated
                        level
                        key_type
            le_data:
                connection_state
                bonded
                features
    sdp_server:
        record_0x2:
            record
            // TODO(fxbug.dev/42129247): Migrate this to UIntArray when support is better.
            registered_psms:
                psm_0x0:
                    psm
                psm_0x1:
                    psm
        record_0x3:
            record
            registered_psms:
                (none)
    low_energy_connection_manager:
        disconnect_explicit_disconnect_count
        disconnect_link_error_count
        disconnect_remote_disconnection_count
        disconnect_zero_ref_count
        incoming_connection_failure_count
        incoming_connection_success_count
        outgoing_connection_failure_count
        outgoing_connection_success_count
        recent_connection_failures
        pending_requests:
            pending_request_0x0:
                peer_id
                callbacks
        outbound_connector:
            peer_id
            is_outbound
            connection_attempt
            state
        connections:
            connection_0x0:
                peer_id
                peer_address
                ref_count
                @time
        last_disconnected:
            0:
                peer_id
                reason
                connected_@time
                @time
    bredr_connection_manager:
        security_mode
        disconnect_acl_link_error_count
        disconnect_interrogation_failed_count
        disconnect_local_api_request_count
        disconnect_pairing_failed_count
        disconnect_peer_disconnection_count
        interrogation_complete_count
        incoming:
            connection_attempts
            failed_connections
            successful_connections
        outgoing:
            connection_attempts
            failed_connections
            successful_connections
        connection_requests:
            request_0x0:
                peer_id
                has_incoming
                callbacks
        connections:
            connection_0x0:
                peer_id
                @time
                pairing_state_manager:
                    pairing_state_type
                    legacy_pairing_state:
                        encryption_status
                        security_properties:
                            encrypted
                            secure_connections
                            authenticated
                            level
                            key_type
                    secure_simple_pairing_state:
                        encryption_status
                        security_properties:
                            encrypted
                            secure_connections
                            authenticated
                            level
                            key_type
        last_disconnected:
            0:
                peer_id
                reason
                connected_@time
                @time
    low_energy_advertiser:
        advertising_handle_map:
            advertising_set_0x0:
                handle
                address
lease_provider:
    token
    leases:
        // Child node for each Lease, for example:
        AclDataChannel:
name
```
