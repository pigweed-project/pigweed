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

#define PW_LOG_MODULE_NAME "FRAMED_TCP"

#include "pw_transport/framed_tcp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#include "pw_assert/check.h"
#include "pw_buf/buf.h"
#include "pw_bytes/endian.h"
#include "pw_bytes/span.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_log/log.h"
#include "pw_string/to_string.h"
#include "pw_thread/attrs.h"

namespace pw::transport {
namespace {

// Each datagram is preceded by its size as a 32-bit big-endian integer.
constexpr size_t kFrameHeaderSize = sizeof(uint32_t);

// Received zero-length datagrams use this data address to distinguish
// them from null `ConstBuf` reads when the socket closes.
constexpr std::byte kEmptyDatagram{};

constexpr int kListenBacklog = 16;

// Prevents send() from raising SIGPIPE if the peer has disconnected. macOS
// does not support MSG_NOSIGNAL, so SO_NOSIGPIPE is set on each socket instead.
#if defined(__linux__)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif  // defined(__linux__)

constexpr ThreadAttrs kReadThreadAttrs =
    ThreadAttrs().set_name("FramedTcpRead");
constexpr ThreadAttrs kWriteThreadAttrs =
    ThreadAttrs().set_name("FramedTcpWrite");
constexpr ThreadAttrs kAcceptThreadAttrs =
    ThreadAttrs().set_name("FramedTcpAccept");
constexpr ThreadAttrs kConnectThreadAttrs =
    ThreadAttrs().set_name("FramedTcpConn");

void CheckOptions(const FramedTcpOptions& options) {
  PW_CHECK(
      options.max_message_size_bytes <= std::numeric_limits<uint32_t>::max(),
      "max_message_size_bytes must fit in the 32-bit frame header");
  PW_CHECK_UINT_GT(options.read_queue_depth, 0u);
  PW_CHECK_UINT_GT(options.write_queue_depth, 0u);
}

// Returns whether an errno value indicates that the process or system ran out
// of memory or file descriptors.
bool IsResourceError(int error) {
  return error == EMFILE || error == ENFILE || error == ENOBUFS ||
         error == ENOMEM;
}

bool SetNonBlocking(int fd, bool non_blocking) {
  const int flags = fcntl(fd, F_GETFL);
  if (flags < 0) {
    return false;
  }
  const int new_flags =
      non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return new_flags == flags || fcntl(fd, F_SETFL, new_flags) == 0;
}

// Prepares a connected TCP socket for use by `FramedTcpSocket`.
Status ConfigureConnection(int fd) {
  // Connections are opened and accepted with non-blocking sockets, but
  // `FramedTcpSocket` uses blocking I/O. Accepted sockets inherit O_NONBLOCK
  // from the listening socket on macOS.
  if (!SetNonBlocking(fd, false)) {
    PW_LOG_WARN("Failed to configure connection: %s", std::strerror(errno));
    return Status::Unavailable();
  }

#if defined(__APPLE__)
  const int no_sigpipe = 1;
  if (setsockopt(fd,
                 SOL_SOCKET,
                 SO_NOSIGPIPE,
                 &no_sigpipe,
                 static_cast<socklen_t>(sizeof(no_sigpipe))) != 0) {
    PW_LOG_WARN("Failed to configure connection: %s", std::strerror(errno));
    return Status::Unavailable();
  }
#endif  // defined(__APPLE__)

  // Send each datagram when it is committed rather than waiting to combine it
  // with later ones. This is an optimization, so failure is not fatal.
  const int no_delay = 1;
  if (setsockopt(fd,
                 IPPROTO_TCP,
                 TCP_NODELAY,
                 &no_delay,
                 static_cast<socklen_t>(sizeof(no_delay))) != 0) {
    PW_LOG_WARN("Failed to set TCP_NODELAY: %s", std::strerror(errno));
  }
  return OkStatus();
}

// Receives exactly `buffer.size()` bytes.
//
// Returns:
//   OK: The buffer was filled.
//   OUT_OF_RANGE: The peer closed the connection before sending any bytes.
//   DATA_LOSS: The peer closed the connection before filling the buffer.
//   UNAVAILABLE: recv() failed and set errno.
Status ReceiveExact(int fd, ByteSpan buffer) {
  size_t received = 0;
  while (received < buffer.size()) {
    const ssize_t result =
        recv(fd, buffer.data() + received, buffer.size() - received, 0);
    if (result == 0) {
      return received == 0 ? Status::OutOfRange() : Status::DataLoss();
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::Unavailable();
    }
    received += static_cast<size_t>(result);
  }
  return OkStatus();
}

// Sends a frame header and `payload` together.
//
// Returns:
//   OK: The datagram was sent.
//   UNAVAILABLE: sendmsg() failed and set errno.
Status SendFrame(int fd, ConstByteSpan payload) {
  std::array<std::byte, kFrameHeaderSize> header =
      bytes::CopyInOrder(endian::big, static_cast<uint32_t>(payload.size()));
  std::array<iovec, 2> chunks{};
  chunks[0].iov_base = header.data();
  chunks[0].iov_len = header.size();
  chunks[1].iov_base = const_cast<std::byte*>(payload.data());
  chunks[1].iov_len = payload.size();

  msghdr message = {};
  message.msg_iov = chunks.data();
  message.msg_iovlen =
      static_cast<decltype(message.msg_iovlen)>(payload.empty() ? 1 : 2);

  while (message.msg_iovlen > 0) {
    const ssize_t result = sendmsg(fd, &message, kSendFlags);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::Unavailable();
    }

    // Skip the bytes that were sent in case the send was partial.
    size_t sent = static_cast<size_t>(result);
    while (message.msg_iovlen > 0 && sent >= message.msg_iov->iov_len) {
      sent -= message.msg_iov->iov_len;
      ++message.msg_iov;
      --message.msg_iovlen;
    }
    if (message.msg_iovlen > 0) {
      message.msg_iov->iov_base =
          static_cast<std::byte*>(message.msg_iov->iov_base) + sent;
      message.msg_iov->iov_len -= sent;
    }
  }
  return OkStatus();
}

// Closes `fd` after a failed connection attempt and sets errno to `error`.
Status CloseAfterConnectionError(int fd, int error) {
  close(fd);
  errno = error;
  return Status::Unavailable();
}

// Opens a TCP connection to `address`. The attempt is canceled if `wake_fd`
// becomes readable.
//
// Returns:
//   OK: The file descriptor of the connected socket, which is non-blocking.
//   CANCELLED: `wake_fd` became readable.
//   RESOURCE_EXHAUSTED: The process ran out of memory or file descriptors.
//   UNAVAILABLE: The connection could not be established. errno is set to the
//     reason.
Result<int> ConnectToAddress(const addrinfo& address, int wake_fd) {
  const int fd =
      socket(address.ai_family, address.ai_socktype, address.ai_protocol);
  if (fd < 0) {
    return IsResourceError(errno) ? Status::ResourceExhausted()
                                  : Status::Unavailable();
  }

  // Connect asynchronously so that the attempt can be canceled. A connect()
  // that is interrupted by a signal also continues asynchronously.
  if (!SetNonBlocking(fd, true) ||
      (connect(fd, address.ai_addr, address.ai_addrlen) != 0 &&
       errno != EINPROGRESS && errno != EINTR)) {
    return CloseAfterConnectionError(fd, errno);
  }

  std::array<pollfd, 2> fds{};
  fds[0].fd = fd;
  fds[0].events = POLLOUT;
  fds[1].fd = wake_fd;
  fds[1].events = POLLIN;
  while (poll(fds.data(), static_cast<nfds_t>(fds.size()), -1) < 0) {
    if (errno != EINTR) {
      return CloseAfterConnectionError(fd, errno);
    }
  }
  if (fds[1].revents != 0) {
    close(fd);
    return Status::Cancelled();
  }

  int error = 0;
  socklen_t error_size = static_cast<socklen_t>(sizeof(error));
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) != 0) {
    error = errno;
  }
  if (error != 0) {
    return CloseAfterConnectionError(fd, error);
  }
  return fd;
}

// Opens a socket that listens for IPv4 and IPv6 connections on `port` on all
// interfaces. If `port` is 0, the system picks a port. `reuse_address` sets
// SO_REUSEADDR, which allows binding to a port that recently closed
// connections still use.
//
// The socket is non-blocking so that accept() can't block if a pending
// connection is reset after poll() reports it.
//
// Returns the file descriptor, or -1 with errno set.
int OpenListeningSocket(uint16_t port, bool reuse_address) {
  const int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  // Clearing IPV6_V6ONLY accepts IPv4 connections too.
  const int enable = 1;
  const int ipv6_only = 0;
  sockaddr_in6 address = {};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_any;
  address.sin6_port = htons(port);

  if ((reuse_address &&
       setsockopt(fd,
                  SOL_SOCKET,
                  SO_REUSEADDR,
                  &enable,
                  static_cast<socklen_t>(sizeof(enable))) != 0) ||
      setsockopt(fd,
                 IPPROTO_IPV6,
                 IPV6_V6ONLY,
                 &ipv6_only,
                 static_cast<socklen_t>(sizeof(ipv6_only))) != 0 ||
      !SetNonBlocking(fd, true) ||
      bind(fd,
           reinterpret_cast<const sockaddr*>(&address),
           static_cast<socklen_t>(sizeof(address))) != 0 ||
      listen(fd, kListenBacklog) != 0) {
    const int error = errno;
    close(fd);
    errno = error;
    return -1;
  }
  return fd;
}

// Returns a port that no IPv4 socket is bound to, or 0 if none was found.
uint16_t FindUnusedIpv4Port() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }

  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  socklen_t address_size = static_cast<socklen_t>(sizeof(address));

  uint16_t port = 0;
  if (bind(fd, reinterpret_cast<const sockaddr*>(&address), address_size) ==
          0 &&
      getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) ==
          0) {
    port = ntohs(address.sin_port);
  }
  close(fd);
  return port;
}

// Opens a listening socket on a port that no other socket uses.
//
// On some systems, such as macOS, binding a dual-stack socket to port 0 may
// pick a port that another socket uses for a specific IPv4 address, such as
// 127.0.0.1. IPv4 connections to that address then go to the other socket.
// To avoid this, find a port that no IPv4 socket uses, then bind to it without
// SO_REUSEADDR, which fails if any socket uses the port.
//
// Returns the file descriptor, or -1 with errno set.
int OpenListeningSocketOnUnusedPort() {
  constexpr int kMaxAttempts = 10;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const uint16_t port = FindUnusedIpv4Port();
    if (port == 0) {
      break;
    }
    const int fd = OpenListeningSocket(port, /*reuse_address=*/false);
    // Another socket may have bound to the port after it was found.
    if (fd != -1 || errno != EADDRINUSE) {
      return fd;
    }
  }
  return OpenListeningSocket(0, /*reuse_address=*/false);
}

// A `ReliableDatagramSocketImpl` for a connected TCP socket.
//
// A read thread receives datagrams into a bounded queue, and a write thread
// sends committed datagrams. Each reservation or unsent datagram that holds a
// buffer uses one of `write_queue_depth` write slots. Zero-length reservations
// have no buffer, so they don't use a slot.
//
// Reads on a closed socket return nothing, so when the connection ends, the
// socket stays open until the datagrams that were already received have been
// read. Commits fail in the meantime.
class FramedTcpSocket final : public ReliableDatagramSocketImpl {
 public:
  // Creates a socket that owns `fd`, which must be a connected TCP socket in
  // blocking mode. Closes `fd` on failure.
  static Result<ReliableDatagramSocket> Create(Allocator& allocator,
                                               int fd,
                                               const FramedTcpOptions& options);

  // Use `Create`. The constructor is only public for `Allocator::New`.
  FramedTcpSocket(Allocator& allocator, int fd, const FramedTcpOptions& options)
      : ReliableDatagramSocketImpl(allocator, options.max_message_size_bytes),
        buffer_allocator_(allocator),
        fd_(fd),
        read_queue_depth_(options.read_queue_depth),
        write_queue_depth_(options.write_queue_depth),
        read_queue_(allocator),
        write_queue_(allocator) {}

  ~FramedTcpSocket() override;

  void lock() const PW_EXCLUSIVE_LOCK_FUNCTION() override { lock_.lock(); }
  void unlock() const PW_UNLOCK_FUNCTION() override { lock_.unlock(); }

 private:
  async2::Poll<ConstBuf> DoRead() override PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  std::optional<WriteReservation> DoTryReserveWrite(
      size_t min_size_bytes) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  void DoClose() override PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  bool DoCommitWrite(Buf&& buffer) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  void DoCancelWrite(Buf&& buffer) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  // Allocates the queues and starts the I/O threads.
  bool Start() PW_LOCKS_EXCLUDED(*this);

  void ReadLoop() PW_LOCKS_EXCLUDED(*this);
  void WriteLoop() PW_LOCKS_EXCLUDED(*this);

  // Waits until the read queue has room. Returns false if the connection
  // closed.
  bool WaitForReadQueueSpace() PW_LOCKS_EXCLUDED(*this);

  // Waits for a datagram to send and moves it to `datagram`. Returns false if
  // the connection closed.
  bool WaitForQueuedDatagram(Buf& datagram) PW_LOCKS_EXCLUDED(*this);

  // Frees `buffer` and releases its write slot. Returns `true` if the memory
  // was reclaimed, or false if `buffer` is null.
  bool ReleaseWriteSlot(Buf&& buffer) PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  // Ends the connection after recv() or send() failed with `status`. `error`
  // is the errno value for an UNAVAILABLE status.
  void EndConnectionAfterIoFailureLocked(Status status, int error)
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  void EndConnection() PW_LOCKS_EXCLUDED(*this);

  // Disconnects because the connection failed or the peer closed it. The
  // socket closes once the datagrams that were already received are read.
  void EndConnectionLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  // Disconnects and closes the socket, discarding unread datagrams.
  void ShutDownLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  // Closes the connection, which stops the I/O threads, and discards unsent
  // datagrams.
  void DisconnectLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  Allocator& buffer_allocator_;

  // Closed in the destructor after the I/O threads exit, so that the
  // descriptor can't be reused while they still use it.
  const int fd_;

  const uint16_t read_queue_depth_;
  const uint16_t write_queue_depth_;

  mutable sync::Mutex lock_;
  DynamicDeque<ConstBuf> read_queue_ PW_GUARDED_BY(*this);
  DynamicDeque<Buf> write_queue_ PW_GUARDED_BY(*this);

  // Write slots in use by reservations and by queued or in-flight datagrams.
  uint16_t outstanding_writes_ PW_GUARDED_BY(*this) = 0;

  // Whether the write thread is sending a datagram that it removed from the
  // queue.
  bool write_in_progress_ PW_GUARDED_BY(*this) = false;

  // Whether the connection is closed. The I/O threads exit once it is.
  bool disconnected_ PW_GUARDED_BY(*this) = false;

  // Released when the read queue stops being full or the connection closes.
  sync::ThreadNotification read_queue_space_;

  // Released when a datagram is queued or the connection closes.
  sync::ThreadNotification datagram_queued_;

  DefaultThreadContext read_thread_context_;
  DefaultThreadContext write_thread_context_;
  Thread read_thread_;
  Thread write_thread_;
};

Result<ReliableDatagramSocket> FramedTcpSocket::Create(
    Allocator& allocator, int fd, const FramedTcpOptions& options) {
  FramedTcpSocket* impl =
      allocator.New<FramedTcpSocket>(allocator, fd, options);
  if (impl == nullptr) {
    PW_LOG_ERROR("Failed to allocate socket");
    close(fd);
    return Status::ResourceExhausted();
  }

  // The handle owns the socket. If Start() fails, dropping the handle destroys
  // the socket, which closes `fd`.
  ReliableDatagramSocket handle = impl->WrapSocket();
  if (!impl->Start()) {
    return Status::ResourceExhausted();
  }
  return handle;
}

FramedTcpSocket::~FramedTcpSocket() {
  {
    std::lock_guard lock(*this);
    ShutDownLocked();
  }
  if (read_thread_.joinable()) {
    read_thread_.join();
  }
  if (write_thread_.joinable()) {
    write_thread_.join();
  }
  close(fd_);
}

async2::Poll<ConstBuf> FramedTcpSocket::DoRead() {
  if (read_queue_.empty()) {
    return async2::Pending();
  }
  if (read_queue_.size() >= read_queue_depth_) {
    read_queue_space_.release();
  }
  ConstBuf datagram = std::move(read_queue_.front());
  read_queue_.pop_front();

  // After the connection ends, the socket closes once the last datagram that
  // was received is read.
  if (disconnected_ && read_queue_.empty()) {
    MarkClosed();
  }
  return async2::Ready(std::move(datagram));
}

std::optional<WriteReservation> FramedTcpSocket::DoTryReserveWrite(
    size_t min_size_bytes) {
  if (!is_open() || outstanding_writes_ >= write_queue_depth_) {
    return std::nullopt;
  }
  Buf buffer = Buf::TryAllocate(buffer_allocator_, min_size_bytes);
  if (buffer == nullptr) {
    return std::nullopt;
  }
  ++outstanding_writes_;
  return CreateReservation(std::move(buffer));
}

void FramedTcpSocket::DoClose() {
  // Datagrams that were already committed are sent first. If any are pending,
  // the write thread shuts down the socket after sending the last one. Nothing
  // can be sent after the connection ends, so close immediately in that case.
  if (disconnected_ || (write_queue_.empty() && !write_in_progress_)) {
    ShutDownLocked();
  }
}

bool FramedTcpSocket::DoCommitWrite(Buf&& buffer) {
  // Writes committed after Close() was called or the connection ended are not
  // sent.
  if (is_open() && !disconnected_) {
    // The queue has room for every datagram that uses a slot, so this only
    // allocates when zero-length datagrams take up space. The entry is added
    // before `buffer` is moved into it, so that `buffer` can still be released
    // if allocation fails.
    if (write_queue_.try_emplace_back()) {
      write_queue_.back() = std::move(buffer);
      datagram_queued_.release();
      return true;
    }
    PW_LOG_ERROR("Failed to queue datagram for sending");
  }
  if (ReleaseWriteSlot(std::move(buffer))) {
    WakeOneWriter();
  }
  return false;
}

void FramedTcpSocket::DoCancelWrite(Buf&& buffer) {
  // CancelWrite() wakes a writer after this returns.
  ReleaseWriteSlot(std::move(buffer));
}

bool FramedTcpSocket::Start() {
  {
    std::lock_guard lock(*this);
    if (!read_queue_.try_reserve_exact(read_queue_depth_) ||
        !write_queue_.try_reserve_exact(write_queue_depth_)) {
      PW_LOG_ERROR("Failed to allocate datagram queues");
      return false;
    }
  }
  read_thread_ =
      Thread(read_thread_context_, kReadThreadAttrs, [this] { ReadLoop(); });
  write_thread_ =
      Thread(write_thread_context_, kWriteThreadAttrs, [this] { WriteLoop(); });
  return true;
}

void FramedTcpSocket::ReadLoop() {
  while (WaitForReadQueueSpace()) {
    std::array<std::byte, kFrameHeaderSize> header{};
    Status status = ReceiveExact(fd_, header);
    if (!status.ok()) {
      const int error = errno;
      std::lock_guard lock(*this);
      EndConnectionAfterIoFailureLocked(status, error);
      return;
    }

    const uint32_t size = bytes::ReadInOrder<uint32_t>(endian::big, header);
    if (size > max_read_message_size_bytes()) {
      PW_LOG_ERROR(
          "Received %u-byte datagram, which exceeds the %zu-byte limit",
          static_cast<unsigned>(size),
          max_read_message_size_bytes());
      EndConnection();
      return;
    }

    ConstBuf datagram = ConstBuf::Unowned(&kEmptyDatagram, 0);
    if (size > 0) {
      Buf buffer = Buf::TryAllocate(buffer_allocator_, size);
      if (buffer == nullptr) {
        PW_LOG_ERROR("Failed to allocate %u bytes for received datagram",
                     static_cast<unsigned>(size));
        EndConnection();
        return;
      }

      status = ReceiveExact(fd_, ByteSpan(buffer.data(), buffer.size()));
      if (!status.ok()) {
        const int error = errno;
        std::lock_guard lock(*this);
        // The peer disconnected after sending the frame header.
        EndConnectionAfterIoFailureLocked(
            status.IsOutOfRange() ? Status::DataLoss() : status, error);
        return;
      }
      datagram = std::move(buffer);
    }

    std::lock_guard lock(*this);
    if (disconnected_) {
      return;
    }
    // WaitForReadQueueSpace() ensured there is room, and Start() reserved it,
    // so this does not allocate.
    read_queue_.push_back(std::move(datagram));
    WakeReader();
  }
}

void FramedTcpSocket::WriteLoop() {
  Buf datagram;
  while (WaitForQueuedDatagram(datagram)) {
    const Status status =
        SendFrame(fd_, ConstByteSpan(datagram.data(), datagram.size()));
    const int error = errno;

    std::lock_guard lock(*this);
    write_in_progress_ = false;
    if (ReleaseWriteSlot(std::move(datagram))) {
      WakeOneWriter();
    }
    if (!status.ok()) {
      EndConnectionAfterIoFailureLocked(status, error);
      return;
    }
    if (is_closing() && write_queue_.empty()) {
      ShutDownLocked();
      return;
    }
  }
}

bool FramedTcpSocket::WaitForReadQueueSpace() {
  while (true) {
    {
      std::lock_guard lock(*this);
      if (disconnected_) {
        return false;
      }
      if (read_queue_.size() < read_queue_depth_) {
        return true;
      }
    }
    read_queue_space_.acquire();
  }
}

bool FramedTcpSocket::WaitForQueuedDatagram(Buf& datagram) {
  while (true) {
    {
      std::lock_guard lock(*this);
      if (disconnected_) {
        return false;
      }
      if (!write_queue_.empty()) {
        datagram = std::move(write_queue_.front());
        write_queue_.pop_front();
        write_in_progress_ = true;
        return true;
      }
    }
    datagram_queued_.acquire();
  }
}

bool FramedTcpSocket::ReleaseWriteSlot(Buf&& buffer) {
  if (buffer == nullptr) {
    return false;
  }
  buffer = nullptr;

  // Saturate rather than underflow if a layered socket forwards a buffer that
  // did not come from this socket.
  if (outstanding_writes_ > 0) {
    --outstanding_writes_;
  }
  return true;
}

void FramedTcpSocket::EndConnectionAfterIoFailureLocked(Status status,
                                                        int error) {
  if (disconnected_) {
    return;  // Closing the connection locally interrupted the I/O.
  }
  if (status.IsUnavailable()) {
    PW_LOG_WARN("Connection failed: %s", std::strerror(error));
  } else if (status.IsDataLoss()) {
    PW_LOG_WARN("Peer disconnected in the middle of a datagram");
  }
  // OUT_OF_RANGE means the peer disconnected between datagrams, which is a
  // normal way for a connection to end.
  EndConnectionLocked();
}

void FramedTcpSocket::EndConnection() {
  std::lock_guard lock(*this);
  EndConnectionLocked();
}

void FramedTcpSocket::EndConnectionLocked() {
  if (!is_open() || read_queue_.empty()) {
    ShutDownLocked();
    return;
  }
  DisconnectLocked();
  // DoRead() closes the socket after the last datagram is read. Until then,
  // waiting writers get reservations, but committing them fails, so a task
  // that is waiting to write before it reads again can't get stuck.
  WakeAllWriters();
}

void FramedTcpSocket::ShutDownLocked() {
  DisconnectLocked();
  // Discard datagrams that were received but not read. A ReadFuture that was
  // created before the socket closed could otherwise still return them.
  read_queue_.clear();
  MarkClosed();
}

void FramedTcpSocket::DisconnectLocked() {
  if (disconnected_) {
    return;
  }
  disconnected_ = true;

  // Wake I/O threads that are blocked in recv() or send().
  shutdown(fd_, SHUT_RDWR);
  read_queue_space_.release();
  datagram_queued_.release();

  // Discard unsent datagrams. Release their write slots, since the socket may
  // stay open until the received datagrams are read.
  for (Buf& datagram : write_queue_) {
    ReleaseWriteSlot(std::move(datagram));
  }
  write_queue_.clear();
}

}  // namespace

namespace internal {

WakePipe::~WakePipe() {
  if (read_fd_ != -1) {
    close(read_fd_);
  }
  if (write_fd_ != -1) {
    close(write_fd_);
  }
}

Status WakePipe::Open() {
  if (read_fd_ != -1) {
    return OkStatus();
  }
  std::array<int, 2> fds{};
  if (pipe(fds.data()) != 0) {
    PW_LOG_ERROR("Failed to create pipe: %s", std::strerror(errno));
    return Status::ResourceExhausted();
  }
  read_fd_ = fds[0];
  write_fd_ = fds[1];
  return OkStatus();
}

void WakePipe::Signal() {
  if (write_fd_ == -1) {
    return;
  }
  // The pipe is never drained, so a single byte keeps it readable.
  const std::byte wake{1};
  while (write(write_fd_, &wake, sizeof(wake)) < 0 && errno == EINTR) {
  }
}

}  // namespace internal

FramedTcpListener::FramedTcpListener(Allocator& allocator,
                                     const FramedTcpOptions& options)
    : allocator_(allocator), options_(options) {
  CheckOptions(options_);
}

FramedTcpListener::~FramedTcpListener() {
  Stop();
  wake_pipe_.Signal();
  unclaimed_slot_available_.release();
  if (thread_.joinable()) {
    thread_.join();
  }
  if (listen_fd_ != -1) {
    close(listen_fd_);
  }
}

Status FramedTcpListener::Listen(uint16_t port) {
  std::lock_guard lock(lock_);
  if (state_ != State::kIdle) {
    return Status::FailedPrecondition();
  }
  if (!wake_pipe_.Open().ok()) {
    return Status::Unavailable();
  }

  // A requested port may still be used by recently closed connections, such
  // as those of a previous run of the program, so allow reusing it.
  const int fd = port == 0 ? OpenListeningSocketOnUnusedPort()
                           : OpenListeningSocket(port, /*reuse_address=*/true);
  sockaddr_in6 address = {};
  socklen_t address_size = static_cast<socklen_t>(sizeof(address));
  if (fd == -1 ||
      getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) !=
          0) {
    const int error = errno;
    if (fd != -1) {
      close(fd);
    }
    PW_LOG_ERROR("Failed to listen on port %u: %s",
                 static_cast<unsigned>(port),
                 std::strerror(error));
    return Status::Unavailable();
  }

  listen_fd_ = fd;
  port_ = ntohs(address.sin6_port);
  state_ = State::kListening;
  thread_ =
      Thread(thread_context_, kAcceptThreadAttrs, [this] { AcceptLoop(); });
  return OkStatus();
}

uint16_t FramedTcpListener::port() const {
  std::lock_guard lock(lock_);
  return port_;
}

ReliableDatagramListener::AcceptFuture FramedTcpListener::Accept() {
  std::lock_guard lock(lock_);
  if (unclaimed_.has_value()) {
    AcceptFuture future = AcceptFuture::Resolved(std::move(*unclaimed_));
    unclaimed_.reset();
    unclaimed_slot_available_.release();
    return future;
  }
  if (state_ == State::kStopped) {
    return AcceptFuture::Resolved(Status::FailedPrecondition());
  }
  // Before Listen() is called, the future waits for the listener to start.
  return accept_provider_.Get();
}

void FramedTcpListener::AcceptLoop() {
  while (WaitForUnclaimedSlot()) {
    std::array<pollfd, 2> fds{};
    fds[0].fd = listen_fd_;
    fds[0].events = POLLIN;
    fds[1].fd = wake_pipe_.fd();
    fds[1].events = POLLIN;
    if (poll(fds.data(), static_cast<nfds_t>(fds.size()), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      PW_LOG_ERROR("Failed to wait for connections: %s", std::strerror(errno));
      Stop();
      return;
    }
    if (fds[1].revents != 0) {
      return;  // The listener is being destroyed.
    }

    const int fd = accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      const int error = errno;
      if (IsResourceError(error)) {
        PW_LOG_WARN("Failed to accept connection: %s", std::strerror(error));
        Deliver(Status::ResourceExhausted());
        continue;
      }
      if (error == EBADF || error == EINVAL || error == ENOTSOCK) {
        PW_LOG_ERROR("Failed to accept connections: %s", std::strerror(error));
        Stop();
        return;
      }
      // Other errors, such as a connection that was reset before it was
      // accepted, only affect a single connection.
      continue;
    }

    if (!ConfigureConnection(fd).ok()) {
      close(fd);
      continue;
    }
    Deliver(FramedTcpSocket::Create(allocator_, fd, options_));
  }
}

bool FramedTcpListener::WaitForUnclaimedSlot() {
  while (true) {
    {
      std::lock_guard lock(lock_);
      if (state_ != State::kListening) {
        return false;
      }
      if (!unclaimed_.has_value()) {
        return true;
      }
    }
    unclaimed_slot_available_.acquire();
  }
}

void FramedTcpListener::Deliver(Result<ReliableDatagramSocket>&& result) {
  std::lock_guard lock(lock_);
  if (state_ != State::kListening) {
    // `result` is a temporary in the caller, so it is destroyed after the
    // lock is released.
    return;
  }
  const bool resolved =
      accept_provider_.ResolveFirstMatching([&result](AcceptFuture&) {
        return std::make_optional(std::move(result));
      });
  if (!resolved) {
    unclaimed_ = std::move(result);
  }
}

void FramedTcpListener::Stop() {
  std::lock_guard lock(lock_);
  state_ = State::kStopped;
  accept_provider_.ResolveAll(
      [](AcceptFuture&) { return Status::FailedPrecondition(); });
}

FramedTcpConnector::FramedTcpConnector(Allocator& allocator,
                                       std::string_view host,
                                       uint16_t port,
                                       const FramedTcpOptions& options)
    : allocator_(allocator), options_(options), host_(host), port_(port) {
  CheckOptions(options_);
}

FramedTcpConnector::~FramedTcpConnector() {
  Thread thread;
  {
    std::lock_guard lock(lock_);
    stopping_ = true;
    thread = std::move(thread_);
  }
  wake_pipe_.Signal();
  connect_requested_.release();
  if (thread.joinable()) {
    thread.join();
  }
  connect_provider_.ResolveAll(
      [](ConnectFuture&) { return Status::FailedPrecondition(); });
}

ReliableDatagramConnector::ConnectFuture FramedTcpConnector::Connect() {
  std::lock_guard lock(lock_);
  if (!thread_.joinable()) {
    if (!wake_pipe_.Open().ok()) {
      return ConnectFuture::Resolved(Status::ResourceExhausted());
    }
    thread_ =
        Thread(thread_context_, kConnectThreadAttrs, [this] { ConnectLoop(); });
  }
  ConnectFuture future = connect_provider_.Get();
  connect_requested_.release();
  return future;
}

void FramedTcpConnector::ConnectLoop() {
  while (true) {
    connect_requested_.acquire();

    // Open one connection for each pending Connect() call.
    while (true) {
      {
        std::lock_guard lock(lock_);
        if (stopping_) {
          return;
        }
      }
      if (connect_provider_.empty()) {
        break;
      }

      Result<ReliableDatagramSocket> result = ConnectToPeer();
      if (result.status().IsCancelled()) {
        return;  // The connector is being destroyed.
      }
      // ResolveFirst() only takes `result` if a Connect() future is pending.
      // If the future was dropped, `result` is destroyed at the end of this
      // iteration, which closes the connection.
      connect_provider_.ResolveFirst(std::move(result));
    }
  }
}

Result<ReliableDatagramSocket> FramedTcpConnector::ConnectToPeer() {
  std::array<char, 6> port_string{};  // Up to 5 digits and a null terminator.
  PW_CHECK_OK(ToString(port_, port_string).status());

  addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;

  addrinfo* addresses = nullptr;
  if (const int result =
          getaddrinfo(host_.c_str(), port_string.data(), &hints, &addresses);
      result != 0) {
    PW_LOG_WARN(
        "Failed to resolve %s: %s", host_.c_str(), gai_strerror(result));
    return Status::Unavailable();
  }

  Result<int> fd = Status::Unavailable();
  int error = 0;
  for (const addrinfo* address = addresses; address != nullptr;
       address = address->ai_next) {
    fd = ConnectToAddress(*address, wake_pipe_.fd());
    if (fd.ok() || fd.status().IsCancelled()) {
      break;
    }
    error = errno;
  }
  freeaddrinfo(addresses);

  if (fd.status().IsUnavailable()) {
    PW_LOG_WARN("Failed to connect to %s port %u: %s",
                host_.c_str(),
                static_cast<unsigned>(port_),
                std::strerror(error));
  }
  if (!fd.ok()) {
    return fd.status();
  }
  if (!ConfigureConnection(*fd).ok()) {
    close(*fd);
    return Status::Unavailable();
  }
  return FramedTcpSocket::Create(allocator_, *fd, options_);
}

}  // namespace pw::transport
