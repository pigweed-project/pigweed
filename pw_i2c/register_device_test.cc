// Copyright 2021 The Pigweed Authors
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
#include "pw_i2c/register_device.h"

#include <cstring>

#include "pw_assert/check.h"
#include "pw_bytes/byte_builder.h"
#include "pw_unit_test/framework.h"

namespace pw {
namespace i2c {
namespace {

using ::pw::Status;
using namespace std::literals::chrono_literals;

constexpr uint8_t kErrorValue = 0x11;
constexpr Address kTestDeviceAddress = Address::SevenBit<0x3F>();

constexpr chrono::SystemClock::duration kTimeout =
    std::chrono::duration_cast<chrono::SystemClock::duration>(100ms);

// Default test object. Mimics closely to I2c devices.
class TestInitiator : public Initiator {
 public:
  explicit TestInitiator() : Initiator(Initiator::Feature::kStandard) {}

  ByteBuilder& GetWriteBuffer() { return write_buffer_; }

  void SetReadData(ByteSpan read_data) {
    read_buffer_.append(read_data.data(), read_data.size());
  }

 private:
  Status DoTransferFor(span<const Message> messages,
                       chrono::SystemClock::duration) override {
    // Ensure we don't have more than one read or write for this test procedure.
    bool saw_read = false;
    bool saw_write = false;
    for (unsigned int i = 0; i < messages.size(); ++i) {
      if (messages[i].IsRead()) {
        PW_CHECK(!saw_read);
        saw_read = true;

        PW_CHECK_UINT_EQ(read_buffer_.size(),
                         messages[i].GetData().size(),
                         "Buffer to read is wrong size");
        std::copy(read_buffer_.begin(),
                  read_buffer_.end(),
                  messages[i].GetMutableData().begin());
      } else {
        PW_CHECK(!saw_write);
        saw_write = true;

        write_buffer_.append(messages[i].GetData().data(),
                             messages[i].GetData().size());
      }
    }

    return OkStatus();
  }

  ByteBuffer<10> write_buffer_;
  ByteBuffer<10> read_buffer_;
};

TEST(RegisterDevice, Construction) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k1Byte);
}

TEST(RegisterDevice, WriteRegisters8With2RegistersAnd1ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k1Byte);

  std::array<std::byte, 2> register_data = {std::byte{0xCD}, std::byte{0xEF}};
  std::array<std::byte, 3> builder;
  constexpr uint32_t kRegisterAddress = 0xAB;
  EXPECT_EQ(
      device.WriteRegisters(kRegisterAddress, register_data, builder, kTimeout),
      pw::OkStatus());

  ByteBuilder& test_device_builder = initiator.GetWriteBuffer();
  EXPECT_EQ(sizeof(builder), test_device_builder.size());

  // Check address.
  EXPECT_EQ(kRegisterAddress,
            static_cast<uint32_t>(test_device_builder.data()[0]));

  // Check data.
  constexpr uint32_t kAddressSize =
      static_cast<uint32_t>(RegisterAddressSize::k1Byte);
  for (uint32_t i = 0; i < test_device_builder.size() - kAddressSize; i++) {
    EXPECT_EQ(register_data[i], test_device_builder.data()[i + kAddressSize]);
  }
}

TEST(RegisterDevice, WriteRegisters8With2RegistersAnd2ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k2Bytes);

  constexpr uint32_t kRegisterAddress = 0x89AB;
  std::byte register_data[2] = {std::byte{0xCD}, std::byte{0xEF}};
  std::array<std::byte, 4> builder;
  EXPECT_EQ(
      device.WriteRegisters(kRegisterAddress, register_data, builder, kTimeout),
      pw::OkStatus());

  ByteBuilder& test_device_builder = initiator.GetWriteBuffer();
  EXPECT_EQ(sizeof(builder), test_device_builder.size());

  // Check address.
  uint16_t kActualAddress;
  std::memcpy(
      &kActualAddress, test_device_builder.data(), sizeof(kActualAddress));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  constexpr uint32_t kAddressSize =
      static_cast<uint32_t>(RegisterAddressSize::k2Bytes);
  for (uint32_t i = 0; i < test_device_builder.size() - kAddressSize; i++) {
    EXPECT_EQ(register_data[i], test_device_builder.data()[i + kAddressSize]);
  }
}

TEST(RegisterDevice, WriteRegisters16With2RegistersAnd2ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k2Bytes);

  constexpr uint32_t kRegisterAddress = 0x89AB;
  std::array<uint16_t, 2> register_data = {0xCDEF, 0x1234};
  std::array<std::byte, 6> builder;
  EXPECT_EQ(device.WriteRegisters16(
                kRegisterAddress, register_data, builder, kTimeout),
            pw::OkStatus());

  ByteBuilder& test_device_builder = initiator.GetWriteBuffer();
  EXPECT_EQ(sizeof(builder), test_device_builder.size());

  // Check address.
  uint16_t kActualAddress;
  std::memcpy(
      &kActualAddress, test_device_builder.data(), sizeof(kActualAddress));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  constexpr uint32_t kAddressSize =
      static_cast<uint32_t>(RegisterAddressSize::k2Bytes);

  for (uint32_t i = kAddressSize, j = 0; i < test_device_builder.size();
       i += kAddressSize, j++) {
    uint16_t actualData;
    std::memcpy(
        &actualData, &test_device_builder.data()[i], sizeof(actualData));
    EXPECT_EQ(register_data[j], actualData);
  }
}

TEST(RegisterDevice, WriteRegisters16With2RegistersAnd2ByteAddressBigEndian) {
  TestInitiator initiator;
  RegisterDevice device(
      initiator, kTestDeviceAddress, endian::big, RegisterAddressSize::k2Bytes);

  constexpr uint32_t kRegisterAddress = 0x89AB;
  std::array<uint16_t, 2> register_data = {0xCDEF, 0x1234};
  std::array<std::byte, 6> builder;
  EXPECT_EQ(device.WriteRegisters16(
                kRegisterAddress, register_data, builder, kTimeout),
            pw::OkStatus());

  ByteBuilder& test_device_builder = initiator.GetWriteBuffer();
  EXPECT_EQ(sizeof(builder), test_device_builder.size());

  // Check address.
  uint16_t kActualAddress;
  std::memcpy(
      &kActualAddress, test_device_builder.data(), sizeof(kActualAddress));
  EXPECT_EQ(bytes::ReadInOrder<uint16_t>(endian::big, &kRegisterAddress),
            kActualAddress);

  // Check data.
  constexpr uint32_t kAddressSize =
      static_cast<uint32_t>(RegisterAddressSize::k2Bytes);

  for (uint32_t i = kAddressSize, j = 0; i < test_device_builder.size();
       i += kAddressSize, j++) {
    uint16_t actualData;
    std::memcpy(
        &actualData, &test_device_builder.data()[i], sizeof(actualData));
    EXPECT_EQ(bytes::ReadInOrder<uint16_t>(endian::big, &register_data[j]),
              actualData);
  }
}

TEST(RegisterDevice, WriteRegisters8BufferTooSmall) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k2Bytes);

  constexpr uint32_t kRegisterAddress = 0x89AB;
  std::array<std::byte, 2> register_data = {std::byte{0xCD}, std::byte{0xEF}};
  std::array<std::byte, 2> builder;
  EXPECT_EQ(
      device.WriteRegisters(kRegisterAddress, register_data, builder, kTimeout),
      pw::Status::OutOfRange());
}

TEST(RegisterDevice, WriteRegister16With1ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k1Byte);

  constexpr uint32_t kRegisterAddress = 0xAB;
  constexpr uint16_t kRegisterData = 0xBCDE;
  EXPECT_EQ(device.WriteRegister16(kRegisterAddress, kRegisterData, kTimeout),
            pw::OkStatus());

  constexpr uint32_t kAddressSize =
      static_cast<uint32_t>(RegisterAddressSize::k1Byte);
  ByteBuilder& test_device_builder = initiator.GetWriteBuffer();
  EXPECT_EQ(test_device_builder.size(), kAddressSize + sizeof(kRegisterData));

  // Check address.
  EXPECT_EQ(kRegisterAddress,
            static_cast<uint32_t>(test_device_builder.data()[0]));

  // Check data.
  for (uint32_t i = 0; i < test_device_builder.size() - kAddressSize; i++) {
    EXPECT_EQ(
        (kRegisterData >> (8 * i)) & 0xFF,
        static_cast<uint16_t>(test_device_builder.data()[i + kAddressSize]));
  }
}

TEST(RegisterDevice, WriteRegister32With1ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k1Byte);

  constexpr uint32_t kRegisterAddress = 0xAB;
  constexpr uint32_t kRegisterData = 0xBCCDDEEF;
  EXPECT_EQ(device.WriteRegister32(kRegisterAddress, kRegisterData, kTimeout),
            pw::OkStatus());

  constexpr uint32_t kAddressSize =
      static_cast<uint32_t>(RegisterAddressSize::k1Byte);
  ByteBuilder& test_device_builder = initiator.GetWriteBuffer();
  EXPECT_EQ(test_device_builder.size(), kAddressSize + sizeof(kRegisterData));

  // Check address.
  EXPECT_EQ(kRegisterAddress,
            static_cast<uint32_t>(test_device_builder.data()[0]));

  // Check data.
  for (uint32_t i = 0; i < test_device_builder.size() - kAddressSize; i++) {
    EXPECT_EQ(
        (kRegisterData >> (8 * i)) & 0xFF,
        static_cast<uint32_t>(test_device_builder.data()[i + kAddressSize]));
  }
}

TEST(RegisterDevice, WriteRegister16with2ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k2Bytes);

  constexpr uint32_t kRegisterAddress = 0xAB23;
  constexpr uint16_t kRegisterData = 0xBCDD;
  EXPECT_EQ(device.WriteRegister16(kRegisterAddress, kRegisterData, kTimeout),
            pw::OkStatus());

  constexpr uint32_t kAddressSize =
      static_cast<uint32_t>(RegisterAddressSize::k2Bytes);
  ByteBuilder& test_device_builder = initiator.GetWriteBuffer();
  EXPECT_EQ(test_device_builder.size(), kAddressSize + sizeof(kRegisterData));

  // Check address.
  uint16_t kActualAddress;
  std::memcpy(
      &kActualAddress, test_device_builder.data(), sizeof(kActualAddress));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  for (uint32_t i = 0; i < test_device_builder.size() - kAddressSize; i++) {
    EXPECT_EQ(
        (kRegisterData >> (8 * i)) & 0xFF,
        static_cast<uint16_t>(test_device_builder.data()[i + kAddressSize]));
  }
}

TEST(RegisterDevice, WriteRegister16With1ByteAddressAndBigEndian) {
  TestInitiator initiator;
  RegisterDevice device(
      initiator, kTestDeviceAddress, endian::big, RegisterAddressSize::k1Byte);

  constexpr uint32_t kRegisterAddress = 0xAB;
  constexpr uint16_t kRegisterData = 0xBCDE;
  EXPECT_EQ(device.WriteRegister16(kRegisterAddress, kRegisterData, kTimeout),
            pw::OkStatus());

  constexpr uint32_t kAddressSize =
      static_cast<uint32_t>(RegisterAddressSize::k1Byte);
  ByteBuilder& test_device_builder = initiator.GetWriteBuffer();
  EXPECT_EQ(test_device_builder.size(), kAddressSize + sizeof(kRegisterData));

  // Check address.
  EXPECT_EQ(kRegisterAddress,
            static_cast<uint32_t>(test_device_builder.data()[0]));

  // Check data.
  for (uint32_t i = 0; i < test_device_builder.size() - kAddressSize; i++) {
    const uint32_t shift = static_cast<uint32_t>(test_device_builder.size() -
                                                 kAddressSize - (i + 1));
    EXPECT_EQ(
        (kRegisterData >> (8 * shift)) & 0xFF,
        static_cast<uint16_t>(test_device_builder.data()[i + kAddressSize]));
  }
}

TEST(RegisterDevice, WriteRegister32With1ByteAddressAndBigEndian) {
  TestInitiator initiator;
  RegisterDevice device(
      initiator, kTestDeviceAddress, endian::big, RegisterAddressSize::k1Byte);

  constexpr uint32_t kRegisterAddress = 0xAB;
  constexpr uint32_t kRegisterData = 0xBCCDDEEF;
  EXPECT_EQ(device.WriteRegister32(kRegisterAddress, kRegisterData, kTimeout),
            pw::OkStatus());

  constexpr uint32_t kAddressSize =
      static_cast<uint32_t>(RegisterAddressSize::k1Byte);
  ByteBuilder& test_device_builder = initiator.GetWriteBuffer();
  EXPECT_EQ(test_device_builder.size(), kAddressSize + sizeof(kRegisterData));

  // Check address.
  EXPECT_EQ(kRegisterAddress,
            static_cast<uint32_t>(test_device_builder.data()[0]));

  // Check data.
  for (uint32_t i = 0; i < test_device_builder.size() - kAddressSize; i++) {
    const uint32_t shift = static_cast<uint32_t>(test_device_builder.size() -
                                                 kAddressSize - (i + 1));
    EXPECT_EQ(
        (kRegisterData >> (8 * shift)) & 0xFF,
        static_cast<uint32_t>(test_device_builder.data()[i + kAddressSize]));
  }
}

TEST(RegisterDevice, WriteRegister16With2ByteAddressAndBigEndian) {
  TestInitiator initiator;
  RegisterDevice device(
      initiator, kTestDeviceAddress, endian::big, RegisterAddressSize::k2Bytes);

  constexpr uint32_t kRegisterAddress = 0xAB11;
  constexpr uint16_t kRegisterData = 0xBCDF;
  EXPECT_EQ(device.WriteRegister16(kRegisterAddress, kRegisterData, kTimeout),
            pw::OkStatus());

  constexpr uint32_t kAddressSize =
      static_cast<uint32_t>(RegisterAddressSize::k2Bytes);
  ByteBuilder& test_device_builder = initiator.GetWriteBuffer();
  EXPECT_EQ(test_device_builder.size(), kAddressSize + sizeof(kRegisterData));

  // Check address.
  uint16_t kActualAddress;
  std::memcpy(
      &kActualAddress, test_device_builder.data(), sizeof(kActualAddress));
  EXPECT_EQ(bytes::ReadInOrder<uint16_t>(endian::big, &kRegisterAddress),
            kActualAddress);

  // Check data.
  for (uint32_t i = 0; i < test_device_builder.size() - kAddressSize; i++) {
    const uint32_t shift = static_cast<uint32_t>(test_device_builder.size() -
                                                 kAddressSize - (i + 1));
    EXPECT_EQ(
        (kRegisterData >> (8 * shift)) & 0xFF,
        static_cast<uint16_t>(test_device_builder.data()[i + kAddressSize]));
  }
}

TEST(RegisterDevice, ReadRegisters8ByteWith2RegistersAnd1ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k1Byte);

  std::array<std::byte, 2> register_data = {std::byte{0xCD}, std::byte{0xEF}};
  initiator.SetReadData(register_data);

  std::array<std::byte, 2> buffer;
  constexpr uint32_t kRegisterAddress = 0xAB;
  EXPECT_EQ(device.ReadRegisters(kRegisterAddress, buffer, kTimeout),
            pw::OkStatus());

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k1Byte),
            address_buffer.size());

  const uint8_t kActualAddress = *(reinterpret_cast<uint8_t*>(
      const_cast<std::byte*>(address_buffer.data())));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  for (uint32_t i = 0; i < sizeof(buffer); i++) {
    EXPECT_EQ(buffer[i], register_data[i]);
  }
}

TEST(RegisterDevice, ReadRegisters8IntWith2RegistersAnd1ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k1Byte);

  std::array<uint8_t, 2> register_data = {0xCD, 0xEF};
  initiator.SetReadData(
      as_writable_bytes(span(register_data.data(), register_data.size())));

  std::array<uint8_t, 2> buffer;
  constexpr uint32_t kRegisterAddress = 0xAB;
  EXPECT_EQ(device.ReadRegisters8(kRegisterAddress, buffer, kTimeout),
            pw::OkStatus());

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k1Byte),
            address_buffer.size());

  const uint8_t kActualAddress = *(reinterpret_cast<uint8_t*>(
      const_cast<std::byte*>(address_buffer.data())));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  for (uint32_t i = 0; i < sizeof(buffer); i++) {
    EXPECT_EQ(buffer[i], register_data[i]);
  }
}

TEST(RegisterDevice, ReadRegisters8ByteWith2RegistersAnd2ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k2Bytes);

  std::array<std::byte, 2> register_data = {std::byte{0xCD}, std::byte{0xEF}};
  initiator.SetReadData(register_data);

  std::array<std::byte, 2> buffer;
  constexpr uint32_t kRegisterAddress = 0xABBA;
  EXPECT_EQ(device.ReadRegisters(kRegisterAddress, buffer, kTimeout),
            pw::OkStatus());

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k2Bytes),
            address_buffer.size());

  uint16_t kActualAddress;
  std::memcpy(&kActualAddress, address_buffer.data(), sizeof(kActualAddress));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  for (uint32_t i = 0; i < sizeof(buffer); i++) {
    EXPECT_EQ(buffer[i], register_data[i]);
  }
}

TEST(RegisterDevice, ReadRegisters16With2RegistersAnd2ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k2Bytes);

  std::array<uint16_t, 2> register_data = {0xCDEF, 0x1234};
  initiator.SetReadData(
      as_writable_bytes(span(register_data.data(), register_data.size())));

  std::array<uint16_t, 2> buffer;
  constexpr uint32_t kRegisterAddress = 0xAB;
  EXPECT_EQ(device.ReadRegisters16(kRegisterAddress, buffer, kTimeout),
            pw::OkStatus());

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k2Bytes),
            address_buffer.size());

  uint16_t kActualAddress;
  std::memcpy(&kActualAddress, address_buffer.data(), sizeof(kActualAddress));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  for (uint32_t i = 0; i < buffer.size(); i++) {
    EXPECT_EQ(buffer[i], register_data[i]);
  }
}

TEST(RegisterDevice, ReadRegisters16With2RegistersAnd2ByteAddressBigEndian) {
  TestInitiator initiator;
  RegisterDevice device(
      initiator, kTestDeviceAddress, endian::big, RegisterAddressSize::k2Bytes);

  std::array<uint16_t, 2> register_data = {0xCDEF, 0x1234};
  initiator.SetReadData(
      as_writable_bytes(span(register_data.data(), register_data.size())));

  std::array<uint16_t, 2> buffer;
  constexpr uint32_t kRegisterAddress = 0xAB;
  EXPECT_EQ(device.ReadRegisters16(kRegisterAddress, buffer, kTimeout),
            pw::OkStatus());

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k2Bytes),
            address_buffer.size());

  uint16_t kActualAddress;
  std::memcpy(&kActualAddress, address_buffer.data(), sizeof(kActualAddress));
  EXPECT_EQ(bytes::ReadInOrder<uint16_t>(endian::big, &kRegisterAddress),
            kActualAddress);

  // Check data.
  for (uint32_t i = 0; i < buffer.size(); i++) {
    EXPECT_EQ(bytes::ReadInOrder<uint16_t>(endian::big, &register_data[i]),
              buffer[i]);
  }
}

TEST(RegisterDevice, ReadRegister16With1ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k1Byte);

  std::array<std::byte, 2> register_data = {std::byte{0xCD}, std::byte{0xEF}};
  initiator.SetReadData(register_data);

  constexpr uint32_t kRegisterAddress = 0xAB;
  Result<uint16_t> result = device.ReadRegister16(kRegisterAddress, kTimeout);
  EXPECT_TRUE(result.ok());
  uint16_t read_data = result.value_or(kErrorValue);

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k1Byte),
            address_buffer.size());

  const uint8_t kActualAddress = *(reinterpret_cast<uint8_t*>(
      const_cast<std::byte*>(address_buffer.data())));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  uint8_t* read_pointer = reinterpret_cast<uint8_t*>(&read_data);
  for (uint32_t i = 0; i < sizeof(read_data); i++) {
    EXPECT_EQ(read_pointer[i], static_cast<uint8_t>(register_data[i]));
  }
}

TEST(RegisterDevice, ReadRegister32With1ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k1Byte);

  std::array<std::byte, 4> register_data = {
      std::byte{0x98}, std::byte{0x76}, std::byte{0x54}, std::byte{0x32}};
  initiator.SetReadData(register_data);

  constexpr uint32_t kRegisterAddress = 0xAB;
  Result<uint32_t> result = device.ReadRegister32(kRegisterAddress, kTimeout);
  EXPECT_TRUE(result.ok());
  uint32_t read_data = result.value_or(kErrorValue);

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k1Byte),
            address_buffer.size());

  const uint8_t kActualAddress = *(reinterpret_cast<uint8_t*>(
      const_cast<std::byte*>(address_buffer.data())));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  uint8_t* read_pointer = reinterpret_cast<uint8_t*>(&read_data);
  for (uint32_t i = 0; i < sizeof(read_data); i++) {
    EXPECT_EQ(read_pointer[i], static_cast<uint8_t>(register_data[i]));
  }
}

TEST(RegisterDevice, ReadRegister16With2ByteAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::little,
                        RegisterAddressSize::k2Bytes);

  std::array<std::byte, 2> register_data = {std::byte{0x98}, std::byte{0x76}};
  initiator.SetReadData(register_data);

  constexpr uint32_t kRegisterAddress = 0xA4AB;
  Result<uint16_t> result = device.ReadRegister16(kRegisterAddress, kTimeout);
  EXPECT_TRUE(result.ok());
  uint16_t read_data = result.value_or(kErrorValue);

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k2Bytes),
            address_buffer.size());

  uint16_t kActualAddress;
  std::memcpy(&kActualAddress, address_buffer.data(), sizeof(kActualAddress));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  uint8_t* read_pointer = reinterpret_cast<uint8_t*>(&read_data);
  for (uint32_t i = 0; i < sizeof(read_data); i++) {
    EXPECT_EQ(read_pointer[i], static_cast<uint8_t>(register_data[i]));
  }
}

TEST(RegisterDevice, ReadRegister16With1ByteAddressAndBigEndian) {
  TestInitiator initiator;
  RegisterDevice device(
      initiator, kTestDeviceAddress, endian::big, RegisterAddressSize::k1Byte);

  std::array<std::byte, 2> register_data = {std::byte{0x98}, std::byte{0x76}};
  initiator.SetReadData(register_data);

  constexpr uint32_t kRegisterAddress = 0xAB;
  Result<uint16_t> result = device.ReadRegister16(kRegisterAddress, kTimeout);
  EXPECT_TRUE(result.ok());
  uint16_t read_data = result.value_or(kErrorValue);

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k1Byte),
            address_buffer.size());

  const uint8_t kActualAddress = *(reinterpret_cast<uint8_t*>(
      const_cast<std::byte*>(address_buffer.data())));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  uint8_t* read_pointer = reinterpret_cast<uint8_t*>(&read_data);
  for (uint32_t i = 0; i < sizeof(read_data); i++) {
    const uint32_t kReadPointerIndex = sizeof(read_data) - 1 - i;
    EXPECT_EQ(read_pointer[kReadPointerIndex],
              static_cast<uint8_t>(register_data[i]));
  }
}

TEST(RegisterDevice, ReadRegister32With1ByteAddressAndBigEndian) {
  TestInitiator initiator;
  RegisterDevice device(
      initiator, kTestDeviceAddress, endian::big, RegisterAddressSize::k1Byte);

  std::array<std::byte, 4> register_data = {
      std::byte{0x98}, std::byte{0x76}, std::byte{0x54}, std::byte{0x32}};
  initiator.SetReadData(register_data);

  constexpr uint32_t kRegisterAddress = 0xAB;
  Result<uint32_t> result = device.ReadRegister32(kRegisterAddress, kTimeout);
  EXPECT_TRUE(result.ok());
  uint32_t read_data = result.value_or(kErrorValue);

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k1Byte),
            address_buffer.size());

  const uint8_t kActualAddress = *(reinterpret_cast<uint8_t*>(
      const_cast<std::byte*>(address_buffer.data())));
  EXPECT_EQ(kRegisterAddress, kActualAddress);

  // Check data.
  uint8_t* read_pointer = reinterpret_cast<uint8_t*>(&read_data);
  for (uint32_t i = 0; i < sizeof(read_data); i++) {
    const uint32_t kReadPointerIndex = sizeof(read_data) - 1 - i;
    EXPECT_EQ(read_pointer[kReadPointerIndex],
              static_cast<uint8_t>(register_data[i]));
  }
}

TEST(RegisterDevice, ReadRegister16With2ByteAddressAndBigEndian) {
  TestInitiator initiator;
  RegisterDevice device(
      initiator, kTestDeviceAddress, endian::big, RegisterAddressSize::k2Bytes);

  std::array<std::byte, 2> register_data = {std::byte{0x98}, std::byte{0x76}};
  initiator.SetReadData(register_data);

  constexpr uint32_t kRegisterAddress = 0xABEF;
  Result<uint16_t> result = device.ReadRegister16(kRegisterAddress, kTimeout);
  EXPECT_TRUE(result.ok());
  uint16_t read_data = result.value_or(kErrorValue);

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k2Bytes),
            address_buffer.size());

  uint16_t kActualAddress;
  std::memcpy(&kActualAddress, address_buffer.data(), sizeof(kActualAddress));
  EXPECT_EQ(bytes::ReadInOrder<uint16_t>(endian::big, &kRegisterAddress),
            kActualAddress);

  // Check data.
  uint8_t* read_pointer = reinterpret_cast<uint8_t*>(&read_data);
  for (uint32_t i = 0; i < sizeof(read_data); i++) {
    const uint32_t kReadPointerIndex = sizeof(read_data) - 1 - i;
    EXPECT_EQ(read_pointer[kReadPointerIndex],
              static_cast<uint8_t>(register_data[i]));
  }
}

TEST(RegisterDevice, ReadRegister16With2ByteBigEndianAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::big,
                        endian::little,
                        RegisterAddressSize::k2Bytes);

  std::array<std::byte, 2> register_data = {std::byte{0x98}, std::byte{0x76}};
  initiator.SetReadData(register_data);

  constexpr uint32_t kRegisterAddress = 0xABEF;
  Result<uint16_t> result = device.ReadRegister16(kRegisterAddress, kTimeout);
  EXPECT_TRUE(result.ok());
  uint16_t read_data = result.value_or(kErrorValue);

  // Check address.
  ByteBuilder& address_buffer = initiator.GetWriteBuffer();
  EXPECT_EQ(static_cast<uint32_t>(RegisterAddressSize::k2Bytes),
            address_buffer.size());

  uint16_t kActualAddress;
  std::memcpy(&kActualAddress, address_buffer.data(), sizeof(kActualAddress));
  EXPECT_EQ(bytes::ReadInOrder<uint16_t>(endian::big, &kRegisterAddress),
            kActualAddress);

  // Check data.
  uint8_t* read_pointer = reinterpret_cast<uint8_t*>(&read_data);
  for (uint32_t i = 0; i < sizeof(read_data); i++) {
    EXPECT_EQ(read_pointer[i], static_cast<uint8_t>(register_data[i]));
  }
}

TEST(RegisterDevice, WriteRegister16with2ByteBigEndianAddress) {
  TestInitiator initiator;
  RegisterDevice device(initiator,
                        kTestDeviceAddress,
                        endian::big,
                        endian::little,
                        RegisterAddressSize::k2Bytes);

  constexpr uint32_t kRegisterAddress = 0xAB11;
  constexpr uint16_t kRegisterData = 0xBCDF;
  EXPECT_EQ(device.WriteRegister16(kRegisterAddress, kRegisterData, kTimeout),
            pw::OkStatus());

  constexpr uint32_t kAddressSize =
      static_cast<uint32_t>(RegisterAddressSize::k2Bytes);
  ByteBuilder& test_device_builder = initiator.GetWriteBuffer();
  EXPECT_EQ(test_device_builder.size(), kAddressSize + sizeof(kRegisterData));

  // Check address.
  uint16_t kActualAddress;
  std::memcpy(
      &kActualAddress, test_device_builder.data(), sizeof(kActualAddress));
  EXPECT_EQ(bytes::ReadInOrder<uint16_t>(endian::big, &kRegisterAddress),
            kActualAddress);

  // Check data.
  for (uint32_t i = 0; i < test_device_builder.size() - kAddressSize; i++) {
    EXPECT_EQ(
        (kRegisterData >> (8 * i)) & 0xFF,
        static_cast<uint16_t>(test_device_builder.data()[i + kAddressSize]));
  }
}

}  // namespace
}  // namespace i2c
}  // namespace pw
