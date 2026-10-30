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

#include "pw_bluetooth_sapphire/internal/host/att/attribute.h"

#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::att {
namespace {

constexpr PeerId kTestPeerId(1);
constexpr Handle kTestHandle = 0x0001;
constexpr UUID kTestType1(uint16_t{0x0001});
constexpr UUID kTestType2(uint16_t{0x0002});
constexpr UUID kTestType3(uint16_t{0x0003});
constexpr UUID kTestType4(uint16_t{0x0004});

const StaticByteBuffer kTestValue('t', 'e', 's', 't');

TEST(AttributeDeathTest, OverflowingMaxHandleOnAttributeGroupDies) {
  ASSERT_DEATH_IF_SUPPORTED(
      {
        AttributeGrouping A(kTestType1, kHandleMax - 1, size_t{2}, kTestValue);
      },
      ".*attr_count.*");
}

TEST(AttributeTest, AccessRequirementsDefault) {
  AccessRequirements reqs;
  EXPECT_FALSE(reqs.allowed());
  EXPECT_FALSE(reqs.encryption_required());
  EXPECT_FALSE(reqs.authentication_required());
  EXPECT_FALSE(reqs.authorization_required());
}

TEST(AttributeTest, AccessRequirements) {
  AccessRequirements reqs0(
      /*encryption=*/true, /*authentication=*/false, /*authorization=*/false);
  EXPECT_TRUE(reqs0.allowed());
  EXPECT_TRUE(reqs0.encryption_required());
  EXPECT_FALSE(reqs0.authentication_required());
  EXPECT_FALSE(reqs0.authorization_required());
  EXPECT_EQ(reqs0.min_enc_key_size(), sm::kMaxEncryptionKeySize);

  AccessRequirements reqs1(/*encryption=*/true,
                           /*authentication=*/false,
                           /*authorization=*/false,
                           8);
  EXPECT_TRUE(reqs1.allowed());
  EXPECT_TRUE(reqs1.encryption_required());
  EXPECT_FALSE(reqs1.authentication_required());
  EXPECT_FALSE(reqs1.authorization_required());
  EXPECT_EQ(reqs1.min_enc_key_size(), 8u);

  AccessRequirements reqs2(
      /*encryption=*/false, /*authentication=*/true, /*authorization=*/false);
  EXPECT_TRUE(reqs2.allowed());
  EXPECT_FALSE(reqs2.encryption_required());
  EXPECT_TRUE(reqs2.authentication_required());
  EXPECT_FALSE(reqs2.authorization_required());
  EXPECT_EQ(reqs2.min_enc_key_size(), sm::kMaxEncryptionKeySize);

  AccessRequirements reqs3(
      /*encryption=*/false, /*authentication=*/false, /*authorization=*/true);
  EXPECT_TRUE(reqs3.allowed());
  EXPECT_FALSE(reqs3.encryption_required());
  EXPECT_FALSE(reqs3.authentication_required());
  EXPECT_TRUE(reqs3.authorization_required());
  EXPECT_EQ(reqs3.min_enc_key_size(), sm::kMaxEncryptionKeySize);

  AccessRequirements reqs4(
      /*encryption=*/false, /*authentication=*/false, /*authorization=*/false);
  EXPECT_TRUE(reqs4.allowed());
  EXPECT_FALSE(reqs4.encryption_required());
  EXPECT_FALSE(reqs4.authentication_required());
  EXPECT_FALSE(reqs4.authorization_required());
}

TEST(AttributeTest, GroupingDeclAttr) {
  constexpr size_t kAttrCount = 0u;

  AttributeGrouping group(kTestType1, kTestHandle, kAttrCount, kTestValue);

  // The grouping consists of just the group declaration and is thus complete.
  EXPECT_TRUE(group.complete());
  EXPECT_EQ(kTestType1, group.group_type());
  EXPECT_EQ(kTestHandle, group.start_handle());
  EXPECT_EQ(kTestHandle, group.end_handle());
  EXPECT_EQ(1u, group.attributes().size());

  // The grouping is already complete.
  EXPECT_FALSE(group.AddAttribute(
      kTestType2, AccessRequirements(), AccessRequirements()));

  const Attribute& decl_attr = group.attributes()[0];
  EXPECT_EQ(kTestHandle, decl_attr.handle());
  EXPECT_EQ(kTestType1, decl_attr.type());
  ASSERT_TRUE(decl_attr.value());
  EXPECT_TRUE(ContainersEqual(kTestValue, *decl_attr.value()));
  EXPECT_TRUE(decl_attr.read_reqs().allowed());
  EXPECT_FALSE(decl_attr.read_reqs().encryption_required());
  EXPECT_FALSE(decl_attr.read_reqs().authentication_required());
  EXPECT_FALSE(decl_attr.read_reqs().authorization_required());
  EXPECT_FALSE(decl_attr.write_reqs().allowed());
  EXPECT_EQ(&group, &decl_attr.group());
}

TEST(AttributeTest, GroupingAddAttribute) {
  constexpr size_t kAttrCount = 2;

  AttributeGrouping group(kTestType1, kTestHandle, kAttrCount, kTestValue);
  EXPECT_FALSE(group.complete());
  EXPECT_EQ(kTestHandle, group.start_handle());
  EXPECT_EQ(kTestHandle + kAttrCount, group.end_handle());

  Attribute* attr = group.AddAttribute(
      kTestType2, AccessRequirements(), AccessRequirements());
  ASSERT_TRUE(attr);
  EXPECT_EQ(kTestType2, attr->type());
  EXPECT_EQ(kTestHandle + 1, attr->handle());
  EXPECT_EQ(&group, &attr->group());

  // The attribute should have no value until assigned.
  EXPECT_FALSE(attr->value());
  attr->SetValue(kTestValue);
  ASSERT_TRUE(attr->value());
  EXPECT_TRUE(ContainersEqual(kTestValue, *attr->value()));

  // The group is not complete until |kAttrCount| attributes have been added.
  EXPECT_FALSE(group.complete());
  EXPECT_EQ(2u, group.attributes().size());

  attr = group.AddAttribute(
      kTestType3, AccessRequirements(), AccessRequirements());
  ASSERT_TRUE(attr);
  EXPECT_EQ(kTestType3, attr->type());

  EXPECT_TRUE(group.complete());
  EXPECT_EQ(group.end_handle(), attr->handle());
  EXPECT_EQ(3u, group.attributes().size());

  EXPECT_FALSE(group.AddAttribute(
      kTestType4, AccessRequirements(), AccessRequirements()));
}

TEST(AttributeTest, ReadAsyncReadNotAllowed) {
  AttributeGrouping group(kTestType1, kTestHandle, 1, kTestValue);
  Attribute* attr = group.AddAttribute(
      kTestType2, AccessRequirements(), AccessRequirements());
  EXPECT_FALSE(attr->ReadAsync(kTestPeerId, 0, [](auto, const auto&) {}));
}

TEST(AttributeTest, ReadAsyncReadNoHandler) {
  AttributeGrouping group(kTestType1, kTestHandle, 1, kTestValue);
  Attribute* attr = group.AddAttribute(
      kTestType2,
      AccessRequirements(/*encryption=*/false,
                         /*authentication=*/false,
                         /*authorization=*/false),  // read (no security)
      AccessRequirements());                        // write not allowed
  EXPECT_FALSE(attr->ReadAsync(kTestPeerId, 0, [](auto, const auto&) {}));
}

TEST(AttributeTest, ReadAsync) {
  constexpr uint16_t kTestOffset = 5;

  AttributeGrouping group(kTestType1, kTestHandle, 1, kTestValue);
  Attribute* attr = group.AddAttribute(
      kTestType2,
      AccessRequirements(/*encryption=*/false,
                         /*authentication=*/false,
                         /*authorization=*/false),  // read (no security)
      AccessRequirements());                        // write not allowed

  bool callback_called = false;
  auto callback = [&](fit::result<ErrorCode> status, const auto& value) {
    EXPECT_EQ(fit::ok(), status);
    EXPECT_TRUE(ContainersEqual(StaticByteBuffer('h', 'i'), value));
    callback_called = true;
  };

  auto handler =
      [&](PeerId peer_id, Handle handle, uint16_t offset, auto result_cb) {
        EXPECT_EQ(kTestPeerId, peer_id);
        EXPECT_EQ(attr->handle(), handle);
        EXPECT_EQ(kTestOffset, offset);
        EXPECT_TRUE(result_cb);

        result_cb(fit::ok(), StaticByteBuffer('h', 'i'));
      };

  attr->set_read_handler(handler);
  EXPECT_TRUE(attr->ReadAsync(kTestPeerId, kTestOffset, callback));
  EXPECT_TRUE(callback_called);
}

TEST(AttributeTest, WriteAsyncWriteNotAllowed) {
  AttributeGrouping group(kTestType1, kTestHandle, 1, kTestValue);
  Attribute* attr = group.AddAttribute(
      kTestType2, AccessRequirements(), AccessRequirements());
  EXPECT_FALSE(attr->WriteAsync(kTestPeerId, 0, BufferView(), [](auto) {}));
}

TEST(AttributeTest, WriteAsyncWriteNoHandler) {
  AttributeGrouping group(kTestType1, kTestHandle, 1, kTestValue);
  Attribute* attr = group.AddAttribute(
      kTestType2,
      AccessRequirements(),  // read not allowed
      AccessRequirements(/*encryption=*/false,
                         /*authentication=*/false,
                         /*authorization=*/false));  // write no security
  EXPECT_FALSE(attr->WriteAsync(kTestPeerId, 0, BufferView(), [](auto) {}));
}

TEST(AttributeTest, WriteAsync) {
  constexpr uint16_t kTestOffset = 5;

  AttributeGrouping group(kTestType1, kTestHandle, 1, kTestValue);
  Attribute* attr = group.AddAttribute(
      kTestType2,
      AccessRequirements(),  // read not allowed
      AccessRequirements(/*encryption=*/false,
                         /*authentication=*/false,
                         /*authorization=*/false));  // write no security

  bool callback_called = false;
  auto callback = [&](fit::result<ErrorCode> status) {
    EXPECT_EQ(fit::ok(), status);
    callback_called = true;
  };

  auto handler = [&](PeerId peer_id,
                     Handle handle,
                     uint16_t offset,
                     const auto& value,
                     auto result_cb) {
    EXPECT_EQ(kTestPeerId, peer_id);
    EXPECT_EQ(attr->handle(), handle);
    EXPECT_EQ(kTestOffset, offset);
    EXPECT_TRUE(ContainersEqual(StaticByteBuffer('h', 'i'), value));
    EXPECT_TRUE(result_cb);

    result_cb(fit::ok());
  };

  attr->set_write_handler(handler);
  EXPECT_TRUE(attr->WriteAsync(
      kTestPeerId, kTestOffset, StaticByteBuffer('h', 'i'), callback));
  EXPECT_TRUE(callback_called);
}

}  // namespace
}  // namespace bt::att
