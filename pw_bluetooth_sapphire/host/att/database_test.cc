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

#include "pw_bluetooth_sapphire/internal/host/att/database.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::att {
namespace {

constexpr Handle kTestRangeStart = 1;
constexpr Handle kTestRangeEnd = 10;

constexpr UUID kTestType1(uint16_t{1});
constexpr UUID kTestType2(uint16_t{2});
constexpr UUID kTestType3(uint16_t{3});

const AccessRequirements kAllowed(/*encryption=*/false,
                                  /*authentication=*/false,
                                  /*authorization=*/false);
const sm::SecurityProperties kNoSecurity(sm::SecurityLevel::kNoSecurity,
                                         16,
                                         /*secure_connections=*/false);

// Values with different lengths
const StaticByteBuffer kTestValue1('x', 'x');
const StaticByteBuffer kTestValue2('x', 'x', 'x');

// Returns the handles of each attribute visited by advancing |iter| until the
// end.
std::vector<Handle> IterHandles(Database::Iterator* iter) {
  PW_DCHECK(iter);

  std::vector<Handle> handles;
  for (; !iter->AtEnd(); iter->Advance()) {
    handles.push_back(iter->get()->handle());
  }
  return handles;
}

TEST(DatabaseTest, NewGroupingWhileEmptyError) {
  constexpr size_t kTooLarge = kTestRangeEnd - kTestRangeStart + 1;
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  EXPECT_FALSE(db->NewGrouping(kTestType1, kTooLarge, kTestValue1));
}

TEST(DatabaseTest, NewGroupingWhileEmptyFill) {
  constexpr size_t kExact = kTestRangeEnd - kTestRangeStart;

  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  auto* grp = db->NewGrouping(kTestType1, kExact, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(kTestType1, grp->group_type());
  EXPECT_EQ(kTestRangeStart, grp->start_handle());
  EXPECT_EQ(kTestRangeEnd, grp->end_handle());

  // Ran out of space.
  EXPECT_FALSE(db->NewGrouping(kTestType1, 0, kTestValue1));
}

// This test case performs multiple insertions and removals on the same
// database.
TEST(DatabaseTest, NewGroupingMultipleInsertions) {
  // [__________]
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);

  // Insert to empty db
  // [XXX_______] (insert X)
  auto* grp = db->NewGrouping(kTestType1, 2, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(1, grp->start_handle());
  EXPECT_EQ(3, grp->end_handle());

  // Not enough space
  grp = db->NewGrouping(kTestType1, 7, kTestValue1);
  EXPECT_FALSE(grp);

  // Insert back
  // [XXXYYYYY__] (insert Y)
  grp = db->NewGrouping(kTestType1, 4, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(4, grp->start_handle());
  EXPECT_EQ(8, grp->end_handle());

  // Not enough space
  grp = db->NewGrouping(kTestType1, 2, kTestValue1);
  EXPECT_FALSE(grp);

  // Insert back
  // [XXXYYYYYZZ] (insert Z)
  grp = db->NewGrouping(kTestType1, 1, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(9, grp->start_handle());
  EXPECT_EQ(10, grp->end_handle());

  // Out of space
  EXPECT_FALSE(db->NewGrouping(kTestType1, 0, kTestValue1));

  // Remove first grouping. It should be possible to reinsert a smaller group.
  // [___YYYYYZZ]
  EXPECT_TRUE(db->RemoveGrouping(1));

  // Not enough space
  grp = db->NewGrouping(kTestType1, 3, kTestValue1);
  EXPECT_FALSE(grp);

  // Insert front
  // [XX_YYYYYZZ] (insert X)
  grp = db->NewGrouping(kTestType1, 1, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(1, grp->start_handle());
  EXPECT_EQ(2, grp->end_handle());

  // Handle doesn't exist.
  EXPECT_FALSE(db->RemoveGrouping(3));

  // Insert in the middle
  // [XXWYYYYYZZ] (insert W)
  grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(3, grp->start_handle());
  EXPECT_EQ(3, grp->end_handle());

  // [XXW_____ZZ] (remove Y)
  EXPECT_TRUE(db->RemoveGrouping(4));

  // Insert in the middle
  // [XXWAAA__ZZ] (insert A)
  grp = db->NewGrouping(kTestType1, 2, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(4, grp->start_handle());
  EXPECT_EQ(6, grp->end_handle());

  // Insert in the middle
  // [XXWAAABBZZ] (insert B)
  grp = db->NewGrouping(kTestType1, 1, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(7, grp->start_handle());
  EXPECT_EQ(8, grp->end_handle());

  // Out of space
  EXPECT_FALSE(db->NewGrouping(kTestType1, 0, kTestValue1));
}

TEST(DatabaseTest, RemoveWhileEmpty) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  EXPECT_FALSE(db->RemoveGrouping(kTestRangeStart));
}

TEST(DatabaseTest, FindAttributeInvalidHandle) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  EXPECT_EQ(nullptr, db->FindAttribute(kInvalidHandle));
}

TEST(DatabaseTest, FindAttributeGroupingNotFound) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);

  // Create the following layout:
  //
  // handle 0x0001: occupied
  // handle 0x0002: empty
  // handle 0x0003: occupied
  db->NewGrouping(kTestType1, 0, kTestValue1)->set_active(true);
  auto* grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp->set_active(true);
  db->NewGrouping(kTestType1, 0, kTestValue1)->set_active(true);
  db->RemoveGrouping(grp->start_handle());

  EXPECT_EQ(nullptr, db->FindAttribute(0xFFFF));
  EXPECT_EQ(nullptr, db->FindAttribute(0x0002));
  EXPECT_NE(nullptr, db->FindAttribute(0x0001));
  EXPECT_NE(nullptr, db->FindAttribute(0x0003));
}

TEST(DatabaseTest, FindAttributeIncompleteGrouping) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  auto* grp = db->NewGrouping(kTestType1, 1, kTestValue1);
  EXPECT_EQ(nullptr, db->FindAttribute(grp->start_handle()));
}

TEST(DatabaseTest, FindAttributeInactiveGrouping) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  auto* grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  EXPECT_EQ(nullptr, db->FindAttribute(grp->start_handle()));
}

TEST(DatabaseTest, FindAttributeOnePerGrouping) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  auto* grp1 = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp1->set_active(true);
  auto* grp2 = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp2->set_active(true);

  EXPECT_EQ(&grp1->attributes()[0], db->FindAttribute(grp1->start_handle()));
  EXPECT_EQ(&grp2->attributes()[0], db->FindAttribute(grp2->start_handle()));
}

TEST(DatabaseTest, FindAttributeIndexIntoGrouping) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);

  auto* grp = db->NewGrouping(kTestType1, 1, kTestValue1);
  auto* attr =
      grp->AddAttribute(kTestType2, AccessRequirements(), AccessRequirements());
  grp->set_active(true);

  EXPECT_EQ(attr, db->FindAttribute(grp->end_handle()));
}

TEST(DatabaseTest, IteratorEmpty) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  auto iter = db->GetIterator(kTestRangeStart, kTestRangeEnd);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());

  // Advance should have no effect.
  iter.Advance();
  EXPECT_TRUE(iter.AtEnd());
}

TEST(DatabaseTest, IteratorGroupOnlySingleInactive) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  db->NewGrouping(kTestType1, 0, kTestValue1);

  // |grp| is not active
  auto iter = db->GetIterator(
      kTestRangeStart, kTestRangeEnd, /*type=*/nullptr, /*groups_only=*/true);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());
}

TEST(DatabaseTest, IteratorGroupOnlySingle) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  auto grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp->set_active(true);

  // Not within range.
  auto iter = db->GetIterator(grp->start_handle() + 1,
                              kTestRangeEnd,
                              /*type=*/nullptr,
                              /*groups_only=*/true);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());

  iter = db->GetIterator(
      kTestRangeStart, kTestRangeEnd, /*type=*/nullptr, /*groups_only=*/true);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(grp->start_handle(), handles[0]);
}

TEST(DatabaseTest, IteratorGroupOnlyMultiple) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  auto grp1 = db->NewGrouping(kTestType1, 0, kTestValue1);
  auto grp2 = db->NewGrouping(kTestType1, 0, kTestValue1);
  auto grp3 = db->NewGrouping(kTestType1, 0, kTestValue1);
  auto grp4 = db->NewGrouping(kTestType1, 0, kTestValue1);

  // Leave |grp2| as inactive.
  grp1->set_active(true);
  grp3->set_active(true);
  grp4->set_active(true);

  auto iter = db->GetIterator(
      kTestRangeStart, kTestRangeEnd, /*type=*/nullptr, /*groups_only=*/true);
  EXPECT_FALSE(iter.AtEnd());

  // |grp2| should be omitted.
  auto handles = IterHandles(&iter);
  ASSERT_EQ(3u, handles.size());
  EXPECT_EQ(grp1->start_handle(), handles[0]);
  EXPECT_EQ(grp3->start_handle(), handles[1]);
  EXPECT_EQ(grp4->start_handle(), handles[2]);

  grp2->set_active(true);

  // Pick a narrow range that excludes |grp1| and |grp4|.
  iter = db->GetIterator(grp2->start_handle(),
                         grp3->end_handle(),
                         /*type=*/nullptr,
                         /*groups_only=*/true);
  handles = IterHandles(&iter);
  ASSERT_EQ(2u, handles.size());
  EXPECT_EQ(grp2->start_handle(), handles[0]);
  EXPECT_EQ(grp3->start_handle(), handles[1]);
}

TEST(DatabaseTest, IteratorGroupOnlySingleWithFilter) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);

  auto grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp->set_active(true);

  // No match.
  auto iter = db->GetIterator(
      kTestRangeStart, kTestRangeEnd, &kTestType2, /*groups_only=*/true);
  EXPECT_TRUE(iter.AtEnd());

  iter = db->GetIterator(
      kTestRangeStart, kTestRangeEnd, &kTestType1, /*groups_only=*/true);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(grp->start_handle(), handles[0]);
}

TEST(DatabaseTest, IteratorGroupOnlyManyWithFilter) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);

  auto grp1 = db->NewGrouping(kTestType1, 1, kTestValue1);  // match
  grp1->AddAttribute(kTestType1);  // match but skipped - not group decl.
  grp1->set_active(true);

  // Matching but inactive.
  db->NewGrouping(kTestType1, 0, kTestValue1);

  auto grp2 = db->NewGrouping(kTestType2, 0, kTestValue1);
  grp2->set_active(true);
  auto grp3 = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp3->set_active(true);
  auto grp4 = db->NewGrouping(kTestType2, 0, kTestValue1);
  grp4->set_active(true);
  auto grp5 = db->NewGrouping(kTestType2, 0, kTestValue1);
  grp5->set_active(true);
  auto grp6 = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp6->set_active(true);

  // Filter by |kTestType1|
  auto iter = db->GetIterator(
      kTestRangeStart, kTestRangeEnd, &kTestType1, /*groups_only=*/true);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);
  ASSERT_EQ(3u, handles.size());
  EXPECT_EQ(grp1->start_handle(), handles[0]);
  EXPECT_EQ(grp3->start_handle(), handles[1]);
  EXPECT_EQ(grp6->start_handle(), handles[2]);

  // Filter by |kTestType2|
  iter = db->GetIterator(
      kTestRangeStart, kTestRangeEnd, &kTestType2, /*groups_only=*/true);
  EXPECT_FALSE(iter.AtEnd());

  handles = IterHandles(&iter);
  ASSERT_EQ(3u, handles.size());
  EXPECT_EQ(grp2->start_handle(), handles[0]);
  EXPECT_EQ(grp4->start_handle(), handles[1]);
  EXPECT_EQ(grp5->start_handle(), handles[2]);

  // Search narrower range.
  iter = db->GetIterator(grp1->end_handle(),
                         grp5->end_handle(),
                         &kTestType1,
                         /*groups_only=*/true);
  EXPECT_FALSE(iter.AtEnd());

  handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(grp3->start_handle(), handles[0]);
}

TEST(DatabaseTest, IteratorSingleInactive) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  auto grp = db->NewGrouping(kTestType1, 1, kTestValue1);

  auto iter = db->GetIterator(kTestRangeStart, kTestRangeEnd);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());

  // Complete but still inactive.
  grp->AddAttribute(kTestType1);
  iter = db->GetIterator(kTestRangeStart, kTestRangeEnd);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());
}

TEST(DatabaseTest, IteratorSingle) {
  auto db = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  auto grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp->set_active(true);

  // Not within range.
  auto iter = db->GetIterator(grp->start_handle() + 1, kTestRangeEnd);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());

  iter = db->GetIterator(kTestRangeStart, kTestRangeEnd);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(grp->start_handle(), handles[0]);
}

class DatabaseIteratorManyTest : public ::testing::Test {
 public:
  DatabaseIteratorManyTest() = default;
  ~DatabaseIteratorManyTest() override = default;

 protected:
  static constexpr size_t kActiveAttrCount = 8;

  void SetUp() override {
    db_ = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);

    auto grp1 = db()->NewGrouping(kTestType1, 3, kTestValue1);  // 1
    grp1->AddAttribute(kTestType2);                             // 2
    grp1->AddAttribute(kTestType2);                             // 3
    grp1->AddAttribute(kTestType1);                             // 4
    grp1->set_active(true);

    auto grp2 = db()->NewGrouping(kTestType2, 2, kTestValue1);  // 5
    grp2->AddAttribute(kTestType1);                             // 6
    grp2->AddAttribute(kTestType2);                             // 7
    grp2->set_active(true);

    auto grp3 = db()->NewGrouping(kTestType1, 1, kTestValue1);  // 8 (inactive)
    grp3->AddAttribute(kTestType2);                             // 9 (inactive)

    auto grp4 = db()->NewGrouping(kTestType1, 0, kTestValue1);  // 10
    grp4->set_active(true);
  }

  Database::WeakPtr db() const { return db_->GetWeakPtr(); }

 private:
  std::unique_ptr<Database> db_;

  BT_DISALLOW_COPY_ASSIGN_AND_MOVE(DatabaseIteratorManyTest);
};

TEST_F(DatabaseIteratorManyTest, NoFilter) {
  auto iter = db()->GetIterator(kTestRangeStart, kTestRangeEnd);
  EXPECT_FALSE(iter.AtEnd());

  // Should cover all but the inactive attribute.
  auto handles = IterHandles(&iter);

  // All active attribute handles.
  const std::array<Handle, kActiveAttrCount> kExpected = {
      1, 2, 3, 4, 5, 6, 7, 10};
  ASSERT_EQ(kExpected.size(), handles.size());

  for (size_t i = 0; i < handles.size(); i++) {
    EXPECT_EQ(kExpected[i], handles[i]);
  }
}

TEST_F(DatabaseIteratorManyTest, FilterTestType1) {
  // Filter by |kTestType1|.
  auto iter = db()->GetIterator(kTestRangeStart, kTestRangeEnd, &kTestType1);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);

  // Handles of attributes with type |kTestType1|.
  const std::array<Handle, 4u> kExpected = {1, 4, 6, 10};
  ASSERT_EQ(kExpected.size(), handles.size());

  for (size_t i = 0; i < handles.size(); i++) {
    EXPECT_EQ(kExpected[i], handles[i]);
  }
}

TEST_F(DatabaseIteratorManyTest, FilterTestType2) {
  // Filter by |kTestType2|.
  auto iter = db()->GetIterator(kTestRangeStart, kTestRangeEnd, &kTestType2);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);

  // Handles of attributes with type |kTestType2|.
  const std::array<Handle, 4u> kExpected = {2, 3, 5, 7};
  ASSERT_EQ(kExpected.size(), handles.size());

  for (size_t i = 0; i < handles.size(); i++) {
    EXPECT_EQ(kExpected[i], handles[i]);
  }
}

TEST_F(DatabaseIteratorManyTest, FilterTestType3) {
  // Filter by |kTestType3|.
  auto iter = db()->GetIterator(kTestRangeStart, kTestRangeEnd, &kTestType3);
  EXPECT_TRUE(iter.AtEnd());
}

TEST_F(DatabaseIteratorManyTest, UnaryRange) {
  // Test ranges with a single attribute. Test group begin, middle, and end
  // cases.
  constexpr Handle kBegin = 5;
  constexpr Handle kMiddle = 6;
  constexpr Handle kEnd = 7;

  auto iter = db()->GetIterator(kBegin, kBegin);
  EXPECT_FALSE(iter.AtEnd());
  auto handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(kBegin, handles[0]);

  iter = db()->GetIterator(kMiddle, kMiddle);
  EXPECT_FALSE(iter.AtEnd());
  handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(kMiddle, handles[0]);

  iter = db()->GetIterator(kEnd, kEnd);
  EXPECT_FALSE(iter.AtEnd());
  handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(kEnd, handles[0]);
}

TEST_F(DatabaseIteratorManyTest, Range) {
  auto iter = db()->GetIterator(4, 6);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);

  // All active attribute handles.
  const std::array<Handle, 3> kExpected = {4, 5, 6};
  ASSERT_EQ(kExpected.size(), handles.size());

  for (size_t i = 0; i < handles.size(); i++) {
    EXPECT_EQ(kExpected[i], handles[i]);
  }
}

class DatabaseExecuteWriteQueueTest : public ::testing::Test {
 public:
  DatabaseExecuteWriteQueueTest() = default;
  ~DatabaseExecuteWriteQueueTest() override = default;

 protected:
  struct PendingWrite {
    PeerId peer_id;
    Handle handle;
    uint16_t offset;
    DynamicByteBuffer value;
    Attribute::WriteResultCallback result_callback;
  };

  void SetUp() override {
    db_ = std::make_unique<Database>(kTestRangeStart, kTestRangeEnd);
  }

  void ExecuteWriteQueue(PeerId peer_id,
                         PrepareWriteQueue wq,
                         const sm::SecurityProperties& security = kNoSecurity) {
    db_->ExecuteWriteQueue(peer_id,
                           std::move(wq),
                           security,
                           [this](Database::WriteQueueResult result) {
                             callback_count_++;
                             result_.emplace(result);
                           });
  }

  // Sets up an attribute grouping with 4 attributes.
  void SetUpAttributes() {
    auto* grp = db()->NewGrouping(kTestType1, 3, kTestValue1);  // handle: 1
    group_decl_handle_ = grp->start_handle();

    auto* attr =
        grp->AddAttribute(kTestType2, kAllowed, kAllowed);  // handle: 2
    SetAttributeWriteHandler(attr);
    test_handle1_ = attr->handle();

    attr = grp->AddAttribute(kTestType2, kAllowed, kAllowed);  // handle: 3
    SetAttributeWriteHandler(attr);
    test_handle2_ = attr->handle();

    attr = grp->AddAttribute(kTestType2, kAllowed, kAllowed);  // handle: 4
    SetAttributeWriteHandler(attr);
    test_handle3_ = attr->handle();

    grp->set_active(true);
  }

  void ResolveNextPendingWrite(fit::result<ErrorCode> status) {
    ASSERT_FALSE(pending_writes_.empty());
    auto pw = std::move(pending_writes_.front());
    pending_writes_.pop();

    pw.result_callback(status);
  }

  // Executes a queue holding a single write of |value| at |offset| to
  // test_handle1().
  void ExecuteSingleWrite(PeerId peer_id,
                          uint16_t offset,
                          const ByteBuffer& value) {
    PrepareWriteQueue wq;
    wq.push(QueuedWrite(test_handle1_, offset, value));
    ExecuteWriteQueue(peer_id, std::move(wq));
  }

  std::optional<Database::WriteQueueResult> result() const { return result_; }
  int callback_count() const { return callback_count_; }

  Handle group_decl_handle() const { return group_decl_handle_; }
  Handle test_handle1() const { return test_handle1_; }
  Handle test_handle2() const { return test_handle2_; }
  Handle test_handle3() const { return test_handle3_; }

  void SetAttributeWriteHandler(Attribute* attribute) {
    attribute->set_write_handler(
        fit::bind_member<&DatabaseExecuteWriteQueueTest::WriteHandler>(this));
  }

  const std::queue<PendingWrite>& pending_writes() const {
    return pending_writes_;
  }

  Database::WeakPtr db() { return db_->GetWeakPtr(); }

 private:
  void WriteHandler(PeerId peer_id,
                    Handle handle,
                    uint16_t offset,
                    const ByteBuffer& value,
                    Attribute::WriteResultCallback result_callback) {
    PendingWrite pw;
    pw.peer_id = peer_id;
    pw.handle = handle;
    pw.offset = offset;
    pw.value = DynamicByteBuffer(value);
    pw.result_callback = std::move(result_callback);

    pending_writes_.push(std::move(pw));
  }

  std::optional<Database::WriteQueueResult> result_;
  int callback_count_ = 0;
  std::unique_ptr<Database> db_;
  std::queue<PendingWrite> pending_writes_;

  // Handles of the test attributes.
  Handle group_decl_handle_ = kInvalidHandle;
  Handle test_handle1_ = kInvalidHandle;
  Handle test_handle2_ = kInvalidHandle;
  Handle test_handle3_ = kInvalidHandle;

  BT_DISALLOW_COPY_ASSIGN_AND_MOVE(DatabaseExecuteWriteQueueTest);
};

constexpr PeerId kPeerId(1);

TEST_F(DatabaseExecuteWriteQueueTest, EmptyQueueSucceedsImmediately) {
  ExecuteWriteQueue(kPeerId, {});
  EXPECT_EQ(1, callback_count());
  EXPECT_EQ(fit::ok(), result());
}

TEST_F(DatabaseExecuteWriteQueueTest, InvalidHandle) {
  constexpr Handle kHandle = 1;
  PrepareWriteQueue wq;
  wq.push(QueuedWrite(kHandle, 0, BufferView()));

  ExecuteWriteQueue(kPeerId, std::move(wq));
  EXPECT_EQ(1, callback_count());
  ASSERT_EQ(fit::failed(), result());
  EXPECT_EQ(std::tuple(kHandle, ErrorCode::kInvalidHandle),
            result()->error_value());
}

TEST_F(DatabaseExecuteWriteQueueTest, ValueLength) {
  auto* grp = db()->NewGrouping(kTestType1, 1, kTestValue1);
  auto* attr = grp->AddAttribute(kTestType2);
  grp->set_active(true);

  PrepareWriteQueue wq;
  wq.push(QueuedWrite(
      attr->handle(), 0, DynamicByteBuffer(kMaxAttributeValueLength + 1)));

  ExecuteWriteQueue(kPeerId, std::move(wq));
  EXPECT_EQ(1, callback_count());
  ASSERT_EQ(fit::failed(), result());
  EXPECT_EQ(std::tuple(attr->handle(), ErrorCode::kInvalidAttributeValueLength),
            result()->error_value());
}

// Reject a write with an out-of-range offset.
TEST_F(DatabaseExecuteWriteQueueTest,
       OffsetBeyondMaxAttributeValueLengthIsInvalidOffset) {
  SetUpAttributes();
  ExecuteSingleWrite(kPeerId, kMaxAttributeValueLength + 1, kTestValue1);
  ASSERT_EQ(fit::failed(), result());
  EXPECT_EQ(std::tuple(test_handle1(), ErrorCode::kInvalidOffset),
            result()->error_value());
  EXPECT_TRUE(pending_writes().empty());
}

// Reject a write that starts in range but ends past the maximum value length.
TEST_F(DatabaseExecuteWriteQueueTest,
       OffsetPlusLengthBeyondMaxAttributeValueLengthIsInvalidValueLength) {
  SetUpAttributes();
  ExecuteSingleWrite(
      kPeerId,
      static_cast<uint16_t>(kMaxAttributeValueLength - kTestValue1.size() + 1),
      kTestValue1);
  ASSERT_EQ(fit::failed(), result());
  EXPECT_EQ(std::tuple(test_handle1(), ErrorCode::kInvalidAttributeValueLength),
            result()->error_value());
  EXPECT_TRUE(pending_writes().empty());
}

// Deliver a write that ends exactly at the maximum value length.
TEST_F(DatabaseExecuteWriteQueueTest,
       WriteEndingAtMaxAttributeValueLengthIsDelivered) {
  SetUpAttributes();
  const uint16_t kOffset =
      static_cast<uint16_t>(kMaxAttributeValueLength - kTestValue1.size());
  ExecuteSingleWrite(kPeerId, kOffset, kTestValue1);
  ASSERT_EQ(1u, pending_writes().size());
  EXPECT_EQ(kOffset, pending_writes().front().offset);

  ResolveNextPendingWrite(fit::ok());
  EXPECT_EQ(fit::ok(), result());
}

TEST_F(DatabaseExecuteWriteQueueTest, WritingStaticValueNotPermitted) {
  auto* grp = db()->NewGrouping(kTestType1, 1, kTestValue1);
  auto* attr = grp->AddAttribute(kTestType2);  // read/write not permitted
  grp->set_active(true);

  PrepareWriteQueue wq;
  wq.push(QueuedWrite(attr->handle(), 0, kTestValue1));

  ExecuteWriteQueue(kPeerId, std::move(wq));
  EXPECT_EQ(1, callback_count());
  ASSERT_EQ(fit::failed(), result());
  EXPECT_EQ(std::tuple(attr->handle(), ErrorCode::kWriteNotPermitted),
            result()->error_value());
}

TEST_F(DatabaseExecuteWriteQueueTest, SecurityChecks) {
  auto* grp = db()->NewGrouping(kTestType1, 1, kTestValue1);
  auto* attr = grp->AddAttribute(
      kTestType2,
      att::AccessRequirements(),
      att::AccessRequirements(
          /*encryption=*/true,
          /*authentication=*/false,
          /*authorization=*/false));  // write requires encryption
  SetAttributeWriteHandler(attr);
  grp->set_active(true);

  PrepareWriteQueue wq;
  wq.push(QueuedWrite(attr->handle(), 0, kTestValue1));

  ExecuteWriteQueue(kPeerId, std::move(wq), kNoSecurity);
  EXPECT_EQ(1, callback_count());
  ASSERT_EQ(fit::failed(), result());
  EXPECT_EQ(std::tuple(attr->handle(), ErrorCode::kInsufficientAuthentication),
            result()->error_value());

  wq = PrepareWriteQueue();
  wq.push(QueuedWrite(attr->handle(), 0, kTestValue1));

  // Request should succeed with an encrypted link.
  ExecuteWriteQueue(
      kPeerId,
      std::move(wq),
      sm::SecurityProperties(
          sm::SecurityLevel::kEncrypted, 16, /*secure_connections=*/false));
  ResolveNextPendingWrite(fit::ok());
  EXPECT_EQ(2, callback_count());
  EXPECT_EQ(fit::ok(), result());
}

// If an error is caught before delivering the request to the delegate then we
// expect subsequent entries to not be delivered.
TEST_F(DatabaseExecuteWriteQueueTest, UndelegatedWriteErrorAborts) {
  SetUpAttributes();
  PrepareWriteQueue wq;

  // Queue a request to one of the delegated handles. This won't generate a
  // result until we explicitly send a reply via ResolveNextPendingWrite().
  wq.push(QueuedWrite(test_handle1(), 0, kTestValue1));

  // Queue a write to the group declaration handle. This should get rejected
  // right away since it's not writable. Since the database catches this error
  // internally, we expect the following queued writes to get aborted.
  wq.push(QueuedWrite(group_decl_handle(), 0, kTestValue1));

  // Queue more writes.
  wq.push(QueuedWrite(test_handle2(), 1, kTestValue2));
  wq.push(QueuedWrite(test_handle3(), 2, kTestValue1));
  wq.push(QueuedWrite(test_handle1(), 3, kTestValue2));

  ExecuteWriteQueue(kPeerId, std::move(wq));

  // The database should have generated an error response.
  EXPECT_EQ(1, callback_count());
  ASSERT_EQ(fit::failed(), result());
  EXPECT_EQ(std::tuple(group_decl_handle(), ErrorCode::kWriteNotPermitted),
            result()->error_value());

  // Only the first write should have been delivered.
  ASSERT_EQ(1u, pending_writes().size());
  EXPECT_EQ(test_handle1(), pending_writes().front().handle);
  EXPECT_EQ(0, pending_writes().front().offset);
  EXPECT_TRUE(ContainersEqual(kTestValue1, pending_writes().front().value));

  // Resolving the request should have no effect.
  ResolveNextPendingWrite(fit::error(ErrorCode::kUnlikelyError));
  EXPECT_EQ(1, callback_count());  // still 1
}

TEST_F(DatabaseExecuteWriteQueueTest, ErrorInMultipleQueuedWrites) {
  SetUpAttributes();
  PrepareWriteQueue wq;

  // Queue writes to the writable handles in arbitrary order. We expect all of
  // them to be delivered to the delegate.
  wq.push(QueuedWrite(test_handle1(), 0, kTestValue1));
  wq.push(QueuedWrite(test_handle2(), 1, kTestValue2));
  wq.push(QueuedWrite(test_handle3(), 2, kTestValue1));
  wq.push(QueuedWrite(test_handle1(), 3, kTestValue2));

  ExecuteWriteQueue(kPeerId, std::move(wq));

  // The execute write request should be pending.
  EXPECT_EQ(0, callback_count());

  // All 4 requests should have been delivered.
  ASSERT_EQ(4u, pending_writes().size());

  // Resolve the first request with success. The execute write request should
  // remain pending.
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle1(), next.handle);
    EXPECT_EQ(0, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue1, next.value));

    ResolveNextPendingWrite(fit::ok());
    EXPECT_EQ(0, callback_count());
  }

  // Resolve the second request with an error.
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle2(), next.handle);
    EXPECT_EQ(1, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue2, next.value));

    ResolveNextPendingWrite(fit::error(ErrorCode::kUnlikelyError));
    EXPECT_EQ(1, callback_count());
    ASSERT_EQ(fit::failed(), result());
    EXPECT_EQ(std::tuple(test_handle2(), ErrorCode::kUnlikelyError),
              result()->error_value());
  }

  // Resolving the remaining writes should have no effect.
  ResolveNextPendingWrite(fit::ok());
  ResolveNextPendingWrite(fit::error(ErrorCode::kUnlikelyError));
  EXPECT_EQ(1, callback_count());
}

TEST_F(DatabaseExecuteWriteQueueTest, MultipleQueuedWritesSucceed) {
  SetUpAttributes();
  PrepareWriteQueue wq;

  // Queue writes to the writable handles in arbitrary order. We expect all of
  // them to be delivered to the delegate.
  wq.push(QueuedWrite(test_handle1(), 0, kTestValue1));
  wq.push(QueuedWrite(test_handle2(), 1, kTestValue2));
  wq.push(QueuedWrite(test_handle3(), 2, kTestValue1));
  wq.push(QueuedWrite(test_handle1(), 3, kTestValue2));

  ExecuteWriteQueue(kPeerId, std::move(wq));

  // The execute write request should be pending.
  EXPECT_EQ(0, callback_count());

  // All 4 requests should have been delivered.
  ASSERT_EQ(4u, pending_writes().size());

  // Resolve all requests with success.
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle1(), next.handle);
    EXPECT_EQ(0, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue1, next.value));

    ResolveNextPendingWrite(fit::ok());
    EXPECT_EQ(0, callback_count());
  }
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle2(), next.handle);
    EXPECT_EQ(1, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue2, next.value));

    ResolveNextPendingWrite(fit::ok());
    EXPECT_EQ(0, callback_count());
  }
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle3(), next.handle);
    EXPECT_EQ(2, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue1, next.value));

    ResolveNextPendingWrite(fit::ok());
    EXPECT_EQ(0, callback_count());
  }

  // Resolving the last request should complete the execute write request.
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle1(), next.handle);
    EXPECT_EQ(3, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue2, next.value));

    ResolveNextPendingWrite(fit::ok());
    EXPECT_EQ(1, callback_count());
    EXPECT_EQ(fit::ok(), result());
  }
}

}  // namespace
}  // namespace bt::att
