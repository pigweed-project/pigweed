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

#include "pw_json/builder.h"

#include <array>
#include <cstring>
#include <string_view>

#include "pw_assert/check.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/framework.h"

namespace {

using namespace std::string_view_literals;

// First example for the docs.
static_assert([] {
  bool is_simple = true;
  int safety_percentage = 100;

  std::string_view features[] = {"values", "arrays", "objects", "nesting!"};

  // DOCTSAG: [pw-json-builder-example-1]
  pw::JsonBuffer<256> json_buffer;
  pw::JsonObject& object = json_buffer.StartObject();
  object.Add("tagline", "Easy, efficient JSON serialization!")
      .Add("simple", is_simple)
      .Add("safe", safety_percentage)
      .Add("dynamic allocation", false);

  pw::NestedJsonArray nested_array = object.AddNestedArray("features");
  for (const std::string_view feature : features) {
    nested_array.Append(feature);
  }
  // DOCTSAG: [pw-json-builder-example-1]
  return json_buffer;
}() == R"({"tagline": "Easy, efficient JSON serialization!", "simple": true,)"
       R"( "safe": 100, "dynamic allocation": false, "features":)"
       R"( ["values", "arrays", "objects", "nesting!"]})"sv);

// Second example for the docs.
static_assert([] {
  constexpr char empty[128] = {};
  std::string_view huge_string_that_wont_fit(empty, sizeof(empty));

  // DOCTSAG: [pw-json-builder-example-2]
  // Declare a JsonBuffer (JsonBuilder with included buffer) and start a JSON
  // object in it.
  pw::JsonBuffer<128> json_buffer;
  pw::JsonObject& json = json_buffer.StartObject();

  const char* name = "Crag";
  constexpr const char* kOccupationKey = "job";

  // Add key-value pairs to a JSON object.
  json.Add("name", name).Add(kOccupationKey, "hacker");

  // Add an array as the value in a key-value pair.
  pw::NestedJsonArray nested_array = json.AddNestedArray("skills");

  // Append items to an array.
  nested_array.Append(20).Append(1).Append(1).Append(1);

  // Check that everything fit in the JSON buffer.
  PW_ASSERT(json.ok());

  // Compare the contents of the JSON to a std::string_view.
  PW_ASSERT(std::string_view(json) ==
            R"({"name": "Crag", "job": "hacker", "skills": [20, 1, 1, 1]})");

  // Add an object as the value in a key-value pair.
  pw::NestedJsonObject nested_object = json.AddNestedObject("items");

  // Declare another JsonArray, and add it as nested value.
  pw::JsonBuffer<10> inner_buffer;
  inner_buffer.StartArray().Append(nullptr);
  nested_object.Add("misc", inner_buffer);

  // Add a value that is too large for the JsonBuffer.
  json.Add("way too big!", huge_string_that_wont_fit);

  // Adding the last entry failed, but the JSON is still valid.
  PW_ASSERT(json.status().IsResourceExhausted());

  PW_ASSERT(std::string_view(json) ==
            R"({"name": "Crag", "job": "hacker", "skills": [20, 1, 1, 1],)"
            R"( "items": {"misc": [null]}})");
  // DOCTSAG: [pw-json-builder-example-2]
  return json_buffer;
}() == R"({"name": "Crag", "job": "hacker", "skills": [20, 1, 1, 1],)"
       R"( "items": {"misc": [null]}})"sv);

}  // namespace

namespace pw {
namespace {

class JsonOverflowTest : public ::testing::Test {
 public:
  ~JsonOverflowTest() override {
    EXPECT_STREQ(&buffer_[end_], kTag) << "Overflow occurred!";
  }

 protected:
  void MarkBufferEnd(const JsonBuilder& json) {
    end_ = json.max_size() + 1;
    ASSERT_LT(end_, sizeof(buffer_) - sizeof(kTag));
    std::memcpy(&buffer_[end_], kTag, sizeof(kTag));
  }

  char buffer_[512];

 private:
  static constexpr const char kTag[] = "Hi! Your buffer is safe.";

  size_t end_ = 0;
};

TEST(JsonObject, BasicJson) {
  char buffer[128];
  std::memset(buffer, '?', sizeof(buffer));
  JsonBuilder json_buffer(buffer);
  JsonObject& json = json_buffer.StartObject();

  EXPECT_EQ(buffer, json.data());
  EXPECT_STREQ("{}", json.data());
  EXPECT_EQ(2u, json.size());

  EXPECT_EQ(OkStatus(), json.Add("foo", "bar").status());
  EXPECT_STREQ(R"({"foo": "bar"})", json.data());
  EXPECT_EQ(std::strlen(json.data()), json.size());

  EXPECT_EQ(OkStatus(), json.Add("bar", 0).status());
  EXPECT_STREQ(R"({"foo": "bar", "bar": 0})", json.data());
  EXPECT_EQ(std::strlen(json.data()), json.size());

  EXPECT_EQ(OkStatus(), json.Add("baz", nullptr).status());
  EXPECT_STREQ(R"({"foo": "bar", "bar": 0, "baz": null})", json.data());
  EXPECT_EQ(std::strlen(json.data()), json.size());

  EXPECT_EQ(OkStatus(), json.Add("EMPTY STR!", "").status());
  EXPECT_STREQ(R"({"foo": "bar", "bar": 0, "baz": null, "EMPTY STR!": ""})",
               json.data());
  EXPECT_EQ(std::strlen(json.data()), json.size());
}

TEST(JsonObject, OverflowAtKey) {
  JsonBuffer<19> json_buffer;
  JsonObject& json = json_buffer.StartObject();

  EXPECT_EQ(OkStatus(), json.Add("a", 5l).Add("b", "!").status());
  EXPECT_STREQ(R"({"a": 5, "b": "!"})", json.data());  // 18 chars + \0

  EXPECT_EQ(Status::ResourceExhausted(), json.Add("b", "!").status());
  EXPECT_STREQ(R"({"a": 5, "b": "!"})", json.data());
  EXPECT_EQ(Status::ResourceExhausted(), json.Add("b", "").status());
  EXPECT_STREQ(R"({"a": 5, "b": "!"})", json.data());
  EXPECT_EQ(Status::ResourceExhausted(), json.status());
  EXPECT_EQ(Status::ResourceExhausted(), json.last_status());
  json.clear_status();
  EXPECT_STREQ(R"({"a": 5, "b": "!"})", json.data());
  EXPECT_EQ(OkStatus(), json.status());
  EXPECT_EQ(OkStatus(), json.last_status());
}

TEST_F(JsonOverflowTest, ObjectOverflowAtFirstEntry) {
  JsonBuilder json_builder(buffer_, 5);
  MarkBufferEnd(json_builder);

  JsonObject& json = json_builder.StartObject();
  ASSERT_STREQ("{}", json.data());
  EXPECT_EQ(Status::ResourceExhausted(), json.Add("some_key", "").status());
  EXPECT_STREQ("{}", json.data());
}

TEST_F(JsonOverflowTest, ObjectOverflowAtStringValue) {
  JsonBuilder json_builder(buffer_, 32);
  MarkBufferEnd(json_builder);

  JsonObject& json = json_builder.StartObject();
  EXPECT_EQ(OkStatus(), json.Add("a", 5l).status());
  EXPECT_STREQ(R"({"a": 5})", json.data());

  EXPECT_EQ(
      Status::ResourceExhausted(),
      json.Add("b", "This string is so long that it won't fit!!!!").status());
  EXPECT_STREQ(R"({"a": 5})", json.data());
  EXPECT_EQ(Status::ResourceExhausted(), json.status());

  EXPECT_EQ(OkStatus(), json.Add("b", "This will!").last_status());
  EXPECT_STREQ(R"({"a": 5, "b": "This will!"})", json.data());
  EXPECT_EQ(Status::ResourceExhausted(), json.status());
  EXPECT_EQ(OkStatus(), json.last_status());
}

TEST_F(JsonOverflowTest, OverflowAtUnicodeCharacter) {
  JsonBuilder json_builder(buffer_, 10);
  MarkBufferEnd(json_builder);

  JsonValue& overflow_at_unicode = json_builder.StartValue();
  EXPECT_EQ(Status::ResourceExhausted(), overflow_at_unicode.Set("234\x01"));
  EXPECT_STREQ("null", overflow_at_unicode.data());
  EXPECT_EQ(Status::ResourceExhausted(), overflow_at_unicode.Set("2345\x01"));
  EXPECT_STREQ("null", overflow_at_unicode.data());
  EXPECT_EQ(Status::ResourceExhausted(),
            overflow_at_unicode.Set("23456789\x01"));
  EXPECT_STREQ("null", overflow_at_unicode.data());
}

TEST_F(JsonOverflowTest, ObjectOverflowAtNumber) {
  JsonBuilder json_builder(buffer_, 14);
  MarkBufferEnd(json_builder);

  JsonObject& json = json_builder.StartObject();
  EXPECT_EQ(OkStatus(), json.Add("a", 123456).status());
  EXPECT_STREQ(R"({"a": 123456})", json.data());
  EXPECT_EQ(json.max_size(), json.size());
  json.clear();

  EXPECT_EQ(Status::ResourceExhausted(), json.Add("a", 1234567).status());
  EXPECT_STREQ(R"({})", json.data());
  EXPECT_EQ(2u, json.size());
  json.clear();

  EXPECT_EQ(Status::ResourceExhausted(), json.Add("a", 12345678).status());
  EXPECT_STREQ(R"({})", json.data());
  EXPECT_EQ(2u, json.size());
}

TEST(JsonObject, StringValueFillsAllSpace) {
  JsonBuffer<15> json_buffer;

  JsonObject& json = json_buffer.StartObject();
  EXPECT_EQ(OkStatus(), json.Add("key", "12\\").status());
  EXPECT_STREQ(R"({"key": "12\\"})", json.data());
  EXPECT_EQ(15u, json.size());

  json.clear();
  EXPECT_EQ(Status::ResourceExhausted(), json.Add("key", "123\\").status());
  EXPECT_STREQ(R"({})", json.data());
}

TEST(JsonObject, NestedJson) {
  JsonBuffer<64> outside_builder;
  JsonBuffer<32> inside_builder;

  JsonObject& outside = outside_builder.StartObject();
  JsonObject& inside = inside_builder.StartObject();

  ASSERT_EQ(OkStatus(), inside.Add("inside", 123).status());
  ASSERT_STREQ(R"({"inside": 123})", inside.data());

  EXPECT_EQ(OkStatus(), outside.Add("some_value", inside).status());
  EXPECT_STREQ(R"({"some_value": {"inside": 123}})", outside.data());

  inside.clear();
  EXPECT_EQ(OkStatus(), outside.Add("MT", inside).status());
  EXPECT_STREQ(R"({"some_value": {"inside": 123}, "MT": {}})", outside.data());

  outside.AddNestedArray("key").Append(99).Append(1);
  EXPECT_EQ(outside,
            R"({"some_value": {"inside": 123}, "MT": {}, "key": [99, 1]})"sv);
}

TEST(JsonObject, NestedArrayOverflowWhenNesting) {
  JsonBuffer<5> buffer;
  JsonArray& array = buffer.StartArray();
  array.Append(123);
  ASSERT_EQ(array, "[123]"sv);

  NestedJsonArray nested_array = array.AppendNestedArray();
  EXPECT_EQ(Status::ResourceExhausted(), array.status());
  nested_array.Append(1);
  EXPECT_EQ(array, "[123]"sv);
}

TEST(JsonObject, NestedArrayOverflowAppend) {
  JsonBuffer<5> buffer;
  JsonArray& array = buffer.StartArray();
  NestedJsonArray nested_array = array.AppendNestedArray();

  EXPECT_EQ(OkStatus(), array.status());
  nested_array.Append(10);
  EXPECT_EQ(Status::ResourceExhausted(), array.status());
  EXPECT_EQ(array, "[[]]"sv);
}

TEST(JsonObject, NestedArrayOverflowSecondAppend) {
  JsonBuffer<7> buffer;
  JsonArray& array = buffer.StartArray();
  NestedJsonArray nested_array = array.AppendNestedArray();

  EXPECT_EQ(OkStatus(), array.status());
  nested_array.Append(1);
  EXPECT_EQ(array, "[[1]]"sv);
  EXPECT_EQ(OkStatus(), array.status());

  nested_array.Append(2);
  EXPECT_EQ(array, "[[1]]"sv);
  EXPECT_EQ(Status::ResourceExhausted(), array.status());
}

TEST(JsonObject, NestedObjectOverflowWhenNesting) {
  JsonBuffer<5> buffer;
  JsonArray& array = buffer.StartArray();
  array.Append(123);
  ASSERT_EQ(array, "[123]"sv);

  std::ignore = array.AppendNestedObject();
  EXPECT_EQ(Status::ResourceExhausted(), array.status());
  EXPECT_EQ(array, "[123]"sv);
}

TEST(JsonObject, NestedObjectOverflowAppend) {
  JsonBuffer<5> buffer;
  JsonArray& array = buffer.StartArray();
  NestedJsonObject nested_object = array.AppendNestedObject();

  EXPECT_EQ(OkStatus(), array.status());
  nested_object.Add("k", 10);
  EXPECT_EQ(Status::ResourceExhausted(), array.status());
  EXPECT_EQ(array, "[{}]"sv);
}

TEST(JsonObject, NestedObjectOverflowSecondAppend) {
  JsonBuffer<14> buffer;
  JsonArray& array = buffer.StartArray();
  NestedJsonObject nested_object = array.AppendNestedObject();

  EXPECT_EQ(OkStatus(), array.status());
  nested_object.Add("k", 1);
  EXPECT_EQ(array, R"([{"k": 1}])"sv);
  EXPECT_EQ(OkStatus(), array.status());

  nested_object.Add("K", 2);
  EXPECT_EQ(array, R"([{"k": 1}])"sv);
  EXPECT_EQ(Status::ResourceExhausted(), array.status());
}

TEST_F(JsonOverflowTest, ObjectNestedJsonOverflow) {
  JsonBuffer<32> inside_buffer;
  JsonObject& inside = inside_buffer.StartObject();

  JsonBuilder outside_builder(buffer_, 20);
  MarkBufferEnd(outside_builder);
  JsonObject& outside = outside_builder.StartObject();

  ASSERT_EQ(OkStatus(), inside.Add("k", 78).status());
  ASSERT_EQ(9u, inside.size());  // 9 bytes, will fit

  EXPECT_EQ(OkStatus(), outside.Add("data", inside).status());
  EXPECT_STREQ(R"({"data": {"k": 78}})", outside.data());  // 20 bytes total

  inside.clear();
  ASSERT_EQ(OkStatus(), inside.Add("k", 789).status());
  ASSERT_EQ(10u, inside.size());  // 10 bytes, won't fit

  outside.clear();
  EXPECT_EQ(Status::ResourceExhausted(), outside.Add("data", inside).status());
  EXPECT_EQ(Status::ResourceExhausted(), outside.last_status());
  EXPECT_EQ(Status::ResourceExhausted(), outside.status());
  EXPECT_STREQ(R"({})", outside.data());

  inside.clear();
  EXPECT_EQ(OkStatus(), outside.Add("data", inside).last_status());
  EXPECT_EQ(OkStatus(), outside.last_status());
  EXPECT_EQ(Status::ResourceExhausted(), outside.status());
}

TEST(JsonValue, BasicValues) {
  JsonBuffer<13> json;
  EXPECT_EQ(OkStatus(), json.SetValue(-15));
  EXPECT_STREQ("-15", json.data());
  EXPECT_EQ(3u, json.size());

  EXPECT_EQ(OkStatus(), json.SetValue(0));
  EXPECT_STREQ("0", json.data());
  EXPECT_EQ(1u, json.size());

  EXPECT_EQ(OkStatus(), json.SetValue(static_cast<char>(35)));
  EXPECT_STREQ("35", json.data());
  EXPECT_EQ(2u, json.size());

  EXPECT_EQ(OkStatus(), json.SetValue(nullptr));
  EXPECT_STREQ("null", json.data());
  EXPECT_EQ(4u, json.size());

  EXPECT_EQ(OkStatus(), json.SetValue(static_cast<const char*>(nullptr)));
  EXPECT_STREQ("null", json.data());
  EXPECT_EQ(4u, json.size());

  EXPECT_EQ(OkStatus(), json.SetValue(""));
  EXPECT_STREQ(R"("")", json.data());
  EXPECT_EQ(2u, json.size());

  EXPECT_EQ(OkStatus(), json.SetValue("Hey\n!"));
  EXPECT_STREQ(R"("Hey\n!")", json.data());
  EXPECT_EQ(8u, json.size());

  JsonValue& json_value = json.StartValue();
  EXPECT_STREQ("null", json_value.data());

  char str[] = R"(Qu"o"tes)";
  EXPECT_EQ(OkStatus(), json_value.Set(str));
  EXPECT_STREQ(R"("Qu\"o\"tes")", json_value.data());
  EXPECT_EQ(12u, json_value.size());

  EXPECT_EQ(OkStatus(), json_value.Set(true));
  EXPECT_STREQ("true", json_value.data());
  EXPECT_EQ(4u, json_value.size());

  bool false_value = false;
  EXPECT_EQ(OkStatus(), json.SetValue(false_value));
  EXPECT_STREQ("false", json.data());
  EXPECT_EQ(5u, json.size());

  EXPECT_EQ(OkStatus(), json_value.Set(static_cast<double>(1)));
  EXPECT_EQ(json_value, "1"sv);
  EXPECT_EQ(OkStatus(), json_value.Set(-1.0f));
  EXPECT_EQ(json_value, "-1"sv);
}

TEST_F(JsonOverflowTest, ValueOverflowUnquoted) {
  JsonBuilder json(buffer_, 5);
  MarkBufferEnd(json);
  ASSERT_EQ(4u, json.max_size());

  EXPECT_EQ(Status::ResourceExhausted(), json.SetValue(12345));
  EXPECT_STREQ("null", json.data());
  EXPECT_EQ(4u, json.size());

  EXPECT_EQ(OkStatus(), json.SetValue(1234));
  EXPECT_STREQ("1234", json.data());
  EXPECT_EQ(4u, json.size());

  EXPECT_EQ(Status::ResourceExhausted(), json.SetValue(false));
  EXPECT_STREQ("null", json.data());
  EXPECT_EQ(4u, json.size());

  EXPECT_EQ(OkStatus(), json.SetValue(true));
  EXPECT_STREQ("true", json.data());
  EXPECT_EQ(4u, json.size());
}

TEST_F(JsonOverflowTest, ValueOverflowQuoted) {
  JsonBuilder json(buffer_, 8);
  MarkBufferEnd(json);
  ASSERT_EQ(7u, json.max_size());

  EXPECT_EQ(OkStatus(), json.SetValue("34567"));
  EXPECT_STREQ(R"("34567")", json.data());
  EXPECT_EQ(7u, json.size());

  EXPECT_EQ(Status::ResourceExhausted(), json.SetValue("345678"));
  EXPECT_STREQ("null", json.data());
  EXPECT_EQ(4u, json.size());

  EXPECT_EQ(OkStatus(), json.SetValue("567\n"));
  EXPECT_STREQ(R"("567\n")", json.data());
  EXPECT_EQ(7u, json.size());

  EXPECT_EQ(Status::ResourceExhausted(), json.SetValue("5678\n"));
  EXPECT_STREQ("null", json.data());
  EXPECT_EQ(4u, json.size());

  EXPECT_EQ(Status::ResourceExhausted(), json.SetValue("\x05"));
  EXPECT_STREQ("null", json.data());
  EXPECT_EQ(4u, json.size());

  JsonBuffer<9> bigger_json;
  EXPECT_EQ(OkStatus(), bigger_json.SetValue("\x05"));
  EXPECT_STREQ(R"("\u0005")", bigger_json.data());
  EXPECT_EQ(8u, bigger_json.size());
}

TEST(JsonValue, NestedJson) {
  JsonBuffer<11> json;
  JsonBuffer<12> object_buffer;
  JsonObject& object = object_buffer.StartObject();

  ASSERT_EQ(OkStatus(), object.Add("3", 7890).status());
  ASSERT_STREQ(R"({"3": 7890})", object.data());
  ASSERT_EQ(json.max_size(), object.size());

  EXPECT_EQ(OkStatus(), json.SetValue(object));
  EXPECT_STREQ(R"({"3": 7890})", json.data());
  EXPECT_EQ(11u, json.size());

  object.clear();
  ASSERT_EQ(OkStatus(), object.Add("3", 78901).status());
  ASSERT_STREQ(R"({"3": 78901})", object.data());
  ASSERT_EQ(object.max_size(), object.size());
  ASSERT_GT(object.size(), json.size());

  EXPECT_EQ(Status::ResourceExhausted(), json.SetValue(object));
  EXPECT_STREQ("null", json.data());
  EXPECT_EQ(4u, json.size());

  JsonBuffer<12> value;
  const char* something = nullptr;
  EXPECT_EQ(OkStatus(), value.SetValue(something));

  EXPECT_EQ(OkStatus(), json.SetValue(value));
  EXPECT_STREQ("null", json.data());
  EXPECT_EQ(4u, json.size());
}

TEST(JsonValue, SetFromOtherJsonValue) {
  JsonBuffer<32> first = JsonBuffer<32>::Value("$$02$ok$$C");
  constexpr const char kExpected[] = R"("$$02$ok$$C")";
  ASSERT_STREQ(kExpected, first.data());
  ASSERT_EQ(sizeof(kExpected) - 1, first.size());

  JsonBuffer<24> second;
  EXPECT_EQ(OkStatus(), second.SetValue(first));
  EXPECT_STREQ(kExpected, second.data());
  EXPECT_EQ(sizeof(kExpected) - 1, second.size());
}

TEST(JsonValue, ToJsonValue) {
  static constexpr auto value = JsonBuffer<4>::Value(1234);

  EXPECT_STREQ("1234", value.data());
  EXPECT_STREQ("\"1234\"", JsonBuffer<6>::Value("1234").data());
  EXPECT_STREQ("null", JsonBuffer<4>::Value(nullptr).data());
  EXPECT_STREQ("false", JsonBuffer<5>::Value(false).data());

#if PW_NC_TEST(ValueDoesNotFit)
  PW_NC_EXPECT("PW_ASSERT\(json.SetValue\(initial_value\).ok\(\)\)");
  [[maybe_unused]] static constexpr auto fail = JsonBuffer<4>::Value(12345);
#endif  // PW_NC_TEST
}

static_assert([] {
  JsonBuffer<32> buffer;
  buffer.StartObject().Add("hello", "world").Add("ptr", nullptr);
  return buffer;
}() == std::string_view(R"({"hello": "world", "ptr": null})"));

TEST(JsonArray, BasicUse) {
  JsonBuffer<48> list_buffer;
  JsonArray& list = list_buffer.StartArray();
  ASSERT_EQ(OkStatus(), list.Append(nullptr).last_status());
  ASSERT_EQ(OkStatus(), list.Append("what").status());

  JsonBuffer<96> big_list_buffer;
  JsonArray& big_list = big_list_buffer.StartArray();
  EXPECT_EQ(OkStatus(), big_list.Append(list).status());
  EXPECT_EQ(OkStatus(), big_list.Append(123).status());

  JsonBuffer<48> object_buffer;
  JsonObject& object = object_buffer.StartObject();
  ASSERT_EQ(OkStatus(), object.Add("foo", "bar").status());
  ASSERT_EQ(OkStatus(), object.Add("bar", list).status());
  EXPECT_EQ(OkStatus(), big_list.Append(object).status());

  EXPECT_EQ(OkStatus(), big_list.Append('\0').status());

  std::array<bool, 2> bools{true, false};
  EXPECT_EQ(OkStatus(), big_list.Extend(bools).status());

  const char kExpected[] =
      R"([[null, "what"], 123, {"foo": "bar", "bar": [null, "what"]}, )"
      R"(0, true, false])";
  EXPECT_STREQ(kExpected, big_list.data());
  EXPECT_EQ(sizeof(kExpected) - 1, big_list.size());
}

TEST(JsonArray, FromArray) {
  JsonBuffer<31> array_buffer;
  JsonArray& array = array_buffer.StartArray();
  EXPECT_EQ(OkStatus(), array.Extend({1, 2, 3, 4, 5}).status());
  EXPECT_STREQ("[1, 2, 3, 4, 5]", array.data());
  EXPECT_EQ(15u, array.size());

  EXPECT_EQ(OkStatus(), array.Extend({6, 7, 8, 9, 0}).status());
  EXPECT_STREQ("[1, 2, 3, 4, 5, 6, 7, 8, 9, 0]", array.data());
  EXPECT_EQ(30u, array.size());
}

TEST_F(JsonOverflowTest, FromArrayOverflow) {
  JsonBuilder array_buffer(buffer_, 31);
  MarkBufferEnd(array_buffer);
  JsonArray& array = array_buffer.StartArray();

  EXPECT_EQ(OkStatus(), array.Extend({1, 2, 3, 4, 5}).status());
  EXPECT_STREQ("[1, 2, 3, 4, 5]", array.data());
  EXPECT_EQ(15u, array.size());

  EXPECT_EQ(Status::ResourceExhausted(),
            array.Extend({6, 7, 8, 9, 0, 1, 2, 3}).status());
  EXPECT_STREQ("[1, 2, 3, 4, 5]", array.data());
  EXPECT_EQ(15u, array.size());

  EXPECT_EQ(Status::ResourceExhausted(),
            array.Extend({6, 7, 8}).Extend({9, 0}).status());
  EXPECT_STREQ("[1, 2, 3, 4, 5, 6, 7, 8, 9, 0]", array.data());
  EXPECT_EQ(30u, array.size());

  EXPECT_EQ(Status::ResourceExhausted(), array.Extend({5}).status());
  EXPECT_STREQ("[1, 2, 3, 4, 5, 6, 7, 8, 9, 0]", array.data());
  EXPECT_EQ(30u, array.size());
}

TEST(JsonArray, AppendIndividualExtendContainer) {
  JsonBuffer<64> array_buffer;
  JsonArray& array = array_buffer.StartArray();
  constexpr int kInts[] = {1, 2, 3};
#if PW_NC_TEST(CannotAppendArrays)
  PW_NC_EXPECT("JSON values may only be numbers, strings, JSON");
  array.Append(kInts);
#endif  // PW_NC_TEST

  ASSERT_EQ(OkStatus(), array.Extend(kInts).status());
  EXPECT_STREQ("[1, 2, 3]", array.data());
}

TEST(JsonArray, NestingArray) {
  JsonBuffer<64> array_buffer;
  JsonArray& array = array_buffer.StartArray();
  std::ignore = array.AppendNestedArray();

  EXPECT_STREQ(array.data(), "[[]]");

  NestedJsonArray nested = array.AppendNestedArray();
  EXPECT_EQ(OkStatus(), array.last_status());
  EXPECT_STREQ(array.data(), "[[], []]");

  nested.Append(123);
  EXPECT_EQ(array.size(), sizeof("[[], [123]]") - 1);
  EXPECT_STREQ(array.data(), "[[], [123]]");

  nested.Append("");
  EXPECT_STREQ(array.data(), "[[], [123, \"\"]]");
}

TEST(JsonArray, NestingObject) {
  JsonBuffer<64> array_buffer;
  JsonArray& array = array_buffer.StartArray();
  NestedJsonObject object = array.AppendNestedObject();

  EXPECT_STREQ(array.data(), "[{}]");

  object.Add("key", 123);
  EXPECT_EQ(array, R"([{"key": 123}])"sv);

  object.Add("k", true);
  EXPECT_EQ(array, R"([{"key": 123, "k": true}])"sv);

  array.AppendNestedArray().Append("done").Append("!");
  EXPECT_EQ(array, R"([{"key": 123, "k": true}, ["done", "!"]])"sv);
}

TEST(JsonBuilder, DeepNesting) {
  JsonBuffer<64> buffer;
  JsonArray& arr1 = buffer.StartArray();
  std::ignore = arr1.AppendNestedObject();

  EXPECT_EQ(buffer, "[{}]"sv);

  auto arr2 = arr1.AppendNestedObject().Add("a", 1).AddNestedArray("b");
  arr2.Append(0).Append(1).AppendNestedObject().Add("yes", "no");
  arr2.Append(2);

  EXPECT_EQ(buffer, R"([{}, {"a": 1, "b": [0, 1, {"yes": "no"}, 2]}])"sv);

  arr1.Append(true);
  EXPECT_EQ(buffer, R"([{}, {"a": 1, "b": [0, 1, {"yes": "no"}, 2]}, true])"sv);
}

TEST(JsonBuilder, ConvertBetween) {
  JsonBuffer<64> buffer;
  EXPECT_STREQ("null", buffer.data());
  EXPECT_TRUE(buffer.IsValue());
  EXPECT_FALSE(buffer.IsObject());
  EXPECT_FALSE(buffer.IsArray());

  JsonObject& object = buffer.StartObject();
  EXPECT_STREQ("{}", buffer.data());
  EXPECT_FALSE(object.IsValue());
  EXPECT_FALSE(object.IsArray());
  EXPECT_TRUE(object.IsObject());
  object.Add("123", true);
  EXPECT_STREQ(R"({"123": true})", buffer.data());

  JsonArray& array = buffer.StartArray();

  EXPECT_FALSE(object.IsObject()) << "No longer an object";
  EXPECT_TRUE(object.ok()) << "Still OK, just not an object";
  EXPECT_FALSE(array.IsValue());
  EXPECT_TRUE(array.IsArray());
  EXPECT_FALSE(array.IsObject());

  EXPECT_STREQ("[]", buffer.data());
  EXPECT_EQ(OkStatus(), array.Extend({1, 2, 3}).status());
  EXPECT_STREQ("[1, 2, 3]", buffer.data());

  EXPECT_EQ(OkStatus(), array.Append(false).Append(-1).status());
  EXPECT_STREQ("[1, 2, 3, false, -1]", buffer.data());

  object.clear();
  EXPECT_EQ(OkStatus(), object.Add("yes", nullptr).status());
  EXPECT_STREQ(R"({"yes": null})", buffer.data());
  EXPECT_EQ(OkStatus(), buffer.status());
}

static_assert([] {
  JsonBuffer<64> buffer;
  auto& object = buffer.StartObject();
  auto nested_array = object.AddNestedArray("array");
  nested_array.Append(1);
  object.Add("key", "value");
  if (buffer != R"({"array": [1], "key": "value"})"sv) {
    return false;
  }

#if PW_NC_TEST(NestedJsonAttemptsToDetectWrongType)
  PW_NC_EXPECT("PW_ASSERT\(.*// Nested structure must match the expected type");
  nested_array.Append(2);
#elif PW_NC_TEST(NestedJsonDetectsClearedJson)
  PW_NC_EXPECT("PW_ASSERT\(.*// JSON must not have been cleared since nesting");
  object.clear();
  nested_array.Append(2);
#endif  // PW_NC_TEST
  return true;
}());

static_assert([] {
  JsonBuffer<64> buffer;
  auto& array = buffer.StartArray();

  NestedJsonArray nested = array.AppendNestedArray();
  for (int i = 1; i < 16; ++i) {
    nested = nested.AppendNestedArray();
  }
  // 17 arrays total (1 outer array, 16 levels of nesting inside it).
  //              1234567890123456712345678901234567
  if (array != R"([[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]])"sv) {
    return JsonBuffer<64>{};
  }
  nested.Append("-_-");

#if PW_NC_TEST(NestingLimit)
  PW_NC_EXPECT(
      "PW_ASSERT\(.*// Arrays or objects may be nested at most 17 times");
  std::ignore = nested.AppendNestedArray();
#endif  // PW_NC_TEST
  return buffer;
}() == R"([[[[[[[[[[[[[[[[["-_-"]]]]]]]]]]]]]]]]])"sv);

TEST(JsonBuffer, SetClear) {
  JsonBuffer<4> buffer;
  EXPECT_EQ(buffer, "null"sv);
  ASSERT_EQ(OkStatus(), buffer.SetValue(""));
  EXPECT_TRUE(buffer.ok());
  EXPECT_EQ(buffer, "\"\""sv);

  ASSERT_EQ(Status::ResourceExhausted(), buffer.SetValue("234"));
  EXPECT_FALSE(buffer.ok());
  EXPECT_EQ(buffer, "null"sv);

  buffer.clear();
  EXPECT_TRUE(buffer.ok());
  EXPECT_EQ(buffer, "null"sv);
}

TEST(JsonBuffer, Copy) {
  JsonBuffer<64> foo;
  ASSERT_EQ(OkStatus(), foo.SetValue("yes"));

  JsonBuffer<48> bar;
  auto& object = bar.StartObject().Add("no", true);
  EXPECT_EQ(object, R"({"no": true})"sv);
  EXPECT_EQ(OkStatus(), bar.StartArray().Append(1).Append(2).status());

  foo = bar;
  EXPECT_STREQ("[1, 2]", foo.data());
  EXPECT_EQ(6u, foo.size());

  JsonBuffer<128> baz(foo);
  EXPECT_STREQ(foo.data(), baz.data());
}

// Tests character escaping using a table generated with the following Python:
//
// import json
// print(', '.join('R"_({})_"'.format(json.dumps(chr(i))) for i in range(128)))
TEST(JsonBuilder, TestEscape) {
  static constexpr std::array<const char*, 128> kEscapedCharacters = {
      R"_("\u0000")_", R"_("\u0001")_", R"_("\u0002")_", R"_("\u0003")_",
      R"_("\u0004")_", R"_("\u0005")_", R"_("\u0006")_", R"_("\u0007")_",
      R"_("\b")_",     R"_("\t")_",     R"_("\n")_",     R"_("\u000b")_",
      R"_("\f")_",     R"_("\r")_",     R"_("\u000e")_", R"_("\u000f")_",
      R"_("\u0010")_", R"_("\u0011")_", R"_("\u0012")_", R"_("\u0013")_",
      R"_("\u0014")_", R"_("\u0015")_", R"_("\u0016")_", R"_("\u0017")_",
      R"_("\u0018")_", R"_("\u0019")_", R"_("\u001a")_", R"_("\u001b")_",
      R"_("\u001c")_", R"_("\u001d")_", R"_("\u001e")_", R"_("\u001f")_",
      R"_(" ")_",      R"_("!")_",      R"_("\"")_",     R"_("#")_",
      R"_("$")_",      R"_("%")_",      R"_("&")_",      R"_("'")_",
      R"_("(")_",      R"_(")")_",      R"_("*")_",      R"_("+")_",
      R"_(",")_",      R"_("-")_",      R"_(".")_",      R"_("/")_",
      R"_("0")_",      R"_("1")_",      R"_("2")_",      R"_("3")_",
      R"_("4")_",      R"_("5")_",      R"_("6")_",      R"_("7")_",
      R"_("8")_",      R"_("9")_",      R"_(":")_",      R"_(";")_",
      R"_("<")_",      R"_("=")_",      R"_(">")_",      R"_("?")_",
      R"_("@")_",      R"_("A")_",      R"_("B")_",      R"_("C")_",
      R"_("D")_",      R"_("E")_",      R"_("F")_",      R"_("G")_",
      R"_("H")_",      R"_("I")_",      R"_("J")_",      R"_("K")_",
      R"_("L")_",      R"_("M")_",      R"_("N")_",      R"_("O")_",
      R"_("P")_",      R"_("Q")_",      R"_("R")_",      R"_("S")_",
      R"_("T")_",      R"_("U")_",      R"_("V")_",      R"_("W")_",
      R"_("X")_",      R"_("Y")_",      R"_("Z")_",      R"_("[")_",
      R"_("\\")_",     R"_("]")_",      R"_("^")_",      R"_("_")_",
      R"_("`")_",      R"_("a")_",      R"_("b")_",      R"_("c")_",
      R"_("d")_",      R"_("e")_",      R"_("f")_",      R"_("g")_",
      R"_("h")_",      R"_("i")_",      R"_("j")_",      R"_("k")_",
      R"_("l")_",      R"_("m")_",      R"_("n")_",      R"_("o")_",
      R"_("p")_",      R"_("q")_",      R"_("r")_",      R"_("s")_",
      R"_("t")_",      R"_("u")_",      R"_("v")_",      R"_("w")_",
      R"_("x")_",      R"_("y")_",      R"_("z")_",      R"_("{")_",
      R"_("|")_",      R"_("}")_",      R"_("~")_",      R"_("\u007f")_"};

  JsonBuffer<9> buffer;

  for (size_t i = 0; i < kEscapedCharacters.size(); ++i) {
    const char character = static_cast<char>(i);
    ASSERT_EQ(OkStatus(), buffer.SetValue(std::string_view(&character, 1)));
    ASSERT_STREQ(kEscapedCharacters[i], buffer.data());
  }
}

class JsonObjectTest : public ::testing::Test {
 protected:
  static constexpr size_t kMaxSize = 127;
  static constexpr size_t kBufferSize = kMaxSize + 1;

  JsonObjectTest() : object_(json_buffer_.StartObject()) {}

  JsonBuffer<kMaxSize> json_buffer_;
  JsonObject& object_;
};

TEST_F(JsonObjectTest, TestSingleStringValue) {
  EXPECT_EQ(OkStatus(), object_.Add("key", "value").status());
  EXPECT_STREQ("{\"key\": \"value\"}", object_.data());
}

TEST_F(JsonObjectTest, TestEscapedQuoteString) {
  const char* buf = "{\"key\": \"\\\"value\\\"\"}";
  EXPECT_STREQ(buf, object_.Add("key", "\"value\"").data());
}

TEST_F(JsonObjectTest, TestEscapedSlashString) {
  const char* buf = "{\"key\": \"\\\\\"}";
  EXPECT_STREQ(buf, object_.Add("key", "\\").data());
}

TEST_F(JsonObjectTest, TestEscapedCharactersString) {
  const char* buf = "{\"key\": \"\\r\\n\\t\"}";
  EXPECT_STREQ(buf, object_.Add("key", "\r\n\t").data());
}

TEST_F(JsonObjectTest, TestEscapedControlCharacterString) {
  EXPECT_STREQ("{\"key\": \"\\u001f\"}", object_.Add("key", "\x1F").data());
  object_.clear();
  EXPECT_STREQ("{\"key\": \"\\u0080\"}", object_.Add("key", "\x80").data());
}

TEST_F(JsonObjectTest, TestNullptrString) {
  EXPECT_STREQ("{\"key\": null}",
               object_.Add("key", static_cast<const char*>(nullptr)).data());
}

TEST_F(JsonObjectTest, TestCharValue) {
  EXPECT_STREQ("{\"key\": 88}",
               object_.Add("key", static_cast<unsigned char>('X')).data());
  object_.clear();
  EXPECT_STREQ("{\"key\": 88}", object_.Add("key", 'X').data());
}

TEST_F(JsonObjectTest, TestShortValue) {
  EXPECT_STREQ("{\"key\": 88}",
               object_.Add("key", static_cast<unsigned short>(88)).data());
  object_.clear();
  EXPECT_STREQ("{\"key\": -88}",
               object_.Add("key", static_cast<short>(-88)).data());
}

TEST_F(JsonObjectTest, TestIntValue) {
  EXPECT_STREQ("{\"key\": 88}",
               object_.Add("key", static_cast<unsigned int>(88)).data());
  object_.clear();
  EXPECT_STREQ("{\"key\": -88}", object_.Add("key", -88).data());
}

TEST_F(JsonObjectTest, TestLongValue) {
  EXPECT_STREQ("{\"key\": 88}", object_.Add("key", 88UL).data());
  object_.clear();
  EXPECT_STREQ("{\"key\": -88}", object_.Add("key", -88L).data());
}

TEST_F(JsonObjectTest, TestMultipleValues) {
  char buf[16] = "nonconst";
  EXPECT_STREQ("{\"one\": \"nonconst\", \"two\": null, \"three\": -3}",
               object_.Add("one", buf)
                   .Add("two", static_cast<char*>(nullptr))
                   .Add("three", -3)
                   .data());
}

TEST_F(JsonObjectTest, TestOverflow) {
  // Create a buffer that is just large enough to overflow with "key".
  std::array<char, kBufferSize + 1 /* NUL */ - (sizeof("{\"key\": \"\"}") - 1)>
      buf;
  std::memset(buf.data(), 'z', sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';

  // Make sure the overflow happens at exactly the right character.
  EXPECT_EQ(Status::ResourceExhausted(),
            object_.Add("key", buf.data()).status());
  EXPECT_EQ(Status::ResourceExhausted(), object_.status());

  object_.clear();
  EXPECT_EQ(OkStatus(), object_.Add("ke", buf.data()).status());
  EXPECT_EQ(OkStatus(), object_.status());

  // Ensure the internal buffer is NUL-terminated still, even on overflow.
  EXPECT_EQ(object_.data()[object_.size()], '\0');
}

TEST(JsonBuilder, KeyEscapingOverflowRecovery) {
  pw::JsonBuffer<10> json;
  pw::JsonObject& obj = json.StartObject();

  EXPECT_STREQ(json.data(), "{}");
  EXPECT_EQ(json.size(), 2u);

  // Try to add key that overflows due to escaping.
  obj.Add("\x01", "value");
  EXPECT_EQ(json.status(), pw::Status::ResourceExhausted());

  EXPECT_STREQ(json.data(), "{}");
  EXPECT_EQ(json.size(), 2u);
  EXPECT_EQ(json.data()[json.size()], '\0');
  EXPECT_EQ(std::string_view(json), "{}"sv);
}

}  // namespace
}  // namespace pw
