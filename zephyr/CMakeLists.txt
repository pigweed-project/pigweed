# Copyright 2023 The Pigweed Authors
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

# Add library linkage for Zephyr
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_BASE64                  pw_base64)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_BYTES                   pw_bytes)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_CHECKSUM                pw_checksum)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_CONTAINERS              pw_containers)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_DIGITAL_IO              pw_digital_io)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_FUNCTION                pw_function)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_HDLC_RPC                pw_hdlc.default_addresses pw_hdlc.encoder pw_hdlc.decoder )
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_I2C                     pw_i2c.address pw_i2c.initiator pw_i2c.register_device pw_i2c.device)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_MULTISINK               pw_multisink)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_MULTISINK_UTIL          pw_multisink.util)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_PREPROCESSOR            pw_preprocessor)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_POLYFILL                pw_polyfill)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_RESULT                  pw_result)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_ROUTER_EGRESS           pw_router.egress)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_ROUTER_EGRESS_FUNCTION  pw_router.egress_function)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_ROUTER_PACKET_PARSER    pw_router.packet_parser)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_ROUTER_STATIC_ROUTER    pw_router.static_router)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_RPC_CLIENT              pw_rpc.client)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_RPC_CLIENT_SERVER       pw_rpc.client_server)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_RPC_COMMON              pw_rpc.common)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_RPC_SERVER              pw_rpc.server)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_RPC_NANOPB_CLIENT       pw_rpc.nanopb.client_api)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_RPC_NANOPB_COMMON       pw_rpc.nanopb.common)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_RPC_NANOPB_ECHO_SERVICE pw_rpc.nanopb.echo_service)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_RPC_NANOPB_METHOD       pw_rpc.nanopb.method)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_RPC_NANOPB_METHOD_UNION pw_rpc.nanopb.method_union)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_SPAN                    pw_span)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_STATUS                  pw_status)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_STREAM                  pw_stream)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_STRING                  pw_string)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_SYS_IO                  pw_sys_io)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_THREAD                  pw_thread.id)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_THREAD                  pw_thread.thread)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_THREAD_ITERATION        pw_thread.thread_iteration)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_THREAD_SLEEP            pw_thread.sleep)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_THREAD_YIELD            pw_thread.yield)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_TOKENIZER               pw_tokenizer)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_TOKENIZER_BASE64        pw_tokenizer.base64)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_DETOKENIZER             pw_tokenizer.decoder)
pw_zephyrize_libraries_ifdef(CONFIG_PIGWEED_VARINT                  pw_varint)

# Set Pigweed configs from Kconfig
pw_set_config_from_zephyr(CONFIG_PIGWEED_MULTISINK_LOCK_INTERRUPT_SAFE PW_MULTISINK_CONFIG_LOCK_INTERRUPT_SAFE)
