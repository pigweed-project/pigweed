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

#include <tuple>

#include "pw_allocator/allocator.h"
#include "pw_async2/dispatcher.h"
#include "pw_channel/channel.h"
#include "pw_function/function.h"
#include "pw_rpc/server.h"

namespace pw {

/// @module{pw_system}

/// Opinionated system framework
namespace system {

class AsyncCore;  // Forward declaration for function declaration.

}  // namespace system

/// Returns a reference to the global pw_system instance. pw::System() provides
/// several features for applications: a memory allocator, an async dispatcher,
/// and a RPC server.
system::AsyncCore& System();

/// @copydoc ::system::StartAndClobberTheStack
/// @deprecated Use pw::system::StartAndClobberTheStack() for more visibility
/// at the call site. The two functions are otherwise the same.
[[deprecated("Use pw::system::StartAndClobberTheStack()")]] [[noreturn]] void
SystemStart(channel::ByteReaderWriter& io_channel);

namespace system {

/// Starts running `pw_system:async` with the provided IO channel. This function
/// never returns, and depending on the backend it may completely clobber the
/// callers stack so that the scheduled threads can use all of that memory.
///
/// @note Do not call this function if you constructed anything on the
/// stack which must remain intact, such as an RPC service derived from
/// pw::rpc::Service. You should instead either declare those as static or
/// allocate them from the heap to prevent them from being randomly overwritten
/// by this call.
///
/// @par
/// To minimize trouble, we recommend you structure your code to perform all
/// initialization in a function that then returns to main(). Then main() can
/// call StartAndClobberTheStack() with "nothing" on the stack. Even then, the
/// C/C++ runtime initialization may have put values on the stack in order to
/// call main(), and those would be trashed as well.
///
/// @code{.cpp}
///  void main() {
///    Initialization();
///    pw::system::StartAndClobberTheStack();
///  }
/// @endcode
///
[[noreturn]] void StartAndClobberTheStack(
    channel::ByteReaderWriter& io_channel);

/// The global `pw::System()` instance. This object is safe to access, whether
/// `pw::SystemStart` has been called or not.
class AsyncCore {
 public:
  /// Returns the system `pw::Allocator` instance.
  Allocator& allocator();

  /// Returns the system `pw::async2::Dispatcher` instance.
  async2::Dispatcher& dispatcher();

  /// Returns the system `pw::rpc::Server` instance.
  rpc::Server& rpc_server() { return rpc_server_; }

  /// Runs a function once on a separate thread. If the function blocks, it may
  /// prevent other functions from running.
  ///
  /// @returns true if the function was enqueued to run, false if the function
  /// queue is full
  [[nodiscard]] bool RunOnce(Function<void()>&& function);

 private:
  friend AsyncCore& pw::System();
  friend void pw::system::StartAndClobberTheStack(channel::ByteReaderWriter&);

  explicit _PW_RPC_CONSTEXPR AsyncCore()
      : rpc_channels_{}, rpc_server_(rpc_channels_) {}

  void Init(channel::ByteReaderWriter& io_channel);

  static async2::Poll<> InitTask(async2::Context&);

  rpc::Channel rpc_channels_[1];
  rpc::Server rpc_server_;
};

}  // namespace system

inline system::AsyncCore& System() {
  _PW_RPC_CONSTINIT static system::AsyncCore system_core;
  return system_core;
}

}  // namespace pw
