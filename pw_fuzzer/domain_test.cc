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

#include <cctype>

#include "pw_fuzzer/fuzztest.h"
#include "pw_result/result.h"
#include "pw_unit_test/framework.h"

// Most of the tests in this file only validate that the domains provided by
// Pigweed build, both with and without FuzzTest. Each domain comprises one or
// more FuzzTest domains, so the validation of the distribution of values
// produced by domains is left to and assumed from FuzzTest's own domain tests.

namespace pw::fuzzer {
namespace {

////////////////////////////////////////////////////////////////
// Constants, macros, and types used by the domain tests below.

constexpr size_t kSize = 8;
constexpr char kMin = 4;
constexpr char kMax = 16;

/// Generates a target function and fuzz test for a specific type.
///
/// The generated test simply checks that the provided domain can produce values
/// of the appropriate type. Generally, explicitly providing a target function
/// is preferred for readability, but this macro can be useful for templated
/// domains that are tested for many repetitive numerical types.
///
/// This macro should not be used directly. Instead, use
/// `FUZZ_TEST_FOR_INTEGRAL`, `FUZZ_TEST_FOR_FLOATING_POINT`, or
/// `FUZZ_TEST_FOR_ARITHMETIC`.
#define FUZZ_TEST_FOR_TYPE(Suite, Target, Domain, Type, ...) \
  void Target(Type t) { Take##Domain<Type>(t); }             \
  FUZZ_TEST(Suite, Target).WithDomains(Domain<Type>(__VA_ARGS__))

/// Generates target functions and fuzz tests for a integral types.
#define FUZZ_TEST_FOR_INTEGRAL(Suite, Target, Domain, ...)                     \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_Char, Domain, char, __VA_ARGS__);         \
  FUZZ_TEST_FOR_TYPE(                                                          \
      Suite, Target##_UChar, Domain, unsigned char, __VA_ARGS__);              \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_Short, Domain, short, __VA_ARGS__);       \
  FUZZ_TEST_FOR_TYPE(                                                          \
      Suite, Target##_UShort, Domain, unsigned short, __VA_ARGS__);            \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_Int, Domain, int, __VA_ARGS__);           \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_UInt, Domain, unsigned int, __VA_ARGS__); \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_Long, Domain, long, __VA_ARGS__);         \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_ULong, Domain, unsigned long, __VA_ARGS__)

/// Generates target functions and fuzz tests for a floating point types.
#define FUZZ_TEST_FOR_FLOATING_POINT(Suite, Target, Domain, ...)         \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_Float, Domain, float, __VA_ARGS__); \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_Double, Domain, double, __VA_ARGS__)

/// Generates target functions and fuzz tests for a all arithmetic types.
#define FUZZ_TEST_FOR_ARITHMETIC(Suite, Target, Domain, ...)  \
  FUZZ_TEST_FOR_INTEGRAL(Suite, Target, Domain, __VA_ARGS__); \
  FUZZ_TEST_FOR_FLOATING_POINT(Suite, Target, Domain, __VA_ARGS__)

/// Generates target functions and fuzz tests for signed integral types.
#define FUZZ_TEST_FOR_SIGNED_INTEGRAL(Suite, Target, Domain, ...)              \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_SChar, Domain, signed char, __VA_ARGS__); \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_Short, Domain, short, __VA_ARGS__);       \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_Int, Domain, int, __VA_ARGS__);           \
  FUZZ_TEST_FOR_TYPE(Suite, Target##_Long, Domain, long, __VA_ARGS__)

/// Generates target functions and fuzz tests for all signed arithmetic types.
#define FUZZ_TEST_FOR_SIGNED_ARITHMETIC(Suite, Target, Domain, ...)  \
  FUZZ_TEST_FOR_SIGNED_INTEGRAL(Suite, Target, Domain, __VA_ARGS__); \
  FUZZ_TEST_FOR_FLOATING_POINT(Suite, Target, Domain, __VA_ARGS__)

// Test struct that can be produced by FuzzTest.
struct StructForTesting {
  int a;
  long b;
};

// Test class that can be produced by FuzzTest.
class ClassForTesting {
 public:
  ClassForTesting(unsigned char c, short d) : c_(c), d_(d) {}
  unsigned char c() const { return c_; }
  short d() const { return d_; }

 private:
  unsigned char c_;
  short d_;
};

////////////////////////////////////////////////////////////////
// Arbitrary domains forwarded or stubbed from FuzzTest

template <typename T>
void TakeArbitrary(T) {}

void TakeArbitraryBool(bool b) { TakeArbitrary<bool>(b); }
FUZZ_TEST(ArbitraryTest, TakeArbitraryBool).WithDomains(Arbitrary<bool>());

FUZZ_TEST_FOR_ARITHMETIC(ArbitraryTest, TakeArbitrary, Arbitrary);

void TakeArbitraryStruct(const StructForTesting& s) {
  TakeArbitrary<StructForTesting>(s);
}
FUZZ_TEST(ArbitraryTest, TakeArbitraryStruct)
    .WithDomains(Arbitrary<StructForTesting>());

void TakeArbitraryTuple(const std::tuple<int, long>& t) {
  TakeArbitrary<std::tuple<int, long>>(t);
}
FUZZ_TEST(ArbitraryTest, TakeArbitraryTuple)
    .WithDomains(Arbitrary<std::tuple<int, long>>());

void TakeArbitraryOptional(const std::optional<int>& o) {
  TakeArbitrary<std::optional<int>>(o);
}
FUZZ_TEST(ArbitraryTest, TakeArbitraryOptional)
    .WithDomains(Arbitrary<std::optional<int>>());

////////////////////////////////////////////////////////////////
// Numerical domains forwarded or stubbed from FuzzTest

template <typename Arithmetic>
void TakeInRange(Arithmetic x) {
  EXPECT_GE(x, Arithmetic(kMin));
  EXPECT_LE(x, Arithmetic(kMax));
}
FUZZ_TEST_FOR_ARITHMETIC(DomainTest, TakeInRange, InRange, kMin, kMax);

template <typename Arithmetic>
void TakeNonZero(Arithmetic x) {
  EXPECT_NE(x, Arithmetic(0));
}
FUZZ_TEST_FOR_ARITHMETIC(DomainTest, TakeNonZero, NonZero);

template <typename Arithmetic>
void TakePositive(Arithmetic x) {
  EXPECT_GT(x, Arithmetic(0));
}
FUZZ_TEST_FOR_ARITHMETIC(DomainTest, TakePositive, Positive);

template <typename Arithmetic>
void TakeNonNegative(Arithmetic x) {
  EXPECT_GE(x, Arithmetic(0));
}
FUZZ_TEST_FOR_ARITHMETIC(DomainTest, TakeNonNegative, NonNegative);

template <typename Arithmetic>
void TakeNegative(Arithmetic x) {
  EXPECT_LT(x, Arithmetic(0));
}
FUZZ_TEST_FOR_SIGNED_ARITHMETIC(DomainTest, TakeNegative, Negative);

template <typename Arithmetic>
void TakeNonPositive(Arithmetic x) {
  EXPECT_LE(x, Arithmetic(0));
}
FUZZ_TEST_FOR_SIGNED_ARITHMETIC(DomainTest, TakeNonPositive, NonPositive);

template <typename FloatingPoint>
void TakeFinite(FloatingPoint f) {
  EXPECT_TRUE(std::isfinite(f));
}
FUZZ_TEST_FOR_FLOATING_POINT(DomainTest, TakeFinite, Finite);

////////////////////////////////////////////////////////////////
// Character domains forwarded or stubbed from FuzzTest

void TakeNonZeroChar(char c) { EXPECT_NE(c, 0); }
FUZZ_TEST(DomainTest, TakeNonZeroChar).WithDomains(NonZeroChar());

void TakeNumericChar(char c) { EXPECT_TRUE(std::isdigit(c)); }
FUZZ_TEST(DomainTest, TakeNumericChar).WithDomains(NumericChar());

void TakeLowerChar(char c) { EXPECT_TRUE(std::islower(c)); }
FUZZ_TEST(DomainTest, TakeLowerChar).WithDomains(LowerChar());

void TakeUpperChar(char c) { EXPECT_TRUE(std::isupper(c)); }
FUZZ_TEST(DomainTest, TakeUpperChar).WithDomains(UpperChar());

void TakeAlphaChar(char c) { EXPECT_TRUE(std::isalpha(c)); }
FUZZ_TEST(DomainTest, TakeAlphaChar).WithDomains(AlphaChar());

void TakeAlphaNumericChar(char c) { EXPECT_TRUE(std::isalnum(c)); }
FUZZ_TEST(DomainTest, TakeAlphaNumericChar).WithDomains(AlphaNumericChar());

void TakePrintableAsciiChar(char c) { EXPECT_TRUE(std::isprint(c)); }
FUZZ_TEST(DomainTest, TakePrintableAsciiChar).WithDomains(PrintableAsciiChar());

void TakeAsciiChar(char c) {
  EXPECT_GE(c, 0);
  EXPECT_LE(c, 127);
}
FUZZ_TEST(DomainTest, TakeAsciiChar).WithDomains(AsciiChar());

////////////////////////////////////////////////////////////////
// Regular expression domains forwarded or stubbed from FuzzTest

// TODO: b/285775246 - Add support for `fuzztest::InRegexp`.
// void TakeMatch(std::string_view sv) {
//   ASSERT_EQ(sv.size(), 3U);
//   EXPECT_EQ(sv[0], 'a');
//   EXPECT_EQ(sv[2], 'c');
// }
// FUZZ_TEST(DomainTest, TakeMatch).WithDomains(InRegexp("a.c"));

////////////////////////////////////////////////////////////////
// Enumerated domains forwarded or stubbed from FuzzTest

void TakeSingleDigitEvenNumber(int n) {
  EXPECT_LT(n, 10);
  EXPECT_EQ(n % 2, 0);
}
FUZZ_TEST(DomainTest, TakeSingleDigitEvenNumber)
    .WithDomains(ElementOf({0, 2, 4, 6, 8}));

enum Flags : uint8_t {
  kFlag1 = 1 << 0,
  kFlag2 = 1 << 1,
  kFlag3 = 1 << 2,
};
void TakeFlagCombination(uint8_t flags) { EXPECT_FALSE(flags & Flags::kFlag2); }
FUZZ_TEST(DomainTest, TakeFlagCombination)
    .WithDomains(BitFlagCombinationOf({Flags::kFlag1, Flags::kFlag3}));

////////////////////////////////////////////////////////////////
// Aggregate domains forwarded or stubbed from FuzzTest

void TakeStructForTesting(const StructForTesting& obj) {
  EXPECT_NE(obj.a, 0);
  EXPECT_LT(obj.b, 0);
}
FUZZ_TEST(DomainTest, TakeStructForTesting)
    .WithDomains(StructOf<StructForTesting>(NonZero<int>(), Negative<long>()));

void TakeClassForTesting(const ClassForTesting& obj) {
  EXPECT_GE(obj.c(), kMin);
  EXPECT_LE(obj.c(), kMax);
  EXPECT_GE(obj.d(), 0);
}
FUZZ_TEST(DomainTest, TakeClassForTesting)
    .WithDomains(ConstructorOf<ClassForTesting>(InRange<unsigned char>(kMin,
                                                                       kMax),
                                                NonNegative<short>()));

void TakePair(const std::pair<char, float>& p) {
  EXPECT_TRUE(std::islower(p.first));
  EXPECT_TRUE(std::isfinite(p.second));
}
FUZZ_TEST(DomainTest, TakePair)
    .WithDomains(PairOf(LowerChar(), Finite<float>()));

void TakeTuple(std::tuple<short, int> a, long b) {
  EXPECT_NE(std::get<short>(a), 0);
  EXPECT_NE(std::get<int>(a), 0);
  EXPECT_NE(b, 0);
}
FUZZ_TEST(DomainTest, TakeTuple)
    .WithDomains(TupleOf(NonZero<short>(), NonZero<int>()), NonZero<long>());

void TakeVariant(const std::variant<int, long>&) {}
FUZZ_TEST(DomainTest, TakeVariant)
    .WithDomains(VariantOf(Arbitrary<int>(), Arbitrary<long>()));

void TakeOptional(const std::optional<int>&) {}
FUZZ_TEST(DomainTest, TakeOptional).WithDomains(OptionalOf(Arbitrary<int>()));

void TakeNullOpt(const std::optional<int>& option) { EXPECT_FALSE(option); }
FUZZ_TEST(DomainTest, TakeNullOpt).WithDomains(NullOpt<int>());

void TakeNonNull(const std::optional<int>& option) { EXPECT_TRUE(option); }
FUZZ_TEST(DomainTest, TakeNonNull)
    .WithDomains(NonNull(OptionalOf(Arbitrary<int>())));

////////////////////////////////////////////////////////////////
// Other miscellaneous domains forwarded or stubbed from FuzzTest

void TakePositiveOrMinusOne(int n) {
  if (n != -1) {
    EXPECT_GT(n, 0);
  }
}
FUZZ_TEST(DomainTest, TakePositiveOrMinusOne)
    .WithDomains(OneOf(Just(-1), Positive<int>()));

void TakePackedValue(uint32_t value) {
  EXPECT_GE(value & 0xFFFF, 1000U);
  EXPECT_LT(value >> 16, 2048U);
}
FUZZ_TEST(DomainTest, TakePackedValue)
    .WithDomains(
        Map([](uint16_t lower,
               uint16_t upper) { return (uint32_t(upper) << 16) | lower; },
            InRange<uint16_t>(1000U, std::numeric_limits<uint16_t>::max()),
            InRange<uint16_t>(0U, 2047U)));

void TakeOrdered(size_t x, size_t y) { EXPECT_LT(x, y); }
void FlatMapAdapter(const std::pair<size_t, size_t>& p) {
  TakeOrdered(p.first, p.second);
}
FUZZ_TEST(DomainTest, FlatMapAdapter)
    .WithDomains(FlatMap(
        [](size_t x) {
          return PairOf(
              Just(x),
              InRange<size_t>(x + 1, std::numeric_limits<size_t>::max()));
        },
        InRange<size_t>(0, std::numeric_limits<size_t>::max() - 1)));

void TakeEven(unsigned int n) { EXPECT_EQ(n % 2, 0U); }
FUZZ_TEST(DomainTest, TakeEven)
    .WithDomains(Filter([](unsigned int n) { return n % 2 == 0; },
                        Arbitrary<unsigned int>()));

////////////////////////////////////////////////////////////////
// pw_status-related types

void TakeStatus(const Status&) {}
FUZZ_TEST(ArbitraryTest, TakeStatus).WithDomains(Arbitrary<Status>());

void TakeStatusWithSize(const StatusWithSize&) {}
FUZZ_TEST(ArbitraryTest, TakeStatusWithSize)
    .WithDomains(Arbitrary<StatusWithSize>());

void TakeNonOkStatus(const Status& status) { EXPECT_FALSE(status.ok()); }
FUZZ_TEST(FilterTest, TakeNonOkStatus).WithDomains(NonOkStatus());

////////////////////////////////////////////////////////////////
// pw_result-related types

void TakeResult(const Result<int>&) {}
FUZZ_TEST(DomainTest, TakeResult).WithDomains(ResultOf(Arbitrary<int>()));
FUZZ_TEST(ArbitraryTest, TakeResult).WithDomains(Arbitrary<Result<int>>());

////////////////////////////////////////////////////////////////
// pw_containers-related types

void TakeVector(const Vector<int>& vector) {
  EXPECT_EQ(vector.max_size(), kSize);
}
FUZZ_TEST(DomainTest, TakeVector)
    .WithDomains(VectorOf<kSize>(Arbitrary<int>()));
FUZZ_TEST(ArbitraryTest, TakeVector)
    .WithDomains(Arbitrary<Vector<int, kSize>>());

void TakeVectorAsContainer(const Vector<int>&) {}
FUZZ_TEST(ContainerTest, TakeVectorAsContainer)
    .WithDomains(ContainerOf<Vector<int, kSize>>(Arbitrary<int>()));

void TakeVectorNonEmpty(const Vector<int>& vector) {
  EXPECT_FALSE(vector.empty());
}
FUZZ_TEST(ContainerTest, TakeVectorNonEmpty)
    .WithDomains(NonEmpty(ContainerOf<Vector<int, kSize>>(Arbitrary<int>())));

void TakeVectorLessThan3(const Vector<int>& vector) {
  EXPECT_LT(vector.size(), 3U);
}
FUZZ_TEST(ContainerTest, TakeVectorLessThan3)
    .WithDomains(
        ContainerOf<Vector<int, kSize>>(Arbitrary<int>()).WithMaxSize(2));

void TakeVectorAtLeast3(const Vector<int>& vector) {
  EXPECT_GE(vector.size(), 3U);
}
FUZZ_TEST(ContainerTest, TakeVectorAtLeast3)
    .WithDomains(
        ContainerOf<Vector<int, kSize>>(Arbitrary<int>()).WithMinSize(3));

void TakeVectorExactly3(const Vector<int>& vector) {
  EXPECT_EQ(vector.size(), 3U);
}
FUZZ_TEST(ContainerTest, TakeVectorExactly3)
    .WithDomains(ContainerOf<Vector<int, kSize>>(Arbitrary<int>()).WithSize(3));

void TakeVectorUnique(const Vector<int>& vector) {
  for (auto i = vector.begin(); i != vector.end(); ++i) {
    for (auto j = i + 1; j != vector.end(); ++j) {
      EXPECT_NE(*i, *j);
    }
  }
}
FUZZ_TEST(ContainerTest, TakeVectorUnique)
    .WithDomains(
        UniqueElementsContainerOf<Vector<int, kSize>>(Arbitrary<int>()));
FUZZ_TEST(DomainTest, TakeVectorUnique)
    .WithDomains(UniqueElementsVectorOf<kSize>(Arbitrary<int>()));

void TakeFlatMap(const containers::FlatMap<int, size_t, kSize>&) {}
FUZZ_TEST(DomainTest, TakeFlatMap)
    .WithDomains(FlatMapOf<kSize>(Arbitrary<int>(), Arbitrary<size_t>()));
FUZZ_TEST(ArbitraryTest, TakeFlatMap)
    .WithDomains(Arbitrary<containers::FlatMap<int, size_t, kSize>>());
FUZZ_TEST(ContainerTest, TakeFlatMap)
    .WithDomains(ContainerOf<containers::FlatMap<int, size_t, kSize>>(
        FlatMapPairOf(Arbitrary<int>(), Arbitrary<size_t>())));
FUZZ_TEST(MapToTest, TakeFlatMap)
    .WithDomains(MapToFlatMap<kSize>(
        UniqueElementsVectorOf<kSize>(Arbitrary<int>()).WithSize(kSize),
        ArrayOf<kSize>(Arbitrary<size_t>())));

void TakeDeque(const InlineDeque<int, kSize>& deque) {
  EXPECT_EQ(deque.max_size(), kSize);
}
FUZZ_TEST(DomainTest, TakeDeque).WithDomains(DequeOf<kSize>(Arbitrary<int>()));
FUZZ_TEST(ArbitraryTest, TakeDeque)
    .WithDomains(Arbitrary<InlineDeque<int, kSize>>());

void TakeQueue(const InlineQueue<int, kSize>& queue) {
  EXPECT_EQ(queue.max_size(), kSize);
}
FUZZ_TEST(DomainTest, TakeQueue).WithDomains(QueueOf<kSize>(Arbitrary<int>()));
FUZZ_TEST(ArbitraryTest, TakeQueue)
    .WithDomains(Arbitrary<InlineQueue<int, kSize>>());

// Test item that can be added to an intrusive list.
class TestItem : public IntrusiveList<TestItem>::Item {
 private:
 public:
  constexpr explicit TestItem(long value) : value_(value) {}
  long value() const { return value_; }

  TestItem(TestItem&& other) { *this = std::move(other); }
  TestItem& operator=(TestItem&& other) {
    value_ = other.value_;
    return *this;
  }

 private:
  long value_;
};

// IntrusiveLists cannot be generated directly, but ScopedLists can.
void TakeIntrusiveList(const IntrusiveList<TestItem>& list) {
  EXPECT_LE(list.size(), kSize);
}
void ScopedListAdapter(const ScopedList<TestItem, kSize>& scoped) {
  TakeIntrusiveList(scoped.list());
}
FUZZ_TEST(DomainTest, ScopedListAdapter)
    .WithDomains(ScopedListOf<TestItem, kSize>(Arbitrary<long>()));

////////////////////////////////////////////////////////////////
// pw_string-related types

void TakeString(const InlineString<>& string) {
  EXPECT_EQ(string.max_size(), kSize);
}
FUZZ_TEST(DomainTest, TakeString)
    .WithDomains(StringOf<kSize>(Arbitrary<char>()));
FUZZ_TEST(ArbitraryTest, TakeString)
    .WithDomains(Arbitrary<InlineString<kSize>>());
FUZZ_TEST(FilterTest, TakeString).WithDomains(String<kSize>());

void TakeStringAsContainer(const InlineString<>&) {}
FUZZ_TEST(ContainerTest, TakeStringAsContainer)
    .WithDomains(ContainerOf<InlineString<kSize>>(Arbitrary<char>()));

void TakeStringNonEmpty(const InlineString<>& string) {
  EXPECT_FALSE(string.empty());
}
FUZZ_TEST(ContainerTest, TakeStringNonEmpty)
    .WithDomains(NonEmpty(ContainerOf<InlineString<kSize>>(Arbitrary<char>())));

void TakeStringLessThan3(const InlineString<>& string) {
  EXPECT_LT(string.size(), 3U);
}
FUZZ_TEST(ContainerTest, TakeStringLessThan3)
    .WithDomains(
        ContainerOf<InlineString<kSize>>(Arbitrary<char>()).WithMaxSize(2));

void TakeStringAtLeast3(const InlineString<>& string) {
  EXPECT_GE(string.size(), 3U);
}
FUZZ_TEST(ContainerTest, TakeStringAtLeast3)
    .WithDomains(
        ContainerOf<InlineString<kSize>>(Arbitrary<char>()).WithMinSize(3));

void TakeStringExactly3(const InlineString<>& string) {
  EXPECT_EQ(string.size(), 3U);
}
FUZZ_TEST(ContainerTest, TakeStringExactly3)
    .WithDomains(
        ContainerOf<InlineString<kSize>>(Arbitrary<char>()).WithSize(3));

void TakeAsciiString(const InlineString<>& string) {
  EXPECT_TRUE(std::all_of(
      string.begin(), string.end(), [](int c) { return c < 0x80; }));
}
FUZZ_TEST(FilterTest, TakeAsciiString).WithDomains(AsciiString<kSize>());

void TakePrintableAsciiString(const InlineString<>& string) {
  EXPECT_TRUE(std::all_of(string.begin(), string.end(), isprint));
}
FUZZ_TEST(FilterTest, TakePrintableAsciiString)
    .WithDomains(PrintableAsciiString<kSize>());

}  // namespace
}  // namespace pw::fuzzer
