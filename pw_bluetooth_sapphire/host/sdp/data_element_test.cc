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

#include "pw_bluetooth_sapphire/internal/host/sdp/data_element.h"

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/sdp.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::sdp {
namespace {

template <std::size_t N>
DynamicByteBuffer make_dynamic_byte_buffer(std::array<uint8_t, N> bytes) {
  DynamicByteBuffer ret(N);
  ret.Write(bytes.data(), N);
  return ret;
}

TEST(DataElementTest, CreateIsNull) {
  DataElement elem;
  EXPECT_EQ(DataElement::Type::kNull, elem.type());
  EXPECT_TRUE(elem.Get<std::nullptr_t>());
  EXPECT_EQ(nullptr, *elem.Get<std::nullptr_t>());

  StaticByteBuffer expected(0x00);
  DynamicByteBuffer buf(1);
  EXPECT_EQ(1u, elem.Write(&buf));
  EXPECT_TRUE(ContainersEqual(expected, buf));
}

TEST(DataElementTest, SetAndGet) {
  DataElement elem;

  elem.Set(uint8_t{5});

  EXPECT_TRUE(elem.Get<uint8_t>());
  EXPECT_EQ(5u, *elem.Get<uint8_t>());
  EXPECT_FALSE(elem.Get<int16_t>());

  elem.Set(std::string("Fuchsia💖"));

  EXPECT_FALSE(elem.Get<uint8_t>());
  EXPECT_TRUE(elem.Get<std::string>());
  EXPECT_EQ(std::string("Fuchsia💖"), *elem.Get<std::string>());
}

TEST(DataElementTest, Read) {
  StaticByteBuffer buf(
      0x25,  // Type (4: String) & Size (5: in an additional byte) = 0b00100 101
      0x0B,  // Bytes
      'F',
      'u',
      'c',
      'h',
      's',
      'i',
      'a',
      0xF0,
      0x9F,
      0x92,
      0x96,  // String
      0xDE,
      0xAD,
      0xBE,
      0xEF  // Extra data (shouldn't be parsed)
  );

  DataElement elem;
  EXPECT_EQ(13u, DataElement::Read(&elem, buf));

  EXPECT_EQ(DataElement::Type::kString, elem.type());
  EXPECT_EQ(std::string("Fuchsia💖"), *elem.Get<std::string>());

  // Invalid - 0xDE: 0x11011 110 = 37 (invalid) + 6 (2 following byte size)
  EXPECT_EQ(0u, DataElement::Read(&elem, buf.view(13)));

  // elem shouldn't have been touched
  EXPECT_EQ(DataElement::Type::kString, elem.type());
  EXPECT_EQ(std::string("Fuchsia💖"), *elem.Get<std::string>());
}

TEST(DataElementTest, ReadInvalidType) {
  StaticByteBuffer buf(
      0xFD,  // Type (Invalid) & Size (5: in an additional byte) = 0b11111 101
      0x0B,  // Bytes
      'F',
      'u',
      'c',
      'h',
      's',
      'i',
      'a',
      0xF0,
      0x9F,
      0x92,
      0x96  // String
  );

  DataElement elem;
  EXPECT_EQ(0u, DataElement::Read(&elem, buf));
}

TEST(DataElementTest, ReadUUID) {
  StaticByteBuffer buf(
      0x19,  // Type (3: UUID) & Size (1: two bytes) = 0b00011 001
      0x01,
      0x00  // L2CAP
  );

  DataElement elem;
  EXPECT_EQ(3u, DataElement::Read(&elem, buf));
  EXPECT_EQ(DataElement::Type::kUuid, elem.type());
  EXPECT_EQ(UUID(uint16_t{0x0100}), *elem.Get<UUID>());

  StaticByteBuffer buf2(
      0x1A,  // Type (3: UUID) & Size (2: four bytes) = 0b00011 010
      0x01,
      0x02,
      0x03,
      0x04);

  EXPECT_EQ(5u, DataElement::Read(&elem, buf2));
  EXPECT_EQ(DataElement::Type::kUuid, elem.type());
  EXPECT_EQ(UUID(uint32_t{0x01020304}), *elem.Get<UUID>());

  StaticByteBuffer buf3(
      0x1B,  // Type (3: UUID) & Size (3: eight bytes) = 0b00011 011
      0x01,
      0x02,
      0x03,
      0x04,
      0x01,
      0x02,
      0x03,
      0x04,
      0x01,
      0x02,
      0x03,
      0x04,
      0x01,
      0x02,
      0x03,
      0x04);

  EXPECT_EQ(0u, DataElement::Read(&elem, buf3));

  StaticByteBuffer buf4(
      0x1C,  // Type (3: UUID) & Size (3: eight bytes) = 0b00011 100
      0x01,
      0x02,
      0x03,
      0x04,
      0x05,
      0x06,
      0x07,
      0x08,
      0x09,
      0x0A,
      0x0B,
      0x0C,
      0x0D,
      0x0E,
      0x0F,
      0x10);

  EXPECT_EQ(17u, DataElement::Read(&elem, buf4));
  EXPECT_EQ(DataElement::Type::kUuid, elem.type());
  // UInt128 in UUID is little-endian
  EXPECT_EQ(UUID({0x10,
                  0x0F,
                  0x0E,
                  0x0D,
                  0x0C,
                  0x0B,
                  0x0A,
                  0x09,
                  0x08,
                  0x07,
                  0x06,
                  0x05,
                  0x04,
                  0x03,
                  0x02,
                  0x01}),
            *elem.Get<UUID>());
}

TEST(DataElementTest, Write) {
  // This represents a plausible attribute_lists parameter of a
  // SDP_ServiceSearchAttributeResponse PDU for an SPP service.
  std::vector<DataElement> attribute_list;

  // SerialPort from Assigned Numbers
  std::vector<DataElement> service_class_list;
  service_class_list.emplace_back(DataElement(UUID(uint16_t{0x1101})));
  DataElement service_class_value(std::move(service_class_list));
  attribute_list.emplace_back(DataElement(kServiceClassIdList));
  attribute_list.emplace_back(std::move(service_class_value));

  // Protocol Descriptor List

  std::vector<DataElement> protocol_list_value;

  // ( L2CAP, PSM=RFCOMM )
  std::vector<DataElement> protocol_l2cap;
  protocol_l2cap.emplace_back(DataElement(protocol::kL2CAP));
  protocol_l2cap.emplace_back(DataElement(uint16_t{0x0003}));  // RFCOMM

  protocol_list_value.emplace_back(DataElement(std::move(protocol_l2cap)));

  // ( RFCOMM, CHANNEL=1 )
  std::vector<DataElement> protocol_rfcomm;
  protocol_rfcomm.push_back(DataElement(protocol::kRFCOMM));
  protocol_rfcomm.push_back(DataElement(uint8_t{1}));  // Server Channel = 1

  protocol_list_value.emplace_back(DataElement(std::move(protocol_rfcomm)));

  attribute_list.emplace_back(DataElement(kProtocolDescriptorList));
  attribute_list.emplace_back(DataElement(std::move(protocol_list_value)));

  // Bluetooth Profile Descriptor List
  std::vector<DataElement> profile_sequence_list;
  std::vector<DataElement> spp_sequence;
  spp_sequence.push_back(DataElement(UUID(uint16_t{0x1101})));
  spp_sequence.push_back(DataElement(uint16_t{0x0102}));

  profile_sequence_list.emplace_back(std::move(spp_sequence));

  attribute_list.push_back(DataElement(kBluetoothProfileDescriptorList));
  attribute_list.push_back((DataElement(std::move(profile_sequence_list))));

  // A custom attribute that has a uint64_t in it.
  attribute_list.emplace_back(DataElement(UUID(uint16_t(0xF00D))));
  attribute_list.emplace_back(DataElement(uint64_t(0xB0BAC0DECAFEFACE)));

  DataElement attribute_lists_elem(std::move(attribute_list));

  // clang-format off
  StaticByteBuffer expected(
      0x35, 0x35,  // Sequence uint8 41 bytes
      0x09,        // uint16_t type
      UpperBits(kServiceClassIdList), LowerBits(kServiceClassIdList),
      0x35, 0x03,  // Sequence uint8 3 bytes
      0x19,        // UUID (16 bits)
      0x11, 0x01,  // Serial Port from assigned numbers
      0x09,        // uint16_t type
      UpperBits(kProtocolDescriptorList), LowerBits(kProtocolDescriptorList),
      0x35, 0x0F,  // Sequence uint8 15 bytes
      0x35, 0x06,  // Sequence uint8 6 bytes
      0x19,        // Type: UUID (16 bits)
      0x01, 0x00,  // L2CAP UUID
      0x09,        // Type: uint16_t
      0x00, 0x03,  // RFCOMM PSM
      0x35, 0x05,  // Sequence uint8 5 bytes
      0x19,        // Type: UUID (16 bits)
      0x00, 0x03,  // RFCOMM UUID
      0x08,        // Type: uint8_t
      0x01,        // RFCOMM Channel 1
      0x09,        // uint16_t type
      UpperBits(kBluetoothProfileDescriptorList),
      LowerBits(kBluetoothProfileDescriptorList),
      0x35, 0x08,  // Sequence uint8 8 bytes
      0x35, 0x06,  // Sequence uint8 6 bytes
      0x19,        // Type: UUID (16 bits)
      0x11, 0x01,  // 0x1101 (SPP)
      0x09,        // Type: uint16_t
      0x01, 0x02,  // v1.2
      0x19,        // Type: UUID (16 bits)
      0xF0, 0x0D,  // Custom attribute ID,
      0x0B,        // uint64_t type
      0xB0, 0xBA, 0xC0, 0xDE, 0xCA, 0xFE, 0xFA, 0xCE // Data for uint64_t
  );
  // clang-format on

  DynamicByteBuffer block(55);

  size_t written = attribute_lists_elem.Write(&block);

  EXPECT_EQ(expected.size(), written);
  EXPECT_EQ(written, attribute_lists_elem.WriteSize());
  EXPECT_TRUE(ContainersEqual(expected, block));
}

TEST(DataElementTest, ReadSequence) {
  // clang-format off
  StaticByteBuffer buf(
      0x35, 0x08, // Sequence with 1 byte length (8)
      0x09, 0x00, 0x01,  // uint16_t: 1
      0x0A, 0x00, 0x00, 0x00, 0x02   // uint32_t: 2
  );
  // clang-format on

  DataElement elem;
  EXPECT_EQ(buf.size(), DataElement::Read(&elem, buf));
  EXPECT_EQ(DataElement::Type::kSequence, elem.type());
  auto* it = elem.At(0);
  EXPECT_EQ(DataElement::Type::kUnsignedInt, it->type());
  EXPECT_EQ(1u, *it->Get<uint16_t>());

  it = elem.At(1);
  EXPECT_EQ(DataElement::Type::kUnsignedInt, it->type());
  EXPECT_EQ(2u, *it->Get<uint32_t>());
}

TEST(DataElementTest, ReadNestedSequence) {
  StaticByteBuffer buf(0x35,
                       0x1C,  // Sequence uint8 28 bytes
                              // Sequence 0
                       0x35,
                       0x08,  // Sequence uint8 8 bytes
                       0x09,
                       0x00,
                       0x00,  // Element: uint16_t (0)
                       0x0A,
                       0xFE,
                       0xED,
                       0xBE,
                       0xEF,  // Element: uint32_t (0xFEEDBEEF)
                       // Sequence 1
                       0x35,
                       0x10,  // Sequence uint8 16 bytes
                       0x09,
                       0x00,
                       0x00,  // Element: uint16_t (0)
                       0x0A,
                       0xFE,
                       0xDB,
                       0xAC,
                       0x01,  // Element: uint32_t (0xFEDBAC01)
                       0x09,
                       0x00,
                       0x01,  // Handle: uint16_t (1 = kServiceClassIdList)
                       0x35,
                       0x03,
                       0x19,
                       0x11,
                       0x01  // Element: Sequence (3) { UUID(0x1101) }
  );

  DataElement elem;
  EXPECT_EQ(buf.size(), DataElement::Read(&elem, buf));
  EXPECT_EQ(DataElement::Type::kSequence, elem.type());
  auto* outer_it = elem.At(0);
  EXPECT_EQ(DataElement::Type::kSequence, outer_it->type());

  auto* it = outer_it->At(0);
  EXPECT_EQ(0u, *it->Get<uint16_t>());

  it = outer_it->At(1);
  EXPECT_EQ(0xfeedbeef, *it->Get<uint32_t>());

  outer_it = elem.At(1);
  EXPECT_EQ(DataElement::Type::kSequence, outer_it->type());

  it = outer_it->At(0);
  EXPECT_EQ(DataElement::Type::kUnsignedInt, it->type());
  EXPECT_EQ(0u, *it->Get<uint16_t>());

  it = outer_it->At(1);
  EXPECT_EQ(DataElement::Type::kUnsignedInt, it->type());
  EXPECT_EQ(0xfedbac01, *it->Get<uint32_t>());

  it = outer_it->At(2);
  EXPECT_EQ(DataElement::Type::kUnsignedInt, it->type());
  EXPECT_EQ(1u, *it->Get<uint16_t>());

  it = outer_it->At(3);
  EXPECT_EQ(DataElement::Type::kSequence, it->type());

  auto inner_it = it->At(0);
  EXPECT_EQ(DataElement::Type::kUuid, inner_it->type());
}

TEST(DataElementTest, ReadDeeplyNestedSequenceFails) {
  std::vector<uint8_t> bytes = {0x00};  // Innermost Null
  for (int i = 0; i < 1000; ++i) {
    size_t len = bytes.size();
    if (len <= 0xFF) {
      bytes.insert(bytes.begin(), static_cast<uint8_t>(len));
      bytes.insert(bytes.begin(), 0x35);
    } else {
      bytes.insert(bytes.begin(), static_cast<uint8_t>(len & 0xFF));
      bytes.insert(bytes.begin(), static_cast<uint8_t>((len >> 8) & 0xFF));
      bytes.insert(bytes.begin(), 0x36);
    }
  }

  DynamicByteBuffer buf(bytes.size());
  buf.Write(bytes.data(), bytes.size());

  DataElement elem;
  size_t read_bytes = DataElement::Read(&elem, buf);

  EXPECT_EQ(0u, read_bytes);
}

TEST(DataElementTest, ToString) {
  EXPECT_EQ("Null", DataElement().ToString());
  EXPECT_EQ("Boolean(true)", DataElement(true).ToString());
  EXPECT_EQ("UnsignedInt:1(27)", DataElement(uint8_t{27}).ToString());
  EXPECT_EQ("SignedInt:4(-54321)", DataElement(int32_t{-54321}).ToString());
  EXPECT_EQ("UUID(00000100-0000-1000-8000-00805f9b34fb)",
            DataElement(protocol::kL2CAP).ToString());
  EXPECT_EQ("String(fuchsia💖)",
            DataElement(std::string("fuchsia💖")).ToString());
  // This test and the following one print invalid unicode strings by replacing
  // nonASCII characters
  //  with '.'.  Somewhat confusingly, individual bytes of invalid unicode
  //  sequences can be valid
  // ASCII bytes.  In particular, the '\x28' in the invalid unicode sequences
  // below is a perfectly valid '(' in ASCII, so it prints as that.
  EXPECT_EQ(
      "String(ABC.(XYZ)",
      DataElement(std::string("ABC\xc3\x28XYZ")).ToString());  // Invalid UTF8.
  EXPECT_EQ("String(ABC.(XYZ....)",
            DataElement(std::string("ABC\xc3\x28XYZ💖"))
                .ToString());  // Invalid UTF8 means the whole
                               // string must be treated as ASCII.
  DataElement elem;
  elem.SetUrl(std::string("http://example.com"));
  EXPECT_EQ("Url(http://example.com)", elem.ToString());
  std::vector<DataElement> strings;
  strings.emplace_back(std::string("hello"));
  strings.emplace_back(std::string("sapphire🔷"));
  EXPECT_EQ("Sequence { String(hello) String(sapphire🔷) }",
            DataElement(std::move(strings)).ToString());
  DataElement alts;
  strings.clear();
  strings.emplace_back(std::string("hello"));
  strings.emplace_back(std::string("sapphire🔷"));
  alts.SetAlternative(std::move(strings));
  EXPECT_EQ("Alternatives { String(hello) String(sapphire🔷) }",
            alts.ToString());
}

TEST(DataElementTest, SetAndGetUrl) {
  DataElement elem;
  elem.SetUrl(std::string("https://foobar.dev"));

  EXPECT_FALSE(elem.Get<std::string>());
  EXPECT_EQ(DataElement::Type::kUrl, elem.type());
  EXPECT_EQ(std::string("https://foobar.dev"), *elem.GetUrl());
}

TEST(DataElementTest, SetEmptyUrlIgnored) {
  DataElement url_elem;
  url_elem.SetUrl(std::string(""));
  std::optional<std::string> url = url_elem.GetUrl();
  EXPECT_FALSE(url);
}

TEST(DataElementTest, SetAndGetAlt) {
  std::vector<DataElement> alternatives;
  DataElement elem1;
  elem1.Set<uint8_t>(5);
  alternatives.emplace_back(std::move(elem1));
  DataElement elem2;
  elem2.Set(std::string("foo"));
  alternatives.emplace_back(std::move(elem2));
  DataElement alt_elem;
  alt_elem.SetAlternative(std::move(alternatives));

  EXPECT_EQ(DataElement::Type::kAlternative, alt_elem.type());
  auto alt_get = alt_elem.Get<std::vector<DataElement>>();
  EXPECT_TRUE(alt_get);
  EXPECT_EQ(alt_get.value().size(), 2u);
  EXPECT_EQ(alt_get.value().at(0).Get<uint8_t>().value(), 5);
  EXPECT_EQ(alt_get.value().at(1).Get<std::string>().value(),
            std::string("foo"));
}

TEST(DataElementTest, SetInvalidUrlStringIsNoOp) {
  DataElement elem;
  EXPECT_EQ(DataElement::Type::kNull, elem.type());
  elem.SetUrl(std::string("https://foobar🔷.dev"));

  EXPECT_FALSE(elem.GetUrl());
  EXPECT_EQ(DataElement::Type::kNull, elem.type());
}

TEST(DataElementTest, ReadUrlFromBuffer) {
  StaticByteBuffer buf(
      0x45,  // Type (8: URL) & Size (5: in an additional byte) = 0b01000 101
      0x0B,  // 11 Bytes
      'F',
      'u',
      'c',
      'h',
      's',
      'i',
      'a',
      '.',
      'd',
      'e',
      'v',  // URL String
      0xDE,
      0xAD,
      0xBE,
      0xEF  // Extra data (shouldn't be parsed)
  );

  DataElement read_elem;
  EXPECT_EQ(13u, DataElement::Read(&read_elem, buf));

  EXPECT_EQ(DataElement::Type::kUrl, read_elem.type());
  EXPECT_EQ(std::string("Fuchsia.dev"), *read_elem.GetUrl());
}

TEST(DataElementTest, WriteUrlToBuffer) {
  DataElement url_elem;
  url_elem.SetUrl(std::string("Test.com"));

  auto expected = StaticByteBuffer(
      0x45,  // Type (8: URL) & Size (5: in an additional byte) = 0b01000 101
      0x08,  // 8 Bytes
      'T',
      'e',
      's',
      't',
      '.',
      'c',
      'o',
      'm'  // URL String
  );

  DynamicByteBuffer write_buf(10);

  size_t written = url_elem.Write(&write_buf);

  EXPECT_EQ(expected.size(), written);
  EXPECT_EQ(written, url_elem.WriteSize());
  EXPECT_TRUE(ContainersEqual(expected, write_buf));
}

TEST(DataElementTest, SetAndGetStrings) {
  auto buffer_set_string = make_dynamic_byte_buffer<10>(
      {'s', 'e', 't', ' ', 's', 't', 'r', 'i', 'n', 'g'});
  std::string string_set_string("set string");

  auto buffer_set_buffer = make_dynamic_byte_buffer<10>(
      {'s', 'e', 't', ' ', 'b', 'u', 'f', 'f', 'e', 'r'});
  std::string string_set_buffer("set buffer");

  DataElement elem_set_string;
  elem_set_string.Set(string_set_string);

  EXPECT_EQ(elem_set_string.Get<std::string>(), string_set_string);
  EXPECT_EQ(elem_set_string.Get<DynamicByteBuffer>(), buffer_set_string);

  DataElement elem_set_buffer;
  elem_set_buffer.Set(string_set_buffer);

  EXPECT_EQ(elem_set_buffer.Get<DynamicByteBuffer>(), buffer_set_buffer);
  EXPECT_EQ(elem_set_buffer.Get<std::string>(), string_set_buffer);
}

}  // namespace
}  // namespace bt::sdp
