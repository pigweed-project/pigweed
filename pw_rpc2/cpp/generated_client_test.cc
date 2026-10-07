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

#include <cstddef>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_buf/buf.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/pw_rpc2_test.pwpb.rpc2.h"
#include "pw_rpc2/pw_rpc2_test.raw.rpc2.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/service_client.h"
#include "pw_rpc2/write_reservation.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2 {
namespace {

using EchoRequest = test::pwpb::EchoRequest::Message;
using EchoResponse = test::pwpb::EchoResponse::Message;

// Generated clients route calls with the service and method IDs from the
// public method descriptors, and the raw and pwpb clients of a service agree
// on them.
TEST(GeneratedClientTest, ClientsRouteCallsByMethodDescriptorIds) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  test::pw_rpc2::pwpb::TestEcho::Client pwpb_client(peer.client());
  test::pw_rpc2::raw::TestEcho::Client raw_client(peer.client());

  async2::FutureTask unary_call(pwpb_client.EchoUnary(EchoRequest{.val = 1}));
  dispatcher.Post(unary_call);
  auto call0 =
      peer.ExpectInvocation<test::pw_rpc2::pwpb::TestEcho::EchoUnary>();

  async2::FutureTask server_stream_call(
      pwpb_client.CountUpServerStream(EchoRequest{.val = 3}));
  dispatcher.Post(server_stream_call);
  auto call1 = peer.ExpectInvocation<
      test::pw_rpc2::pwpb::TestEcho::CountUpServerStream>();

  async2::FutureTask raw_call(raw_client.AccumulateClientStream());
  dispatcher.Post(raw_call);
  // Claim the raw client's call with the pwpb method descriptor to verify that
  // the raw and pwpb clients route with the same service and method IDs.
  auto call2 = peer.ExpectInvocation<
      test::pw_rpc2::pwpb::TestEcho::AccumulateClientStream>();

  // The clients draw call IDs from the one shared connection.
  EXPECT_NE(call0.call_id(), call1.call_id());
  EXPECT_NE(call1.call_id(), call2.call_id());
  EXPECT_NE(call0.call_id(), call2.call_id());
}

TEST(GeneratedPwpbClientTest, ServerStreamSendsRequestAndReadsMessages) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  test::pw_rpc2::pwpb::TestEcho::Client client(peer.client());

  auto reader = test::RunToCompletion(
      dispatcher, client.CountUpServerStream(EchoRequest{.val = 10}));
  PW_TEST_ASSERT_OK(reader);

  auto call = peer.ExpectInvocation<
      test::pw_rpc2::pwpb::TestEcho::CountUpServerStream>();
  auto request = call.request();
  PW_TEST_ASSERT_OK(request);
  EXPECT_EQ(request->val, 10u);

  call.Write(EchoResponse{.val = 11});
  auto first = test::RunToCompletion(dispatcher, reader->Read());
  PW_TEST_ASSERT_OK(first);
  EXPECT_EQ(first->val, 11u);

  call.Write(EchoResponse{.val = 12});
  auto second = test::RunToCompletion(dispatcher, reader->Read());
  PW_TEST_ASSERT_OK(second);
  EXPECT_EQ(second->val, 12u);

  call.Finish();
  auto end = test::RunToCompletion(dispatcher, reader->Read());
  EXPECT_EQ(end.status(), Status::OutOfRange());
}

// Messages the server sends before the client reads wait in order until the
// client reads them, and the end of the stream follows the last one.
TEST(GeneratedPwpbClientTest, ServerStreamQueuesMessagesUntilRead) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  test::pw_rpc2::pwpb::TestEcho::Client client(peer.client());

  auto reader = test::RunToCompletion(
      dispatcher, client.CountUpServerStream(EchoRequest{.val = 10}));
  PW_TEST_ASSERT_OK(reader);

  auto call = peer.ExpectInvocation<
      test::pw_rpc2::pwpb::TestEcho::CountUpServerStream>();
  call.Write(EchoResponse{.val = 11});
  call.Write(EchoResponse{.val = 12});
  call.Finish();

  auto first = test::RunToCompletion(dispatcher, reader->Read());
  PW_TEST_ASSERT_OK(first);
  EXPECT_EQ(first->val, 11u);

  auto second = test::RunToCompletion(dispatcher, reader->Read());
  PW_TEST_ASSERT_OK(second);
  EXPECT_EQ(second->val, 12u);

  auto end = test::RunToCompletion(dispatcher, reader->Read());
  EXPECT_EQ(end.status(), Status::OutOfRange());
}

TEST(GeneratedPwpbClientTest, ClientStreamWritesMessagesAndReceivesResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  test::pw_rpc2::pwpb::TestEcho::Client client(peer.client());

  auto stream =
      test::RunToCompletion(dispatcher, client.AccumulateClientStream());
  PW_TEST_ASSERT_OK(stream);

  auto call = peer.ExpectInvocation<
      test::pw_rpc2::pwpb::TestEcho::AccumulateClientStream>();
  EXPECT_EQ(call.stream_message_count(), 0u);

  PW_TEST_ASSERT_OK(test::RunToCompletion(
      dispatcher, stream->writer().Write(EchoRequest{.val = 20})));
  PW_TEST_ASSERT_OK(test::RunToCompletion(
      dispatcher, stream->writer().Write(EchoRequest{.val = 22})));

  // The peer receives the streamed requests in order.
  ASSERT_EQ(call.stream_message_count(), 2u);
  auto first = call.stream_message(0);
  PW_TEST_ASSERT_OK(first);
  EXPECT_EQ(first->val, 20u);
  auto second = call.stream_message(1);
  PW_TEST_ASSERT_OK(second);
  EXPECT_EQ(second->val, 22u);
  EXPECT_EQ(call.stream_message(2).status(), Status::OutOfRange());

  EXPECT_FALSE(call.client_stream_finished());
  PW_TEST_ASSERT_OK(
      test::RunToCompletion(dispatcher, stream->writer().Finish()));
  EXPECT_TRUE(call.client_stream_finished());

  async2::FutureTask response(std::move(*stream).response());
  dispatcher.Post(response);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(response.has_value());

  call.Finish(EchoResponse{.val = 42});
  ASSERT_TRUE(response.has_value());
  PW_TEST_ASSERT_OK(response.value());
  EXPECT_EQ(response.value()->val, 42u);
}

TEST(GeneratedPwpbClientTest, BidiStreamExchangesMessages) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  test::pw_rpc2::pwpb::TestEcho::Client client(peer.client());

  auto stream = test::RunToCompletion(dispatcher, client.EchoBidiStream());
  PW_TEST_ASSERT_OK(stream);

  auto call =
      peer.ExpectInvocation<test::pw_rpc2::pwpb::TestEcho::EchoBidiStream>();

  PW_TEST_ASSERT_OK(test::RunToCompletion(
      dispatcher, stream->writer().Write(EchoRequest{.val = 7})));
  ASSERT_EQ(call.stream_message_count(), 1u);
  auto request = call.stream_message(0);
  PW_TEST_ASSERT_OK(request);
  EXPECT_EQ(request->val, 7u);

  call.Write(EchoResponse{.val = 14});
  auto response = test::RunToCompletion(dispatcher, stream->reader().Read());
  PW_TEST_ASSERT_OK(response);
  EXPECT_EQ(response->val, 14u);

  PW_TEST_ASSERT_OK(
      test::RunToCompletion(dispatcher, stream->writer().Finish()));
  EXPECT_TRUE(call.client_stream_finished());

  call.Finish();
  auto end = test::RunToCompletion(dispatcher, stream->reader().Read());
  EXPECT_EQ(end.status(), Status::OutOfRange());
}

// Raw unary and server-streaming calls reserve the request in place: await
// the reservation future, write the payload, and `Commit()` it to send the
// request and obtain the reader.
constexpr size_t kMaxRawPayloadSize = 16;

TEST(GeneratedRawClientTest, UnaryCallSendsReservedRequestAndReadsResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  test::pw_rpc2::raw::TestEcho::Client client(peer.client());

  auto reservation =
      test::RunToCompletion(dispatcher, client.EchoUnary(kMaxRawPayloadSize));
  PW_TEST_ASSERT_OK(reservation);
  EXPECT_GE(reservation->size(), kMaxRawPayloadSize);

  constexpr std::byte kRequest[] = {std::byte{0x12}, std::byte{0x34}};
  Result<RawResponseFuture> response_future =
      test::CommitCopy(*reservation, kRequest);
  PW_TEST_ASSERT_OK(response_future);

  async2::FutureTask response(std::move(*response_future));
  dispatcher.Post(response);

  auto invocation =
      peer.ExpectInvocation<test::pw_rpc2::raw::TestEcho::EchoUnary>();
  EXPECT_FALSE(response.has_value());
  auto request = invocation.request();
  PW_TEST_ASSERT_OK(request);
  test::ExpectBytes(*request, kRequest);

  constexpr std::byte kResponse[] = {std::byte{0x56}, std::byte{0x78}};
  invocation.Finish(ConstBuf::Unowned(kResponse));
  ASSERT_TRUE(response.has_value());
  PW_TEST_ASSERT_OK(response.value());
  test::ExpectBytes(*response.value(), kResponse);
}

TEST(GeneratedRawClientTest, UnaryCallWithEmptyPayloads) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  test::pw_rpc2::raw::TestEcho::Client client(peer.client());

  auto reservation = test::RunToCompletion(dispatcher, client.EchoUnary(0));
  PW_TEST_ASSERT_OK(reservation);
  Result<RawResponseFuture> response_future = reservation->Commit(0);
  PW_TEST_ASSERT_OK(response_future);

  async2::FutureTask response(std::move(*response_future));
  dispatcher.Post(response);

  auto invocation =
      peer.ExpectInvocation<test::pw_rpc2::raw::TestEcho::EchoUnary>();
  auto request = invocation.request();
  PW_TEST_ASSERT_OK(request);
  EXPECT_EQ(request->size(), 0u);

  invocation.Finish(ConstBuf());
  ASSERT_TRUE(response.has_value());
  PW_TEST_ASSERT_OK(response.value());
  EXPECT_EQ(response.value()->size(), 0u);
}

TEST(GeneratedRawClientTest, ServerStreamSendsReservedRequestAndReadsMessages) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  test::pw_rpc2::raw::TestEcho::Client client(peer.client());

  auto reservation = test::RunToCompletion(
      dispatcher, client.CountUpServerStream(kMaxRawPayloadSize));
  PW_TEST_ASSERT_OK(reservation);

  constexpr std::byte kRequest[] = {std::byte{0x01}};
  Result<RawReader> reader = test::CommitCopy(*reservation, kRequest);
  PW_TEST_ASSERT_OK(reader);

  auto invocation = peer.ExpectInvocation<
      test::pw_rpc2::raw::TestEcho::CountUpServerStream>();
  auto request = invocation.request();
  PW_TEST_ASSERT_OK(request);
  test::ExpectBytes(*request, kRequest);

  constexpr std::byte kFirst[] = {std::byte{0xA1}};
  invocation.Write(ConstBuf::Unowned(kFirst));
  auto first = test::RunToCompletion(dispatcher, reader->Read());
  PW_TEST_ASSERT_OK(first);
  test::ExpectBytes(*first, kFirst);

  constexpr std::byte kSecond[] = {std::byte{0xB1}, std::byte{0xB2}};
  invocation.Write(ConstBuf::Unowned(kSecond));
  auto second = test::RunToCompletion(dispatcher, reader->Read());
  PW_TEST_ASSERT_OK(second);
  test::ExpectBytes(*second, kSecond);

  invocation.Finish();
  auto end = test::RunToCompletion(dispatcher, reader->Read());
  EXPECT_EQ(end.status(), Status::OutOfRange());
}

TEST(GeneratedRawClientTest, ClientStreamWritesMessagesAndReceivesResponse) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  test::pw_rpc2::raw::TestEcho::Client client(peer.client());

  auto stream =
      test::RunToCompletion(dispatcher, client.AccumulateClientStream());
  PW_TEST_ASSERT_OK(stream);

  auto invocation = peer.ExpectInvocation<
      test::pw_rpc2::raw::TestEcho::AccumulateClientStream>();

  static constexpr std::byte kFirst[] = {std::byte{0x01}, std::byte{0x02}};
  static constexpr std::byte kSecond[] = {std::byte{0x03}};
  PW_TEST_ASSERT_OK(
      test::RunToCompletion(dispatcher, stream->writer().WriteCopy(kFirst)));
  PW_TEST_ASSERT_OK(
      test::RunToCompletion(dispatcher, stream->writer().WriteCopy(kSecond)));

  ASSERT_EQ(invocation.stream_message_count(), 2u);
  auto first = invocation.stream_message(0);
  PW_TEST_ASSERT_OK(first);
  test::ExpectBytes(*first, kFirst);
  auto second = invocation.stream_message(1);
  PW_TEST_ASSERT_OK(second);
  test::ExpectBytes(*second, kSecond);

  EXPECT_FALSE(invocation.client_stream_finished());
  PW_TEST_ASSERT_OK(
      test::RunToCompletion(dispatcher, stream->writer().Finish()));
  EXPECT_TRUE(invocation.client_stream_finished());

  async2::FutureTask response(std::move(*stream).response());
  dispatcher.Post(response);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(response.has_value());

  constexpr std::byte kResponse[] = {std::byte{0x99}};
  invocation.Finish(ConstBuf::Unowned(kResponse));
  ASSERT_TRUE(response.has_value());
  PW_TEST_ASSERT_OK(response.value());
  test::ExpectBytes(*response.value(), kResponse);
}

TEST(GeneratedRawClientTest, BidiStreamExchangesMessages) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto peer = test::MakeMockPeer(dispatcher, allocator);
  test::pw_rpc2::raw::TestEcho::Client client(peer.client());

  auto stream = test::RunToCompletion(dispatcher, client.EchoBidiStream());
  PW_TEST_ASSERT_OK(stream);

  auto invocation =
      peer.ExpectInvocation<test::pw_rpc2::raw::TestEcho::EchoBidiStream>();

  // Write the request in place through a reservation.
  constexpr std::byte kRequest[] = {std::byte{0x07}, std::byte{0x08}};
  auto reservation = test::RunToCompletion(
      dispatcher, stream->writer().ReserveWrite(sizeof(kRequest)));
  PW_TEST_ASSERT_OK(reservation);
  PW_TEST_ASSERT_OK(test::CommitCopy(*reservation, kRequest));
  dispatcher.RunUntilStalled();

  ASSERT_EQ(invocation.stream_message_count(), 1u);
  auto request = invocation.stream_message(0);
  PW_TEST_ASSERT_OK(request);
  test::ExpectBytes(*request, kRequest);

  constexpr std::byte kResponse[] = {std::byte{0x14}};
  invocation.Write(ConstBuf::Unowned(kResponse));
  auto response = test::RunToCompletion(dispatcher, stream->reader().Read());
  PW_TEST_ASSERT_OK(response);
  test::ExpectBytes(*response, kResponse);

  PW_TEST_ASSERT_OK(
      test::RunToCompletion(dispatcher, stream->writer().Finish()));
  EXPECT_TRUE(invocation.client_stream_finished());

  invocation.Finish();
  auto end = test::RunToCompletion(dispatcher, stream->reader().Read());
  EXPECT_EQ(end.status(), Status::OutOfRange());
}

}  // namespace
}  // namespace pw::rpc2
