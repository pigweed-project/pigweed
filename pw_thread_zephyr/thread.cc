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
#include "pw_thread/thread.h"

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>

#include "public/pw_thread_zephyr/context.h"
#include "public/pw_thread_zephyr/options.h"
#include "pw_assert/check.h"
#include "pw_preprocessor/compiler.h"
#include "pw_thread_zephyr/context.h"
#include "pw_thread_zephyr/options.h"

using pw::thread::backend::NativeContext;
using pw::thread::backend::NativeOptions;

namespace pw::thread {
namespace {

k_spinlock global_thread_done_lock;

}  // namespace

void NativeContext::ThreadEntryPoint(void* void_context_ptr, void*, void*) {
  NativeContext& context = *static_cast<NativeContext*>(void_context_ptr);

  // Invoke the user's thread function. This may never return.
  context.fn_();
  context.fn_ = nullptr;

  k_spinlock_key_t key = k_spin_lock(&global_thread_done_lock);
  if (context.detached()) {
    context.task_handle_ = nullptr;
  } else {
    // Defer cleanup to Thread's join() or detach().
    context.set_thread_done();
  }
  k_spin_unlock(&global_thread_done_lock, key);
}

void NativeContext::CreateThread(Function<void()>&& thread_fn,
                                 const NativeOptions& options) {
  PW_CHECK(!fn_);
  detached_ = false;
  thread_done_ = false;
  fn_ = std::move(thread_fn);
  string::Assign(name_, options.name()).IgnoreError();

  // Verify we have a valid stack.
  span<z_thread_stack_element> thread_stack;
  if (!options.stack().empty()) {
    thread_stack = options.stack();  // Default to the option's stack.
  } else {
    thread_stack = stack();  // Fall back to the ContextWithStack's stack.
  }
  PW_CHECK(!thread_stack.empty());
  const k_tid_t task_handle = k_thread_create(&thread_info_,
                                              thread_stack.data(),
                                              thread_stack.size(),
                                              NativeContext::ThreadEntryPoint,
                                              this,
                                              nullptr,
                                              nullptr,
                                              options.priority(),
                                              options.native_options(),
                                              K_NO_WAIT);
  PW_CHECK_NOTNULL(task_handle);  // Ensure it succeeded.
  task_handle_ = task_handle;

  if constexpr (IS_ENABLED(CONFIG_THREAD_NAME)) {
    // If we can set the name in the native thread, do so
    const int thread_name_set_result =
        k_thread_name_set(task_handle, options.name());
    // Of possible return status, we should not fault reading this memory
    // (EFAULT) and we should have this function available as we just checked
    // the configuration in the preprocessor statement above (ENOSYS).
    //
    // Truncating the name (EINVAL) is fine as at the time of commit, the string
    // gets set anyway, and of course, successful return is fine.
    PW_DASSERT((thread_name_set_result != -EFAULT) &&
               (thread_name_set_result != -ENOSYS));
  }
}

NativeContext* NativeOptions::CreateThread(Function<void()>&& thread_fn) {
  PW_CHECK_NOTNULL(context_);

  context_->CreateThread(std::move(thread_fn), *this);
  return context_;
}

Thread::Thread(const thread::Options& facade_options, Function<void()>&& entry)
    : native_type_(nullptr) {
  // Cast the generic facade options to the backend specific option of which
  // only one type can exist at compile time.
  auto options = static_cast<const NativeOptions&>(facade_options);
  native_type_ = options.CreateThread(std::move(entry));
}

void Thread::detach() {
  PW_CHECK(joinable());

  k_spinlock_key_t key = k_spin_lock(&global_thread_done_lock);
  native_type_->set_detached();
  const bool thread_done = native_type_->thread_done();

  if (thread_done) {
    // The task finished (hit end of Context::ThreadEntryPoint) before we
    // invoked detach, clean up the task handle to allow the Context reuse.
    native_type_->task_handle_ = nullptr;
  } else {
    // We're detaching before the task finished, defer cleanup to the task
    // at the end of Context::ThreadEntryPoint.
  }

  k_spin_unlock(&global_thread_done_lock, key);

  // Update to no longer represent a thread of execution.
  native_type_ = nullptr;
}

void Thread::join() {
  PW_CHECK(joinable());
  PW_CHECK(this_thread::get_id() != get_id());

  PW_CHECK_INT_EQ(0, k_thread_join(native_type_->task_handle_, K_FOREVER));

  native_type_->task_handle_ = nullptr;

  // Update to no longer represent a thread of execution.
  native_type_ = nullptr;
}

}  // namespace pw::thread
