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
#pragma once

/// @file pw_json/builder.h
///
/// The pw_json module provides utilities for interacting with <a
/// href="http://www.json.org">JSON</a>. JSON is a structured data format that
/// supports strings, integers, floating point numbers, booleans, null, arrays,
/// and objects (key-value pairs).
///
/// `pw::JsonBuilder` is a simple, efficient utility for serializing JSON to a
/// fixed-sized buffer. It works directly with the JSON wire format. It does not
/// support manipulation of previously serialized data.
///
/// All `JsonBuilder` functions are `constexpr`, so may be used in `constexpr`
/// and `constinit` statements.

#include <cstddef>
#include <string_view>
#include <type_traits>

#include "pw_assert/assert.h"
#include "pw_json/internal/nesting.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_string/type_to_string.h"

namespace pw {

/// @submodule{pw_json,builder_api}

/// A `JsonArray` nested inside an object or array. Only provides functions for
/// appending values to the nested array.
///
/// A `NestedJsonArray` is immediately invalidated if the enclosing JSON is
/// updated. Attempting to append to the nested array is an error.
class [[nodiscard]] NestedJsonArray {
 public:
  NestedJsonArray(const NestedJsonArray&) = delete;
  NestedJsonArray& operator=(const NestedJsonArray&) = delete;

  constexpr NestedJsonArray(NestedJsonArray&&) = default;
  constexpr NestedJsonArray& operator=(NestedJsonArray&&) = default;

  /// Appends to the nested array.
  template <typename T>
  constexpr NestedJsonArray& Append(const T& value);

  /// Appends a new nested array to this nested array.
  constexpr NestedJsonArray AppendNestedArray();

  /// Appends a new nested object to this nested array.
  constexpr NestedJsonObject AppendNestedObject();

 private:
  friend class JsonArray;
  friend class JsonObject;
  friend class NestedJsonObject;

  constexpr NestedJsonArray(json_impl::NestedJson&& nested)
      : json_(std::move(nested)) {}

  json_impl::NestedJson json_;
};

/// A `JsonObject` nested inside an array or object. Only provides functions for
/// adding key-value pairs to the nested object.
///
/// A `NestedJsonObject` is immediately invalidated if the enclosing JSON is
/// updated. Attempting to add to the nested object fails an assertion.
class [[nodiscard]] NestedJsonObject {
 public:
  NestedJsonObject(const NestedJsonObject&) = delete;
  NestedJsonObject& operator=(const NestedJsonObject&) = delete;

  constexpr NestedJsonObject(NestedJsonObject&&) = default;
  constexpr NestedJsonObject& operator=(NestedJsonObject&&) = default;

  /// Adds a key-value pair to the nested object.
  template <typename T>
  constexpr NestedJsonObject& Add(std::string_view key, const T& value);

  /// Adds a nested array to the nested object.
  constexpr NestedJsonArray AddNestedArray(std::string_view key);

  /// Adds a nested object to the nested object.
  constexpr NestedJsonObject AddNestedObject(std::string_view key);

 private:
  friend class JsonArray;
  friend class JsonObject;
  friend class NestedJsonArray;

  constexpr NestedJsonObject(json_impl::NestedJson&& nested)
      : json_(std::move(nested)) {}

  json_impl::NestedJson json_;
};

/// Stores a simple JSON value: a string, integer, float, boolean, or null.
/// Provides a `Set()` function as well as the common functions for accessing
/// the serialized data (see documentation for `JsonBuilder`).
class JsonValue {
 public:
  constexpr JsonValue(const JsonValue&) = delete;
  constexpr JsonValue& operator=(const JsonValue&) = delete;

  // Functions common to all JSON types.
  [[nodiscard]] constexpr bool IsValue() const;
  [[nodiscard]] constexpr bool IsArray() const;
  [[nodiscard]] constexpr bool IsObject() const;

  constexpr operator std::string_view() const;
  constexpr const char* data() const;
  constexpr size_t size() const;
  constexpr size_t max_size() const;

  [[nodiscard]] constexpr bool ok() const;
  constexpr Status status() const;
  constexpr Status last_status() const;
  constexpr void clear();
  constexpr void clear_status();

  /// Sets the JSON value to a boolean, number, string, or `null`. Sets and
  /// returns the status. If a `Set` call fails, the value is set to `null`.
  ///
  /// It is an error to call `Set()` on a `JsonValue` if `StartArray` or
  /// `StartObject` was called on the `JsonBuilder`. Setting the `JsonValue` to
  /// a JSON object or array is also an error.
  ///
  /// @returns
  /// * @OK: The value serialized successfully.
  /// * @RESOURCE_EXHAUSTED: There is insufficient buffer space to serialize.
  template <typename T>
  constexpr Status Set(const T& value);

 private:
  friend class JsonBuilder;

  constexpr JsonValue() = default;
};

/// Stores a JSON array: a sequence of values. Provides functions for adding
/// items to the array, as well as the common functions for accessing the
/// serialized data (see documentation for `JsonBuilder`).
class JsonArray {
 public:
  constexpr JsonArray(const JsonArray&) = delete;
  constexpr JsonArray& operator=(const JsonArray&) = delete;

  // Functions common to all JSON types. See documentation for `JsonBuilder`.
  [[nodiscard]] constexpr bool IsValue() const;
  [[nodiscard]] constexpr bool IsArray() const;
  [[nodiscard]] constexpr bool IsObject() const;

  constexpr operator std::string_view() const;
  constexpr const char* data() const;
  constexpr size_t size() const;
  constexpr size_t max_size() const;

  [[nodiscard]] constexpr bool ok() const;
  constexpr Status status() const;
  constexpr Status last_status() const;
  constexpr void clear();
  constexpr void clear_status();

  /// Adds a value to the JSON array. Updates the status.
  ///
  /// It is an error to call `Append()` if the underlying `JsonBuilder` is no
  /// longer an array.
  template <typename T>
  constexpr JsonArray& Append(const T& value);

  /// Appends a nested array to this array.
  constexpr NestedJsonArray AppendNestedArray();

  /// Appends a nested object to this array.
  constexpr NestedJsonObject AppendNestedObject();

  /// Appends all elements from an iterable container. If there is an error,
  /// changes are reverted.
  template <typename Iterable>
  constexpr JsonArray& Extend(const Iterable& iterable);

  /// Appends all elements from an iterable container. If there is an error,
  /// changes are reverted.
  template <typename T, size_t kSize>
  constexpr JsonArray& Extend(const T (&iterable)[kSize]);

 private:
  friend class JsonBuilder;

  constexpr JsonArray() = default;
};

/// Stores a JSON object: a sequence of key-value pairs. Provides functions
/// for adding entries to the object, as well as the common functions for
/// accessing the serialized data (see documentation for `JsonBuilder`).
class JsonObject {
 public:
  constexpr JsonObject(const JsonObject&) = delete;
  constexpr JsonObject& operator=(const JsonObject&) = delete;

  // Functions common to all JSON types. See documentation for `JsonBuilder`.
  [[nodiscard]] constexpr bool IsValue() const;
  [[nodiscard]] constexpr bool IsArray() const;
  [[nodiscard]] constexpr bool IsObject() const;

  constexpr operator std::string_view() const;
  constexpr const char* data() const;
  constexpr size_t size() const;
  constexpr size_t max_size() const;

  [[nodiscard]] constexpr bool ok() const;
  constexpr Status status() const;
  constexpr Status last_status() const;
  constexpr void clear();
  constexpr void clear_status();

  /// Adds a key-value pair to the JSON object. Updates the status.
  ///
  /// It is an error to call `Add()` if the underlying `JsonBuilder` is no
  /// longer an object.
  ///
  /// @returns
  /// * @OK: The value was appended successfully.
  /// * @RESOURCE_EXHAUSTED: Insufficient buffer space to serialize.
  template <typename T>
  constexpr JsonObject& Add(std::string_view key, const T& value);

  template <typename T>
  constexpr JsonObject& Add(std::nullptr_t, const T& value) = delete;

  constexpr NestedJsonArray AddNestedArray(std::string_view key);

  constexpr NestedJsonObject AddNestedObject(std::string_view key);

 private:
  friend class JsonBuilder;

  constexpr JsonObject() = default;
};

/// `JsonBuilder` is used to create arbitrary JSON. Contains a JSON value, which
/// may be an object or array. Arrays and objects may contain other values,
/// objects, or arrays.
class JsonBuilder : private JsonValue, private JsonArray, private JsonObject {
 public:
  /// `JsonBuilder` requires at least 5 characters in its buffer.
  static constexpr size_t MinBufferSize() { return 5; }

  /// Initializes to the value `null`. `buffer.size()` must be at least 5.
  constexpr JsonBuilder(span<char> buffer)
      : JsonBuilder(buffer.data(), buffer.size()) {}

  /// Initializes to the value `null`. `buffer_size` must be at least 5.
  constexpr JsonBuilder(char* buffer, size_t buffer_size)
      : JsonBuilder(buffer, buffer_size, Uninitialized{}) {
    PW_ASSERT(buffer_size >= MinBufferSize());  // Must be at least 5 characters
    MakeNull();
  }

  /// True if the top-level JSON entity is a simple value (not array or object).
  [[nodiscard]] constexpr bool IsValue() const {
    return !IsObject() && !IsArray();
  }

  /// True if the top-level JSON entity is an array.
  [[nodiscard]] constexpr bool IsArray() const { return buffer_[0] == '['; }

  /// True if the top-level JSON entity is an object.
  [[nodiscard]] constexpr bool IsObject() const { return buffer_[0] == '{'; }

  /// `JsonBuilder` converts to `std::string_view`.
  constexpr operator std::string_view() const { return {data(), size()}; }

  /// Pointer to the serialized JSON, which is always a null-terminated string.
  constexpr const char* data() const { return buffer_; }

  /// The current size of the JSON string, excluding the null terminator.
  constexpr size_t size() const { return json_size_; }

  /// The maximum size of the JSON string, excluding the null terminator.
  constexpr size_t max_size() const { return max_size_; }

  /// True if @cpp_func{status} is `pw::OkStatus()`; no errors have occurred.
  [[nodiscard]] constexpr bool ok() const { return status().ok(); }

  /// Returns the `JsonBuilder`'s status, which reflects the first error that
  /// occurred while updating the JSON. After an update fails, the non-`OK`
  /// status remains until it is reset with `clear`, `clear_status`, or
  /// `SetValue`.
  ///
  /// @returns
  /// * @OK: All previous updates have succeeded.
  /// * @RESOURCE_EXHAUSTED: An update did not fit in the buffer.
  constexpr Status status() const { return static_cast<Status::Code>(status_); }

  /// Returns the status from the most recent change to the JSON. This is set
  /// with each JSON update and may be `pw::OkStatus()` while `status()` is not.
  constexpr Status last_status() const {
    return static_cast<Status::Code>(last_status_);
  }

  /// Sets the JSON `null` and clears the status.
  constexpr void clear() { JsonValueClear(); }

  /// Resets `status()` and `last_status()`.
  constexpr void clear_status() { set_statuses(OkStatus()); }

  /// Clears the JSON and sets it to a single JSON value (see `JsonValue::Set`).
  template <typename T>
  constexpr Status SetValue(const T& value);

  /// Sets the JSON to `null` and returns a `JsonValue` reference to this
  /// `JsonBuilder`.
  [[nodiscard]] constexpr JsonValue& StartValue() {
    JsonValueClear();
    return *this;
  }

  /// Clears the JSON and sets it to an empty array (`[]`). Returns a
  /// `JsonArray` reference to this `JsonBuilder`. For example:
  ///
  /// @code{.cpp}
  ///   builder.StartArray()
  ///       .Append("item1")
  ///       .Append(2)
  ///       .Extend({"3", "4", "5"});
  /// @endcode
  [[nodiscard]] constexpr JsonArray& StartArray() {
    JsonArrayClear();
    return *this;
  }

  /// Clears the JSON and sets it to an empty object (`{}`). Returns a
  /// `JsonObject` reference to this `JsonBuilder`. For example:
  ///
  /// @code{.cpp}
  ///   JsonBuffer<128> builder;
  ///   JsonObject& object = builder.StartObject()
  ///       .Add("key1", 1)
  ///       .Add("key2", "val2");
  ///   object.Add("another", "entry");
  /// @endcode
  [[nodiscard]] constexpr JsonObject& StartObject() {
    JsonObjectClear();
    return *this;
  }

 protected:
  enum class Uninitialized {};

  // Constructor that doesn't initialize the buffer.
  constexpr JsonBuilder(char* buffer, size_t buffer_size, Uninitialized)
      : buffer_(buffer),
        max_size_(buffer_size - 1),
        json_size_(0),
        status_(OkStatus().code()),
        last_status_(OkStatus().code()) {}

  // Sets the buffer to the null value.
  constexpr void MakeNull() {
    buffer_[0] = 'n';
    buffer_[1] = 'u';
    buffer_[2] = 'l';
    buffer_[3] = 'l';
    buffer_[4] = '\0';
    json_size_ = 4;
  }

  constexpr void set_json_size(size_t json_size) { json_size_ = json_size; }

  constexpr void set_statuses(Status status, Status last_status) {
    status_ = status.code();
    last_status_ = last_status.code();
  }

 private:
  friend class JsonValue;
  friend class JsonArray;
  friend class JsonObject;

  friend class NestedJsonArray;
  friend class NestedJsonObject;

  constexpr size_t remaining() const { return max_size() - size(); }

  // Sets last_status_ and updates status_ if an error occurred.
  constexpr void update_status(Status new_status);

  constexpr void set_statuses(Status status) { set_statuses(status, status); }

  constexpr void JsonValueClear() {
    MakeNull();
    set_statuses(OkStatus());
  }

  constexpr void JsonArrayClear() {
    MakeEmpty('[', ']');
    set_statuses(OkStatus());
  }

  constexpr void JsonObjectClear() {
    MakeEmpty('{', '}');
    set_statuses(OkStatus());
  }

  template <typename T>
  constexpr Status JsonValueSet(const T& value);

  template <typename T>
  constexpr JsonArray& JsonArrayAppend(const T& value);

  template <typename Iterator>
  constexpr JsonArray& JsonArrayExtend(Iterator begin, Iterator end);

  template <typename T>
  constexpr JsonObject& JsonObjectAdd(std::string_view key, const T& value);

  [[nodiscard]] constexpr bool JsonArrayAddElement();

  // Adds the key if there's room for the key and at least one more character.
  [[nodiscard]] constexpr bool JsonObjectAddKey(std::string_view key);

  constexpr size_t NestedJsonOffset(const json_impl::Nesting& nesting) const {
    // Point to the start of the nested JSON array or object. This will be three
    // characters, plus one for each prior layer of nesting {..., "": []}.
    return json_size_ - 3 - nesting.depth();
  }

  constexpr json_impl::Nesting::Type type() const {
    return IsArray() ? json_impl::Nesting::kArray : json_impl::Nesting::kObject;
  }

  constexpr json_impl::NestedJson JsonArrayAppendNested(
      const char (&open_close)[2], const json_impl::Nesting& nesting);

  constexpr json_impl::NestedJson JsonObjectAddNested(
      std::string_view key,
      const char (&open_close)[2],
      const json_impl::Nesting& nesting);

  // Nesting works by shrinking the JsonBuilder to be just the nested structure,
  // then expanding back out when done adding items.
  constexpr void AddNestedStart(const json_impl::Nesting& nesting);
  constexpr void AddNestedFinish(const json_impl::Nesting& nesting);

  template <typename T>
  constexpr void NestedJsonArrayAppend(const T& value,
                                       const json_impl::Nesting& nesting);

  template <typename T>
  constexpr void NestedJsonObjectAdd(std::string_view key,
                                     const T& value,
                                     const json_impl::Nesting& nesting);

  // For a single JSON value, checks if writing succeeded and clears on failure.
  constexpr Status HandleSet(StatusWithSize written);

  // For a value added to an array or object, checks if writing the characters
  // succeeds, sets the status, and terminates the buffer as appropriate.
  constexpr void HandleAdd(StatusWithSize written,
                           size_t starting_size,
                           char terminator);

  constexpr void MakeEmpty(char open, char close) {
    buffer_[0] = open;
    buffer_[1] = close;
    buffer_[2] = '\0';
    json_size_ = 2;
  }

  // TODO: b/326097937 - Use StringBuilder here.
  char* buffer_;
  size_t max_size_;  // max size of the JSON string, excluding the '\0'
  size_t json_size_;

  // If any errors have occurred, status_ stores the most recent error Status.
  // last_status_ stores the status from the most recent operation.
  uint8_t status_;
  uint8_t last_status_;
};

/// A `JsonBuilder` with an integrated buffer. The buffer will be sized to fit
/// `kMaxSize` characters.
template <size_t kMaxSize>
class JsonBuffer final : public JsonBuilder {
 public:
  // Constructs a JsonBuffer with the value null.
  constexpr JsonBuffer()
      : JsonBuilder(static_buffer_, sizeof(static_buffer_), Uninitialized{}),
        static_buffer_{} {
    MakeNull();
  }

  template <typename T>
  static constexpr JsonBuffer Value(const T& initial_value) {
    JsonBuffer<kMaxSize> json;
    PW_ASSERT(json.SetValue(initial_value).ok());  // Failed serialization.
    return json;
  }

  // JsonBuffers may be copied or assigned, as long as the source buffer is not
  // larger than this buffer.
  constexpr JsonBuffer(const JsonBuffer& other) : JsonBuffer() {
    CopyFrom(other);
  }

  template <size_t kOtherSize>
  constexpr JsonBuffer(const JsonBuffer<kOtherSize>& other) : JsonBuffer() {
    CopyFrom(other);
  }

  constexpr JsonBuffer& operator=(const JsonBuffer& rhs) {
    CopyFrom(rhs);
    return *this;
  }

  template <size_t kOtherSize>
  constexpr JsonBuffer& operator=(const JsonBuffer<kOtherSize>& rhs) {
    CopyFrom(rhs);
    return *this;
  }

  static constexpr size_t max_size() { return kMaxSize; }

 private:
  static_assert(kMaxSize + 1 /* null */ >= JsonBuilder::MinBufferSize(),
                "JsonBuffers require at least 4 bytes");

  template <size_t kOtherSize>
  constexpr void CopyFrom(const JsonBuffer<kOtherSize>& other) {
    static_assert(kOtherSize <= kMaxSize,
                  "A JsonBuffer cannot be copied into a smaller buffer");
    CopyFrom(static_cast<const JsonBuilder&>(other));
  }

  constexpr void CopyFrom(const JsonBuilder& other) {
    for (size_t i = 0; i < other.size() + 1 /* include null */; ++i) {
      static_buffer_[i] = other.data()[i];
    }
    JsonBuilder::set_json_size(other.size());
    JsonBuilder::set_statuses(other.status(), other.last_status());
  }

  char static_buffer_[kMaxSize + 1];
};
/// @endsubmodule

// Implementation details
namespace json_impl {

inline constexpr char kArray[2] = {'[', ']'};
inline constexpr char kObject[2] = {'{', '}'};

constexpr StatusWithSize WriteString(std::string_view value,
                                     char* buffer,
                                     size_t remaining) {
  if (value.size() + 1 /* null */ > remaining) {
    return StatusWithSize::ResourceExhausted();
  }
  for (char c : value) {
    *buffer++ = c;
  }
  *buffer = '\0';
  return StatusWithSize(value.size());
}

constexpr char NibbleToHex(uint8_t nibble) {
  return nibble + (nibble < 10 ? '0' : ('a' - 10));
}

// In accordance with RFC 8259, JSON strings must escape control characters,
// quotation marks, and reverse solidus (\). This function copies a string and
// escapes these characters as shown in http://www.json.org/string.gif.
//
// The return value is the number of bytes written to the destination
// buffer or -1 if the string does not fit in the destination buffer. Since
// escaped characters result in two or six bytes in the output, the return value
// won't necessarily equal the number of bytes read from the source buffer.
//
// The destination buffer is NEVER null-terminated!
//
// Currently this function ONLY supports ASCII! Bytes ≥128 will be escaped
// individually, rather than treated as multibyte Unicode characters.
constexpr int EscapedStringCopy(char* destination,
                                int copy_limit,
                                std::string_view source) {
  int destination_index = 0;

  for (char source_char : source) {
    if (destination_index >= copy_limit) {
      return -1;
    }

    char escaped_character = '\0';

    if (source_char >= '\b' && source_char <= '\r' && source_char != '\v') {
      constexpr char kControlChars[] = {'b', 't', 'n', '?', 'f', 'r'};
      escaped_character = kControlChars[source_char - '\b'];
    } else if (source_char == '"' || source_char == '\\') {
      escaped_character = source_char;
    } else if (source_char >= ' ' && source_char <= '~') {
      // This is a printable character; no escaping is needed.
      destination[destination_index++] = source_char;
      continue;  // Skip the escaping step below.
    } else {
      // Escape control characters that haven't already been handled. These take
      // six bytes to encode (e.g. \u0056).
      if (copy_limit - destination_index < 6) {
        return -1;
      }

      destination[destination_index++] = '\\';
      destination[destination_index++] = 'u';
      destination[destination_index++] = '0';  // Only handle ASCII for now
      destination[destination_index++] = '0';
      destination[destination_index++] = NibbleToHex((source_char >> 4) & 0x0f);
      destination[destination_index++] = NibbleToHex(source_char & 0x0f);
      continue;  // Already escaped; skip the single-character escaping step.
    }

    // Escape the \b, \t, \n, \f, \r, \", or \\ character, if it fits.
    if (copy_limit - destination_index < 2) {
      return -1;
    }

    destination[destination_index++] = '\\';
    destination[destination_index++] = escaped_character;
  }
  return destination_index;
}

// Writes "<value>", escaping special characters; returns true if successful.
// Null terminates ONLY if successful.
constexpr StatusWithSize WriteQuotedString(std::string_view value,
                                           char* buffer,
                                           size_t buffer_size) {
  constexpr size_t kOverhead = 2 /* quotes */ + 1 /* null */;
  if (value.size() + kOverhead > buffer_size) {
    return StatusWithSize::ResourceExhausted();
  }
  // If the string might fit, try to copy it. May still run out of room due to
  // escaping.
  const int written = EscapedStringCopy(
      buffer + 1 /* quote */, static_cast<int>(buffer_size - kOverhead), value);
  if (written < 0) {
    return StatusWithSize::ResourceExhausted();
  }

  buffer[0] = '"';            // open quote
  buffer[written + 1] = '"';  // close quote
  buffer[written + 2] = '\0';
  return StatusWithSize(written + 2);  // characters written excludes \0
}

constexpr StatusWithSize WriteCharPointer(const char* ptr,
                                          char* buffer,
                                          size_t buffer_size) {
  if (ptr == nullptr) {
    return WriteString("null", buffer, buffer_size);
  }
  return WriteQuotedString(ptr, buffer, buffer_size);
}

template <typename T>
inline constexpr bool kIsJson =
    std::is_base_of_v<JsonValue, T> || std::is_base_of_v<JsonArray, T> ||
    std::is_base_of_v<JsonObject, T>;

template <typename T>
struct InvalidJsonType : std::false_type {};

struct LiteralChars {
  const char (&open_close)[2];
};

template <typename T>
constexpr StatusWithSize SerializeJson(const T& value,
                                       char* buffer,
                                       size_t remaining) {
  if constexpr (kIsJson<T>) {  // nested JsonBuilder, JsonArray, JsonObject
    return WriteString(value, buffer, remaining);
  } else if constexpr (std::is_same_v<T, LiteralChars>) {  // Nested append/add
    return WriteString(
        std::string_view(value.open_close, 2), buffer, remaining);
  } else if constexpr (std::is_null_pointer_v<T> ||  // nullptr & C strings
                       std::is_same_v<T, char*> ||
                       std::is_same_v<T, const char*>) {
    return WriteCharPointer(value, buffer, remaining);
  } else if constexpr (std::is_convertible_v<T, std::string_view>) {  // strings
    return WriteQuotedString(value, buffer, remaining);
  } else if constexpr (std::is_floating_point_v<T>) {
    return string::FloatAsIntToString(static_cast<float>(value),
                                      {buffer, remaining});
  } else if constexpr (std::is_same_v<T, bool>) {  // boolean
    return WriteString(value ? "true" : "false", buffer, remaining);
  } else if constexpr (std::is_integral_v<T>) {  // integers
    return string::IntToString(value, {buffer, remaining});
  } else {
    static_assert(InvalidJsonType<T>(),
                  "JSON values may only be numbers, strings, JSON arrays, JSON "
                  "objects, or null");
    return StatusWithSize::Internal();
  }
}

}  // namespace json_impl

// Forward NestedJsonArray, NestedJsonObject, JsonArray, and JsonObject
// function calls to JsonBuilder.

#define PW_JSON_COMMON_INTERFACE_IMPL(name)                                    \
  constexpr bool name::IsValue() const {                                       \
    return static_cast<const JsonBuilder*>(this)->IsValue();                   \
  }                                                                            \
  constexpr bool name::IsArray() const {                                       \
    return static_cast<const JsonBuilder*>(this)->IsArray();                   \
  }                                                                            \
  constexpr bool name::IsObject() const {                                      \
    return static_cast<const JsonBuilder*>(this)->IsObject();                  \
  }                                                                            \
  constexpr name::operator std::string_view() const {                          \
    return static_cast<const JsonBuilder*>(this)->operator std::string_view(); \
  }                                                                            \
  constexpr const char* name::data() const {                                   \
    return static_cast<const JsonBuilder*>(this)->data();                      \
  }                                                                            \
  constexpr size_t name::size() const {                                        \
    return static_cast<const JsonBuilder*>(this)->size();                      \
  }                                                                            \
  constexpr size_t name::max_size() const {                                    \
    return static_cast<const JsonBuilder*>(this)->max_size();                  \
  }                                                                            \
  constexpr bool name::ok() const {                                            \
    return static_cast<const JsonBuilder*>(this)->ok();                        \
  }                                                                            \
  constexpr Status name::status() const {                                      \
    return static_cast<const JsonBuilder*>(this)->status();                    \
  }                                                                            \
  constexpr Status name::last_status() const {                                 \
    return static_cast<const JsonBuilder*>(this)->last_status();               \
  }                                                                            \
  constexpr void name::clear() {                                               \
    static_cast<JsonBuilder*>(this)->name##Clear();                            \
  }                                                                            \
  constexpr void name::clear_status() {                                        \
    static_cast<JsonBuilder*>(this)->clear_status();                           \
  }                                                                            \
  static_assert(true)

PW_JSON_COMMON_INTERFACE_IMPL(JsonValue);
PW_JSON_COMMON_INTERFACE_IMPL(JsonArray);
PW_JSON_COMMON_INTERFACE_IMPL(JsonObject);

#undef PW_JSON_COMMON_INTERFACE_IMPL

template <typename T>
constexpr NestedJsonArray& NestedJsonArray::Append(const T& value) {
  json_.builder().NestedJsonArrayAppend(value, json_.nesting());
  return *this;
}
constexpr NestedJsonArray NestedJsonArray::AppendNestedArray() {
  return json_.builder().JsonArrayAppendNested(json_impl::kArray,
                                               json_.nesting());
}
constexpr NestedJsonObject NestedJsonArray::AppendNestedObject() {
  return json_.builder().JsonArrayAppendNested(json_impl::kObject,
                                               json_.nesting());
}
constexpr NestedJsonArray NestedJsonObject::AddNestedArray(
    std::string_view key) {
  return json_.builder().JsonObjectAddNested(
      key, json_impl::kArray, json_.nesting());
}
constexpr NestedJsonObject NestedJsonObject::AddNestedObject(
    std::string_view key) {
  return json_.builder().JsonObjectAddNested(
      key, json_impl::kObject, json_.nesting());
}
template <typename T>
constexpr NestedJsonObject& NestedJsonObject::Add(std::string_view key,
                                                  const T& value) {
  json_.builder().NestedJsonObjectAdd(key, value, json_.nesting());
  return *this;
}
template <typename T>
constexpr Status JsonValue::Set(const T& value) {
  return static_cast<JsonBuilder*>(this)->JsonValueSet(value);
}
template <typename T>
constexpr JsonArray& JsonArray::Append(const T& value) {
  return static_cast<JsonBuilder*>(this)->JsonArrayAppend(value);
}
constexpr NestedJsonArray JsonArray::AppendNestedArray() {
  return static_cast<JsonBuilder*>(this)->JsonArrayAppendNested(
      json_impl::kArray, {});
}
constexpr NestedJsonObject JsonArray::AppendNestedObject() {
  return static_cast<JsonBuilder*>(this)->JsonArrayAppendNested(
      json_impl::kObject, {});
}
template <typename Iterable>
constexpr JsonArray& JsonArray::Extend(const Iterable& iterable) {
  return static_cast<JsonBuilder*>(this)->JsonArrayExtend(std::cbegin(iterable),
                                                          std::cend(iterable));
}
template <typename T, size_t kSize>
constexpr JsonArray& JsonArray::Extend(const T (&iterable)[kSize]) {
  return static_cast<JsonBuilder*>(this)->JsonArrayExtend(std::cbegin(iterable),
                                                          std::cend(iterable));
}
template <typename T>
constexpr JsonObject& JsonObject::Add(std::string_view key, const T& value) {
  return static_cast<JsonBuilder*>(this)->JsonObjectAdd(key, value);
}
constexpr NestedJsonArray JsonObject::AddNestedArray(std::string_view key) {
  return static_cast<JsonBuilder*>(this)->JsonObjectAddNested(
      key, json_impl::kArray, {});
}
constexpr NestedJsonObject JsonObject::AddNestedObject(std::string_view key) {
  return static_cast<JsonBuilder*>(this)->JsonObjectAddNested(
      key, json_impl::kObject, {});
}

// JsonBuilder function implementations.

template <typename T>
constexpr Status JsonBuilder::SetValue(const T& value) {
  return HandleSet(json_impl::SerializeJson(value, buffer_, max_size_ + 1));
}

constexpr void JsonBuilder::update_status(Status new_status) {
  last_status_ = new_status.code();
  if (!new_status.ok() && status().ok()) {
    status_ = new_status.code();
  }
}

template <typename T>
constexpr Status JsonBuilder::JsonValueSet(const T& value) {
  if constexpr (json_impl::kIsJson<T>) {
    PW_ASSERT(this != &value);   // Self-nesting is disallowed.
    PW_ASSERT(value.IsValue());  // Cannot set JsonValue to an array or object.
  }
  return SetValue(value);
}

template <typename T>
constexpr JsonArray& JsonBuilder::JsonArrayAppend(const T& value) {
  if constexpr (json_impl::kIsJson<T>) {
    PW_ASSERT(this != &value);  // Self-nesting is disallowed.
  }

  const size_t starting_size = size();
  if (JsonArrayAddElement()) {
    // The buffer size is remaining() + 1 bytes, but drop the + 1 to leave room
    // for the closing ].
    HandleAdd(json_impl::SerializeJson(value, &buffer_[size()], remaining()),
              starting_size,
              ']');
  }
  return *this;
}

template <typename Iterator>
constexpr JsonArray& JsonBuilder::JsonArrayExtend(Iterator begin,
                                                  Iterator end) {
  const size_t starting_size = size();

  for (Iterator cur = begin; cur != end; ++cur) {
    const Status status = Append(*cur).last_status();
    if (!status.ok()) {  // Undo changes if there is an issue.
      json_size_ = starting_size;
      buffer_[size() - 1] = ']';
      buffer_[size()] = '\0';
      break;
    }
  }
  return *this;
}

template <typename T>
constexpr JsonObject& JsonBuilder::JsonObjectAdd(std::string_view key,
                                                 const T& value) {
  if constexpr (json_impl::kIsJson<T>) {
    PW_ASSERT(this != &value);  // Self-nesting is disallowed.
  }

  const size_t starting_size = size();
  if (JsonObjectAddKey(key)) {
    // The buffer size is remaining() + 1 bytes, but drop the + 1 to leave room
    // for the closing }.
    HandleAdd(json_impl::SerializeJson(value, &buffer_[size()], remaining()),
              starting_size,
              '}');
  }
  return *this;
}

constexpr bool JsonBuilder::JsonArrayAddElement() {
  PW_ASSERT(IsArray());  // Attempted to append to an object or value

  // Needs space for at least 3 new characters (, 1)
  if (size() + 3 > max_size()) {
    update_status(Status::ResourceExhausted());
    return false;
  }

  // If this is the first element, just drop the ]. Otherwise, add a comma.
  if (size() == 2) {
    json_size_ = 1;
  } else {
    buffer_[json_size_ - 1] = ',';
    buffer_[json_size_++] = ' ';
  }

  return true;
}

constexpr bool JsonBuilder::JsonObjectAddKey(std::string_view key) {
  PW_ASSERT(IsObject());  // Attempted add a key-value pair to an array or value

  // Each key needs 7 more characters: (, "": ) plus at least 1 for the value.
  // The ',' replaces the terminal '}', but a new '}' is placed at the end, so
  // the total remains 7. The first key could get away with 5, but oh well.
  if (size() + key.size() + 7 > max_size()) {
    update_status(Status::ResourceExhausted());
    return false;
  }

  const size_t original_size = json_size_;

  // If this is the first key, just drop the }. Otherwise, add a comma.
  if (size() == 2) {
    json_size_ = 1;
  } else {
    buffer_[json_size_ - 1] = ',';  // change the last } to ,
    buffer_[json_size_++] = ' ';
  }

  // Buffer size is remaining() + 1, but - 4 for at least ": 0}" after the key.
  auto written =
      json_impl::WriteQuotedString(key, &buffer_[json_size_], remaining() - 3);
  if (!written.ok()) {
    update_status(written.status());
    json_size_ = original_size;
    buffer_[json_size_ - 1] = '}';
    buffer_[json_size_] = '\0';
    return false;
  }

  json_size_ += written.size();  // Now have {"key" or {..., "key"
  buffer_[json_size_++] = ':';
  buffer_[json_size_++] = ' ';
  return true;
}

constexpr json_impl::NestedJson JsonBuilder::JsonArrayAppendNested(
    const char (&open_close)[2], const json_impl::Nesting& nesting) {
  AddNestedStart(nesting);
  json_impl::Nesting::Type nesting_within = type();
  JsonArrayAppend(json_impl::LiteralChars{open_close});  // [..., {}]
  AddNestedFinish(nesting);
  return json_impl::NestedJson(
      *this,
      last_status().ok()
          ? nesting.Nest(NestedJsonOffset(nesting), nesting_within)
          : json_impl::Nesting());
}

constexpr json_impl::NestedJson JsonBuilder::JsonObjectAddNested(
    std::string_view key,
    const char (&open_close)[2],
    const json_impl::Nesting& nesting) {
  AddNestedStart(nesting);
  json_impl::Nesting::Type nesting_within = type();
  JsonObjectAdd(key, json_impl::LiteralChars{open_close});  // {..., "key": {}}
  AddNestedFinish(nesting);
  return json_impl::NestedJson(
      *this,
      last_status().ok()
          ? nesting.Nest(NestedJsonOffset(nesting), nesting_within)
          : json_impl::Nesting());
}

constexpr void JsonBuilder::AddNestedStart(const json_impl::Nesting& nesting) {
  // A nested structure must be the last thing in the JSON. Back up to where the
  // first of the closing ] or } should be, and check from there.
  PW_ASSERT(  // JSON must not have been cleared since nesting.
      json_size_ >= nesting.offset() + nesting.depth() + 2 /* [] or {} */);
  PW_ASSERT(  // Nested structure must match the expected type
      buffer_[json_size_ - nesting.depth() - 1] ==
      buffer_[nesting.offset()] + 2 /* convert [ to ] or { to } */);
  nesting.CheckNesting(&buffer_[json_size_ - nesting.depth()]);

  buffer_ += nesting.offset();
  json_size_ -= nesting.offset() + nesting.depth();
  max_size_ -= nesting.offset() + nesting.depth();
}

constexpr void JsonBuilder::AddNestedFinish(const json_impl::Nesting& nesting) {
  buffer_ -= nesting.offset();
  max_size_ += nesting.offset() + nesting.depth();

  json_size_ += nesting.offset();
  nesting.Terminate(&buffer_[json_size_]);
  json_size_ += nesting.depth();
}

template <typename T>
constexpr void JsonBuilder::NestedJsonArrayAppend(
    const T& value, const json_impl::Nesting& nesting) {
  AddNestedStart(nesting);
  JsonArrayAppend(value);
  AddNestedFinish(nesting);
}

template <typename T>
constexpr void JsonBuilder::NestedJsonObjectAdd(
    std::string_view key, const T& value, const json_impl::Nesting& nesting) {
  AddNestedStart(nesting);
  JsonObjectAdd(key, value);
  AddNestedFinish(nesting);
}

constexpr Status JsonBuilder::HandleSet(StatusWithSize written) {
  if (written.ok()) {
    json_size_ = written.size();
  } else {
    MakeNull();
  }
  set_statuses(written.status());  // status is always reset when setting value
  return last_status();
}

constexpr void JsonBuilder::HandleAdd(StatusWithSize written,
                                      size_t starting_size,
                                      char terminator) {
  update_status(written.status());  // save room for } or ]
  if (last_status().ok()) {
    json_size_ += written.size();
  } else {
    json_size_ = starting_size - 1;  // make room for closing character
  }

  buffer_[json_size_++] = terminator;
  buffer_[json_size_] = '\0';
}

}  // namespace pw
