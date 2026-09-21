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

#include "pw_transport/transport.h"

#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include "pw_allocator/testing.h"
#include "pw_async2/await.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_bytes/span.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_transport/socket.h"
#include "pw_unit_test/framework.h"

namespace pw::transport {
namespace {

/// A mock transport that vends connections.
class MockTransport : public ReliableDatagramListener,
                      public ReliableDatagramConnector {
 public:
  using ReliableDatagramListener::WrapSocket;
  ~MockTransport() override {
    accept_provider_.Resolve(Status::FailedPrecondition());
    connect_provider_.Resolve(Status::FailedPrecondition());
  }

  AcceptFuture Accept() override { return accept_provider_.Get(); }
  ConnectFuture Connect() override { return connect_provider_.Get(); }

  void ResolveAccept(Result<ReliableDatagramSocket>&& connection) {
    accept_provider_.Resolve(std::move(connection));
  }

  void ResolveConnect(Result<ReliableDatagramSocket>&& connection) {
    connect_provider_.Resolve(std::move(connection));
  }

 private:
  async2::ValueProvider<Result<ReliableDatagramSocket>> accept_provider_;
  async2::ValueProvider<Result<ReliableDatagramSocket>> connect_provider_;
};

/// A mock connection that acts as a one-deep loopback queue.
/// Writes feed directly into reads.
class MockSocket : public ReliableDatagramSocketImpl {
 public:
  friend class MockLayeredSocket;

  // Only exposed for unit testing.
  using ReliableDatagramSocketImpl::Adopt;
  using ReliableDatagramSocketImpl::AdoptLocked;

  static ReliableDatagramSocket Allocated(Allocator& allocator,
                                          bool* destructed = nullptr) {
    auto* ptr = allocator.New<MockSocket>(allocator, allocator, destructed);
    return ptr != nullptr ? MockTransport::WrapSocket(*ptr)
                          : ReliableDatagramSocket();
  }

  MockSocket(Allocator& connection_allocator,
             Allocator& packet_allocator,
             bool* destructed = nullptr)
      : ReliableDatagramSocketImpl(connection_allocator, 1500),
        allocator_(packet_allocator),
        block_writes_(false),
        destructed_(destructed) {}

  ~MockSocket() override {
    if (destructed_) {
      *destructed_ = true;
    }
  }

  void lock() const PW_EXCLUSIVE_LOCK_FUNCTION() override { lock_.lock(); }
  void unlock() const PW_UNLOCK_FUNCTION() override { lock_.unlock(); }

  async2::Poll<pw::ConstBuf> DoRead() override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    if (packet_.has_value()) {
      pw::ConstBuf buffer(std::move(*packet_));
      packet_.reset();
      WakeOneWriter();
      return async2::Ready(std::move(buffer));
    }
    return async2::Pending();
  }

  std::optional<WriteReservation> DoTryReserveWrite(size_t min_bytes) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    if (block_writes_) {
      return std::nullopt;
    }
    if (available_bytes_.has_value()) {
      if (min_bytes > *available_bytes_) {
        return std::nullopt;
      }
      *available_bytes_ -= min_bytes;
    }
    return CreateMockReservation(min_bytes);
  }

  void DoClose() override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) { MarkClosed(); }

  void SetAvailableBytes(std::optional<size_t> bytes) PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    available_bytes_ = bytes;
  }

  void WakeOneWriterLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    WakeOneWriter();
  }

  void SetBlockWrites(bool block) PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    block_writes_ = block;
    if (!block_writes_) {
      WakeAllWriters();
    }
  }

  void SimulateClose() PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    MarkClosed();
  }

  void set_fail_commits(bool fail) PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    fail_commits_ = fail;
  }

  void EnqueueForRead(pw::ConstBuf buffer) PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    packet_ = std::move(buffer);
    WakeReader();
  }

  size_t commit_count() const PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    return commit_count_;
  }
  bool HasReadPacket() const PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    return packet_.has_value();
  }

  pw::ConstBuf PopPacket() PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    pw::ConstBuf p;
    if (packet_.has_value()) {
      p = std::move(*packet_);
      packet_.reset();
      WakeOneWriter();
    }
    return p;
  }

 protected:
  bool DoCommitWrite(pw::Buf&& buffer) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    if (fail_commits_) {
      return false;
    }
    commit_count_++;
    packet_ = pw::ConstBuf(std::move(buffer));
    WakeReader();
    return true;
  }

  void DoCancelWrite(pw::Buf&& buffer) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    if (available_bytes_.has_value()) {
      *available_bytes_ += buffer.size();
    }
  }

 private:
  WriteReservation CreateMockReservation(size_t size)
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return CreateReservation(pw::Buf::Allocate(allocator_, size));
  }

  Allocator& allocator_;
  std::optional<pw::ConstBuf> packet_ PW_GUARDED_BY(*this);
  size_t commit_count_ PW_GUARDED_BY(*this) = 0;
  bool fail_commits_ PW_GUARDED_BY(*this) = false;
  bool block_writes_ PW_GUARDED_BY(*this);
  std::optional<size_t> available_bytes_ PW_GUARDED_BY(*this);
  bool* destructed_;
  mutable sync::InterruptSpinLock lock_;
};

/// An example layered connection that wraps another connection and adds a
/// 4-byte length header and a 4-byte CRC checksum footer, demonstrating
/// zero-copy layered packet construction via Adopt().
class MockLayeredSocket : public ReliableDatagramSocketImpl {
 public:
  static constexpr size_t kHeaderSize = sizeof(uint32_t);
  static constexpr size_t kFooterSize = sizeof(uint32_t);
  static constexpr size_t kOverhead = kHeaderSize + kFooterSize;
  static constexpr uint32_t kCrcMagic = 0xDEADBEEF;

  static ReliableDatagramSocket Allocated(
      Allocator& allocator,
      ReliableDatagramSocket lower_connection,
      MockSocket* lower_mock = nullptr,
      bool* destructed = nullptr) {
    auto* ptr = allocator.New<MockLayeredSocket>(
        allocator, std::move(lower_connection), lower_mock, destructed);
    return ptr != nullptr ? MockTransport::WrapSocket(*ptr)
                          : ReliableDatagramSocket();
  }

  MockLayeredSocket(Allocator& connection_allocator,
                    ReliableDatagramSocket lower_connection,
                    MockSocket* lower_mock = nullptr,
                    bool* destructed = nullptr)
      : ReliableDatagramSocketImpl(
            connection_allocator,
            lower_connection.max_write_message_size_bytes() > kOverhead
                ? lower_connection.max_write_message_size_bytes() - kOverhead
                : 0,
            lower_connection.max_read_message_size_bytes() > kOverhead
                ? lower_connection.max_read_message_size_bytes() - kOverhead
                : 0),
        lower_connection_(std::move(lower_connection)),
        lower_mock_(lower_mock),
        destructed_(destructed) {}

  ~MockLayeredSocket() override {
    if (destructed_) {
      *destructed_ = true;
    }
  }

  void lock() const PW_EXCLUSIVE_LOCK_FUNCTION() override { lock_.lock(); }
  void unlock() const PW_UNLOCK_FUNCTION() override { lock_.unlock(); }

  async2::Poll<pw::ConstBuf> DoRead() override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    if (packet_.has_value()) {
      pw::ConstBuf buffer = std::move(*packet_);
      packet_.reset();
      return async2::Ready(std::move(buffer));
    }
    return async2::Pending();
  }

  std::optional<WriteReservation> DoTryReserveWrite(size_t min_bytes) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    auto res = lower_connection_.TryReserveWrite(min_bytes + kOverhead);
    if (!res.has_value()) {
      return std::nullopt;
    }
    AdoptLocked(*res, kHeaderSize, kFooterSize);
    return res;
  }

  void DoClose() override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    // NOTE: Invoking lower_connection_.Close() under *this lock creates a lock
    // hierarchy (MockLayeredSocket -> lower_connection_). Care must be
    // taken in multi-threaded environments to prevent reverse lock acquisition.
    lower_connection_.Close();
    MarkClosed();
  }

  void EnqueueForRead(pw::ConstBuf raw) {
    auto payload = ProcessRawPacket(std::move(raw));
    if (payload == nullptr) {
      return;
    }
    std::lock_guard lock(*this);
    packet_ = std::move(payload);
    WakeReader();
  }

  static pw::ConstBuf ProcessRawPacket(pw::ConstBuf raw) {
    if (raw.size() < kOverhead) {
      return nullptr;
    }
    uint32_t payload_len = 0;
    std::memcpy(&payload_len, raw.data(), sizeof(uint32_t));
    if (kOverhead + payload_len > raw.size()) {
      return nullptr;
    }
    uint32_t crc = 0;
    std::memcpy(&crc, raw.data() + kHeaderSize + payload_len, sizeof(uint32_t));
    if (crc != kCrcMagic) {
      return nullptr;
    }
    return pw::Slice(std::move(raw), kHeaderSize, payload_len);
  }

 protected:
  bool DoCommitWrite(pw::Buf&& buffer) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    pw::Buf packet_buf =
        pw::Reclaim(std::move(buffer), kHeaderSize, kFooterSize);
    uint32_t payload_len = static_cast<uint32_t>(packet_buf.size() - kOverhead);
    std::memcpy(packet_buf.data(), &payload_len, sizeof(uint32_t));
    uint32_t crc = kCrcMagic;
    std::memcpy(
        packet_buf.data() + kHeaderSize + payload_len, &crc, sizeof(uint32_t));
    // NOTE: Calling lower_mock_->CommitWrite() while holding *this lock creates
    // a lock hierarchy (MockLayeredSocket -> lower_connection_). Any RX
    // path from lower_mock_ delivering to this layer must not lock in reverse
    // order to prevent AB-BA deadlocks.
    if (lower_mock_ != nullptr) {
      return lower_mock_->CommitWrite(std::move(packet_buf));
    }
    return true;
  }

  void DoCancelWrite(pw::Buf&& buffer) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    pw::Buf packet_buf =
        pw::Reclaim(std::move(buffer), kHeaderSize, kFooterSize);
    if (lower_mock_ != nullptr) {
      lower_mock_->CancelWrite(std::move(packet_buf));
    }
  }

 private:
  ReliableDatagramSocket lower_connection_;
  MockSocket* lower_mock_;
  std::optional<pw::ConstBuf> packet_ PW_GUARDED_BY(*this);
  bool* destructed_;
  mutable sync::InterruptSpinLock lock_;
};

/// A task that reads a single packet and stores its result.
class ReaderTask : public async2::Task {
 public:
  ReaderTask(ReliableDatagramSocket connection)
      : connection_(std::move(connection)), started_(false), done_(false) {}

  const pw::ConstBuf& result() const { return result_; }
  bool done() const { return done_; }

 private:
  async2::Poll<> DoPend(async2::Context& cx) override {
    if (!started_) {
      read_future_ = connection_.Read();
      started_ = true;
    }

    PW_AWAIT(auto result, read_future_, cx);
    result_ = std::move(result);
    done_ = true;

    return async2::Ready();
  }

  ReliableDatagramSocket connection_;
  pw::ConstBuf result_;
  ReadFuture read_future_;
  bool started_;
  bool done_;
};

/// A task that reserves space and writes a configurable packet.
class WriterTask : public async2::Task {
 public:
  WriterTask(ReliableDatagramSocket connection, std::byte value, size_t size)
      : connection_(std::move(connection)),
        value_(value),
        size_(size),
        started_(false),
        result_(Status::Unknown()) {}

  const Status& result() const { return result_; }

 private:
  async2::Poll<> DoPend(async2::Context& cx) override {
    if (!started_) {
      reserve_future_ = connection_.ReserveWrite(size_);
      started_ = true;
    }

    PW_AWAIT(auto result, reserve_future_, cx);

    result_ = result.has_value() ? OkStatus() : Status::Aborted();
    if (result.has_value()) {
      WriteReservation reservation = std::move(result.value());
      EXPECT_GE(reservation.size(), size_);
      std::memset(reservation.data(), static_cast<int>(value_), size_);
      EXPECT_TRUE(reservation.Commit(size_));
    }

    return async2::Ready();
  }

  ReliableDatagramSocket connection_;
  std::byte value_;
  size_t size_;
  ReserveWriteFuture reserve_future_;
  bool started_;
  Status result_;
};

/// A task that listens for a connection.
class AcceptTask : public async2::Task {
 public:
  AcceptTask(ReliableDatagramListener& transport)
      : transport_(transport), result_(Status::Unknown()), started_(false) {}

  const Result<ReliableDatagramSocket>& result() const { return result_; }

 private:
  async2::Poll<> DoPend(async2::Context& cx) override {
    if (!started_) {
      accept_future_ = transport_.Accept();
      started_ = true;
    }

    PW_AWAIT(auto result, accept_future_, cx);

    result_ = std::move(result);
    return async2::Ready();
  }

  ReliableDatagramListener& transport_;
  Result<ReliableDatagramSocket> result_;
  ReliableDatagramListener::AcceptFuture accept_future_;
  bool started_;
};

/// A task that opens a connection.
class ConnectTask : public async2::Task {
 public:
  ConnectTask(ReliableDatagramConnector& transport)
      : transport_(transport), result_(Status::Unknown()), started_(false) {}

  const Result<ReliableDatagramSocket>& result() const { return result_; }

 private:
  async2::Poll<> DoPend(async2::Context& cx) override {
    if (!started_) {
      connect_future_ = transport_.Connect();
      started_ = true;
    }

    PW_AWAIT(auto result, connect_future_, cx);

    result_ = std::move(result);
    return async2::Ready();
  }

  ReliableDatagramConnector& transport_;
  Result<ReliableDatagramSocket> result_;
  ReliableDatagramConnector::ConnectFuture connect_future_;
  bool started_;
};

TEST(TransportTest, TaskInteractionFlow) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  ReliableDatagramSocket conn = MockSocket::Allocated(allocator);
  async2::DispatcherForTest dispatcher;

  dispatcher.AllowBlocking();

  std::byte fill_value{0xaa};
  size_t data_size = 5;

  ReaderTask reader(conn);
  WriterTask writer(conn, fill_value, data_size);

  dispatcher.Post(reader);
  dispatcher.Post(writer);

  dispatcher.RunToCompletion();

  // Verify data.
  const pw::ConstBuf& buf = reader.result();
  ASSERT_NE(buf, nullptr);
  ASSERT_FALSE(buf.empty());

  for (size_t i = 0; i < data_size; ++i) {
    EXPECT_EQ(buf[i], fill_value);
  }
}

TEST(TransportTest, AcceptFlow) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;

  MockTransport transport;
  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();

  AcceptTask task(transport);
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.result().status(), Status::Unknown());

  transport.ResolveAccept(MockSocket::Allocated(allocator));

  dispatcher.RunToCompletion();

  ASSERT_TRUE(task.result().ok());
  ReliableDatagramSocket conn = task.result().value();
  ASSERT_TRUE(conn);

  std::byte fill_value{0xbb};
  size_t data_size = 10;

  ReaderTask reader(conn);
  WriterTask writer(conn, fill_value, data_size);

  dispatcher.Post(reader);
  dispatcher.Post(writer);

  dispatcher.RunToCompletion();

  // Verify data.
  const pw::ConstBuf& buf = reader.result();
  ASSERT_NE(buf, nullptr);
  ASSERT_FALSE(buf.empty());

  for (size_t i = 0; i < data_size; ++i) {
    EXPECT_EQ(buf[i], fill_value);
  }
}

TEST(TransportTest, ConnectFlow) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;

  MockTransport transport;

  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();

  ConnectTask task(transport);
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.result().status(), Status::Unknown());

  transport.ResolveConnect(MockSocket::Allocated(allocator));

  dispatcher.RunToCompletion();

  ASSERT_TRUE(task.result().ok());
  ReliableDatagramSocket conn = task.result().value();
  ASSERT_TRUE(conn);

  std::byte fill_value{0xbb};
  size_t data_size = 10;

  ReaderTask reader(conn);
  WriterTask writer(conn, fill_value, data_size);

  dispatcher.Post(reader);
  dispatcher.Post(writer);

  dispatcher.RunToCompletion();

  // Verify data.
  const pw::ConstBuf& buf = reader.result();
  ASSERT_NE(buf, nullptr);
  ASSERT_FALSE(buf.empty());

  for (size_t i = 0; i < data_size; ++i) {
    EXPECT_EQ(buf[i], fill_value);
  }
}

TEST(TransportTest, ListenerDestructionResolvesWithFailedPrecondition) {
  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();

  std::optional<MockTransport> transport;
  transport.emplace();

  AcceptTask task(*transport);
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.result().status(), Status::Unknown());

  transport.reset();

  dispatcher.RunToCompletion();
  EXPECT_EQ(task.result().status(), Status::FailedPrecondition());
}

TEST(TransportTest, ConnectorDestructionResolvesWithFailedPrecondition) {
  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();

  std::optional<MockTransport> transport;
  transport.emplace();

  ConnectTask task(*transport);
  dispatcher.Post(task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(task.result().status(), Status::Unknown());

  transport.reset();

  dispatcher.RunToCompletion();
  EXPECT_EQ(task.result().status(), Status::FailedPrecondition());
}

TEST(TransportTest, ReadClosed) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  MockSocket* connection = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket conn = MockTransport::WrapSocket(*connection);
  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();

  ReaderTask reader(conn);
  dispatcher.Post(reader);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_FALSE(reader.done());

  connection->SimulateClose();

  dispatcher.RunToCompletion();

  EXPECT_TRUE(reader.done());
  EXPECT_EQ(reader.result(), nullptr);
}

TEST(TransportTest, WriteClosed) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  MockSocket* connection = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket conn = MockTransport::WrapSocket(*connection);
  connection->SetBlockWrites(true);

  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();

  WriterTask writer(conn, std::byte{0xcc}, 10);
  dispatcher.Post(writer);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  EXPECT_EQ(writer.result(), Status::Unknown());

  connection->SimulateClose();

  dispatcher.RunToCompletion();

  EXPECT_EQ(writer.result(), Status::Aborted());
}

TEST(TransportTest, CancelWriteReservation) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  ReliableDatagramSocket conn = MockSocket::Allocated(allocator);
  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();

  class CancelWriterTask : public async2::Task {
   public:
    CancelWriterTask(ReliableDatagramSocket connection)
        : connection_(std::move(connection)),
          started_(false),
          result_(Status::Unknown()) {}

    const Status& result() const { return result_; }

   private:
    async2::Poll<> DoPend(async2::Context& cx) override {
      if (!started_) {
        reserve_future_ = connection_.ReserveWrite(10);
        started_ = true;
      }

      PW_AWAIT(auto result, reserve_future_, cx);

      result_ = result.has_value() ? OkStatus() : Status::Aborted();
      if (result.has_value()) {
        reservation_ = std::move(result.value());
        reservation_->Cancel();
      }

      return async2::Ready();
    }

    ReliableDatagramSocket connection_;
    ReserveWriteFuture reserve_future_;
    std::optional<WriteReservation> reservation_;
    bool started_;
    Status result_;
  };

  const size_t connection_size = allocator.allocate_size();
  EXPECT_GT(connection_size, 0u);
  EXPECT_EQ(allocator.deallocate_size(), 0u);

  CancelWriterTask writer(conn);
  dispatcher.Post(writer);

  dispatcher.RunToCompletion();

  EXPECT_EQ(writer.result(), OkStatus());
  EXPECT_EQ(allocator.allocate_size(), allocator.deallocate_size());
}

TEST(TransportTest, ImplicitDropWriteReservation) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  ReliableDatagramSocket conn = MockSocket::Allocated(allocator);
  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();

  class ImplicitDropWriterTask : public async2::Task {
   public:
    ImplicitDropWriterTask(ReliableDatagramSocket connection)
        : connection_(std::move(connection)),
          started_(false),
          result_(Status::Unknown()) {}

    const Status& result() const { return result_; }

   private:
    async2::Poll<> DoPend(async2::Context& cx) override {
      if (!started_) {
        reserve_future_ = connection_.ReserveWrite(10);
        started_ = true;
      }

      PW_AWAIT(auto result, reserve_future_, cx);

      result_ = result.has_value() ? OkStatus() : Status::Aborted();
      if (result.has_value()) {
        // Retrieve and immediately let it go out of scope.
        WriteReservation reservation = std::move(result.value());
      }

      return async2::Ready();
    }

    ReliableDatagramSocket connection_;
    ReserveWriteFuture reserve_future_;
    bool started_;
    Status result_;
  };

  const size_t implicit_connection_size = allocator.allocate_size();
  EXPECT_GT(implicit_connection_size, 0u);
  EXPECT_EQ(allocator.deallocate_size(), 0u);

  ImplicitDropWriterTask writer(conn);
  dispatcher.Post(writer);
  dispatcher.RunToCompletion();

  EXPECT_EQ(writer.result(), OkStatus());
  EXPECT_EQ(allocator.allocate_size(), allocator.deallocate_size());
}

TEST(TransportTest, AllocatedConnectionDeallocatedOnZeroRefs) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  bool destructed = false;

  EXPECT_EQ(allocator.allocate_size(), 0u);
  {
    ReliableDatagramSocket conn1 =
        MockSocket::Allocated(allocator, &destructed);
    EXPECT_GT(allocator.allocate_size(), 0u);
    EXPECT_EQ(allocator.deallocate_size(), 0u);
    EXPECT_FALSE(destructed);
    {
      ReliableDatagramSocket conn2 = conn1;
      EXPECT_FALSE(destructed);
    }
    EXPECT_FALSE(destructed);
  }  // conn1 out of scope

  EXPECT_TRUE(destructed);
  EXPECT_GT(allocator.deallocate_size(), 0u);
  EXPECT_EQ(allocator.allocate_size(), allocator.deallocate_size());
}

TEST(TransportTest, WriteReservationKeepsConnectionAlive) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  bool destructed = false;
  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();

  class TestTask : public async2::Task {
   public:
    TestTask(ReliableDatagramSocket connection, bool& destructed)
        : connection_(std::move(connection)),
          destructed_(destructed),
          started_(false) {}

    async2::Poll<> DoPend(async2::Context& cx) override {
      if (!started_) {
        reserve_future_ = connection_.ReserveWrite(10);
        started_ = true;
      }
      PW_AWAIT(auto result, reserve_future_, cx);
      if (result.has_value()) {
        reservation_ = std::move(result.value());
        // Drop the task's main connection handle; it still holds a reservation.
        connection_ = nullptr;
        EXPECT_FALSE(destructed_);

        // Drop the reservation to destroy the connection.
        reservation_.reset();
        EXPECT_TRUE(destructed_);
      }
      return async2::Ready();
    }

   private:
    ReliableDatagramSocket connection_;
    bool& destructed_;
    ReserveWriteFuture reserve_future_;
    std::optional<WriteReservation> reservation_;
    bool started_;
  };

  ReliableDatagramSocket conn = MockSocket::Allocated(allocator, &destructed);
  TestTask task(conn, destructed);

  // Drop the initial connection handle, leaving the one copied into the task.
  conn = nullptr;
  EXPECT_FALSE(destructed);

  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  // Task completes, drops the reservation, and connection is destructed.
  EXPECT_TRUE(destructed);
}

TEST(TransportTest, MessageSizeLimits) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  ReliableDatagramSocket conn = MockSocket::Allocated(allocator);
  EXPECT_EQ(conn.max_write_message_size_bytes(), 1500u);
  EXPECT_EQ(conn.max_read_message_size_bytes(), 1500u);

  // Exceeding write MTU asserts.
  EXPECT_DEATH_IF_SUPPORTED((void)conn.TryReserveWrite(1501), ".*");
  EXPECT_DEATH_IF_SUPPORTED((void)conn.ReserveWrite(1501), ".*");

  // Reserving within MTU succeeds.
  auto res = conn.TryReserveWrite(100);
  ASSERT_TRUE(res.has_value());
  // Committing more than buffer size asserts.
  EXPECT_DEATH_IF_SUPPORTED((void)res->Commit(101), ".*");
  EXPECT_TRUE(res->Commit(50));
  // Committing again on an already-committed reservation asserts.
  EXPECT_DEATH_IF_SUPPORTED((void)res->Commit(50), ".*");
  // Canceling is always safe to call (idempotent and safe after commit).
  res->Cancel();

  auto res2 = conn.TryReserveWrite(100);
  ASSERT_TRUE(res2.has_value());
  res2->Cancel();
  res2->Cancel();

  // Null connection returns 0 MTU and fails reserves.
  ReliableDatagramSocket null_conn;
  EXPECT_EQ(null_conn.max_write_message_size_bytes(), 0u);
  EXPECT_EQ(null_conn.max_read_message_size_bytes(), 0u);
  EXPECT_FALSE(null_conn.TryReserveWrite(1).has_value());
}

TEST(TransportTest, ZeroSizeBufferHandling) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  MockSocket* mock = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket conn = MockTransport::WrapSocket(*mock);

  // Synchronous 0-byte reservation returns a valid reservation with 0 size.
  auto res_sync = conn.TryReserveWrite(0);
  ASSERT_TRUE(res_sync.has_value());
  EXPECT_EQ(res_sync->size(), 0u);
  EXPECT_TRUE(res_sync->Commit(0));
  EXPECT_EQ(mock->commit_count(), 1u);
  EXPECT_EQ(mock->PopPacket().size(), 0u);

  // Asynchronous 0-byte reservation returns immediately resolved future.
  ReserveWriteFuture res_async_fut = conn.ReserveWrite(0);
  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();
  class TestReserveZeroTask : public async2::Task {
   public:
    TestReserveZeroTask(ReserveWriteFuture fut) : fut_(std::move(fut)) {}
    std::optional<std::optional<WriteReservation>>& result() { return res_; }

   private:
    async2::Poll<> DoPend(async2::Context& cx) override {
      PW_AWAIT(auto r, fut_, cx);
      res_ = std::move(r);
      return async2::Ready();
    }
    ReserveWriteFuture fut_;
    std::optional<std::optional<WriteReservation>> res_;
  };
  TestReserveZeroTask task(std::move(res_async_fut));
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(task.result().has_value());
  ASSERT_TRUE(task.result()->has_value());
  WriteReservation res = std::move(task.result()->value());
  EXPECT_EQ(res.size(), 0u);
  // Canceling 0-byte reservation releases without error.
  res.Cancel();

  // Asynchronous 0-byte reservation on a closed socket returns std::nullopt.
  conn.Close();
  TestReserveZeroTask closed_task(conn.ReserveWrite(0));
  dispatcher.Post(closed_task);
  dispatcher.RunToCompletion();
  ASSERT_TRUE(closed_task.result().has_value());
  EXPECT_FALSE(closed_task.result()->has_value());
}

TEST(TransportTest, ClosedNotification) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  async2::DispatcherForTest dispatcher;

  class ClosedTask : public async2::Task {
   public:
    ClosedTask(const ReliableDatagramSocket& conn)
        : future_(conn.WhenClosed()), resolved_(false) {}

    async2::Poll<> DoPend(async2::Context& cx) override {
      PW_AWAIT([[maybe_unused]] auto result, future_, cx);
      resolved_ = true;
      return async2::Ready();
    }

    bool resolved() const { return resolved_; }

   private:
    CloseFuture future_;
    bool resolved_;
  };

  bool destructed = false;
  ReliableDatagramSocket conn = MockSocket::Allocated(allocator, &destructed);
  ClosedTask task(conn);
  dispatcher.Post(task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(task.resolved());

  // Dropping the socket handle while CloseFuture is active keeps the socket
  // alive.
  ReliableDatagramSocket conn_copy = conn;
  conn = nullptr;
  EXPECT_FALSE(destructed);

  // Closing the socket resolves WhenClosed().
  conn_copy.Close();
  conn_copy = nullptr;
  EXPECT_FALSE(destructed);

  dispatcher.RunUntilStalled();
  EXPECT_TRUE(task.resolved());
  EXPECT_TRUE(destructed);
}

TEST(TransportTest, CommitTruncatesBuffer) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  ReliableDatagramSocket conn = MockSocket::Allocated(allocator);
  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();

  size_t committed_size = 42;

  ReaderTask reader(conn);
  WriterTask writer(conn, std::byte{0x77}, committed_size);

  dispatcher.Post(reader);
  dispatcher.Post(writer);
  dispatcher.RunToCompletion();

  const auto& result = reader.result();
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result.size(), committed_size);
  for (size_t i = 0; i < committed_size; ++i) {
    EXPECT_EQ(result[i], std::byte{0x77});
  }
}

TEST(TransportTest, WriteReservationImplicitlyConvertsToSpan) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  ReliableDatagramSocket conn = MockSocket::Allocated(allocator);

  auto res = conn.TryReserveWrite(64);
  ASSERT_TRUE(res.has_value());

  auto take_byte_span = [](ByteSpan span) { return span.size(); };
  auto take_const_byte_span = [](ConstByteSpan span) { return span.size(); };

  EXPECT_EQ(take_byte_span(*res), 64u);
  EXPECT_EQ(take_const_byte_span(*res), 64u);

  ByteSpan span = *res;
  EXPECT_EQ(span.size(), 64u);

  ConstByteSpan const_span = *res;
  EXPECT_EQ(const_span.size(), 64u);

  const WriteReservation& const_res = *res;
  EXPECT_EQ(take_const_byte_span(const_res), 64u);

  ConstByteSpan const_span2 = const_res;
  EXPECT_EQ(const_span2.size(), 64u);
}

TEST(TransportTest, LayeredConnectionWriteAndRead) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  auto* lower_impl = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket lower_conn = MockTransport::WrapSocket(*lower_impl);
  auto* layered_impl =
      allocator.New<MockLayeredSocket>(allocator, lower_conn, lower_impl);
  ReliableDatagramSocket layered_conn =
      MockTransport::WrapSocket(*layered_impl);

  // Test Read path
  {
    auto raw_packet = allocator.MakeUnique<std::byte[]>(13);
    uint32_t len = 5;
    uint32_t crc = MockLayeredSocket::kCrcMagic;
    std::memcpy(raw_packet.get(), &len, sizeof(uint32_t));
    std::memcpy(raw_packet.get() + 4, "hello", 5);
    std::memcpy(raw_packet.get() + 9, &crc, sizeof(uint32_t));

    layered_impl->EnqueueForRead(pw::ConstBuf(pw::Buf(std::move(raw_packet))));

    async2::DispatcherForTest dispatcher;
    dispatcher.AllowBlocking();
    ReaderTask reader(layered_conn);
    dispatcher.Post(reader);
    dispatcher.RunToCompletion();

    ASSERT_TRUE(reader.done());
    ASSERT_NE(reader.result(), nullptr);
    const pw::ConstBuf& read_buf = reader.result();
    EXPECT_EQ(read_buf.size(), 5u);
    EXPECT_EQ(std::memcmp(read_buf.data(), "hello", 5), 0);
  }

  // Test Write path
  {
    std::optional<WriteReservation> res = layered_conn.TryReserveWrite(5);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->size(), 5u);

    std::memcpy(res->data(), "world", 5);
    EXPECT_TRUE(res->Commit(5));

    EXPECT_TRUE(lower_impl->HasReadPacket());
    pw::ConstBuf outbound_buf = lower_impl->PopPacket();
    EXPECT_FALSE(lower_impl->HasReadPacket());

    EXPECT_EQ(outbound_buf.size(), 13u);

    uint32_t len = 0;
    std::memcpy(&len, outbound_buf.data(), sizeof(uint32_t));
    EXPECT_EQ(len, 5u);

    EXPECT_EQ(std::memcmp(outbound_buf.data() + 4, "world", 5), 0);

    uint32_t crc = 0;
    std::memcpy(&crc, outbound_buf.data() + 9, sizeof(uint32_t));
    EXPECT_EQ(crc, MockLayeredSocket::kCrcMagic);
  }
}

TEST(TransportTest, LayeredConnectionTaskInteraction) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  auto* lower_impl = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket lower_conn = MockTransport::WrapSocket(*lower_impl);
  auto* layered_impl =
      allocator.New<MockLayeredSocket>(allocator, lower_conn, lower_impl);
  ReliableDatagramSocket layered_conn =
      MockTransport::WrapSocket(*layered_impl);

  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();

  std::byte fill_value{0x55};
  size_t data_size = 8;

  WriterTask writer(layered_conn, fill_value, data_size);
  dispatcher.Post(writer);
  dispatcher.RunToCompletion();

  EXPECT_EQ(writer.result(), OkStatus());
  EXPECT_TRUE(lower_impl->HasReadPacket());

  pw::ConstBuf raw_packet = lower_impl->PopPacket();
  EXPECT_EQ(raw_packet.size(), data_size + MockLayeredSocket::kOverhead);

  uint32_t len = 0;
  std::memcpy(&len, raw_packet.data(), sizeof(uint32_t));
  EXPECT_EQ(len, data_size);

  for (size_t i = 0; i < data_size; ++i) {
    EXPECT_EQ(raw_packet[4 + i], fill_value);
  }

  uint32_t crc = 0;
  std::memcpy(&crc, raw_packet.data() + 4 + data_size, sizeof(uint32_t));
  EXPECT_EQ(crc, MockLayeredSocket::kCrcMagic);

  auto* lower_recv_impl = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket lower_recv_conn =
      MockTransport::WrapSocket(*lower_recv_impl);
  auto* layered_recv_impl = allocator.New<MockLayeredSocket>(
      allocator, lower_recv_conn, lower_recv_impl);
  ReliableDatagramSocket layered_recv_conn =
      MockTransport::WrapSocket(*layered_recv_impl);

  layered_recv_impl->EnqueueForRead(std::move(raw_packet));

  ReaderTask reader(layered_recv_conn);
  dispatcher.Post(reader);
  dispatcher.RunToCompletion();

  ASSERT_TRUE(reader.done());
  ASSERT_NE(reader.result(), nullptr);
  const pw::ConstBuf& read_buf = reader.result();
  EXPECT_EQ(read_buf.size(), data_size);
  for (size_t i = 0; i < data_size; ++i) {
    EXPECT_EQ(read_buf[i], fill_value);
  }
}

TEST(TransportTest, LayeredConnectionCancelReservation) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  ReliableDatagramSocket lower_conn = MockSocket::Allocated(allocator);
  ReliableDatagramSocket layered_conn =
      MockLayeredSocket::Allocated(allocator, lower_conn);

  EXPECT_GT(allocator.allocate_size(), 0u);
  EXPECT_EQ(allocator.deallocate_size(), 0u);

  {
    auto res = layered_conn.TryReserveWrite(20);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(allocator.allocate_size(), 20u + MockLayeredSocket::kOverhead);
    res->Cancel();
    EXPECT_EQ(allocator.deallocate_size(), 20u + MockLayeredSocket::kOverhead);
  }
}

TEST(TransportTest, LayeredConnectionKeepsConnectionsAlive) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  bool lower_destructed = false;
  bool layered_destructed = false;

  std::optional<WriteReservation> reservation;
  {
    ReliableDatagramSocket lower_conn =
        MockSocket::Allocated(allocator, &lower_destructed);
    ReliableDatagramSocket layered_conn = MockLayeredSocket::Allocated(
        allocator, lower_conn, nullptr, &layered_destructed);

    reservation = layered_conn.TryReserveWrite(10);
    ASSERT_TRUE(reservation.has_value());
    EXPECT_FALSE(lower_destructed);
    EXPECT_FALSE(layered_destructed);
  }

  EXPECT_FALSE(layered_destructed);
  EXPECT_FALSE(lower_destructed);

  reservation.reset();

  EXPECT_TRUE(layered_destructed);
  EXPECT_TRUE(lower_destructed);
}

TEST(TransportTest, LayeredConnectionCorruptPacketRejected) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  auto* lower_impl = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket lower_conn = MockTransport::WrapSocket(*lower_impl);
  auto* layered_impl =
      allocator.New<MockLayeredSocket>(allocator, lower_conn, lower_impl);
  ReliableDatagramSocket layered_conn =
      MockTransport::WrapSocket(*layered_impl);

  auto corrupt_packet = allocator.MakeUnique<std::byte[]>(13);
  uint32_t len = 5;
  uint32_t bad_crc = 0xBAD0C0DE;
  std::memcpy(corrupt_packet.get(), &len, sizeof(uint32_t));
  std::memcpy(corrupt_packet.get() + 4, "hello", 5);
  std::memcpy(corrupt_packet.get() + 9, &bad_crc, sizeof(uint32_t));

  layered_impl->EnqueueForRead(
      pw::ConstBuf(pw::Buf(std::move(corrupt_packet))));

  async2::DispatcherForTest dispatcher;
  dispatcher.AllowBlocking();
  ReaderTask reader(layered_conn);
  dispatcher.Post(reader);
  dispatcher.RunUntilStalled();

  EXPECT_FALSE(reader.done());
  reader.Deregister();
}

TEST(TransportTest, AdoptTrimmingOutOfBoundsAsserts) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  auto* conn1 = allocator.New<MockSocket>(allocator, allocator);
  auto* conn2 = allocator.New<MockSocket>(allocator, allocator);

  ReliableDatagramSocket c1 = MockTransport::WrapSocket(*conn1);
  ReliableDatagramSocket c2 = MockTransport::WrapSocket(*conn2);
  {
    std::optional<WriteReservation> res = c1.TryReserveWrite(10);
    ASSERT_TRUE(res.has_value());
    EXPECT_DEATH_IF_SUPPORTED(conn2->Adopt(*res, 11, 0), ".*");
  }
  {
    std::optional<WriteReservation> res = c1.TryReserveWrite(10);
    ASSERT_TRUE(res.has_value());
    EXPECT_DEATH_IF_SUPPORTED(conn2->Adopt(*res, 0, 11), ".*");
  }
  {
    std::optional<WriteReservation> res = c1.TryReserveWrite(10);
    ASSERT_TRUE(res.has_value());
    EXPECT_DEATH_IF_SUPPORTED(conn2->Adopt(*res, 6, 5), ".*");
  }
}

TEST(TransportTest, AdoptTrimmingEntireBufferSucceeds) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  auto* conn1 = allocator.New<MockSocket>(allocator, allocator);
  auto* conn2 = allocator.New<MockSocket>(allocator, allocator);

  ReliableDatagramSocket c1 = MockTransport::WrapSocket(*conn1);
  ReliableDatagramSocket c2 = MockTransport::WrapSocket(*conn2);
  std::optional<WriteReservation> res = c1.TryReserveWrite(10);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->size(), 10u);

  conn2->Adopt(*res, 5, 5);
  EXPECT_EQ(res->size(), 0u);
  EXPECT_TRUE(res->Commit(0));
  EXPECT_EQ(conn2->commit_count(), 1u);
}

TEST(TransportTest, CloseAndWhenClosedShareFuture) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  async2::DispatcherForTest dispatcher;

  class AsyncCloseReliableDatagramSocket : public ReliableDatagramSocketImpl {
   public:
    AsyncCloseReliableDatagramSocket(Allocator& alloc)
        : ReliableDatagramSocketImpl(alloc, 1500) {}
    async2::Poll<pw::ConstBuf> DoRead() override
        PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
      return async2::Pending();
    }
    std::optional<WriteReservation> DoTryReserveWrite(size_t) override
        PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
      return std::nullopt;
    }
    bool DoCommitWrite(pw::Buf&&) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
      return true;
    }
    void DoCancelWrite(pw::Buf&&) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {}
    void DoClose() override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
      ++do_close_calls_;
      // Intentionally don't call MarkClosed() immediately to test async
      // resolution.
    }

    void FinishAsyncClose() {
      std::lock_guard lock(*this);
      MarkClosed();
    }

    int do_close_calls() const PW_LOCKS_EXCLUDED(*this) {
      std::lock_guard lock(*this);
      return do_close_calls_;
    }

    void lock() const PW_EXCLUSIVE_LOCK_FUNCTION() override { lock_.lock(); }
    void unlock() const PW_UNLOCK_FUNCTION() override { lock_.unlock(); }

   private:
    int do_close_calls_ PW_GUARDED_BY(*this) = 0;
    mutable sync::InterruptSpinLock lock_;
  };

  class VoidFutureTask : public async2::Task {
   public:
    VoidFutureTask(CloseFuture future)
        : future_(std::move(future)), resolved_(false) {}

    async2::Poll<> DoPend(async2::Context& cx) override {
      PW_AWAIT([[maybe_unused]] auto result, future_, cx);
      resolved_ = true;
      return async2::Ready();
    }

    bool resolved() const { return resolved_; }

   private:
    CloseFuture future_;
    bool resolved_;
  };

  auto* impl = allocator.New<AsyncCloseReliableDatagramSocket>(allocator);
  ReliableDatagramSocket conn = MockTransport::WrapSocket(*impl);

  VoidFutureTask when_closed_task1(conn.WhenClosed());
  VoidFutureTask when_closed_task2(conn.WhenClosed());
  dispatcher.Post(when_closed_task1);
  dispatcher.Post(when_closed_task2);
  dispatcher.RunUntilStalled();

  EXPECT_FALSE(when_closed_task1.resolved());
  EXPECT_FALSE(when_closed_task2.resolved());
  EXPECT_EQ(impl->do_close_calls(), 0);

  // Call Close(), which triggers DoClose() once and returns a shared
  // CloseFuture.
  VoidFutureTask close_task(conn.Close());
  dispatcher.Post(close_task);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(impl->do_close_calls(), 1);
  EXPECT_FALSE(close_task.resolved());
  EXPECT_FALSE(when_closed_task1.resolved());
  // Calling Close() again does not trigger DoClose() a second time.
  VoidFutureTask close_task2(conn.Close());
  dispatcher.Post(close_task2);
  dispatcher.RunUntilStalled();
  EXPECT_EQ(impl->do_close_calls(), 1);
  EXPECT_FALSE(close_task2.resolved());

  // Implementation calls MarkClosed(). All pending Close and WhenClosed futures
  // resolve.
  impl->FinishAsyncClose();
  dispatcher.RunUntilStalled();

  EXPECT_TRUE(close_task.resolved());
  EXPECT_TRUE(close_task2.resolved());
  EXPECT_TRUE(when_closed_task1.resolved());
  EXPECT_TRUE(when_closed_task2.resolved());
}

TEST(TransportTest, SingleReadFutureInvariant) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  auto* conn = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket c = MockTransport::WrapSocket(*conn);

  // Creating multiple unpolled ReadFutures succeeds (lazy registration).
  ReadFuture rf1 = c.Read();
  ReadFuture rf2 = c.Read();

  // Moving the future succeeds.
  ReadFuture rf3 = std::move(rf1);

  // Direct reassignment succeeds without prior reset (no crash on conn.Read()).
  rf3 = c.Read();

  // Polling a reader registers it in read_futures_.
  async2::DispatcherForTest dispatcher;
  ReaderTask reader_task(c);
  dispatcher.Post(reader_task);
  dispatcher.RunUntilStalled();
  EXPECT_FALSE(reader_task.done());

  // Polling a second reader concurrently asserts.
  class SecondReaderTask : public async2::Task {
   public:
    SecondReaderTask(ReliableDatagramSocket connection)
        : connection_(std::move(connection)) {}
    async2::Poll<> DoPend(async2::Context& cx) override {
      future_ = connection_.Read();
      PW_AWAIT([[maybe_unused]] auto res, future_, cx);
      return async2::Ready();
    }

   private:
    ReliableDatagramSocket connection_;
    ReadFuture future_;
  };

  EXPECT_DEATH_IF_SUPPORTED(
      [&]() {
        SecondReaderTask second_task(c);
        dispatcher.Post(second_task);
        dispatcher.RunUntilStalled();
      }(),
      ".*");

  // Enqueueing data allows the active reader to complete.
  conn->EnqueueForRead(pw::Buf::Allocate(allocator, 8));
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(reader_task.done());

  // Can read again after the previous reader completes.
  ReaderTask reader_task2(c);
  dispatcher.Post(reader_task2);
  conn->EnqueueForRead(pw::Buf::Allocate(allocator, 8));
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(reader_task2.done());
}

TEST(TransportTest, MultipleConcurrentReserveWriteFutures) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  auto* conn = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket c = MockTransport::WrapSocket(*conn);
  async2::DispatcherForTest dispatcher;

  conn->SetBlockWrites(true);

  WriterTask writer1(c, std::byte{1}, 16);
  WriterTask writer2(c, std::byte{2}, 32);
  WriterTask writer3(c, std::byte{3}, 64);

  dispatcher.Post(writer1);
  dispatcher.Post(writer2);
  dispatcher.Post(writer3);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(writer1.result(), Status::Unknown());
  EXPECT_EQ(writer2.result(), Status::Unknown());
  EXPECT_EQ(writer3.result(), Status::Unknown());

  // Unblock writes.
  conn->SetBlockWrites(false);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(writer1.result(), OkStatus());
  EXPECT_EQ(writer2.result(), OkStatus());
  EXPECT_EQ(writer3.result(), OkStatus());
  EXPECT_EQ(conn->commit_count(), 3u);
}

TEST(TransportTest, ResolveFirstMatchingSizeRequest) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  auto* conn = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket c = MockTransport::WrapSocket(*conn);
  async2::DispatcherForTest dispatcher;

  // Set available bytes to 0 so futures remain queued in write_futures_.
  conn->SetAvailableBytes(0);

  // Writer 1 requests 100 bytes.
  WriterTask writer1(c, std::byte{1}, 100);
  // Writer 2 requests 20 bytes.
  WriterTask writer2(c, std::byte{2}, 20);
  // Writer 3 requests 60 bytes.
  WriterTask writer3(c, std::byte{3}, 60);

  dispatcher.Post(writer1);
  dispatcher.Post(writer2);
  dispatcher.Post(writer3);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(writer1.result(), Status::Unknown());
  EXPECT_EQ(writer2.result(), Status::Unknown());
  EXPECT_EQ(writer3.result(), Status::Unknown());

  // Allow up to 50 bytes. Writer 1 cannot fit (needs 100). Writer 2 fits (needs
  // 20).
  conn->SetAvailableBytes(50);
  {
    std::lock_guard lock(*conn);
    conn->WakeOneWriterLocked();
  }
  dispatcher.RunUntilStalled();

  EXPECT_EQ(writer1.result(), Status::Unknown());
  EXPECT_EQ(writer2.result(), OkStatus());
  EXPECT_EQ(writer3.result(), Status::Unknown());

  // Now allow up to 80 bytes. Writer 1 cannot fit (needs 100). Writer 3 fits
  // (needs 60).
  conn->SetAvailableBytes(80);
  {
    std::lock_guard lock(*conn);
    conn->WakeOneWriterLocked();
  }
  dispatcher.RunUntilStalled();

  EXPECT_EQ(writer1.result(), Status::Unknown());
  EXPECT_EQ(writer2.result(), OkStatus());
  EXPECT_EQ(writer3.result(), OkStatus());

  // Finally allow 100 bytes. Writer 1 fits and completes.
  conn->SetAvailableBytes(100);
  {
    std::lock_guard lock(*conn);
    conn->WakeOneWriterLocked();
  }
  dispatcher.RunUntilStalled();

  EXPECT_EQ(writer1.result(), OkStatus());
  EXPECT_EQ(writer2.result(), OkStatus());
  EXPECT_EQ(writer3.result(), OkStatus());
  EXPECT_EQ(conn->commit_count(), 3u);
}

TEST(TransportTest, FutureKeepsConnectionAlive) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  bool destructed = false;
  auto* conn = allocator.New<MockSocket>(allocator, allocator, &destructed);

  {
    ReliableDatagramSocket c = MockTransport::WrapSocket(*conn);
    std::optional<ReadFuture> rf = c.Read();
    c = nullptr;
    EXPECT_FALSE(destructed);
    rf.reset();
    EXPECT_TRUE(destructed);
  }

  destructed = false;
  conn = allocator.New<MockSocket>(allocator, allocator, &destructed);
  {
    ReliableDatagramSocket c = MockTransport::WrapSocket(*conn);
    std::optional<ReserveWriteFuture> rwf = c.ReserveWrite(10);
    c = nullptr;
    EXPECT_FALSE(destructed);
    rwf.reset();
    EXPECT_TRUE(destructed);
  }
}

TEST(TransportTest, CancelReservationUnblocksPendingWriter) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  auto* conn = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket c = MockTransport::WrapSocket(*conn);
  async2::DispatcherForTest dispatcher;

  conn->SetAvailableBytes(50);

  std::optional<WriteReservation> res1 = c.TryReserveWrite(50);
  ASSERT_TRUE(res1.has_value());

  WriterTask blocked_writer(c, std::byte{0xaa}, 50);
  dispatcher.Post(blocked_writer);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(blocked_writer.result(), Status::Unknown());

  res1->Cancel();

  dispatcher.RunUntilStalled();

  EXPECT_EQ(blocked_writer.result(), OkStatus());
  EXPECT_EQ(conn->commit_count(), 1u);
}

TEST(TransportTest, MovePendingReserveWriteFuture) {
  const size_t kCapacity = 4096;
  pw::allocator::test::AllocatorForTest<kCapacity> allocator;
  auto* conn = allocator.New<MockSocket>(allocator, allocator);
  ReliableDatagramSocket c = MockTransport::WrapSocket(*conn);
  async2::DispatcherForTest dispatcher;

  conn->SetAvailableBytes(0);

  class MoveWriterTask : public async2::Task {
   public:
    MoveWriterTask(ReliableDatagramSocket connection)
        : connection_(std::move(connection)),
          step_(0),
          result_(Status::Unknown()) {}

    const Status& result() const { return result_; }

   private:
    async2::Poll<> DoPend(async2::Context& cx) override {
      if (step_ == 0) {
        ReserveWriteFuture fut1 = connection_.ReserveWrite(32);
        ReserveWriteFuture fut2 = std::move(fut1);
        future_ = std::move(fut2);
        step_ = 1;
      }

      PW_AWAIT(auto res, future_, cx);
      if (res.has_value()) {
        result_ = res->Commit(32) ? OkStatus() : Status::Internal();
      } else {
        result_ = Status::Aborted();
      }
      return async2::Ready();
    }

    ReliableDatagramSocket connection_;
    ReserveWriteFuture future_;
    int step_;
    Status result_;
  };

  MoveWriterTask writer(c);
  dispatcher.Post(writer);
  dispatcher.RunUntilStalled();

  EXPECT_EQ(writer.result(), Status::Unknown());

  conn->SetAvailableBytes(32);
  {
    std::lock_guard lock(*conn);
    conn->WakeOneWriterLocked();
  }
  dispatcher.RunUntilStalled();

  EXPECT_EQ(writer.result(), OkStatus());
  EXPECT_EQ(conn->commit_count(), 1u);
}

}  // namespace
}  // namespace pw::transport
