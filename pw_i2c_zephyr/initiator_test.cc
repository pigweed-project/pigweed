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

#define CUSTOM_FFF_FUNCTION_TEMPLATE(RETURN, FUNCNAME, ...) \
  std::function<RETURN(__VA_ARGS__)> FUNCNAME

#include "pw_i2c_zephyr/initiator.h"

#include "pw_preprocessor/compiler.h"

// Some Zephyr headers (e.g. zephyr/drivers/mspi.h via emul.h) use partial
// designated initializers, which clang warns about when compiled as C++.
PW_MODIFY_DIAGNOSTICS_PUSH();
PW_MODIFY_DIAGNOSTIC_CLANG(ignored, "-Wmissing-designated-field-initializers");
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/fff.h>
PW_MODIFY_DIAGNOSTICS_POP();

#include "pw_function/function.h"
#include "pw_i2c/device.h"
#include "pw_unit_test/framework.h"

DEFINE_FFF_GLOBALS;

namespace pw::i2c {
namespace {
constexpr uint8_t kTestAddress =
    DT_PHA_BY_IDX(DT_NODELABEL(i2c0), forwards, 0, addr);

// int (const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
FAKE_VALUE_FUNC(int, TargetReadRequested, struct i2c_target_config*, uint8_t*);
FAKE_VALUE_FUNC(int, TargetReadProcessed, struct i2c_target_config*, uint8_t*);
FAKE_VALUE_FUNC(int, TargetWriteRequested, struct i2c_target_config*);
FAKE_VALUE_FUNC(int, TargetWriteReceived, struct i2c_target_config*, uint8_t);
FAKE_VALUE_FUNC(int, TargetStop, struct i2c_target_config*);

// Assign fields individually so the test builds with Zephyr versions that add
// callbacks to i2c_target_callbacks (e.g. the error callback); unset callbacks
// stay null.
struct i2c_target_callbacks mock_callbacks = [] {
  struct i2c_target_callbacks callbacks = {};
  callbacks.write_requested = TargetWriteRequested;
  callbacks.read_requested = TargetReadRequested;
  callbacks.write_received = TargetWriteReceived;
  callbacks.read_processed = TargetReadProcessed;
  callbacks.stop = TargetStop;
  return callbacks;
}();

struct i2c_target_config mock_target_config = {
    .node = {},
    .flags = 0,
    .address = kTestAddress,
    .callbacks = &mock_callbacks,
};

ZephyrInitiator kInitiator(DEVICE_DT_GET(DT_NODELABEL(i2c0)));
Device kI2cDev(kInitiator, Address::SevenBit<kTestAddress>());

class InitiatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RESET_FAKE(TargetReadRequested);
    RESET_FAKE(TargetReadProcessed);
    RESET_FAKE(TargetWriteRequested);
    RESET_FAKE(TargetWriteReceived);
    RESET_FAKE(TargetStop);
    FFF_RESET_HISTORY();

    PW_ASSERT(i2c_target_register(DEVICE_DT_GET(DT_NODELABEL(i2c1)),
                                  &mock_target_config) == 0);
  }
};

TEST_F(InitiatorTest, WriteRead) {
  uint8_t tx_buffer[] = {0x01, 0x02, 0x00, 0x00};
  uint8_t rx_buffer[] = {0x00, 0x00};

  TargetWriteReceived_fake.custom_fake = [&tx_buffer](struct i2c_target_config*,
                                                      uint8_t value) {
    // Copy the value to the tail end of the tx buffer (for later asserts)
    tx_buffer[TargetWriteReceived_fake.call_count + 1] = value;
    return 0;
  };
  // The first read byte calls the read requested function, read a 1
  TargetReadRequested_fake.custom_fake = [](struct i2c_target_config*,
                                            uint8_t* value) {
    *value = UINT8_C(1);
    return 0;
  };
  // All following bytes call read processed function, read a 2
  TargetReadProcessed_fake.custom_fake = [](struct i2c_target_config*,
                                            uint8_t* value) {
    *value = UINT8_C(2);
    return 0;
  };
  ASSERT_EQ(kI2cDev.WriteReadFor(tx_buffer,
                                 sizeof(rx_buffer),
                                 rx_buffer,
                                 sizeof(rx_buffer),
                                 pw::chrono::SystemClock::duration::max()),
            pw::OkStatus());
  ASSERT_EQ(tx_buffer[0], tx_buffer[2]);
  ASSERT_EQ(tx_buffer[1], tx_buffer[3]);
  ASSERT_EQ(1, rx_buffer[0]);
  ASSERT_EQ(2, rx_buffer[1]);
}

TEST_F(InitiatorTest, Write) {
  uint8_t tx_buffer[] = {0x01, 0x02, 0x00, 0x00};

  TargetWriteReceived_fake.custom_fake = [&tx_buffer](struct i2c_target_config*,
                                                      uint8_t value) {
    // Copy the value to the tail end of the tx buffer (for later asserts)
    tx_buffer[TargetWriteReceived_fake.call_count + 1] = value;
    return 0;
  };
  ASSERT_EQ(kI2cDev.WriteFor(tx_buffer,
                             sizeof(tx_buffer),
                             pw::chrono::SystemClock::duration::max()),
            pw::OkStatus());
  ASSERT_EQ(tx_buffer[0], tx_buffer[2]);
  ASSERT_EQ(tx_buffer[1], tx_buffer[3]);
}

TEST_F(InitiatorTest, Read) {
  uint8_t rx_buffer[] = {0x00, 0x00};

  // The first read byte calls the read requested function, read a 1
  TargetReadRequested_fake.custom_fake = [](struct i2c_target_config*,
                                            uint8_t* value) {
    *value = UINT8_C(1);
    return 0;
  };
  // All following bytes call read processed function, read a 2
  TargetReadProcessed_fake.custom_fake = [](struct i2c_target_config*,
                                            uint8_t* value) {
    *value = UINT8_C(2);
    return 0;
  };
  ASSERT_EQ(kI2cDev.ReadFor(rx_buffer,
                            sizeof(rx_buffer),
                            pw::chrono::SystemClock::duration::max()),
            pw::OkStatus());
  ASSERT_EQ(1, rx_buffer[0]);
  ASSERT_EQ(2, rx_buffer[1]);
}

}  // namespace
}  // namespace pw::i2c
