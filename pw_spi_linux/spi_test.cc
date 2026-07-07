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

#include "pw_spi_linux/spi.h"

#include <linux/spi/spidev.h>

#include <array>
#include <vector>

#include "pw_bytes/suffix.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

namespace pw::spi {
namespace {

using ::pw::operator""_b;

const pw::spi::Config kConfig = {.polarity = ClockPolarity::kActiveHigh,
                                 .phase = ClockPhase::kFallingEdge,
                                 .bits_per_word = BitsPerWord(8),
                                 .bit_order = BitOrder::kMsbFirst};
const uint32_t kMaxSpeed = 2345678;
const int kFakeFd = 9999;

bool SpanEq(ConstByteSpan span1, ConstByteSpan span2) {
  if (span1.size() != span2.size()) {
    return false;
  }

  for (size_t i = 0; i < span1.size(); i++) {
    if (span1[i] != span2[i]) {
      return false;
    }
  }

  return true;
}

//
// A mock ioctl() implementation which records requests and transfers.
//
std::vector<unsigned long> ioctl_requests;
std::vector<struct spi_ioc_transfer> ioctl_transfers;

union spi_ioc_arg {
  uint32_t u32;
  struct spi_ioc_transfer* transfer;
};

extern "C" int ioctl(int fd, unsigned long request, union spi_ioc_arg arg) {
  // Only the fake SPI fd is handled.
  if (fd != kFakeFd) {
    return -1;
  }

  // Only "write" ioctls are mocked currently.
  // Otherwise the caller would not get any result.
  // (The rx_buf of SPI_IOC_MESSAGE is an exception.)
  if (_IOC_DIR(request) != _IOC_WRITE) {
    return -1;
  }

  // Only SPI ioctls are mocked.
  if (_IOC_TYPE(request) != SPI_IOC_MAGIC) {
    return -1;
  }

  // Record the ioctl request code.
  ioctl_requests.push_back(request);

  // Record the individual transfers.
  if (_IOC_NR(request) == _IOC_NR(SPI_IOC_MESSAGE(1))) {
    if (_IOC_SIZE(request) % sizeof(struct spi_ioc_transfer) != 0) {
      return -1;
    }
    int num_transfers = _IOC_SIZE(request) / sizeof(struct spi_ioc_transfer);
    for (int i = 0; i < num_transfers; i++) {
      struct spi_ioc_transfer& transfer = arg.transfer[i];
      ioctl_transfers.push_back(transfer);
      // TODO(jrreinhart): If desired, write some data to transfer.rx_buf.
    }
  }

  return 0;
}

class LinuxSpiTest : public ::testing::Test {
 public:
  LinuxSpiTest() : initiator(kFakeFd, kMaxSpeed) {}

 protected:
  void SetUp() override {
    ioctl_requests.clear();
    ioctl_transfers.clear();
  }

  LinuxInitiator initiator;
};

//
// Tests
//

TEST_F(LinuxSpiTest, ConfigureWorks) {
  PW_TEST_EXPECT_OK(initiator.Configure(kConfig));

  std::vector<unsigned long> expect{
      SPI_IOC_WR_MODE32,
      SPI_IOC_WR_LSB_FIRST,
      SPI_IOC_WR_BITS_PER_WORD,
      SPI_IOC_WR_MAX_SPEED_HZ,
  };

  std::sort(ioctl_requests.begin(), ioctl_requests.end());
  std::sort(expect.begin(), expect.end());
  EXPECT_EQ(ioctl_requests, expect);
}

TEST_F(LinuxSpiTest, NoReadOrWrite) {
  EXPECT_EQ(initiator.WriteRead({}, {}), Status::InvalidArgument());

  EXPECT_EQ(ioctl_requests.size(), 0u);
  EXPECT_EQ(ioctl_transfers.size(), 0u);
}

TEST_F(LinuxSpiTest, WriteOnly) {
  // Write only
  constexpr size_t kNumBytes = 4;
  const std::array<std::byte, kNumBytes> write_buf = {1_b, 2_b, 3_b, 4_b};

  PW_TEST_EXPECT_OK(initiator.WriteRead(write_buf, {}));

  EXPECT_EQ(ioctl_requests.size(), 1u);
  EXPECT_EQ(ioctl_transfers.size(), 1u);

  // Transfer 0: tx={1, 2, 3, 4}, rx==null
  auto& xfer0 = ioctl_transfers[0];
  EXPECT_EQ(xfer0.len, kNumBytes);
  EXPECT_EQ(xfer0.rx_buf, 0u);
  EXPECT_EQ(xfer0.tx_buf, reinterpret_cast<uintptr_t>(write_buf.data()));
  ByteSpan xfer0_tx(reinterpret_cast<std::byte*>(xfer0.tx_buf), xfer0.len);
  EXPECT_TRUE(SpanEq(xfer0_tx, write_buf));
}

TEST_F(LinuxSpiTest, ReadOnly) {
  // Read only
  constexpr size_t kNumBytes = 4;
  std::array<std::byte, kNumBytes> read_buf;

  PW_TEST_EXPECT_OK(initiator.WriteRead({}, read_buf));

  EXPECT_EQ(ioctl_requests.size(), 1u);
  EXPECT_EQ(ioctl_transfers.size(), 1u);

  // Transfer 0: tx==null, rx!=null
  auto& xfer0 = ioctl_transfers[0];
  EXPECT_EQ(xfer0.len, kNumBytes);
  EXPECT_EQ(xfer0.rx_buf, reinterpret_cast<uintptr_t>(read_buf.data()));
  EXPECT_EQ(xfer0.tx_buf, 0u);
}

TEST_F(LinuxSpiTest, WriteReadEqualSize) {
  // Write = Read
  constexpr size_t kNumBytes = 4;
  const std::array<std::byte, kNumBytes> write_buf = {1_b, 2_b, 3_b, 4_b};
  std::array<std::byte, kNumBytes> read_buf;

  PW_TEST_EXPECT_OK(initiator.WriteRead(write_buf, read_buf));

  EXPECT_EQ(ioctl_requests.size(), 1u);
  EXPECT_EQ(ioctl_transfers.size(), 1u);

  // Transfer 0: Common tx={1, 2, 3, 4}, rx!=null
  auto& xfer0 = ioctl_transfers[0];
  EXPECT_EQ(xfer0.len, kNumBytes);
  EXPECT_NE(xfer0.rx_buf, 0u);
  EXPECT_NE(xfer0.tx_buf, 0u);
  ByteSpan xfer0_tx(reinterpret_cast<std::byte*>(xfer0.tx_buf), xfer0.len);
  EXPECT_TRUE(SpanEq(xfer0_tx, write_buf));
}

TEST_F(LinuxSpiTest, WriteLargerThanReadSize) {
  // Write > Read
  const std::array<std::byte, 5> write_buf = {1_b, 2_b, 3_b, 4_b, 5_b};
  std::array<std::byte, 2> read_buf;

  PW_TEST_EXPECT_OK(initiator.WriteRead(write_buf, read_buf));

  EXPECT_EQ(ioctl_requests.size(), 1u);
  EXPECT_EQ(ioctl_transfers.size(), 2u);  // split

  // Transfer 0: Common tx={1, 2}, rx!=null
  auto& xfer0 = ioctl_transfers[0];
  EXPECT_EQ(xfer0.len, 2u);
  EXPECT_NE(xfer0.rx_buf, 0u);
  EXPECT_NE(xfer0.tx_buf, 0u);
  ByteSpan xfer0_tx(reinterpret_cast<std::byte*>(xfer0.tx_buf), xfer0.len);
  EXPECT_TRUE(SpanEq(xfer0_tx, std::array{1_b, 2_b}));

  // Transfer 1: Remainder tx={3, 4, 5}, rx=null
  auto& xfer1 = ioctl_transfers[1];
  EXPECT_EQ(xfer1.len, 3u);
  EXPECT_NE(xfer1.tx_buf, 0u);
  EXPECT_EQ(xfer1.rx_buf, 0u);
  ByteSpan xfer1_tx(reinterpret_cast<std::byte*>(xfer1.tx_buf), xfer1.len);
  EXPECT_TRUE(SpanEq(xfer1_tx, std::array{3_b, 4_b, 5_b}));
}

TEST_F(LinuxSpiTest, ReadLargerThanWriteSize) {
  // Read > Write
  const std::array<std::byte, 2> write_buf = {1_b, 2_b};
  std::array<std::byte, 5> read_buf;

  PW_TEST_EXPECT_OK(initiator.WriteRead(write_buf, read_buf));

  EXPECT_EQ(ioctl_requests.size(), 1u);
  EXPECT_EQ(ioctl_transfers.size(), 2u);  // split

  // Transfer 0: Common tx={1, 2}, rx!=null
  auto& xfer0 = ioctl_transfers[0];
  EXPECT_EQ(xfer0.len, 2u);
  EXPECT_NE(xfer0.rx_buf, 0u);
  EXPECT_NE(xfer0.tx_buf, 0u);
  ByteSpan xfer0_tx(reinterpret_cast<std::byte*>(xfer0.tx_buf), xfer0.len);
  EXPECT_TRUE(SpanEq(xfer0_tx, std::array{1_b, 2_b}));

  // Transfer 1: Remainder tx=null, rx!=null
  auto& xfer1 = ioctl_transfers[1];
  EXPECT_EQ(xfer1.len, 3u);
  EXPECT_EQ(xfer1.tx_buf, 0u);
  EXPECT_NE(xfer1.rx_buf, 0u);
}

}  // namespace
}  // namespace pw::spi
