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
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint256.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/sm/delegate.h"
#include "pw_bluetooth_sapphire/internal/host/sm/error.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt::sm::util {
// NOTE:
// All cryptographic utility functions use little-endian input/output unless
// explicitly noted.

// Returns the size of the SMP Packet with the given `Payload` template
// parameter type.
template <typename Payload>
constexpr size_t PacketSize() {
  return sizeof(Header) + sizeof(Payload);
}

// Returns a string representation of a given pairing method.
std::string PairingMethodToString(PairingMethod method);

// Returns a string representation of a PairingDelegate display method.
std::string DisplayMethodToString(Delegate::DisplayMethod method);

// Returns a string representation of a given IOCapability.
std::string IOCapabilityToString(IOCapability capability);

// Returns the HCI version of an SMP IOCapability. Returns
// pw::bluetooth::emboss::IoCapability::NO_INPUT_NO_OUTPUT for values not in
// sm::IOCapability.
pw::bluetooth::emboss::IoCapability IOCapabilityForHci(IOCapability capability);

// Utility function to heap allocate a new PDU.
MutableByteBufferPtr NewPdu(size_t param_size);

// Returns a std::pair<initiator_value, responder_value> based on |local_value|,
// |peer_value| and the local SMP Role |role|.
template <typename T>
std::pair<T, T> MapToRoles(const T& local_value,
                           const T& peer_value,
                           Role role) {
  if (role == Role::kInitiator) {
    return {local_value, peer_value};
  }
  return {peer_value, local_value};
}

// inclusive-language: disable
// Used to select the key generation method as described in Vol 3, Part H,
// 2.3.5.1 based on local and peer authentication parameters:
//   - |secure_connections|: True if Secure Connections pairing is used. False
//     means Legacy Pairing.
//   - |local_oob|: Local OOB auth data is available.
//   - |peer_oob|: Peer OOB auth data is available.
//   - |mitm_required|: True means at least one of the devices requires MITM
//     protection.
//   - |local_ioc|, |peer_ioc|: Local and peer IO capabilities.
//   - |local_initiator|: True means that the local device is the initiator and
//     |local_ioc| represents the initiator's I/O capabilities.
PairingMethod SelectPairingMethod(bool secure_connections,
                                  bool local_oob,
                                  bool peer_oob,
                                  bool mitm_required,
                                  IOCapability local_ioc,
                                  IOCapability peer_ioc,
                                  bool local_initiator);
// inclusive-language: enable

// Implements the "Security Function 'e'" defined in Vol 3, Part H, 2.2.1.
void Encrypt(const UInt128& key,
             const UInt128& plaintext_data,
             UInt128* out_encrypted_data);

// Implements the "Confirm Value Generation" or "c1" function for LE Legacy
// Pairing described in Vol 3, Part H, 2.2.3.
//
//   |tk|: 128-bit TK value
//   |rand|: 128-bit random number
//   |preq|: 56-bit SMP "Pairing Request" PDU
//   |pres|: 56-bit SMP "Pairing Response" PDU
//   |initiator_addr|: Device address of the initiator used while establishing
//                     the connection.
//   |responder_addr|: Device address of the responder used while establishing
//                     the connection.
//
// The generated confirm value will be returned in |out_confirm_value|.
std::optional<UInt128> C1(const UInt128& tk,
                          const UInt128& rand,
                          const ByteBuffer& preq,
                          const ByteBuffer& pres,
                          const DeviceAddress& initiator_addr,
                          const DeviceAddress& responder_addr);

// Implements the "Key Generation Function s1" to generate the STK for LE Legacy
// Pairing described in Vol 3, Part H, 2.2.4.
//
//   |tk|: 128-bit TK value
//   |r1|: 128-bit random value generated by the responder.
//   |r2|: 128-bit random value generated by the initiator.
void S1(const UInt128& tk,
        const UInt128& r1,
        const UInt128& r2,
        UInt128* out_stk);

// Implements the "Random Address Hash Function ah" to resolve RPAs. Described
// in Vol 3, Part H, 222.
//
//   |k|: 128-bit IRK value
//   |r|: 24-bit random part of a RPA.
//
// Returns 24 bit hash value.
uint32_t Ah(const UInt128& k, uint32_t r);

// Returns true if the given |irk| can resolve the given |rpa| using the method
// described in Vol 6, Part B, 1.3.2.3.
bool IrkCanResolveRpa(const UInt128& irk, const DeviceAddress& rpa);

// Generates a RPA using the given IRK based on the method described in Vol 6,
// Part B, 1.3.2.2.
DeviceAddress GenerateRpa(const UInt128& irk);

// Generates a static or non-resolvable private random device address.
DeviceAddress GenerateRandomAddress(bool is_static);

// Implements the AES-CMAC function defined in Vol. 3, Part H, 2.2.5.
//
//   |hash_key|: Little-endian 128-bit value used as the cipher key k in the
//   AES-CMAC algorithm. |msg|: Variable length data to be encoded by
//   |hash_key|. This is a little-endian parameter.
//
// A return value of std::nullopt indicates the calculation failed.
std::optional<UInt128> AesCmac(const UInt128& hash_key, const ByteBuffer& msg);

// Implements the "LE Secure Connections confirm value generation function f4"
// per Vol. 3, Part H, 2.2.6.
//
//   |u|: X-coordinate of the (peer/local) ECDH public key.
//   |v|: X-coordiante of the (local/peer) ECDH public key.
//   |x|: The CMAC key.
//   |z|: 0 for pairing methods besides Passkey Entry, in which case it is one
//   bit of the passkey.
//
// A return value of std::nullopt indicates the calculation failed.
std::optional<UInt128> F4(const UInt256& u,
                          const UInt256& v,
                          const UInt128& x,
                          uint8_t z);

// Implements the "LE Secure Connections key generation function f5" per Vol. 3,
// Part H, 2.2.7.
//
//   |dhkey|: Diffie-Hellman key generated by both parties in Phase 1 of SC
//   (ECDH key agreement). |initiator_nonce|: Nonce value generated by the
//   initiator to avoid replay attacks. |responder_nonce|: Nonce value generated
//   by the responder to avoid replay attacks. |initiator_addr|: Device address
//   of the pairing initiator. |responder_addr|: Device address of the pairing
//   responder.
//
// A return value of std::nullopt indicates the calculation failed. Returns an
// |F5Results| struct instead of the spec-prescribed single 256-bit value for
// easier client access to the MacKey/LTK.
struct F5Results {
  UInt128 mac_key;
  UInt128 ltk;
};
std::optional<F5Results> F5(const UInt256& dhkey,
                            const UInt128& initiator_nonce,
                            const UInt128& responder_nonce,
                            const DeviceAddress& initiator_addr,
                            const DeviceAddress& responder_addr);

// Implements the "LE Secure Connections check value generation function f6" per
// Vol. 3, Part H, 2.2.8. The semantics of each parameter depend on the pairing
// method and current pairing state. See above-noted spec section for parameter
// descriptions. The 24-bit IOCap parameter in the spec signature is separated
// into |auth_req|, |oob|, and |io_cap| parameters for client convenience.
//
// A return value of std::nullopt indicates the calculation failed.
std::optional<UInt128> F6(const UInt128& mackey,
                          const UInt128& n1,
                          const UInt128& n2,
                          const UInt128& r,
                          AuthReqField auth_req,
                          OOBDataFlag oob,
                          IOCapability io_cap,
                          const DeviceAddress& a1,
                          const DeviceAddress& a2);

// Implements the "LE Secure Connections numeric comparison value generation
// function g2" per Vol. 3, Part H, 2.2.9. The value displayed to the user
// should be the least significant 6 decimal digits of the result, i.e. g2 mod
// 10^6.
//
//   |initiator_pubkey_x|: X-coordinate of the initiator's ECDH public key
//   |responder_pubkey_x|: X-coordinate of the responder's ECDH public key
//   |initiator_nonce|: nonce value generated by the initiator to avoid replay
//   attacks |responder_nonce|: nonce value generated by the responder to avoid
//   replay attacks
//
// A return value of std::nullopt indicates the calculation failed.
std::optional<uint32_t> G2(const UInt256& initiator_pubkey_x,
                           const UInt256& responder_pubkey_x,
                           const UInt128& initiator_nonce,
                           const UInt128& responder_nonce);

// Implements the "Link key conversion function h6" per Vol. 3, Part H, 2.2.10.
// `w` is the encryption key for AES-CMAC, and `key_id` is used as the input
// value.
//
// A return value of std::nullopt indicates the calculation failed.
std::optional<UInt128> H6(const UInt128& w, uint32_t key_id);

// Implements the "Link key conversion function h7" per Vol. 3, Part H, 2.2.11.
// `salt` is the encryption key for AES-CMAC, and `w` is the input value.
//
// A return value of std::nullopt indicates the calculation failed.
std::optional<UInt128> H7(const UInt128& salt, const UInt128& w);

// Converts an LE LTK to a BR/EDR link key for Cross Transport Key Derivation as
// defined in v5.2 Vol. 3 Part H 2.4.2.4.
//
// A return value of std::nullopt indicates the conversion failed.
std::optional<UInt128> LeLtkToBrEdrLinkKey(const UInt128& le_ltk,
                                           CrossTransportKeyAlgo hash_function);

// Converts a BR/EDR link key to a LE LTK for Cross Transport Key Derivation as
// defined in Core Spec v6.0 Vol. 3 Part H 2.4.2.5.
//
// A return value of std::nullopt indicates the conversion failed.
std::optional<UInt128> BrEdrLinkKeyToLeLtk(const UInt128& link_key,
                                           CrossTransportKeyAlgo hash_function);

}  // namespace bt::sm::util
