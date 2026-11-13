// Copyright 2021 The Pigweed Authors
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

#include <array>
#include <string_view>

#include "pw_stream/null_stream.h"
#include "pw_tls_client/session.h"
#include "pw_unit_test/framework.h"

namespace pw::tls_client {
namespace {

TEST(TLSClientMbedTLS, CreateSucceed) {
  auto options = SessionOptions().set_transport(stream::NullStream::Instance());
  auto res = Session::Create(options);
  ASSERT_EQ(res.status(), OkStatus());
  ASSERT_NE(res.value(), nullptr);
  delete res.value();
}

TEST(TLSClientMbedTLS, CreateFailOnMissingTransport) {
  auto options = SessionOptions();
  auto res = Session::Create(options);
  ASSERT_NE(res.status(), OkStatus());
}

TEST(TLSClientMbedTLS, EntropySourceFail) {
  backend::SessionImplementation::SetEntropySourceStatus(Status::Internal());
  auto options = SessionOptions().set_transport(stream::NullStream::Instance());
  auto res = Session::Create(options);
  backend::SessionImplementation::SetEntropySourceStatus(OkStatus());
  ASSERT_NE(res.status(), OkStatus());
}

TEST(TLSClientMbedTLS, NonNullTerminatedServerName) {
  // Backing buffer has no trailing null terminator; reading past size() would
  // trigger an out-of-bounds read under ASan.
  const char server_name_buf[] = {
      'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm'};
  const std::string_view server_name(server_name_buf, sizeof(server_name_buf));
  auto options = SessionOptions()
                     .set_transport(stream::NullStream::Instance())
                     .set_server_name(server_name);
  auto res = Session::Create(options);
  ASSERT_EQ(res.status(), OkStatus());
  ASSERT_NE(res.value(), nullptr);
  delete res.value();
}

TEST(TLSClientMbedTLS, SubstringServerName) {
  const std::string_view full = "example.com/extra";
  const std::string_view server_name = full.substr(0, 11);
  auto options = SessionOptions()
                     .set_transport(stream::NullStream::Instance())
                     .set_server_name(server_name);
  auto res = Session::Create(options);
  ASSERT_EQ(res.status(), OkStatus());
  ASSERT_NE(res.value(), nullptr);
  delete res.value();
}

TEST(TLSClientMbedTLS, ServerNameTooLong) {
  std::array<char, 256> long_name;
  long_name.fill('a');
  auto options = SessionOptions()
                     .set_transport(stream::NullStream::Instance())
                     .set_server_name(
                         std::string_view(long_name.data(), long_name.size()));
  auto res = Session::Create(options);
  ASSERT_EQ(res.status(), Status::Internal());
}

TEST(TLSClientMbedTLS, ServerNameWithEmbeddedNull) {
  const char server_name_buf[] = "example.com\0extra";
  const std::string_view server_name(server_name_buf,
                                     sizeof(server_name_buf) - 1);
  auto options = SessionOptions()
                     .set_transport(stream::NullStream::Instance())
                     .set_server_name(server_name);
  auto res = Session::Create(options);
  ASSERT_EQ(res.status(), Status::Internal());
}

}  // namespace
}  // namespace pw::tls_client
