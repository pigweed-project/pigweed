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

#include "pw_rpc2/internal/server_task.h"

#include <mutex>
#include <optional>
#include <utility>

#include "pw_assert/check.h"
#include "pw_async2/future_task.h"
#include "pw_log/log.h"
#include "pw_rpc2/server.h"

namespace pw::rpc2::internal {

ServerTask::~ServerTask() {
  {
    std::lock_guard lock(ControlLock());
    PW_CHECK(state() == State::kUnstarted || state() == State::kClosed,
             "A pw_rpc2 Server was destroyed before it finished closing. Call "
             "Close() and run the dispatcher until the returned future "
             "resolves, or call CloseBlocking() from another thread.");
    set_state_locked(State::kClosed);
    // Only an unstarted server can have anything queued at this point.
    pending_services_.clear();
  }
  Deregister();
  Shutdown();
}

// --- Control plane ---

Status ServerTask::RegisterService(Service& service) {
  std::lock_guard lock(ControlLock());
  if (state() == State::kClosing || state() == State::kClosed) {
    return Status::FailedPrecondition();
  }
  pending_services_.push_front(service);
  SignalControlLocked();
  return OkStatus();
}

ControlFuture ServerTask::RegisterListener(
    transport::ReliableDatagramListener& listener) {
  ControlFuture future(&listener);
  SubmitListener(future);
  return future;
}

ControlFuture ServerTask::Close() {
  ControlFuture future(/*listener=*/nullptr);
  SubmitClose(future);
  return future;
}

Status ServerTask::BlockOn(ControlFuture&& future) {
  // A request applied on the calling thread (before `Start()`, or once the
  // server is closed) comes back already resolved. There may be no dispatcher
  // running to poll it then, so take its status directly.
  if (std::optional<Status> status = future.TryTakeResolved();
      status.has_value()) {
    return *status;
  }
  async2::FutureTask<ControlFuture&> task(future);
  dispatcher_.Post(task);
  return task.Wait();
}

void ServerTask::Start() {
  {
    std::lock_guard lock(ControlLock());
    PW_CHECK(state() == State::kUnstarted,
             "A pw_rpc2 server may only be started once, and not after it has "
             "been closed");
    set_state_locked(State::kRunning);
    // Drain anything queued before `Start()` on the first poll.
    SignalControlLocked();
  }
  dispatcher_.Post(*this);
}

// --- Submitting requests ---

void ServerTask::SignalControlLocked() {
  control_pending_.store(true, std::memory_order_release);
  // Wake while still holding the lock. Once it is released, the task may run,
  // re-arm the waker, or be destroyed. Before `Start()` the waker is empty and
  // this is a no-op.
  control_waker_.Wake();
}

void ServerTask::SubmitListener(ControlFuture& future) {
  // Before `Start()` nothing will drain the inbox, so the request is applied
  // here, on the calling thread. The record is allocated outside the lock and
  // spliced in under it; if the server was started in the meantime, `record`
  // frees it again on the way out and the request is queued instead.
  ForwardList<ListenerRecord> record(allocator_);
  const bool allocated =
      state() == State::kUnstarted && record.try_emplace_front();

  std::lock_guard lock(ControlLock());
  switch (state()) {
    case State::kClosing:
    case State::kClosed:
      future.ResolveLocked(Status::FailedPrecondition());
      return;
    case State::kUnstarted:
      if (!allocated) {
        future.ResolveLocked(Status::ResourceExhausted());
        return;
      }
      record.front().set_listener(future.listener_locked());
      listeners_.splice_after(listeners_.before_begin(), record);
      future.ResolveLocked(OkStatus());
      return;
    case State::kRunning:
      listener_requests_.Push(future);
      SignalControlLocked();
      return;
  }
}

void ServerTask::SubmitClose(ControlFuture& future) {
  bool shutdown_now = false;
  {
    std::lock_guard lock(ControlLock());
    switch (state()) {
      case State::kClosed:
        future.ResolveLocked(OkStatus());
        return;
      case State::kUnstarted:
        // The task never ran, so there is nothing for the dispatcher to do;
        // the close completes on the calling thread.
        set_state_locked(State::kClosed);
        pending_services_.clear();
        future.ResolveLocked(OkStatus());
        shutdown_now = true;
        break;
      case State::kRunning:
        set_state_locked(State::kClosing);
        close_futures_.Push(future);
        SignalControlLocked();
        break;
      case State::kClosing:
        close_futures_.Push(future);
        break;
    }
  }
  if (shutdown_now) {
    // Frees listeners registered before `Start()`.
    Shutdown();
  }
}

// --- Dispatcher ---

async2::Poll<> ServerTask::DoPend(async2::Context& cx) {
  // Re-arm before checking the flag so that a request arriving mid-poll is
  // never lost: it either sets the flag before the exchange below, or it wakes
  // this waker after it.
  PW_ASYNC_STORE_WAKER(cx, control_waker_, "pw::rpc2::Server control");

  if (control_pending_.exchange(false, std::memory_order_acq_rel)) {
    const bool closing = state() == State::kClosing;
    ApplyPendingServices(closing);
    ApplyPendingListeners(closing);

    if (closing) {
      Shutdown();
      std::lock_guard lock(ControlLock());
      set_state_locked(State::kClosed);
      close_futures_.ResolveAllWith(
          [](ControlFuture& future)
              PW_NO_LOCK_SAFETY_ANALYSIS { future.ResolveLocked(OkStatus()); });
      return async2::Ready();
    }
  }

  PollListeners(cx);
  return async2::Pending();
}

void ServerTask::ApplyPendingServices(bool closing) {
  IntrusiveForwardList<Service> pending;
  {
    std::lock_guard lock(ControlLock());
    pending = std::move(pending_services_);
  }
  if (closing) {
    pending.clear();
    return;
  }

  while (!pending.empty()) {
    Service& service = pending.front();
    pending.pop_front();
    const uint32_t service_id = ServiceAccess::service_id(service);
    PW_CHECK(FindService(service_id) == nullptr,
             "A service with ID 0x%08x is already registered. Another instance "
             "of the same service was already registered, two different "
             "services have the same fully-qualified proto name, or their "
             "name hashes collide. Rename one of the services or packages to "
             "resolve.",
             static_cast<unsigned>(service_id));
    services_.push_front(service);
  }
}

void ServerTask::ApplyPendingListeners(bool closing) {
  while (true) {
    {
      std::lock_guard lock(ControlLock());
      if (listener_requests_.empty()) {
        return;
      }
    }

    // Allocate before taking the request, so that nothing happens under the
    // lock but a pop, a splice, and a resolve. If the request was dropped in
    // the meantime, `record` frees the node again once the lock is released.
    ForwardList<ListenerRecord> record(allocator_);
    const bool allocated = !closing && record.try_emplace_front();

    std::lock_guard lock(ControlLock());
    ControlFuture* request = listener_requests_.PopIfAvailable();
    if (request == nullptr) {
      // The request was dropped between the two critical sections.
    } else if (closing) {
      request->ResolveLocked(Status::FailedPrecondition());
    } else if (!allocated) {
      request->ResolveLocked(Status::ResourceExhausted());
    } else {
      record.front().set_listener(request->listener_locked());
      listeners_.splice_after(listeners_.before_begin(), record);
      request->ResolveLocked(OkStatus());
    }
  }
}

void ServerTask::PollListeners(async2::Context& cx) {
  // Set whenever an accept future resolved to a connection, whether or not the
  // connection could be served. Either way the listener's accept future has
  // been consumed and no longer holds this task's waker, so the task must be
  // re-enqueued for its next poll to re-arm `Accept()`.
  bool accepted = false;
  for (ListenerRecord& record : listeners_) {
    if (!record.is_accepting()) {
      continue;
    }

    if (!record.accept_future().is_pendable()) {
      record.accept_future() = record.listener().Accept();
    }

    auto poll = record.accept_future().Pend(cx);
    if (poll.IsPending()) {
      continue;
    }

    if (!poll->ok()) {
      PW_LOG_WARN("Listener Accept() returned error: %s", poll->status().str());
      record.stop_accepting();
      continue;
    }

    // Whatever happens to the connection below, the accept future is consumed.
    accepted = true;
    auto socket = std::move(poll->value());
    record.accept_future() =
        transport::ReliableDatagramListener::AcceptFuture();

    if (!connections_.try_push_front(nullptr)) {
      PW_LOG_ERROR(
          "Failed to allocate connection list node on accept; dropping "
          "connection");
      continue;
    }

    SharedPtr<ServerConnectionTask> task =
        dispatcher_.Post<ServerConnectionTask>(
            allocator_, std::move(socket), allocator_, *this);
    if (task == nullptr) {
      PW_LOG_ERROR(
          "Failed to allocate ServerConnectionTask on accept; dropping "
          "connection");
      connections_.pop_front();
      continue;
    }

    connections_.front() = std::move(task);
  }

  // Yield between accepts rather than looping here, so that one busy listener
  // cannot starve the other tasks sharing this dispatcher.
  if (accepted) {
    cx.ReEnqueue();
  }
}

void ServerTask::Shutdown() {
  while (!connections_.empty()) {
    SharedPtr<ServerConnectionTask> connection =
        std::move(connections_.front());
    connections_.pop_front();
    connection->CloseConnection(Status::Cancelled());
    connection->Deregister();
    connection->ForceRetireAllCalls();
  }

  listeners_.clear();
  services_.clear();
}

Service* ServerTask::FindService(uint32_t service_id) {
  for (Service& service : services_) {
    if (ServiceAccess::service_id(service) == service_id) {
      return &service;
    }
  }
  return nullptr;
}

void ServerTask::RemoveConnection(ServerConnectionTask& connection) {
  auto previous = connections_.before_begin();
  for (auto it = connections_.begin(); it != connections_.end(); ++it) {
    if (it->get() == &connection) {
      connections_.erase_after(previous);
      return;
    }
    previous = it;
  }
}

}  // namespace pw::rpc2::internal
