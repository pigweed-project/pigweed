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

// Tests that logging a status with a tokenizing pw_log backend adds the token
// for "no message" to the token database of the binary. Without it, a status
// that has no message cannot be detokenized.

#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

// A logged status only has a message token if status messages are stored as
// tokens. This test always uses the tokenizing pw_log macros, so
// pw_log/status.h can only be included if the status messages match them.
#if PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_TOKENIZED

#include <cstdint>
#include <string_view>

#include "pw_bytes/span.h"
#include "pw_log/status.h"
#include "pw_result/result.h"
#include "pw_status/message.h"
#include "pw_status/status_base.h"
#include "pw_tokenizer/detokenize.h"
#include "pw_tokenizer/detokenize_from_this_program.h"
#include "tokenized_args_test_enum.h"

namespace {

using this_is_a_test::Link;

// Link is registered with PW_ENUM, so LinkStatus supports messages.
using LinkStatus = pw::StatusOf<Link::kUp>;

// A status code that is not registered with PW_ENUM, so its status cannot carry
// messages and should not emit a "no message" token.
enum class UnregisteredCode : uint8_t {
  kOk = 0,
  kFailed = 1,
};

using UnregisteredStatus = pw::StatusOf<UnregisteredCode::kOk>;

// Returns whether this program's token database has token 0 as an empty string
// in the domain.
bool DefinesNoMessageToken(std::string_view message_domain) {
  const pw::Result<pw::tokenizer::Detokenizer> detokenizer =
      pw::tokenizer::GetDetokenizerFromThisProgram();
  if (!detokenizer.ok()) {
    ADD_FAILURE() << "Failed to read the token database of this program";
    return false;
  }
  const uint32_t no_message_token = 0;
  const pw::tokenizer::DetokenizedString result = detokenizer->Detokenize(
      pw::ObjectAsBytes(no_message_token), message_domain);
  return !result.matches().empty() && result.BestString().empty();
}

TEST(LogStatusToken, NoMessageTokenDefinedForStatusLoggedAtRunTime) {
  const pw::Status status = pw::Status::NotFound();
  EXPECT_EQ(PW_LOG_STATUS(status), 5u);

  EXPECT_TRUE(DefinesNoMessageToken(PW_STATUS_MSG_DOMAIN(PW_STATUS_DOMAIN)));
}

// LinkStatus is only logged in this constant expression, so no code in the
// binary logs it. The logged value is in the binary, so its token must be in
// the token database.
constexpr uint32_t kLoggedLinkDown = PW_LOG_STATUS(LinkStatus(Link::kDown));

TEST(LogStatusToken, NoMessageTokenDefinedForStatusLoggedInConstantExpression) {
  EXPECT_EQ(kLoggedLinkDown, 0u);

  EXPECT_TRUE(
      DefinesNoMessageToken(PW_STATUS_MSG_DOMAIN(THIS_IS_A_TEST_LINK_DOMAIN)));
}

TEST(LogStatusToken, NoMessageTokenNotDefinedForStatusThatIsNotLogged) {
  EXPECT_FALSE(DefinesNoMessageToken(PW_STATUS_MSG_DOMAIN("::not::Logged")));
}

TEST(LogStatusToken, NoMessageTokenNotDefinedForStatusWithoutMessageSupport) {
  const UnregisteredStatus failed(UnregisteredCode::kFailed);
  EXPECT_EQ(PW_LOG_STATUS(failed), 1u);

  EXPECT_FALSE(
      DefinesNoMessageToken(PW_STATUS_MSG_DOMAIN("::UnregisteredCode")));
}

}  // namespace

#endif  // PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_TOKENIZED
