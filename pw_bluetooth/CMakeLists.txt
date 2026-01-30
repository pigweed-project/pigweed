# Copyright 2022 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)

pw_add_module_config(pw_bluetooth_CONFIG)

pw_add_library(pw_bluetooth.config INTERFACE
  HEADERS
    public/pw_bluetooth/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${pw_bluetooth_CONFIG}
)

pw_add_library(pw_bluetooth INTERFACE
  HEADERS
    public/pw_bluetooth/address.h
    public/pw_bluetooth/assigned_uuids.h
    public/pw_bluetooth/gatt/client.h
    public/pw_bluetooth/gatt/constants.h
    public/pw_bluetooth/gatt/error.h
    public/pw_bluetooth/gatt/server.h
    public/pw_bluetooth/gatt/types.h
    public/pw_bluetooth/internal/raii_ptr.h
    public/pw_bluetooth/low_energy/advertising_data.h
    public/pw_bluetooth/low_energy/bond_data.h
    public/pw_bluetooth/low_energy/central.h
    public/pw_bluetooth/low_energy/connection.h
    public/pw_bluetooth/low_energy/peripheral.h
    public/pw_bluetooth/low_energy/security_mode.h
    public/pw_bluetooth/constants.h
    public/pw_bluetooth/controller.h
    public/pw_bluetooth/host.h
    public/pw_bluetooth/pairing_delegate.h
    public/pw_bluetooth/peer.h
    public/pw_bluetooth/result.h
    public/pw_bluetooth/types.h
    public/pw_bluetooth/uuid.h
    public/pw_bluetooth/vendor.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers
    pw_function
    pw_multibuf
    pw_status
    pw_string.hex
    pw_string.string
    pw_chrono.system_clock
    pw_uuid
)

pw_add_library(pw_bluetooth.pw_bluetooth2 INTERFACE
  HEADERS
    public/pw_bluetooth/address.h
    public/pw_bluetooth/assigned_uuids.h
    public/pw_bluetooth/constants.h
    public/pw_bluetooth/config.h
    public/pw_bluetooth/controller2.h
    public/pw_bluetooth/gatt/client2.h
    public/pw_bluetooth/gatt/constants.h
    public/pw_bluetooth/gatt/error.h
    public/pw_bluetooth/gatt/server2.h
    public/pw_bluetooth/gatt/types.h
    public/pw_bluetooth/internal/raii_ptr.h
    public/pw_bluetooth/low_energy/advertising_data.h
    public/pw_bluetooth/low_energy/bond_data2.h
    public/pw_bluetooth/low_energy/central2.h
    public/pw_bluetooth/low_energy/channel.h
    public/pw_bluetooth/low_energy/connection2.h
    public/pw_bluetooth/low_energy/peripheral2.h
    public/pw_bluetooth/low_energy/security_mode.h
    public/pw_bluetooth/pairing_delegate2.h
    public/pw_bluetooth/peer.h
    public/pw_bluetooth/types.h
    public/pw_bluetooth/uuid.h
    public/pw_bluetooth/vendor.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_bluetooth.config
    pw_async2
    pw_async2.value_future
    pw_result.expected
    pw_channel
    pw_containers
    pw_function
    pw_multibuf
    pw_result
    pw_span
    pw_status
    pw_string
    pw_uuid
)


pw_add_library(pw_bluetooth.emboss_util INTERFACE
  HEADERS
    public/pw_bluetooth/emboss_util.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_result
    pw_span
    pw_status
)

pw_add_test(pw_bluetooth.address_test
  SOURCES
    address_test.cc
  PRIVATE_DEPS
    pw_bluetooth
  GROUPS
    modules
)

pw_add_test(pw_bluetooth.api_test
  SOURCES
    api_test.cc
  PRIVATE_DEPS
    pw_bluetooth
  GROUPS
    modules
)

pw_add_test(pw_bluetooth.api2_test
  SOURCES
    api2_test.cc
  PRIVATE_DEPS
    pw_bluetooth.pw_bluetooth2
  GROUPS
    modules
)

pw_add_test(pw_bluetooth.result_test
  SOURCES
    result_test.cc
  PRIVATE_DEPS
    pw_bluetooth
  GROUPS
    modules
)

pw_add_test(pw_bluetooth.uuid_test
  SOURCES
    uuid_test.cc
  PRIVATE_DEPS
    pw_bluetooth
  GROUPS
    modules
)

###############################################################################
##          Everything below here is intended to be emboss only              ##
##          and will be skipped if emboss isn't enabled.                     ##
###############################################################################
if("${dir_pw_third_party_emboss}" STREQUAL "")
  # Skip emboss defs if it's not configured
  return()
endif()

include($ENV{PW_ROOT}/third_party/emboss/emboss.cmake)

emboss_cc_library(pw_bluetooth.emboss_hci_common
  SOURCES
    public/pw_bluetooth/hci_common.emb
)

emboss_cc_library(pw_bluetooth.emboss_hci_android
  SOURCES
    public/pw_bluetooth/hci_android.emb
  IMPORT_DIRS
    public
  DEPS
    pw_bluetooth.emboss_hci_common
)

emboss_cc_library(pw_bluetooth.emboss_hci_commands
  SOURCES
    public/pw_bluetooth/hci_commands.emb
  IMPORT_DIRS
    public
  DEPS
    pw_bluetooth.emboss_hci_common
)

emboss_cc_library(pw_bluetooth.emboss_hci_events
  SOURCES
    public/pw_bluetooth/hci_events.emb
  IMPORT_DIRS
    public
  DEPS
    pw_bluetooth.emboss_hci_common
)

emboss_cc_library(pw_bluetooth.emboss_hci_h4
  SOURCES
    public/pw_bluetooth/hci_h4.emb
  IMPORT_DIRS
    public
)

emboss_cc_library(pw_bluetooth.emboss_hci_data
  SOURCES
    public/pw_bluetooth/hci_data.emb
)

emboss_cc_library(pw_bluetooth.emboss_l2cap_frames
  SOURCES
    public/pw_bluetooth/l2cap_frames.emb
)

emboss_cc_library(pw_bluetooth.emboss_rfcomm_frames
  SOURCES
    public/pw_bluetooth/rfcomm_frames.emb
)

emboss_cc_library(pw_bluetooth.emboss_att
  SOURCES
    public/pw_bluetooth/att.emb
  IMPORT_DIRS
    public
  DEPS
    pw_bluetooth.emboss_hci_data
    pw_bluetooth.emboss_l2cap_frames
)

emboss_cc_library(pw_bluetooth.emboss_hci_test
  SOURCES
    public/pw_bluetooth/hci_test.emb
  IMPORT_DIRS
    public
  DEPS
    pw_bluetooth.emboss_hci_common
    pw_bluetooth.emboss_l2cap_frames
)

emboss_cc_library(pw_bluetooth.emboss_snoop
  SOURCES
    public/pw_bluetooth/snoop.emb
  IMPORT_DIRS
    public
)

pw_add_library(pw_bluetooth.emboss_hci_group INTERFACE
  PUBLIC_DEPS
    pw_bluetooth.emboss_hci_android
    pw_bluetooth.emboss_hci_commands
    pw_bluetooth.emboss_hci_common
    pw_bluetooth.emboss_hci_data
    pw_bluetooth.emboss_hci_events
    pw_bluetooth.emboss_hci_h4
)

pw_add_library(pw_bluetooth.snoop STATIC
  HEADERS
    public/pw_bluetooth/snoop.h
  PUBLIC_INCLUDES
    public
  SOURCES
    snoop.cc
  PUBLIC_DEPS
    pw_assert.check
    pw_bluetooth_proxy
    pw_bluetooth.emboss_snoop
    pw_chrono.system_clock
    pw_containers.inline_var_len_entry_queue
    pw_hex_dump
    pw_log
    pw_stream
    pw_sync.mutex
    pw_sync.virtual_basic_lockable
    pw_result
)

pw_add_library(pw_bluetooth.hci_util STATIC
  HEADERS
    public/pw_bluetooth/hci_util.h
  PUBLIC_INCLUDES
    public
  SOURCES
    hci_util.cc
  PUBLIC_DEPS
    pw_bluetooth.emboss_hci_h4
    pw_result
    pw_span
  PRIVATE_DEPS
    pw_bluetooth.emboss_hci_common
    pw_bluetooth.emboss_hci_data
    pw_status
)

pw_target_link_targets(pw_bluetooth
  INTERFACE
    pw_bluetooth.emboss_att
    pw_bluetooth.emboss_hci_android
    pw_bluetooth.emboss_hci_commands
    pw_bluetooth.emboss_hci_common
    pw_bluetooth.emboss_hci_data
    pw_bluetooth.emboss_hci_events
    pw_bluetooth.emboss_hci_h4
    pw_bluetooth.emboss_l2cap_frames
    pw_bluetooth.emboss_rfcomm_frames
)

pw_add_test(pw_bluetooth.emboss_test
  SOURCES
    emboss_test.cc
  PRIVATE_DEPS

    # All emboss targets are listed (even if they don't have explicit tests) to
    # ensure they are compiled.
    pw_bluetooth.emboss_att
    pw_bluetooth.emboss_hci_group
    pw_bluetooth.emboss_hci_test
    pw_bluetooth.emboss_l2cap_frames
    pw_bluetooth.emboss_rfcomm_frames
    pw_bluetooth.emboss_util
    pw_third_party.fuchsia.stdcompat
  GROUPS
    modules
)

pw_add_test(pw_bluetooth.emboss_util_test
  SOURCES
    emboss_util_test.cc
  PRIVATE_DEPS
    pw_bluetooth.emboss_hci_test
    pw_bluetooth.emboss_util
  GROUPS
    modules
)

pw_add_test(pw_bluetooth.snoop_test
  SOURCES
    snoop_test.cc
  PRIVATE_DEPS
    pw_bluetooth.snoop
  GROUPS
    modules
)

pw_add_test(pw_bluetooth.hci_util_test
  SOURCES
    hci_util_test.cc
  PRIVATE_DEPS
    pw_bluetooth.hci_util
  GROUPS
    modules
)
