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

#include "pw_bluetooth_proxy_private/test_utils.h"

namespace pw::bluetooth::proxy {
namespace {

// ########## UtilsTest

TEST(UtilsTest, SetupKFrameProperlySegments) {
  std::array<uint8_t, 23> expected_payload = {1,  2,  3,  4,  5,  6,  7,  8,
                                              9,  10, 11, 12, 13, 14, 15, 16,
                                              17, 18, 19, 20, 21, 22, 23};
  ASSERT_GE(expected_payload.size(), 2ul);
  constexpr uint16_t kHandle = 0x123;
  constexpr uint16_t kCid = 0x456;

  // Validate the segmentation of `expected_payload` based on every MPS from 2
  // octets up to 5 octets greater than the length of `expected_payload`.
  for (size_t mps = 2; mps < expected_payload.size() + 5; ++mps) {
    size_t segment_no;
    size_t pdu_total_length = expected_payload.size() + kSduLengthFieldSize;
    size_t total_num_segments = 1;
    size_t final_segment_payload_size =
        std::min(mps, expected_payload.size() + kSduLengthFieldSize);
    // Only if total L2CAP PDU payload bytes to be sent are greater than MPS
    // will multiple segments be sent.
    if (mps < pdu_total_length) {
      total_num_segments = pdu_total_length / mps;
      // Account for the final segment, which may be sub-MPS sized and not
      // accounted for in the above int division operation.
      if (pdu_total_length % mps != 0) {
        ++total_num_segments;
        final_segment_payload_size = pdu_total_length % mps;
      }
    }

    for (segment_no = 0; segment_no < total_num_segments; ++segment_no) {
      PW_TEST_ASSERT_OK_AND_ASSIGN(
          KFrameWithStorage kframe,
          SetupKFrame(kHandle,
                      kCid,
                      static_cast<uint16_t>(mps),
                      static_cast<uint16_t>(segment_no),
                      span(expected_payload)));

      uint16_t pdu_length = static_cast<uint16_t>(mps);
      if (segment_no == total_num_segments - 1) {
        pdu_length = static_cast<uint16_t>(final_segment_payload_size);
      }

      // Validate ACL header.
      emboss::AclDataFrameView acl = kframe.acl.writer;
      EXPECT_EQ(acl.header().handle().Read(), kHandle);
      EXPECT_EQ(acl.data_total_length().Read(),
                emboss::BasicL2capHeader::IntrinsicSizeInBytes() + pdu_length);

      uint8_t* kframe_payload_start;
      if (std::holds_alternative<emboss::FirstKFrameWriter>(kframe.writer)) {
        emboss::FirstKFrameWriter first_kframe_writer =
            std::get<emboss::FirstKFrameWriter>(kframe.writer);
        EXPECT_EQ(first_kframe_writer.pdu_length().Read(), pdu_length);
        EXPECT_EQ(first_kframe_writer.channel_id().Read(), kCid);
        EXPECT_EQ(first_kframe_writer.sdu_length().Read(),
                  expected_payload.size());
        kframe_payload_start =
            first_kframe_writer.payload().BackingStorage().data();
      } else {
        emboss::SubsequentKFrameWriter subsequent_kframe_writer =
            std::get<emboss::SubsequentKFrameWriter>(kframe.writer);
        EXPECT_EQ(subsequent_kframe_writer.pdu_length().Read(), pdu_length);
        EXPECT_EQ(subsequent_kframe_writer.channel_id().Read(), kCid);
        kframe_payload_start =
            subsequent_kframe_writer.payload().BackingStorage().data();
      }

      // Validate payload of segment.
      size_t payload_length = pdu_length;
      uint8_t* expected_payload_start =
          expected_payload.data() + segment_no * mps;
      if (segment_no == 0) {
        payload_length -= kSduLengthFieldSize;
      } else {
        expected_payload_start -= kSduLengthFieldSize;
      }
      EXPECT_EQ(
          0,
          std::memcmp(
              kframe_payload_start, expected_payload_start, payload_length));
    }

    // Confirm that requesting a segment one past the final expected segment
    // results in an error.
    EXPECT_EQ(SetupKFrame(kHandle,
                          kCid,
                          static_cast<uint16_t>(mps),
                          static_cast<uint16_t>(segment_no),
                          span(expected_payload))
                  .status(),
              Status::OutOfRange());
  }
}

}  // namespace
}  // namespace pw::bluetooth::proxy
