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

#include "pw_bytes/span.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf_private/multibuf_testing.h"
#include "pw_unit_test/framework.h"

// These negative compilation tests are in separate source file from the rest of
// the `v2::MultiBuf` unit tests to facilitate reusing those for the v1 adapter.
//
// The GN build rules in particular do not allow the v1_adapter/BUILD.gn to
// reference this file directly without creating conflicting rules, and does
// not allow copying source files before NC tests are generated during the
// meta-build phase.
//
// The NC tests are performed for the v1 adapter as part of the Bazel build.

namespace {

using ::pw::multibuf::v2::ConstMultiBuf;
using ::pw::multibuf::v2::FlatConstMultiBuf;
using ::pw::multibuf::v2::FlatMultiBuf;
using ::pw::multibuf::v2::MultiBuf;
using ::pw::multibuf::v2::TrackedConstMultiBuf;
using ::pw::multibuf::v2::TrackedFlatConstMultiBuf;
using ::pw::multibuf::v2::TrackedFlatMultiBuf;
using ::pw::multibuf::v2::TrackedMultiBuf;

using ConstMultiBufInstance = pw::multibuf::test::ConstMultiBufInstance;
using FlatConstMultiBufInstance = pw::multibuf::test::FlatConstMultiBufInstance;
using FlatMultiBufInstance = pw::multibuf::test::FlatMultiBufInstance;
using MultiBufInstance = pw::multibuf::test::MultiBufInstance;
using TrackedConstMultiBufInstance =
    pw::multibuf::test::TrackedConstMultiBufInstance;
using TrackedFlatConstMultiBufInstance =
    pw::multibuf::test::TrackedFlatConstMultiBufInstance;
using TrackedFlatMultiBufInstance =
    pw::multibuf::test::TrackedFlatMultiBufInstance;
using TrackedMultiBufInstance = pw::multibuf::test::TrackedMultiBufInstance;

#if PW_NC_TEST(CannotConvertConstMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}

#elif PW_NC_TEST(CannotConvertConstMultiBufToFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<FlatMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertConstMultiBufToMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}

#elif PW_NC_TEST(CannotConvertConstMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertConstMultiBufToTrackedFlatConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertConstMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertConstMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}

#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<ConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<FlatMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToTrackedFlatConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}

#elif PW_NC_TEST(CannotConvertFlatMultiBufToConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<ConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatMultiBufToMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatMultiBufToTrackedFlatConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertFlatMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(MultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}

#elif PW_NC_TEST(CannotConvertMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(MultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertMultiBufToTrackedFlatConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(MultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(MultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(MultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedConstMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(TrackedConstMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}

#elif PW_NC_TEST(CannotConvertTrackedConstMultiBufToFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedConstMultiBuf& mb) {
  std::ignore = mb.as<FlatMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedConstMultiBufToMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedConstMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedConstMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedConstMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<ConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<FlatMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(TrackedFlatMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatMultiBufToConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatMultiBuf& mb) {
  std::ignore = mb.as<ConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatMultiBufToMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedFlatMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}

#elif PW_NC_TEST(CannotConvertTrackedMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(TrackedMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}

#elif PW_NC_TEST(CannotConvertConstMultiBufToMultiBufInstance)
PW_NC_EXPECT("Read-only data cannot be assigned to mutable data.");
[[maybe_unused]] MultiBufInstance ShouldAssert(ConstMultiBuf&& mb) {
  return MultiBufInstance(std::move(mb));
}

#elif PW_NC_TEST(CannotCopyConstructMultiBuf)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBuf mb1(mb);
  EXPECT_TRUE(mb1.empty());
}

#elif PW_NC_TEST(CannotCopyAssignMultiBuf)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBuf mb1(std::move(mb));
  EXPECT_TRUE(mb1.empty());
}

#elif PW_NC_TEST(CannotMoveConstructMultiBuf)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBuf mb1 = mb;
  EXPECT_TRUE(mb1.empty());
}

#elif PW_NC_TEST(CannotMoveAssignMultiBuf)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBuf mb1 = std::move(mb);
  EXPECT_TRUE(mb1.empty());
}

#elif PW_NC_TEST(CannotConstructFromMultiBufInstance)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBufInstance&& mbi) {
  MultiBuf mb(mbi);
  EXPECT_TRUE(mb.empty());
}

#elif PW_NC_TEST(CannotAssignFromMultiBufInstance)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBufInstance&& mbi) {
  MultiBuf mb = mbi;
  EXPECT_TRUE(mb.empty());
}

#elif PW_NC_TEST(CannotCopyConstructInstance)
PW_NC_EXPECT(
    "(no matching constructor for initialization|Instances can only be created "
    "from existing MultiBufs using move-construction or move-assignment)");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBufInstance tmp(mb);
  EXPECT_TRUE(tmp->empty());
}

#elif PW_NC_TEST(CannotCopyAssignInstance)
PW_NC_EXPECT(
    "(no viable conversion|Instances can only be created from existing "
    "MultiBufs using move-construction or move-assignment)");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBufInstance tmp = mb;
  EXPECT_TRUE(tmp->empty());
}

#elif PW_NC_TEST(MutableDereference)
PW_NC_EXPECT_CLANG(
    "cannot assign to return value because function 'at' returns a const "
    "value");
PW_NC_EXPECT_GCC("assignment of read-only location");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  mb.at(0) = std::byte(0);
}

#elif PW_NC_TEST(CannotReturnMultiBuf)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] MultiBuf ShouldAssert(MultiBuf&& mb) { return mb; }

#endif  // PW_NC_TEST

////////////////////////////////////////////////////////////////////////////////
// The following NC tests do not apply to the v1 adapter.
#if !PW_MULTIBUF_INCLUDE_V1_ADAPTERS

// The v1_adapter's underlying multibuf is always tracked and layered.
#if PW_NC_TEST(CannotAssignMultiBufToFlatMultiBufInstance)
PW_NC_EXPECT("Layered MultiBufs cannot be assigned to flat MultiBufs.");
[[maybe_unused]] MultiBufInstance ShouldAssert(MultiBuf&& mb) {
  return FlatMultiBufInstance(std::move(mb));
}

// The v1_adapter's underlying multibuf is always tracked and layered.
#elif PW_NC_TEST(CannotAssignMultiBufToTrackedMultiBufInstance)
PW_NC_EXPECT("Tracked MultiBufs cannot be assigned to untracked MultiBufs.");
[[maybe_unused]] TrackedMultiBufInstance ShouldAssert(TrackedMultiBuf&& mb) {
  return MultiBufInstance(std::move(mb));
}

// The v1_adapter doesn't have a concept of constness.
#elif PW_NC_TEST(MutableAccess)
PW_NC_EXPECT_CLANG(
    "cannot assign to return value because function 'operator\[\]' returns a "
    "const value");
PW_NC_EXPECT_GCC("assignment of read-only location");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) { mb[0] = std::byte(0); }

// The v1_adapter doesn't have a concept of constness.
#elif PW_NC_TEST(MutableIterators)
PW_NC_EXPECT_CLANG(
    "cannot assign to return value because function 'operator\*' returns a "
    "const value");
PW_NC_EXPECT_GCC("assignment of read-only location");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  *(mb.begin()) = std::byte(0);
}

#endif  // PW_NC_TEST
#endif  // !PW_MULTIBUF_INCLUDE_V1_ADAPTERS

}  // namespace
