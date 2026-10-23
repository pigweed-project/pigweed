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
#pragma once

/// Command line argument parsing.
///
/// The objects defined below can be used to parse command line arguments of
/// different types. These objects are "just enough" defined for current use
/// cases, but the design is intended to be extensible as new types and traits
/// are needed.
///
/// Example:
///
/// Given a boolean flag "verbose", a numerical flag "runs", and a positional
/// "port" argument to be parsed, we can create a vector of parsers. In this
/// example, we modify the parsers during creation to set default values:
///
/// @code
///   Vector<ArgParserVariant, 3> parsers = {
///     BoolParser("-v", "--verbose").set_default(false),
///     UnsignedParser<size_t>("-r", "--runs").set_default(1000),
///     UnsignedParser<uint16_t>("port").set_default(11111),
///   };
/// @endcode
///
/// With this vector, we can then parse command line arguments and extract
/// the values of arguments that were set, e.g.:
///
/// @code
///   if (!ParseArgs(parsers, argc, argv).ok()) {
///     PrintUsage(parsers, argv[0]);
///     return 1;
///   }
///   bool verbose;
///   size_t runs;
///   uint16_t port;
///   if (!GetArg(parsers, "--verbose", &verbose).ok() ||
///       !GetArg(parsers, "--runs", &runs).ok() ||
///       !GetArg(parsers, "port", &port).ok()) {
///     // Shouldn't happen unless names do not match.
///     return 1;
///   }
///
///   // Do stuff with `verbose`, `runs`, and `port`...
/// @endcode

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>

#include "pw_containers/vector.h"
#include "pw_status/status.h"

namespace pw::rpc::fuzz {

/// Enumerates the results of trying to parse a specific command line argument
/// with a particular parsers.
enum ParseStatus {
  /// The argument matched the parser and was successfully parsed without a
  /// value.
  kParsedOne,

  /// The argument matched the parser and was successfully parsed with a value.
  kParsedTwo,

  /// The argument did not match the parser. This is not necessarily an error;
  /// the argument may match a different parser.
  kParseMismatch,

  /// The argument matched a parser, but could not be parsed. This may be due to
  /// a missing value for a flag, a value of the wrong type, a provided value
  /// being out of range, etc. Parsers should log additional details before
  /// returning this value.
  kParseFailure,
};

/// Holds parsed argument values of different types.
using ArgVariant = std::variant<std::monostate, bool, uint64_t>;

/// Base class for argument parsers.
class ArgParserBase {
 public:
  virtual ~ArgParserBase() = default;

  std::string_view short_name() const { return short_name_; }
  std::string_view long_name() const { return long_name_; }
  bool positional() const { return positional_; }

  /// Clears the value. Typically, command line arguments are only parsed once,
  /// but this method is useful for testing.
  void Reset() { value_ = std::monostate(); }

 protected:
  /// Defines an argument parser with a single name. This may be a positional
  /// argument or a flag.
  ArgParserBase(std::string_view name);

  /// Defines an argument parser for a flag with short and long names.
  ArgParserBase(std::string_view shortopt, std::string_view longopt);

  void set_initial(ArgVariant initial) { initial_ = initial; }
  void set_value(ArgVariant value) { value_ = value; }

  /// Examines if the given `arg` matches this parser. A parser for a flag can
  /// match the short name (e.g. '-f') if set, or the long name (e.g. '--foo').
  /// A parser for a positional argument will match anything until it has a
  /// value set.
  bool Match(std::string_view arg);

  /// Returns the parsed value.
  template <typename T>
  T Get() const {
    return std::get<T>(GetValue());
  }

 private:
  const ArgVariant& GetValue() const;

  std::string_view short_name_;
  std::string_view long_name_;
  bool positional_;

  ArgVariant initial_;
  ArgVariant value_;
};

// Argument parsers for boolean arguments. These arguments are always flags, and
// can be specified as, e.g. "-f" (true), "--foo" (true) or "--no-foo" (false).
class BoolParser : public ArgParserBase {
 public:
  BoolParser(std::string_view optname);
  BoolParser(std::string_view shortopt, std::string_view longopt);

  bool value() const { return Get<bool>(); }
  BoolParser& set_default(bool value);

  ParseStatus Parse(std::string_view arg0,
                    std::string_view arg1 = std::string_view());
};

// Type-erasing argument parser for unsigned integer arguments. This object
// always parses values as `uint64_t`s and should not be used directly.
// Instead, use `UnsignedParser<T>` with a type to explicitly narrow to.
class UnsignedParserBase : public ArgParserBase {
 protected:
  UnsignedParserBase(std::string_view name);
  UnsignedParserBase(std::string_view shortopt, std::string_view longopt);

  ParseStatus Parse(std::string_view arg0, std::string_view arg1, uint64_t max);
};

// Argument parser for unsigned integer arguments. These arguments may be flags
// or positional arguments.
template <typename T, typename std::enable_if_t<std::is_unsigned_v<T>, int> = 0>
class UnsignedParser : public UnsignedParserBase {
 public:
  UnsignedParser(std::string_view name) : UnsignedParserBase(name) {}
  UnsignedParser(std::string_view shortopt, std::string_view longopt)
      : UnsignedParserBase(shortopt, longopt) {}

  T value() const { return static_cast<T>(Get<uint64_t>()); }

  UnsignedParser& set_default(T value) {
    set_initial(static_cast<uint64_t>(value));
    return *this;
  }

  ParseStatus Parse(std::string_view arg0,
                    std::string_view arg1 = std::string_view()) {
    return UnsignedParserBase::Parse(arg0, arg1, std::numeric_limits<T>::max());
  }
};

// Holds argument parsers of different types.
using ArgParserVariant = std::variant<BoolParser,
                                      UnsignedParser<unsigned short>,
                                      UnsignedParser<unsigned int>,
                                      UnsignedParser<unsigned long>,
                                      UnsignedParser<unsigned long long>>;

// Parses the command line arguments and sets the values of the given `parsers`.
Status ParseArgs(Vector<ArgParserVariant>& parsers, int argc, char** argv);

// Logs a usage message based on the given `parsers` and the program name given
// by `argv0`.
void PrintUsage(const Vector<ArgParserVariant>& parsers,
                std::string_view argv0);

// Attempts to find the parser in `parsers` with the given `name`, and returns
// its value if found.
std::optional<ArgVariant> GetArg(const Vector<ArgParserVariant>& parsers,
                                 std::string_view name);

inline void GetArgValue(const ArgVariant& arg, bool* out) {
  *out = std::get<bool>(arg);
}

template <typename T, typename std::enable_if_t<std::is_unsigned_v<T>, int> = 0>
void GetArgValue(const ArgVariant& arg, T* out) {
  *out = static_cast<T>(std::get<uint64_t>(arg));
}

// Like `GetArgVariant` above, but extracts the typed value from the variant
// into `out`. Returns an error if no parser exists in `parsers` with the given
// `name`.
template <typename T>
Status GetArg(const Vector<ArgParserVariant>& parsers,
              std::string_view name,
              T* out) {
  const auto& arg = GetArg(parsers, name);
  if (!arg.has_value()) {
    return Status::InvalidArgument();
  }
  GetArgValue(*arg, out);
  return OkStatus();
}

// Resets the parser with the given name. Returns an error if not found.
Status ResetArg(Vector<ArgParserVariant>& parsers, std::string_view name);

}  // namespace pw::rpc::fuzz
