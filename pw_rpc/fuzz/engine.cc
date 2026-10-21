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

// clang-format off
#include "pw_rpc/internal/log_config.h"  // PW_LOG_* macros must be first.

#include "pw_rpc/fuzz/engine.h"
// clang-format on

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <limits>
#include <mutex>

#include "pw_assert/check.h"
#include "pw_bytes/span.h"
#include "pw_log/log.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_string/format.h"

namespace pw::rpc::fuzz {
namespace {

using namespace std::chrono_literals;

// Maximum number of bytes written in a single unary or stream request.
constexpr size_t kMaxWriteLen = MaxSafePayloadSize();
static_assert(kMaxWriteLen * 0x7E <= std::numeric_limits<uint16_t>::max());

struct ActiveVisitor final {
  using result_type = bool;
  result_type operator()(std::monostate&) { return false; }
  result_type operator()(pw::rpc::RawUnaryReceiver& call) {
    return call.active();
  }
  result_type operator()(pw::rpc::RawClientReaderWriter& call) {
    return call.active();
  }
};

struct CloseClientStreamVisitor final {
  using result_type = void;
  result_type operator()(std::monostate&) {}
  result_type operator()(pw::rpc::RawUnaryReceiver&) {}
  result_type operator()(pw::rpc::RawClientReaderWriter& call) {
    call.RequestCompletion().IgnoreError();
  }
};

struct WriteVisitor final {
  using result_type = bool;
  result_type operator()(std::monostate&) { return false; }
  result_type operator()(pw::rpc::RawUnaryReceiver&) { return false; }
  result_type operator()(pw::rpc::RawClientReaderWriter& call) {
    if (!call.active()) {
      return false;
    }
    call.Write(data).IgnoreError();
    return true;
  }
  ConstByteSpan data;
};

struct CancelVisitor final {
  using result_type = void;
  result_type operator()(std::monostate&) {}
  result_type operator()(pw::rpc::RawUnaryReceiver& call) {
    call.Cancel().IgnoreError();
  }
  result_type operator()(pw::rpc::RawClientReaderWriter& call) {
    call.Cancel().IgnoreError();
  }
};

struct AbandonVisitor final {
  using result_type = void;
  result_type operator()(std::monostate&) {}
  result_type operator()(pw::rpc::RawUnaryReceiver& call) { call.Abandon(); }
  result_type operator()(pw::rpc::RawClientReaderWriter& call) {
    call.Abandon();
  }
};

}  // namespace

// `Action` methods.

Action::Action(uint32_t encoded) {
  // The first byte is used to determine the operation. The ranges used set the
  // relative likelihood of each result, e.g. `kWait` is more likely than
  // `kAbandon`.
  uint32_t raw = encoded & 0xFF;
  if (raw == 0) {
    op = kSkip;
  } else if (raw < 0x60) {
    op = kWait;
  } else if (raw < 0x80) {
    op = kWriteUnary;
  } else if (raw < 0xA0) {
    op = kWriteStream;
  } else if (raw < 0xC0) {
    op = kCloseClientStream;
  } else if (raw < 0xD0) {
    op = kCancel;
  } else if (raw < 0xE0) {
    op = kAbandon;
  } else if (raw < 0xF0) {
    op = kSwap;
  } else {
    op = kDestroy;
  }
  target = ((encoded & 0xFF00) >> 8) % Fuzzer::kMaxConcurrentCalls;
  value = encoded >> 16;
}

Action::Action(Op op_, size_t target_, uint16_t value_)
    : op(op_), target(target_), value(value_) {}

Action::Action(Op op_, size_t target_, char val, size_t len)
    : op(op_), target(target_) {
  PW_ASSERT(op == kWriteUnary || op == kWriteStream);
  value = static_cast<uint16_t>(
      ((static_cast<uint8_t>(val) % 0x80u) * kMaxWriteLen) +
      (len % kMaxWriteLen));
}

char Action::DecodeWriteValue(uint16_t value) {
  return static_cast<char>((value / kMaxWriteLen) % 0x7F);
}

size_t Action::DecodeWriteLength(uint16_t value) {
  return value % kMaxWriteLen;
}

uint32_t Action::Encode() const {
  uint32_t encoded = 0;
  switch (op) {
    case kSkip:
      encoded = 0x00;
      break;
    case kWait:
      encoded = 0x5F;
      break;
    case kWriteUnary:
      encoded = 0x7F;
      break;
    case kWriteStream:
      encoded = 0x9F;
      break;
    case kCloseClientStream:
      encoded = 0xBF;
      break;
    case kCancel:
      encoded = 0xCF;
      break;
    case kAbandon:
      encoded = 0xDF;
      break;
    case kSwap:
      encoded = 0xEF;
      break;
    case kDestroy:
      encoded = 0xFF;
      break;
  }
  encoded |=
      ((target < Fuzzer::kMaxConcurrentCalls ? target
                                             : Fuzzer::kMaxConcurrentCalls) %
       0xFF)
      << 8;
  encoded |= (static_cast<uint32_t>(value) << 16);
  return encoded;
}

void Action::Log(bool verbose, size_t num_actions, const char* fmt, ...) const {
  if (!verbose) {
    return;
  }
  char s1[16];
  auto result = callback_id < Fuzzer::kMaxConcurrentCalls
                    ? string::Format(s1, "%-3zu", callback_id)
                    : string::Format(s1, "n/a");
  va_list ap;
  va_start(ap, fmt);
  char s2[128];
  if (result.ok()) {
    result = string::FormatVaList(s2, fmt, ap);
  }
  va_end(ap);
  if (result.ok()) {
    PW_LOG_INFO("#%-12zu\tthread: %zu\tcallback for: %s\ttarget call: %zu\t%s",
                num_actions,
                thread_id,
                s1,
                target,
                s2);
  } else {
    LogFailure(verbose, num_actions, result.status());
  }
}

void Action::LogFailure(bool verbose, size_t num_actions, Status status) const {
  if (verbose && !status.ok()) {
    PW_LOG_INFO("#%-12zu\tthread: %zu\tFailed to log action: %s",
                num_actions,
                thread_id,
                pw_StatusString(status));
  }
}

// FuzzyCall methods.

void FuzzyCall::RecordWrite(size_t num, bool append) {
  std::lock_guard lock(mutex_);
  if (append) {
    last_write_ += num;
  } else {
    last_write_ = num;
  }
  total_written_ += num;
  pending_ = true;
}

void FuzzyCall::Await() {
  std::unique_lock<sync::Mutex> lock(mutex_);
  cv_.wait(lock, [this]() PW_NO_LOCK_SAFETY_ANALYSIS { return !pending_; });
}

void FuzzyCall::Notify() {
  if (pending_.exchange(false)) {
    cv_.notify_all();
  }
}

void FuzzyCall::Swap(FuzzyCall& other) {
  if (index_ == other.index_) {
    return;
  }
  // Manually acquire locks in an order based on call IDs to prevent deadlock.
  if (index_ < other.index_) {
    mutex_.lock();
    other.mutex_.lock();
  } else {
    other.mutex_.lock();
    mutex_.lock();
  }
  call_.swap(other.call_);
  std::swap(id_, other.id_);
  pending_ = other.pending_.exchange(pending_);
  std::swap(last_write_, other.last_write_);
  std::swap(total_written_, other.total_written_);
  mutex_.unlock();
  other.mutex_.unlock();
  cv_.notify_all();
  other.cv_.notify_all();
}

void FuzzyCall::Reset(Variant call) {
  {
    std::lock_guard lock(mutex_);
    call_ = std::move(call);
  }
  cv_.notify_all();
}

void FuzzyCall::Log() {
  if (mutex_.try_lock_for(100ms)) {
    PW_LOG_INFO("call %zu:", index_);
    PW_LOG_INFO("           active: %s",
                std::visit(ActiveVisitor(), call_) ? "true" : "false");
    PW_LOG_INFO("  request pending: %s ", pending_ ? "true" : "false");
    PW_LOG_INFO("       last write: %zu bytes", last_write_);
    PW_LOG_INFO("    total written: %zu bytes", total_written_);
    mutex_.unlock();
  } else {
    PW_LOG_WARN("call %zu: failed to acquire lock", index_);
  }
}

// `Fuzzer` methods.

#define FUZZ_LOG_VERBOSE(...) \
  if (verbose_) {             \
    PW_LOG_INFO(__VA_ARGS__); \
  }

Fuzzer::Fuzzer(Client& client, uint32_t channel_id)
    : client_(client, channel_id),
      timer_([this](chrono::SystemClock::time_point) {
        PW_LOG_ERROR(
            "Workers performed %zu actions before timing out without an "
            "update.",
            num_actions_.load());
        PW_LOG_INFO("Additional call details:");
        for (auto& call : fuzzy_calls_) {
          call.Log();
        }
        PW_CRASH("Fuzzer found a fatal error condition: TIMEOUT.");
      }) {
  for (size_t index = 0; index < kMaxConcurrentCalls; ++index) {
    fuzzy_calls_.emplace_back(index);
    indices_.push_back(index);
    contexts_.push_back(CallbackContext{.id = index, .fuzzer = this});
  }
}

void Fuzzer::Run(uint64_t seed, size_t num_actions) {
  FUZZ_LOG_VERBOSE("Fuzzing RPC client with:");
  FUZZ_LOG_VERBOSE("  num_actions: %zu", num_actions);
  FUZZ_LOG_VERBOSE("         seed: %" PRIu64, seed);
  num_actions_.store(0);
  random::XorShiftStarRng64 rng(seed);
  while (true) {
    {
      size_t actions_done = num_actions_.load();
      if (actions_done >= num_actions) {
        FUZZ_LOG_VERBOSE("Fuzzing complete; %zu actions performed.",
                         actions_done);
        break;
      }
      FUZZ_LOG_VERBOSE("%zu actions remaining.", num_actions - actions_done);
    }
    FUZZ_LOG_VERBOSE("Generating %zu random actions.", kMaxActions);
    pw::Vector<uint32_t, kMaxActions> actions;
    for (size_t i = 0; i < kNumThreads; ++i) {
      size_t num_actions_for_thread;
      rng.GetInt(num_actions_for_thread, kMaxActionsPerThread + 1);
      for (size_t j = 0; j < num_actions_for_thread; ++j) {
        uint32_t encoded = 0;
        while (!encoded) {
          rng.GetInt(encoded);
        }
        actions.push_back(encoded);
      }
      actions.push_back(0);
    }
    Run(actions);
  }
}

void Fuzzer::Run(const pw::Vector<uint32_t>& actions) {
  FUZZ_LOG_VERBOSE("Starting %zu threads to perform %zu actions:",
                   kNumThreads - 1,
                   actions.size());
  FUZZ_LOG_VERBOSE("    timeout: %lldms", timer_.timeout() / 1ms);
  auto iter = actions.begin();
  timer_.Restart();
  for (size_t thread_id = 0; thread_id < kNumThreads; ++thread_id) {
    pw::Vector<uint32_t, kMaxActionsPerThread> thread_actions;
    while (thread_actions.size() < kMaxActionsPerThread &&
           iter != actions.end()) {
      uint32_t encoded = *iter++;
      if (!encoded) {
        break;
      }
      thread_actions.push_back(encoded);
    }
    if (thread_id == 0) {
      std::lock_guard lock(mutex_);
      callback_actions_ = std::move(thread_actions);
      callback_iterator_ = callback_actions_.begin();
    } else {
      threads_.emplace_back(
          [this, thread_id, actions_to_perform = std::move(thread_actions)]() {
            for (const auto& encoded : actions_to_perform) {
              Action action(encoded);
              action.set_thread_id(thread_id);
              Perform(action);
            }
          });
    }
  }
  for (auto& t : threads_) {
    t.join();
  }
  for (auto& fuzzy_call : fuzzy_calls_) {
    fuzzy_call.Reset();
  }
  timer_.Cancel();
}

void Fuzzer::Perform(const Action& action) {
  FuzzyCall& fuzzy_call = FindCall(action.target);
  switch (action.op) {
    case Action::kSkip: {
      if (action.thread_id == 0) {
        action.Log(verbose_, ++num_actions_, "Callback chain completed");
      }
      break;
    }
    case Action::kWait: {
      if (action.callback_id == action.target) {
        // Don't wait in a callback of the target call.
        break;
      }
      if (fuzzy_call.pending()) {
        action.Log(verbose_, ++num_actions_, "Waiting for call.");
        fuzzy_call.Await();
      }
      break;
    }
    case Action::kWriteUnary:
    case Action::kWriteStream: {
      if (action.callback_id == action.target) {
        // Don't create a new call from the call's own callback.
        break;
      }
      char buf[kMaxWriteLen];
      char val = Action::DecodeWriteValue(action.value);
      size_t len = Action::DecodeWriteLength(action.value);
      memset(buf, val, len);
      if (verbose_) {
        char msg_buf[64];
        span msg(msg_buf);
        auto result = string::Format(
            msg,
            "Writing %s request of ",
            action.op == Action::kWriteUnary ? "unary" : "stream");
        if (result.ok()) {
          size_t off = result.size();
          result = string::Format(
              msg.subspan(off),
              isprint(val) ? "['%c'; %zu]." : "['\\x%02x'; %zu].",
              val,
              len);
        }
        size_t num_actions = ++num_actions_;
        if (result.ok()) {
          action.Log(verbose_, num_actions, "%s", msg.data());
        } else if (verbose_) {
          action.LogFailure(verbose_, num_actions, result.status());
        }
      }
      bool append = false;
      if (action.op == Action::kWriteUnary) {
        // Send a unary request.
        fuzzy_call.Reset(client_.UnaryEcho(
            as_bytes(span(buf, len)),
            /* on completed */
            [context = GetContext(action.target)](ConstByteSpan, Status) {
              context->fuzzer->OnCompleted(context->id);
            },
            /* on error */
            [context = GetContext(action.target)](Status status) {
              context->fuzzer->OnError(context->id, status);
            }));

      } else if (fuzzy_call.Visit(
                     WriteVisitor{.data = as_bytes(span(buf, len))})) {
        // Append to an existing stream
        append = true;
      } else {
        // .Open a new stream.
        fuzzy_call.Reset(client_.BidirectionalEcho(
            /* on next */
            [context = GetContext(action.target)](ConstByteSpan) {
              context->fuzzer->OnNext(context->id);
            },
            /* on completed */
            [context = GetContext(action.target)](Status) {
              context->fuzzer->OnCompleted(context->id);
            },
            /* on error */
            [context = GetContext(action.target)](Status status) {
              context->fuzzer->OnError(context->id, status);
            }));
      }
      fuzzy_call.RecordWrite(len, append);
      break;
    }
    case Action::kCloseClientStream:
      action.Log(verbose_, ++num_actions_, "Closing stream.");
      fuzzy_call.Visit(CloseClientStreamVisitor());
      break;
    case Action::kCancel:
      action.Log(verbose_, ++num_actions_, "Canceling call.");
      fuzzy_call.Visit(CancelVisitor());
      break;
    case Action::kAbandon: {
      action.Log(verbose_, ++num_actions_, "Abandoning call.");
      fuzzy_call.Visit(AbandonVisitor());
      break;
    }
    case Action::kSwap: {
      size_t other_target = action.value % kMaxConcurrentCalls;
      if (action.callback_id == action.target ||
          action.callback_id == other_target) {
        // Don't move a call from within its own callback.
        break;
      }
      action.Log(verbose_,
                 ++num_actions_,
                 "Swapping call with call %zu.",
                 other_target);
      std::lock_guard lock(mutex_);
      FuzzyCall& other = FindCallLocked(other_target);
      std::swap(indices_[fuzzy_call.id()], indices_[other.id()]);
      fuzzy_call.Swap(other);
      break;
    }
    case Action::kDestroy: {
      if (action.callback_id == action.target) {
        // Don't destroy a call from within its own callback.
        break;
      }
      action.Log(verbose_, ++num_actions_, "Destroying call.");
      fuzzy_call.Reset();
      break;
    }
    default:
      break;
  }
  timer_.Restart();
}

void Fuzzer::OnNext(size_t callback_id) { FindCall(callback_id).Notify(); }

void Fuzzer::OnCompleted(size_t callback_id) {
  uint32_t encoded = 0;
  {
    std::lock_guard lock(mutex_);
    if (callback_iterator_ != callback_actions_.end()) {
      encoded = *callback_iterator_++;
    }
  }
  Action action(encoded);
  action.set_callback_id(callback_id);
  Perform(action);
  FindCall(callback_id).Notify();
}

void Fuzzer::OnError(size_t callback_id, Status status) {
  FuzzyCall& call = FindCall(callback_id);
  PW_LOG_WARN("Call %zu received an error from the server: %s",
              call.id(),
              pw_StatusString(status));
  call.Notify();
}

}  // namespace pw::rpc::fuzz
