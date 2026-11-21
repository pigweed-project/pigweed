// Copyright 2026 The Pigweed Authors
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

#include <cstdint>

#include "enum_example/i2c_status.h"
#include "pw_log/log.h"
#include "pw_log/status.h"
#include "pw_log/tokenized_args.h"
#include "pw_status/message.h"
#include "pw_status/try.h"
#include "pw_unit_test/framework.h"

namespace {

using my_driver::I2cStatus;

// DOCSTAG: [pw_enum-examples-status-enum-usage]
I2cStatus WriteByte(uint8_t address, uint8_t byte) {
  if (address == 0) {
    return I2cStatus::Nack();  // Factory for I2cStatusCode::kNack
  }
  if (byte == 0xFF) {
    return I2cStatus(I2cStatus::Code::kBusError);  // Explicit construction
  }
  return I2cStatus();  // Default constructed as OK
}

I2cStatus WritePacket(uint8_t address, uint8_t header, uint8_t payload) {
  PW_TRY(WriteByte(address, header));  // Returns early on error
  PW_TRY(WriteByte(address, payload));
  return I2cStatus();
}

bool ShouldRetry(I2cStatus status) {
  return status.IsNack() || status.IsTimeout();  // Query the code
}
// DOCSTAG: [pw_enum-examples-status-enum-usage]

// DOCSTAG: [pw_enum-examples-message-status-usage]
I2cStatus ReadSensor(bool ack) {
  if (!ack) {
    return I2cStatus::Nack(PW_STATUS_MSG("No ACK from sensor"));
  }
  return I2cStatus();
}
// DOCSTAG: [pw_enum-examples-message-status-usage]

// DOCSTAG: [pw_enum-examples-status-enum-logging]
void LogI2cStatus(const I2cStatus& status) {
  // Log the status and its message: pair <STATUS>_FMT with PW_LOG_STATUS. Logs
  // "I2C result: OK" or "I2C result: NACK (No ACK from sensor)" (just
  // "I2C result: NACK" unless PW_STATUS_CFG_MESSAGES enables messages).
  PW_LOG_INFO("I2C result: " MY_DRIVER_I2C_STATUS_FMT, PW_LOG_STATUS(status));

  // Log only the status code: pair <STATUS>_CODE_FMT with PW_LOG_ENUM.
  PW_LOG_INFO("I2C code: " MY_DRIVER_I2C_STATUS_CODE_FMT, PW_LOG_ENUM(status));
}

// Generic code does not know the status's macros, so it logs the status code
// with PW_LOG_ENUM_VERSIONED_FMT and PW_LOG_ENUM_VERSIONED.
template <typename Status>
void LogStatusCode(const Status& status) {
  PW_LOG_INFO("Code: " PW_LOG_ENUM_VERSIONED_FMT(),
              PW_LOG_ENUM_VERSIONED(status));
}
// DOCSTAG: [pw_enum-examples-status-enum-logging]

TEST(StatusEnumExample, Ok) {
  I2cStatus status = WritePacket(0x42, 0x01, 0x02);
  PW_TEST_EXPECT_OK(status);
  EXPECT_EQ(status, I2cStatus::Code::kOk);
  EXPECT_STREQ(status.str(), "OK");
  EXPECT_FALSE(ShouldRetry(status));
}

TEST(StatusEnumExample, Error) {
  I2cStatus status = WritePacket(0, 0x01, 0x02);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status, I2cStatus::Nack());
  EXPECT_EQ(status.code(), I2cStatus::Code::kNack);
  EXPECT_STREQ(status.str(), "NACK");
  EXPECT_TRUE(status.IsNack());
  EXPECT_FALSE(status.IsBusError());
  EXPECT_TRUE(ShouldRetry(status));

  I2cStatus bus_error = WritePacket(0x42, 0x01, 0xFF);
  EXPECT_TRUE(bus_error.IsBusError());
  EXPECT_FALSE(ShouldRetry(bus_error));
}

TEST(StatusEnumExample, Logging) {
  LogI2cStatus(I2cStatus());
  LogI2cStatus(I2cStatus::Timeout());
  LogStatusCode(I2cStatus::Nack());
}

TEST(StatusEnumExample, MessageStatus) {
  I2cStatus ok = ReadSensor(true);
  PW_TEST_EXPECT_OK(ok);
  EXPECT_FALSE(ok.has_message());
  LogI2cStatus(ok);

  I2cStatus err = ReadSensor(false);
  EXPECT_FALSE(err.ok());
  EXPECT_TRUE(err.IsNack());
  // The message is only stored if PW_STATUS_CFG_MESSAGES enables messages.
  EXPECT_EQ(err.has_message(),
            PW_STATUS_CFG_MESSAGES != PW_STATUS_CFG_MESSAGES_NONE);
  LogI2cStatus(err);
}

}  // namespace
