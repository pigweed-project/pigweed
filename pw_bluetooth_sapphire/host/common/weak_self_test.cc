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

#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"

#include <pw_assert/check.h>
#include <pw_async/fake_dispatcher_fixture.h>
#include <pw_async/heap_dispatcher.h>

#include "pw_unit_test/framework.h"

namespace bt {
namespace {

using WeakSelfTest = pw::async::test::FakeDispatcherFixture;

class FunctionTester : public WeakSelf<FunctionTester> {
 public:
  explicit FunctionTester(uint8_t testval, pw::async::Dispatcher& pw_dispatcher)
      : WeakSelf(this), value_(testval), heap_dispatcher_(pw_dispatcher) {}

  void callback_later_with_weak(
      fit::function<void(FunctionTester::WeakPtr)> cb) {
    auto weak = GetWeakPtr();
    (void)heap_dispatcher_.Post(
        [self = std::move(weak), callback = std::move(cb)](
            pw::async::Context /*ctx*/, pw::Status status) {
          if (status.ok()) {
            callback(self);
          }
        });
  }

  uint8_t value() const { return value_; }

 private:
  uint8_t value_;
  pw::async::HeapDispatcher heap_dispatcher_;
};

TEST_F(WeakSelfTest, InvalidatingSelf) {
  bool called = false;
  FunctionTester::WeakPtr ptr;

  // Default-constructed weak pointers are not alive.
  EXPECT_FALSE(ptr.is_alive());

  auto cb = [&ptr, &called](auto weakptr) {
    called = true;
    ptr = weakptr;
  };

  {
    FunctionTester test(0xBA, dispatcher());

    test.callback_later_with_weak(cb);

    // Run the loop until we're called back.
    RunUntilIdle();

    EXPECT_TRUE(called);
    EXPECT_TRUE(ptr.is_alive());
    EXPECT_EQ(&test, &ptr.get());
    EXPECT_EQ(0xBA, ptr->value());

    called = false;
    test.callback_later_with_weak(cb);

    // Now out of scope.
  }

  // Run the loop until we're called back.
  RunUntilIdle();

  EXPECT_TRUE(called);
  EXPECT_FALSE(ptr.is_alive());
  EXPECT_DEATH_IF_SUPPORTED(ptr.get(), "destroyed");
}

TEST_F(WeakSelfTest, InvalidatePtrs) {
  bool called = false;
  FunctionTester::WeakPtr ptr;

  // Default-constructed weak pointers are not alive.
  EXPECT_FALSE(ptr.is_alive());

  auto cb = [&ptr, &called](auto weakptr) {
    called = true;
    ptr = weakptr;
  };

  FunctionTester test(0xBA, dispatcher());

  test.callback_later_with_weak(cb);

  // Run the loop until we're called back.
  RunUntilIdle();

  EXPECT_TRUE(called);
  EXPECT_TRUE(ptr.is_alive());
  EXPECT_EQ(&test, &ptr.get());
  EXPECT_EQ(0xBA, ptr->value());

  called = false;
  test.callback_later_with_weak(cb);

  // Now invalidate the pointers.
  test.InvalidatePtrs();

  // Run the loop until we're called back.
  RunUntilIdle();

  EXPECT_TRUE(called);
  EXPECT_FALSE(ptr.is_alive());
  EXPECT_DEATH_IF_SUPPORTED(ptr.get(), "destroyed");
}

class StaticTester;

class OnlyTwoStaticManager {
 public:
  explicit OnlyTwoStaticManager(StaticTester* self_ptr) : obj_ptr_(self_ptr) {}
  ~OnlyTwoStaticManager() { InvalidateAll(); }

  using RefType = RecyclingWeakRef;

  std::optional<pw::IntrusivePtr<RefType>> GetWeakRef() {
    for (auto& ptr : OnlyTwoStaticManager::pointers_) {
      if (ptr.is_alive() && ptr.get() == obj_ptr_) {
        // Already adopted, add another refptr pointing to it.
        return pw::IntrusivePtr(&ptr);
      }
    }
    for (auto& ptr : OnlyTwoStaticManager::pointers_) {
      if (!ptr.is_in_use()) {
        return ptr.alloc(obj_ptr_);
      }
    }
    return std::nullopt;
  }

  void InvalidateAll() {
    OnlyTwoStaticManager::pointers_[0].maybe_unset(obj_ptr_);
    OnlyTwoStaticManager::pointers_[1].maybe_unset(obj_ptr_);
  }

 private:
  StaticTester* obj_ptr_;
  inline static RecyclingWeakRef pointers_[2];
};

class StaticTester : public WeakSelf<StaticTester, OnlyTwoStaticManager> {
 public:
  explicit StaticTester(uint8_t testval) : WeakSelf(this), value_(testval) {}

  uint8_t value() const { return value_; }

 private:
  uint8_t value_;
};

TEST_F(WeakSelfTest, StaticRecyclingPointers) {
  // We can create more objects than we have weak space for.
  StaticTester test1(1);
  StaticTester test2(2);
  StaticTester test3(3);

  // And create as many weak pointers of one of them as we want.
  auto ptr = test1.GetWeakPtr();
  auto ptr2 = test1.GetWeakPtr();
  auto ptr3 = test1.GetWeakPtr();
  auto ptr4 = ptr;

  // Make the second one have some ptrs too.
  {
    {
      StaticTester test4(4);
      auto second_ptr = test4.GetWeakPtr();
      auto second_ptr2 = test4.GetWeakPtr();
      EXPECT_TRUE(ptr4.is_alive());
      StaticTester* ptr4_old = &ptr4.get();
      ptr4 = second_ptr;
      EXPECT_TRUE(ptr4.is_alive());
      // It's moved to the new one though.
      EXPECT_NE(&ptr4.get(), ptr4_old);
      EXPECT_EQ(&ptr4.get(), &test4);
    }
    // ptr4 outlived it's target.
    EXPECT_FALSE(ptr4.is_alive());
    // Now let's make the second weak pointer unused, recycling it.
    ptr4 = ptr3;
  }

  // Now I can get a second weak ptr still, from our third object.
  auto still_okay = test3.GetWeakPtr();
  auto still_copy = still_okay;
  EXPECT_TRUE(still_copy.is_alive());
}

TEST_F(WeakSelfTest, StaticDeathWhenExhausted) {
  StaticTester test1(1);
  StaticTester test3(3);

  auto ptr1 = test1.GetWeakPtr();
  auto ptr2 = ptr1;
  {
    StaticTester test2(2);

    ptr2 = test2.GetWeakPtr();

    EXPECT_TRUE(ptr2.is_alive());
    EXPECT_TRUE(ptr1.is_alive());
  }

  EXPECT_FALSE(ptr2.is_alive());

  EXPECT_DEATH_IF_SUPPORTED(test3.GetWeakPtr(), ".*");
}

class GetWeakRefTester;

class CountingWeakManager {
 public:
  explicit CountingWeakManager(GetWeakRefTester* self_ptr)
      : manager_(self_ptr) {}

  using RefType = DynamicWeakManager<GetWeakRefTester>::RefType;

  ~CountingWeakManager() = default;

  std::optional<pw::IntrusivePtr<RefType>> GetWeakRef() {
    // Make sure the weak ref doesn't accidentally get cleared after it's set.
    if (count_get_weak_ref_ == 0) {
      PW_CHECK(!manager_.HasWeakRef());
    } else {
      PW_CHECK(manager_.HasWeakRef());
    }
    count_get_weak_ref_++;
    return manager_.GetWeakRef();
  }

  void InvalidateAll() { return manager_.InvalidateAll(); }

 private:
  size_t count_get_weak_ref_{0};
  DynamicWeakManager<GetWeakRefTester> manager_;
};

class GetWeakRefTester
    : public WeakSelf<GetWeakRefTester, CountingWeakManager> {
 public:
  explicit GetWeakRefTester(uint8_t testval)
      : WeakSelf(this), value_(testval) {}

  uint8_t value() const { return value_; }

 private:
  uint8_t value_;
};

TEST_F(WeakSelfTest, GetWeakRefNotMoved) {
  GetWeakRefTester test_val{1};
  {
    // This is the main test, just make sure there are no assertions in
    // `GetWeakPtr`.
    auto ptr1 = test_val.GetWeakPtr();
    auto ptr2 = test_val.GetWeakPtr();

    EXPECT_TRUE(ptr1.is_alive());
    EXPECT_TRUE(ptr2.is_alive());
    EXPECT_EQ(&ptr1.get(), &ptr2.get());
  }

  auto ptr1 = test_val.GetWeakPtr();
  auto ptr2 = test_val.GetWeakPtr();

  EXPECT_TRUE(ptr1.is_alive());
  EXPECT_TRUE(ptr2.is_alive());
  EXPECT_EQ(&ptr1.get(), &ptr2.get());
}

class BaseClass {
 public:
  BaseClass() = default;
  virtual ~BaseClass() = default;

  void set_value(int value) { value_ = value; }

  int value() const { return value_; }

 private:
  int value_ = 0;
};

class ChildClass : public BaseClass, public WeakSelf<ChildClass> {
 public:
  ChildClass() : BaseClass(), WeakSelf<ChildClass>(this) {}
};

TEST_F(WeakSelfTest, Upcast) {
  ChildClass obj;

  WeakPtr<ChildClass> child_weak = obj.GetWeakPtr();
  child_weak->set_value(1);
  EXPECT_EQ(child_weak->value(), 1);

  WeakPtr<BaseClass> base_weak_copy(child_weak);
  EXPECT_TRUE(child_weak.is_alive());
  base_weak_copy->set_value(2);
  EXPECT_EQ(base_weak_copy->value(), 2);

  WeakPtr<BaseClass> base_weak_move(std::move(child_weak));
  EXPECT_FALSE(child_weak.is_alive());
  base_weak_move->set_value(3);
  EXPECT_EQ(base_weak_move->value(), 3);
}

}  // namespace
}  // namespace bt
