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

#include "pw_transport/framed_tcp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "pw_allocator/libc_allocator.h"
#include "pw_allocator/synchronized_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_buf/buf.h"
#include "pw_bytes/array.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_stream/socket_stream.h"
#include "pw_sync/mutex.h"
#include "pw_transport/socket.h"
#include "pw_unit_test/framework.h"

namespace pw::transport {
namespace {

using AcceptFuture = ReliableDatagramListener::AcceptFuture;
using ConnectFuture = ReliableDatagramConnector::ConnectFuture;

constexpr const char* kLocalhost = "127.0.0.1";

// Small enough for the test fixture to fit in the unit test framework's
// memory pool.
constexpr size_t kAllocatorSize = 10 * 1024;

std::string_view AsString(const ConstBuf& buffer) {
  return std::string_view(reinterpret_cast<const char*>(buffer.data()),
                          buffer.size());
}

// Reads exactly `buffer.size()` bytes from `stream`.
Status ReadExactly(stream::SocketStream& stream, ByteSpan buffer) {
  while (!buffer.empty()) {
    Result<ByteSpan> result = stream.Read(buffer);
    PW_TRY(result.status());
    buffer = buffer.subspan(result->size());
  }
  return OkStatus();
}

// Connects a TCP socket to `port` on the loopback interface. Returns the
// socket's file descriptor, or -1 on failure.
int ConnectToLoopback(uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    return -1;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (connect(fd,
              reinterpret_cast<const sockaddr*>(&address),
              static_cast<socklen_t>(sizeof(address))) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

class FramedTcpTest : public ::testing::Test {
 protected:
  FramedTcpTest() : allocator_(test_allocator_) {
    // The transport resolves futures from its own threads.
    dispatcher_.AllowBlocking();
  }

  void TearDown() override {
    // Each test destroys its sockets, listeners, and connectors, which must
    // free all of their memory.
    EXPECT_EQ(test_allocator_.metrics().allocated_bytes.value(), 0u);
  }

  // Runs the dispatcher until `future` completes and returns its value.
  template <typename Future>
  auto Wait(Future& future) {
    async2::FutureTask<Future&> task(future);
    dispatcher_.Post(task);
    dispatcher_.RunToCompletion();
    if constexpr (std::is_void_v<typename Future::value_type>) {
      return;
    } else {
      return std::move(task).value();
    }
  }

  // Connects `connector` to `listener` and sets `client` and `server` to the
  // two ends of the connection.
  Status Connect(FramedTcpListener& listener,
                 FramedTcpConnector& connector,
                 ReliableDatagramSocket& client,
                 ReliableDatagramSocket& server) {
    AcceptFuture accept = listener.Accept();
    ConnectFuture connect = connector.Connect();
    Result<ReliableDatagramSocket> connected = Wait(connect);
    PW_TRY(connected.status());
    Result<ReliableDatagramSocket> accepted = Wait(accept);
    PW_TRY(accepted.status());
    client = std::move(*connected);
    server = std::move(*accepted);
    return OkStatus();
  }

  // Sends `data` as one datagram, waiting for space if necessary.
  bool Send(const ReliableDatagramSocket& socket, std::string_view data) {
    ReserveWriteFuture reserve = socket.ReserveWrite(data.size());
    std::optional<WriteReservation> reservation = Wait(reserve);
    if (!reservation.has_value()) {
      return false;
    }
    if (!data.empty()) {
      std::memcpy(reservation->data(), data.data(), data.size());
    }
    return reservation->Commit(data.size());
  }

  // Receives one datagram. Returns a null buffer if the socket closed.
  ConstBuf Receive(const ReliableDatagramSocket& socket) {
    ReadFuture read = socket.Read();
    return Wait(read);
  }

  allocator::test::AllocatorForTest<kAllocatorSize> test_allocator_;
  allocator::SynchronizedAllocator<sync::Mutex> allocator_;
  async2::DispatcherForTest dispatcher_;
};

TEST_F(FramedTcpTest, Listen_PortZero_PicksPort) {
  FramedTcpListener listener(allocator_);
  EXPECT_EQ(listener.port(), 0u);
  PW_TEST_ASSERT_OK(listener.Listen());
  EXPECT_NE(listener.port(), 0u);
}

TEST_F(FramedTcpTest, Listen_AlreadyListening_FailsPrecondition) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  const uint16_t port = listener.port();

  EXPECT_EQ(listener.Listen(), Status::FailedPrecondition());
  EXPECT_EQ(listener.port(), port);
}

TEST_F(FramedTcpTest, Listen_PortInUse_CanRetry) {
  FramedTcpListener first(allocator_);
  PW_TEST_ASSERT_OK(first.Listen());

  FramedTcpListener second(allocator_);
  EXPECT_EQ(second.Listen(first.port()), Status::Unavailable());
  EXPECT_EQ(second.port(), 0u);
  PW_TEST_EXPECT_OK(second.Listen());
}

TEST_F(FramedTcpTest, SendAndReceive_BothDirections) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(allocator_, kLocalhost, listener.port());
  ReliableDatagramSocket client;
  ReliableDatagramSocket server;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client, server));

  ASSERT_TRUE(Send(client, "hello"));
  ConstBuf request = Receive(server);
  ASSERT_TRUE(request != nullptr);
  EXPECT_EQ(AsString(request), "hello");

  ASSERT_TRUE(Send(server, "world"));
  ConstBuf response = Receive(client);
  ASSERT_TRUE(response != nullptr);
  EXPECT_EQ(AsString(response), "world");
}

TEST_F(FramedTcpTest, SendAndReceive_ManyDatagramsInOrder) {
  // Single-entry queues make the I/O threads wait on each other often.
  FramedTcpOptions options;
  options.read_queue_depth = 1;
  options.write_queue_depth = 1;
  FramedTcpListener listener(allocator_, options);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(
      allocator_, kLocalhost, listener.port(), options);
  ReliableDatagramSocket client;
  ReliableDatagramSocket server;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client, server));

  constexpr int kDatagrams = 26;
  for (int i = 0; i < kDatagrams; ++i) {
    const char letter = static_cast<char>('a' + i);
    ASSERT_TRUE(Send(client, std::string_view(&letter, 1)));
  }
  for (int i = 0; i < kDatagrams; ++i) {
    const char letter = static_cast<char>('a' + i);
    ConstBuf received = Receive(server);
    ASSERT_TRUE(received != nullptr);
    EXPECT_EQ(AsString(received), std::string_view(&letter, 1));
  }
}

TEST_F(FramedTcpTest, SendAndReceive_EmptyDatagram) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(allocator_, kLocalhost, listener.port());
  ReliableDatagramSocket client;
  ReliableDatagramSocket server;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client, server));

  ASSERT_TRUE(Send(client, ""));
  ASSERT_TRUE(Send(client, "after"));

  ConstBuf empty = Receive(server);
  ASSERT_TRUE(empty != nullptr);
  EXPECT_EQ(empty.size(), 0u);
  ConstBuf after = Receive(server);
  ASSERT_TRUE(after != nullptr);
  EXPECT_EQ(AsString(after), "after");
}

TEST_F(FramedTcpTest, SendAndReceive_LargeDatagram) {
  // Large enough to take several reads to receive.
  constexpr size_t kSize = 1024 * 1024;
  FramedTcpOptions options;
  options.max_message_size_bytes = kSize;
  Allocator& allocator = allocator::GetLibCAllocator();
  FramedTcpListener listener(allocator, options);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(allocator, kLocalhost, listener.port(), options);
  ReliableDatagramSocket client;
  ReliableDatagramSocket server;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client, server));

  auto pattern = [](size_t i) { return static_cast<std::byte>(i % 251); };
  ReserveWriteFuture reserve = client.ReserveWrite(kSize);
  std::optional<WriteReservation> reservation = Wait(reserve);
  ASSERT_TRUE(reservation.has_value());
  for (size_t i = 0; i < kSize; ++i) {
    (*reservation)[i] = pattern(i);
  }
  ASSERT_TRUE(reservation->Commit(kSize));

  ConstBuf received = Receive(server);
  ASSERT_EQ(received.size(), kSize);
  size_t mismatches = 0;
  for (size_t i = 0; i < kSize; ++i) {
    if (received[i] != pattern(i)) {
      ++mismatches;
    }
  }
  EXPECT_EQ(mismatches, 0u);
}

TEST_F(FramedTcpTest, SendAndReceive_ThroughMultipleConnections) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(allocator_, kLocalhost, listener.port());
  ReliableDatagramSocket client_1;
  ReliableDatagramSocket server_1;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client_1, server_1));
  ReliableDatagramSocket client_2;
  ReliableDatagramSocket server_2;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client_2, server_2));

  ASSERT_TRUE(Send(client_2, "two"));
  ASSERT_TRUE(Send(client_1, "one"));

  ConstBuf received_1 = Receive(server_1);
  ASSERT_TRUE(received_1 != nullptr);
  EXPECT_EQ(AsString(received_1), "one");
  ConstBuf received_2 = Receive(server_2);
  ASSERT_TRUE(received_2 != nullptr);
  EXPECT_EQ(AsString(received_2), "two");
}

TEST_F(FramedTcpTest, Sockets_OutliveListenerAndConnector) {
  ReliableDatagramSocket client;
  ReliableDatagramSocket server;
  {
    FramedTcpListener listener(allocator_);
    PW_TEST_ASSERT_OK(listener.Listen());
    FramedTcpConnector connector(allocator_, kLocalhost, listener.port());
    PW_TEST_ASSERT_OK(Connect(listener, connector, client, server));
  }

  ASSERT_TRUE(Send(client, "still open"));
  ConstBuf received = Receive(server);
  ASSERT_TRUE(received != nullptr);
  EXPECT_EQ(AsString(received), "still open");
}

TEST_F(FramedTcpTest, TryReserveWrite_LimitedByWriteQueueDepth) {
  FramedTcpOptions options;
  options.write_queue_depth = 2;
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(
      allocator_, kLocalhost, listener.port(), options);
  ReliableDatagramSocket client;
  ReliableDatagramSocket server;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client, server));

  std::optional<WriteReservation> first = client.TryReserveWrite(1);
  std::optional<WriteReservation> second = client.TryReserveWrite(1);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(client.TryReserveWrite(1).has_value());

  // Empty datagrams have no buffer, so they are not limited.
  EXPECT_TRUE(client.TryReserveWrite(0).has_value());

  first->Cancel();
  EXPECT_TRUE(client.TryReserveWrite(1).has_value());
}

TEST_F(FramedTcpTest, Close_SendsCommittedDatagramsFirst) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(allocator_, kLocalhost, listener.port());
  ReliableDatagramSocket client;
  ReliableDatagramSocket server;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client, server));

  constexpr std::array<std::string_view, 3> kDatagrams = {
      "one", "two", "three"};
  for (std::string_view datagram : kDatagrams) {
    std::optional<WriteReservation> reservation =
        client.TryReserveWrite(datagram.size());
    ASSERT_TRUE(reservation.has_value());
    std::memcpy(reservation->data(), datagram.data(), datagram.size());
    ASSERT_TRUE(reservation->Commit(datagram.size()));
  }
  CloseFuture close = client.Close();
  Wait(close);

  for (std::string_view datagram : kDatagrams) {
    ConstBuf received = Receive(server);
    ASSERT_TRUE(received != nullptr);
    EXPECT_EQ(AsString(received), datagram);
  }
  EXPECT_TRUE(Receive(server) == nullptr);
}

TEST_F(FramedTcpTest, Close_CommitAfterCloseFails) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(allocator_, kLocalhost, listener.port());
  ReliableDatagramSocket client;
  ReliableDatagramSocket server;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client, server));

  std::optional<WriteReservation> reservation = client.TryReserveWrite(4);
  ASSERT_TRUE(reservation.has_value());
  CloseFuture close = client.Close();
  EXPECT_FALSE(reservation->Commit(4));
  Wait(close);

  EXPECT_TRUE(Receive(server) == nullptr);
}

TEST_F(FramedTcpTest, Close_WhileReadQueueIsFull) {
  FramedTcpOptions options;
  options.read_queue_depth = 1;
  FramedTcpListener listener(allocator_, options);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(allocator_, kLocalhost, listener.port());
  ReliableDatagramSocket client;
  ReliableDatagramSocket server;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client, server));

  ASSERT_TRUE(Send(client, "first"));
  ASSERT_TRUE(Send(client, "second"));
  ASSERT_TRUE(Send(client, "third"));

  // Wait for the first datagram so that the read queue is known to have been
  // full while the read thread waited.
  ConstBuf first = Receive(server);
  ASSERT_TRUE(first != nullptr);
  EXPECT_EQ(AsString(first), "first");

  CloseFuture close = server.Close();
  Wait(close);
}

TEST_F(FramedTcpTest, Close_DiscardsUnreadDatagrams) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  AcceptFuture accept = listener.Accept();
  const int peer_fd = ConnectToLoopback(listener.port());
  ASSERT_NE(peer_fd, -1);
  stream::SocketStream peer(peer_fd);
  Result<ReliableDatagramSocket> server = Wait(accept);
  PW_TEST_ASSERT_OK(server);

  // Start a read before the socket closes.
  ReadFuture read = server->Read();

  constexpr auto kFrame =
      bytes::Array<0, 0, 0, 6, 'u', 'n', 'r', 'e', 'a', 'd'>();
  PW_TEST_ASSERT_OK(peer.Write(kFrame));

  // Once commits fail, the server has seen that the connection ended, so it
  // has received the datagram. The socket stays open until it is read.
  ASSERT_EQ(shutdown(peer_fd, SHUT_WR), 0);
  while (Send(*server, "x")) {
  }

  CloseFuture close = server->Close();
  Wait(close);
  EXPECT_TRUE(Wait(read) == nullptr);
}

TEST_F(FramedTcpTest, PeerDisconnect_ClosesSocket) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(allocator_, kLocalhost, listener.port());
  ReliableDatagramSocket client;
  ReliableDatagramSocket server;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client, server));

  // Dropping the last handle closes the connection.
  client = nullptr;

  EXPECT_TRUE(Receive(server) == nullptr);
  CloseFuture closed = server.WhenClosed();
  Wait(closed);
}

TEST_F(FramedTcpTest, PeerDisconnect_ReceivedDatagramsCanBeRead) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  AcceptFuture accept = listener.Accept();
  const int peer_fd = ConnectToLoopback(listener.port());
  ASSERT_NE(peer_fd, -1);
  stream::SocketStream peer(peer_fd);
  Result<ReliableDatagramSocket> server = Wait(accept);
  PW_TEST_ASSERT_OK(server);

  constexpr auto kFrames =
      bytes::Array<0, 0, 0, 3, 'o', 'n', 'e', 0, 0, 0, 3, 't', 'w', 'o'>();
  PW_TEST_ASSERT_OK(peer.Write(kFrames));

  // The peer stops sending but keeps receiving, so the server's datagrams are
  // sent successfully until the server sees that the connection ended. After
  // that, commits fail.
  ASSERT_EQ(shutdown(peer_fd, SHUT_WR), 0);
  while (Send(*server, "x")) {
  }

  ConstBuf one = Receive(*server);
  ASSERT_TRUE(one != nullptr);
  EXPECT_EQ(AsString(one), "one");
  ConstBuf two = Receive(*server);
  ASSERT_TRUE(two != nullptr);
  EXPECT_EQ(AsString(two), "two");

  EXPECT_TRUE(Receive(*server) == nullptr);
  CloseFuture closed = server->WhenClosed();
  Wait(closed);
}

TEST_F(FramedTcpTest, OversizedDatagram_ClosesSocket) {
  FramedTcpOptions options;
  options.max_message_size_bytes = 4;
  FramedTcpListener listener(allocator_, options);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(allocator_, kLocalhost, listener.port());
  ReliableDatagramSocket client;
  ReliableDatagramSocket server;
  PW_TEST_ASSERT_OK(Connect(listener, connector, client, server));

  ASSERT_TRUE(Send(client, "too long"));
  EXPECT_TRUE(Receive(server) == nullptr);
}

TEST_F(FramedTcpTest, Accept_BeforeListen_WaitsForConnection) {
  FramedTcpListener listener(allocator_);
  AcceptFuture accept = listener.Accept();
  EXPECT_TRUE(dispatcher_.RunInTaskUntilStalled(accept).IsPending());

  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(allocator_, kLocalhost, listener.port());
  ConnectFuture connect = connector.Connect();
  Result<ReliableDatagramSocket> client = Wait(connect);
  PW_TEST_ASSERT_OK(client);
  PW_TEST_EXPECT_OK(Wait(accept));
}

TEST_F(FramedTcpTest, Accept_AfterPeerConnects_ReturnsConnection) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  FramedTcpConnector connector(allocator_, kLocalhost, listener.port());

  ConnectFuture connect = connector.Connect();
  Result<ReliableDatagramSocket> client = Wait(connect);
  PW_TEST_ASSERT_OK(client);

  AcceptFuture accept = listener.Accept();
  Result<ReliableDatagramSocket> server = Wait(accept);
  PW_TEST_ASSERT_OK(server);

  ASSERT_TRUE(Send(*client, "hello"));
  ConstBuf received = Receive(*server);
  ASSERT_TRUE(received != nullptr);
  EXPECT_EQ(AsString(received), "hello");
}

TEST_F(FramedTcpTest, DestroyListener_FailsPendingAccept) {
  std::optional<FramedTcpListener> listener;
  listener.emplace(allocator_);
  PW_TEST_ASSERT_OK(listener->Listen());
  AcceptFuture accept = listener->Accept();

  listener.reset();
  EXPECT_EQ(Wait(accept).status(), Status::FailedPrecondition());
}

TEST_F(FramedTcpTest, DestroyListener_BeforeListen_FailsPendingAccept) {
  std::optional<FramedTcpListener> listener;
  listener.emplace(allocator_);
  AcceptFuture accept = listener->Accept();

  listener.reset();
  EXPECT_EQ(Wait(accept).status(), Status::FailedPrecondition());
}

TEST_F(FramedTcpTest, Connect_NothingListening_Unavailable) {
  uint16_t port = 0;
  {
    FramedTcpListener listener(allocator_);
    PW_TEST_ASSERT_OK(listener.Listen());
    port = listener.port();
  }

  FramedTcpConnector connector(allocator_, kLocalhost, port);
  ConnectFuture connect = connector.Connect();
  EXPECT_EQ(Wait(connect).status(), Status::Unavailable());
}

TEST_F(FramedTcpTest, DestroyConnector_ResolvesPendingConnect) {
  std::optional<FramedTcpConnector> connector;
  // A documentation address (RFC 5737) that never answers. Depending on the
  // network, the connection either fails at once or waits until canceled.
  connector.emplace(allocator_, "192.0.2.1", 1);
  ConnectFuture connect = connector->Connect();

  connector.reset();
  const Status status = Wait(connect).status();
  EXPECT_TRUE(status.IsFailedPrecondition() || status.IsUnavailable());
}

TEST_F(FramedTcpTest, WireFormat_Receive) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  AcceptFuture accept = listener.Accept();
  stream::SocketStream peer;
  PW_TEST_ASSERT_OK(peer.Connect(kLocalhost, listener.port()));
  Result<ReliableDatagramSocket> server = Wait(accept);
  PW_TEST_ASSERT_OK(server);

  // "hello" followed by an empty datagram.
  constexpr auto kFrames =
      bytes::Array<0, 0, 0, 5, 'h', 'e', 'l', 'l', 'o', 0, 0, 0, 0>();
  PW_TEST_ASSERT_OK(peer.Write(kFrames));

  ConstBuf hello = Receive(*server);
  ASSERT_TRUE(hello != nullptr);
  EXPECT_EQ(AsString(hello), "hello");
  ConstBuf empty = Receive(*server);
  ASSERT_TRUE(empty != nullptr);
  EXPECT_EQ(empty.size(), 0u);
}

TEST_F(FramedTcpTest, WireFormat_Send) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  AcceptFuture accept = listener.Accept();
  stream::SocketStream peer;
  PW_TEST_ASSERT_OK(peer.Connect(kLocalhost, listener.port()));
  Result<ReliableDatagramSocket> server = Wait(accept);
  PW_TEST_ASSERT_OK(server);

  ASSERT_TRUE(Send(*server, "abc"));

  constexpr auto kExpected = bytes::Array<0, 0, 0, 3, 'a', 'b', 'c'>();
  std::array<std::byte, kExpected.size()> frame{};
  PW_TEST_ASSERT_OK(ReadExactly(peer, frame));
  EXPECT_TRUE(frame == kExpected);
}

TEST_F(FramedTcpTest, WireFormat_PeerDisconnectsMidDatagram) {
  FramedTcpListener listener(allocator_);
  PW_TEST_ASSERT_OK(listener.Listen());
  AcceptFuture accept = listener.Accept();
  stream::SocketStream peer;
  PW_TEST_ASSERT_OK(peer.Connect(kLocalhost, listener.port()));
  Result<ReliableDatagramSocket> server = Wait(accept);
  PW_TEST_ASSERT_OK(server);

  // A 10-byte datagram that ends after 3 bytes.
  constexpr auto kPartialFrame = bytes::Array<0, 0, 0, 10, 'a', 'b', 'c'>();
  PW_TEST_ASSERT_OK(peer.Write(kPartialFrame));
  peer.Close();

  EXPECT_TRUE(Receive(*server) == nullptr);
}

TEST_F(FramedTcpTest, DropSocket_WhileSendIsBlocked) {
  // Larger than the TCP buffers, so sending blocks until the peer reads.
  constexpr size_t kSize = 16 * 1024 * 1024;
  FramedTcpOptions options;
  options.max_message_size_bytes = kSize;
  FramedTcpListener listener(allocator::GetLibCAllocator(), options);
  PW_TEST_ASSERT_OK(listener.Listen());
  AcceptFuture accept = listener.Accept();
  stream::SocketStream peer;
  PW_TEST_ASSERT_OK(peer.Connect(kLocalhost, listener.port()));
  Result<ReliableDatagramSocket> server = Wait(accept);
  PW_TEST_ASSERT_OK(server);

  std::optional<WriteReservation> reservation = server->TryReserveWrite(kSize);
  ASSERT_TRUE(reservation.has_value());
  std::memset(reservation->data(), 0, kSize);
  ASSERT_TRUE(reservation->Commit(kSize));
  reservation.reset();

  // Once the frame starts arriving, the write thread is sending it and blocks
  // because the peer stops reading.
  std::array<std::byte, 4> header{};
  PW_TEST_ASSERT_OK(ReadExactly(peer, header));

  // Destroying the socket must interrupt the blocked send.
  *server = nullptr;
}

}  // namespace
}  // namespace pw::transport
