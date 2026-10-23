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

#include "pw_rpc/fuzz/argparse.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_string/string_builder.h"

namespace pw::rpc::fuzz {
namespace {

// Visitor to `ArgVariant` used by `ParseArgs` below.
struct ParseVisitor {
  std::string_view arg0;
  std::string_view arg1;

  template <typename Parser>
  ParseStatus operator()(Parser& parser) {
    return parser.Parse(arg0, arg1);
  }
};

// Visitor to `ArgVariant` used by `GetArg` below.
struct ValueVisitor {
  std::string_view name;

  template <typename Parser>
  std::optional<ArgVariant> operator()(Parser& parser) {
    std::optional<ArgVariant> result;
    if (parser.short_name() == name || parser.long_name() == name) {
      result.emplace(parser.value());
    }
    return result;
  }
};

// Visitor to `ArgVariant` used by `PrintUsage` below.
const size_t kMaxUsageLen = 256;
struct UsageVisitor {
  StringBuffer<kMaxUsageLen>* buffer;

  void operator()(const BoolParser& parser) const {
    auto short_name = parser.short_name();
    auto long_name = parser.long_name();
    *buffer << " [" << short_name << "|--[no-]" << long_name.substr(2) << "]";
  }

  template <typename T>
  void operator()(const UnsignedParser<T>& parser) const {
    auto short_name = parser.short_name();
    auto long_name = parser.long_name();
    *buffer << " ";
    if (!parser.positional()) {
      *buffer << "[";
      if (!short_name.empty()) {
        *buffer << short_name << "|";
      }
      *buffer << long_name << " ";
    }
    for (const auto& c : long_name) {
      *buffer << static_cast<char>(toupper(c));
    }
    if (!parser.positional()) {
      *buffer << "]";
    }
  }
};

// Visitor to `ArgVariant` used by `ResetArg` below.
struct ResetVisitor {
  std::string_view name;

  template <typename Parser>
  bool operator()(Parser& parser) {
    if (parser.short_name() != name && parser.long_name() != name) {
      return false;
    }
    parser.Reset();
    return true;
  }
};

}  // namespace

ArgParserBase::ArgParserBase(std::string_view name) : long_name_(name) {
  PW_CHECK(!name.empty());
  PW_CHECK(name != "--");
  positional_ =
      name[0] != '-' || (name.size() > 2 && name.substr(0, 2) != "--");
}

ArgParserBase::ArgParserBase(std::string_view shortopt,
                             std::string_view longopt)
    : short_name_(shortopt), long_name_(longopt) {
  PW_CHECK(shortopt.size() == 2);
  PW_CHECK(shortopt[0] == '-');
  PW_CHECK(shortopt != "--");
  PW_CHECK(longopt.size() > 2);
  PW_CHECK(longopt.substr(0, 2) == "--");
  positional_ = false;
}

bool ArgParserBase::Match(std::string_view arg) {
  if (arg.empty()) {
    return false;
  }
  if (!positional_) {
    return arg == short_name_ || arg == long_name_;
  }
  if (!std::holds_alternative<std::monostate>(value_)) {
    return false;
  }
  if ((arg.size() == 2 && arg[0] == '-') ||
      (arg.size() > 2 && arg.substr(0, 2) == "--")) {
    PW_LOG_WARN("Argument parsed for '%s' appears to be a flag: '%s'",
                long_name_.data(),
                arg.data());
  }
  return true;
}

const ArgVariant& ArgParserBase::GetValue() const {
  return std::holds_alternative<std::monostate>(value_) ? initial_ : value_;
}

BoolParser::BoolParser(std::string_view name) : ArgParserBase(name) {}
BoolParser::BoolParser(std::string_view shortopt, std::string_view longopt)
    : ArgParserBase(shortopt, longopt) {}

BoolParser& BoolParser::set_default(bool value) {
  set_initial(value);
  return *this;
}

ParseStatus BoolParser::Parse(std::string_view arg0,
                              [[maybe_unused]] std::string_view arg1) {
  if (Match(arg0)) {
    set_value(true);
    return kParsedOne;
  }
  if (arg0.size() > 5 && arg0.substr(0, 5) == "--no-" &&
      arg0.substr(5) == long_name().substr(2)) {
    set_value(false);
    return kParsedOne;
  }
  return kParseMismatch;
}

UnsignedParserBase::UnsignedParserBase(std::string_view name)
    : ArgParserBase(name) {}
UnsignedParserBase::UnsignedParserBase(std::string_view shortopt,
                                       std::string_view longopt)
    : ArgParserBase(shortopt, longopt) {}

ParseStatus UnsignedParserBase::Parse(std::string_view arg0,
                                      std::string_view arg1,
                                      uint64_t max) {
  auto result = kParsedOne;
  if (!Match(arg0)) {
    return kParseMismatch;
  }
  if (!positional()) {
    if (arg1.empty()) {
      PW_LOG_ERROR("Missing value for flag '%s'", arg0.data());
      return kParseFailure;
    }
    arg0 = arg1;
    result = kParsedTwo;
  }
  char* endptr;
  unsigned long long value = std::strtoull(arg0.data(), &endptr, 0);
  if (*endptr) {
    PW_LOG_ERROR("Failed to parse number from '%s'", arg0.data());
    return kParseFailure;
  }
  if (value > max) {
    PW_LOG_ERROR("Parsed value is too large: %llu", value);
    return kParseFailure;
  }
  set_value(static_cast<uint64_t>(value));
  return result;
}

Status ParseArgs(Vector<ArgParserVariant>& parsers, int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    auto arg0 = std::string_view(argv[i]);
    auto arg1 =
        i == (argc - 1) ? std::string_view() : std::string_view(argv[i + 1]);
    bool parsed = false;
    for (auto& parser : parsers) {
      switch (std::visit(ParseVisitor{.arg0 = arg0, .arg1 = arg1}, parser)) {
        case kParsedOne:
          break;
        case kParsedTwo:
          ++i;
          break;
        case kParseMismatch:
          continue;
        case kParseFailure:
          PW_LOG_ERROR("Failed to parse '%s'", arg0.data());
          return Status::InvalidArgument();
      }
      parsed = true;
      break;
    }
    if (!parsed) {
      PW_LOG_ERROR("Unrecognized argument: '%s'", arg0.data());
      return Status::InvalidArgument();
    }
  }
  return OkStatus();
}

void PrintUsage(const Vector<ArgParserVariant>& parsers,
                std::string_view argv0) {
  StringBuffer<kMaxUsageLen> buffer;
  buffer << "usage: " << argv0;
  for (auto& parser : parsers) {
    std::visit(UsageVisitor{.buffer = &buffer}, parser);
  }
  PW_LOG_INFO("%s", buffer.c_str());
}

std::optional<ArgVariant> GetArg(const Vector<ArgParserVariant>& parsers,
                                 std::string_view name) {
  for (auto& parser : parsers) {
    if (auto result = std::visit(ValueVisitor{.name = name}, parser);
        result.has_value()) {
      return result;
    }
  }
  return std::optional<ArgVariant>();
}

Status ResetArg(Vector<ArgParserVariant>& parsers, std::string_view name) {
  for (auto& parser : parsers) {
    if (std::visit(ResetVisitor{.name = name}, parser)) {
      return OkStatus();
    }
  }
  return Status::InvalidArgument();
}

}  // namespace pw::rpc::fuzz
