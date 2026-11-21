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

// Tests that the strings of PW_STATUS_MSG messages are in the token database of
// the binary that uses them. The tests evaluate messages for a status that
// stores tokens, so they apply to every PW_STATUS_CFG_MESSAGES setting.

#include <cstdint>
#include <string>
#include <string_view>

#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_status/message.h"
#include "pw_status/status.h"
#include "pw_tokenizer/detokenize.h"
#include "pw_tokenizer/detokenize_from_this_program.h"
#include "pw_unit_test/framework.h"

namespace {

using TokenStorage =
    pw::internal::StatusStorage<PW_STATUS_OK,
                                pw::internal::StatusMessageType::kToken>;

// Returns the string that this program's token database has for a pw::Status
// message token, or an empty string if it has none.
std::string DetokenizeMessage(uint32_t token) {
  const pw::Result<pw::tokenizer::Detokenizer> detokenizer =
      pw::tokenizer::GetDetokenizerFromThisProgram();
  if (!detokenizer.ok()) {
    ADD_FAILURE() << "Failed to read the token database of this program";
    return "";
  }
  return detokenizer
      ->Detokenize(pw::ObjectAsBytes(token),
                   pw::internal::kStatusMessageDomain<pw_Status>.chars)
      .BestString();
}

TEST(StatusMessageToken, EvaluatedAtRunTime) {
  const auto message = _PW_STATUS_MSG_ENABLED("Evaluated at run time");
  EXPECT_EQ(DetokenizeMessage(message.Evaluate<TokenStorage>()),
            " (Evaluated at run time)");
}

TEST(StatusMessageToken, EmptyMessage) {
  const auto message = _PW_STATUS_MSG_ENABLED("");
  EXPECT_EQ(DetokenizeMessage(message.Evaluate<TokenStorage>()), " ()");
}

TEST(StatusMessageToken, SameMessageUsedTwice) {
  const auto first = _PW_STATUS_MSG_ENABLED("Used twice");
  const auto second = _PW_STATUS_MSG_ENABLED("Used twice");
  EXPECT_EQ(first.Evaluate<TokenStorage>(), second.Evaluate<TokenStorage>());
  EXPECT_EQ(DetokenizeMessage(first.Evaluate<TokenStorage>()), " (Used twice)");
}

// A message in an inline function may be defined in several translation units.
inline uint32_t InlineFunctionToken() {
  return _PW_STATUS_MSG_ENABLED("In an inline function")
      .Evaluate<TokenStorage>();
}

TEST(StatusMessageToken, InInlineFunction) {
  EXPECT_EQ(DetokenizeMessage(InlineFunctionToken()),
            " (In an inline function)");
}

template <typename T>
uint32_t FunctionTemplateToken() {
  static_cast<void>(sizeof(T));
  return _PW_STATUS_MSG_ENABLED("In a function template")
      .template Evaluate<TokenStorage>();
}

TEST(StatusMessageToken, InFunctionTemplate) {
  EXPECT_EQ(DetokenizeMessage(FunctionTemplateToken<int>()),
            " (In a function template)");
  EXPECT_EQ(FunctionTemplateToken<int>(), FunctionTemplateToken<char>());
}

#if defined(__cpp_constexpr) && __cpp_constexpr >= 202211L

// Nothing in the binary evaluates a message that is only used in constant
// expressions, so no code refers to its token entry. The entry must still be in
// the token database, since the token is in the binary.

constexpr uint32_t kConstantToken =
    _PW_STATUS_MSG_ENABLED("Evaluated in a constant expression")
        .Evaluate<TokenStorage>();

TEST(StatusMessageToken, EvaluatedInConstantExpression) {
  EXPECT_EQ(DetokenizeMessage(kConstantToken),
            " (Evaluated in a constant expression)");
}

inline constexpr uint32_t kInlineConstantToken =
    _PW_STATUS_MSG_ENABLED("Evaluated for an inline variable")
        .Evaluate<TokenStorage>();

TEST(StatusMessageToken, EvaluatedForInlineVariable) {
  EXPECT_EQ(DetokenizeMessage(kInlineConstantToken),
            " (Evaluated for an inline variable)");
}

template <typename T>
struct ClassTemplate {
  static constexpr uint32_t kToken =
      _PW_STATUS_MSG_ENABLED("Evaluated in a class template")
          .template Evaluate<TokenStorage>();
};

TEST(StatusMessageToken, EvaluatedInClassTemplate) {
  EXPECT_EQ(DetokenizeMessage(ClassTemplate<int>::kToken),
            " (Evaluated in a class template)");
}

constexpr uint32_t ConstexprFunctionToken() {
  return _PW_STATUS_MSG_ENABLED("Evaluated in a constexpr function")
      .Evaluate<TokenStorage>();
}

TEST(StatusMessageToken, EvaluatedInConstexprFunction) {
  constexpr uint32_t kToken = ConstexprFunctionToken();
  EXPECT_EQ(DetokenizeMessage(kToken), " (Evaluated in a constexpr function)");
}

#if PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_TOKENIZED

constexpr pw::Status kConstantStatus =
    pw::Status::NotFound(PW_STATUS_MSG("Constant status"));

TEST(StatusMessageToken, ConstantStatus) {
  EXPECT_EQ(DetokenizeMessage(pw::internal::StatusMessageAccess::Get(
                kConstantStatus.message())),
            " (Constant status)");
}

#endif  // PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_TOKENIZED

#endif  // __cpp_constexpr >= 202211L

#if PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_TOKENIZED

TEST(StatusMessageToken, Status) {
  const pw::Status status = pw::Status::NotFound(PW_STATUS_MSG("No such key"));
  EXPECT_EQ(DetokenizeMessage(
                pw::internal::StatusMessageAccess::Get(status.message())),
            " (No such key)");
}

#endif  // PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_TOKENIZED

}  // namespace
