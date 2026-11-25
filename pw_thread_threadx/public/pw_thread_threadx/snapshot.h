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
#pragma once

#include "pw_protobuf/encoder.h"
#include "pw_status/status.h"
#include "pw_thread/snapshot.h"
#include "pw_thread_protos/thread.pwpb.h"
#include "tx_api.h"

namespace pw::thread::threadx {

/// @module{pw_thread_threadx}

/// Captures all ThreadX threads in a system as part of a snapshot.
///
/// An updated `running_thread_stack_pointer` must be provided in order for the
/// running thread's context to reflect the running state. For ARM, you might do
/// something like this:
///
/// @code{.cpp}
///   // Capture PSP.
///   void* stack_ptr = 0;
///   asm volatile("mrs %0, psp\n" : "=r"(stack_ptr));
///   pw::thread::ProcessThreadStackCallback cb =
///       [](pw::thread::proto::Thread::StreamEncoder& encoder,
///          pw::ConstByteSpan stack) -> pw::Status {
///     return encoder.WriteRawStack(stack);
///   };
///   pw::thread::threadx::SnapshotThreads(stack_ptr, snapshot_encoder, cb);
/// @endcode
///
/// @pre Interrupts and the ThreadX scheduler must be disabled.
///
/// @warning SMP ports are not yet supported.
///
/// @param[in] running_thread_stack_pointer Updated stack pointer of the
/// currently running thread.
/// @param[in,out] encoder Protobuf encoder to write captured threads into.
/// @param[in,out] thread_stack_callback Callback invoked for each thread to
/// capture either its `raw_stack` or `raw_backtrace`.
///
/// @returns
/// * @OK: All threads were captured successfully.
/// * @OUT_OF_RANGE: A thread's stack pointer underflowed past the stack start.
/// * Any error status returned by `thread_stack_callback` or `encoder`.
Status SnapshotThreads(void* running_thread_stack_pointer,
                       proto::SnapshotThreadInfo::StreamEncoder& encoder,
                       ProcessThreadStackCallback& thread_stack_callback);

/// Captures only the provided thread handle as a `pw.thread.Thread` proto
/// message. After thread info capture, `thread_stack_callback` is called to
/// capture either the `raw_stack` or `raw_backtrace`.
///
/// An updated `running_thread_stack_pointer` must be provided in order for the
/// running thread's context to reflect the current state. If the thread being
/// captured is not the running thread, the value is ignored. Note that the
/// stack pointer in the thread handle is almost always stale on the running
/// thread.
///
/// Captures the following `pw.thread.Thread` proto fields:
/// * `name`
/// * `id`
/// * `state`
/// * `stack_start_pointer`
/// * `stack_end_pointer`
/// * `stack_pointer`
///
/// @pre Interrupts and the ThreadX scheduler must be disabled.
///
/// @warning SMP ports are not yet supported.
///
/// @param[in] thread ThreadX TCB to capture.
/// @param[in] running_thread_stack_pointer Updated stack pointer of the
/// currently running thread (ignored if `thread` is not the running thread).
/// @param[in,out] encoder Protobuf encoder to write the captured thread into.
/// @param[in,out] thread_stack_callback Callback invoked to capture either the
/// `raw_stack` or `raw_backtrace`.
///
/// @returns
/// * @OK: The thread was captured successfully.
/// * @OUT_OF_RANGE: The thread's stack pointer underflowed past the stack
///   start.
/// * Any error status returned by `thread_stack_callback` or `encoder`.
Status SnapshotThread(const TX_THREAD& thread,
                      void* running_thread_stack_pointer,
                      proto::Thread::StreamEncoder& encoder,
                      ProcessThreadStackCallback& thread_stack_callback);

/// @endmodule

}  // namespace pw::thread::threadx
