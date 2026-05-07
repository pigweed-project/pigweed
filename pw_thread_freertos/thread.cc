// Copyright 2020 The Pigweed Authors
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

#include "FreeRTOS.h"
#include "pw_assert/check.h"
#include "pw_preprocessor/compiler.h"
#include "pw_thread_freertos/config.h"
#include "pw_thread_freertos/context.h"
#include "pw_thread_freertos/options.h"
#include "task.h"

using pw::thread::freertos::Context;

namespace pw::thread {
namespace {

#if (INCLUDE_xTaskGetSchedulerState != 1) && (configUSE_TIMERS != 1)
#error "xTaskGetSchedulerState is required for pw::Thread"
#endif

#if PW_THREAD_JOINING_ENABLED
constexpr EventBits_t kThreadDoneBit = 1 << 0;
#endif  // PW_THREAD_JOINING_ENABLED
}  // namespace

void Context::ThreadEntryPoint(void* void_context_ptr) {
  Context& context = *static_cast<Context*>(void_context_ptr);

  // Invoke the user's thread function. This may never return.
  context.fn_();
  context.fn_ = nullptr;

  // Use a task only critical section to guard against join() and detach().
  vTaskSuspendAll();
  if (context.detached()) {
    // There is no threadsafe way to re-use detached threads, as there's no way
    // to signal the vTaskDelete success. Joining MUST be used for this.
    // However to enable unit test coverage we go ahead and clear this.
    context.set_task_handle(nullptr);

#if PW_THREAD_JOINING_ENABLED
    // If the thread handle was detached before the thread finished execution,
    // i.e. got here, then we are responsible for cleaning up the join event
    // group.
    vEventGroupDelete(
        reinterpret_cast<EventGroupHandle_t>(&context.join_event_group()));
#endif  // PW_THREAD_JOINING_ENABLED

#if PW_THREAD_FREERTOS_CONFIG_DYNAMIC_ALLOCATION_ENABLED
    // The thread was detached before the task finished, free any allocations
    // it ran on.
    if (context.dynamically_allocated()) {
      delete &context;
    }
#endif  // PW_THREAD_FREERTOS_CONFIG_DYNAMIC_ALLOCATION_ENABLED

    // Re-enable the scheduler before we delete this execution.
    xTaskResumeAll();
    vTaskDelete(nullptr);
    PW_UNREACHABLE;
  }

  // Otherwise the task finished before the thread was detached or joined, defer
  // cleanup to Thread's join() or detach().
  context.set_thread_done();
  xTaskResumeAll();

#if PW_THREAD_JOINING_ENABLED
  xEventGroupSetBits(
      reinterpret_cast<EventGroupHandle_t>(&context.join_event_group()),
      kThreadDoneBit);
#endif  // PW_THREAD_JOINING_ENABLED

  while (true) {
#if INCLUDE_vTaskSuspend == 1
    // Use indefinite suspension when available.
    vTaskSuspend(nullptr);
#else
    vTaskDelay(portMAX_DELAY);
#endif  // INCLUDE_vTaskSuspend == 1
  }
  PW_UNREACHABLE;
}

void Context::TerminateThread(Context& context) {
  // Stop the other task first.
  PW_DCHECK_NOTNULL(context.task_handle(), "We shall not delete ourselves!");
  vTaskDelete(context.task_handle());

  // Mark the context as unused for potential later re-use.
  context.set_task_handle(nullptr);

#if PW_THREAD_JOINING_ENABLED
  // Just in case someone abused our API, ensure their use of the event group is
  // properly handled by the kernel regardless.
  vEventGroupDelete(
      reinterpret_cast<EventGroupHandle_t>(&context.join_event_group()));
#endif  // PW_THREAD_JOINING_ENABLED

#if PW_THREAD_FREERTOS_CONFIG_DYNAMIC_ALLOCATION_ENABLED
  // Then free any allocations it ran on.
  if (context.dynamically_allocated()) {
    delete &context;
  }
#endif  // PW_THREAD_FREERTOS_CONFIG_DYNAMIC_ALLOCATION_ENABLED
}

void Context::AddToEventGroup() {
#if PW_THREAD_JOINING_ENABLED
  const EventGroupHandle_t event_group_handle =
      xEventGroupCreateStatic(&join_event_group());
  PW_DCHECK_PTR_EQ(event_group_handle,
                   &join_event_group(),
                   "Failed to create the joining event group");
#endif  // PW_THREAD_JOINING_ENABLED
}

void Context::CreateThread(const freertos::Options& options,
                           Function<void()>&& thread_fn,
                           Context*& native_type_out) {
  TaskHandle_t task_handle;
  if (options.static_context() != nullptr) {
    // Use the statically allocated context.
    native_type_out = options.static_context();
    // Can't use a context more than once.
    PW_DCHECK_PTR_EQ(native_type_out->task_handle(), nullptr);
    // Reset the state of the static context in case it was re-used.
    native_type_out->set_detached(false);
    native_type_out->set_thread_done(false);
    native_type_out->AddToEventGroup();

    // In order to support functions which return and joining, a delegate is
    // deep copied into the context with a small wrapping function to actually
    // invoke the task with its arg.
    native_type_out->set_thread_routine(std::move(thread_fn));
    task_handle = xTaskCreateStatic(Context::ThreadEntryPoint,
                                    options.name(),
                                    options.static_context()->stack().size(),
                                    native_type_out,
                                    options.priority(),
                                    options.static_context()->stack().data(),
                                    &options.static_context()->tcb());
  } else {
#if !PW_THREAD_FREERTOS_CONFIG_DYNAMIC_ALLOCATION_ENABLED
    PW_CRASH(
        "dynamic thread allocations are not enabled and no static_context "
        "was provided");
#else   // PW_THREAD_FREERTOS_CONFIG_DYNAMIC_ALLOCATION_ENABLED
    // Dynamically allocate the context and the task.
    native_type_out = new pw::thread::freertos::Context();
    native_type_out->set_dynamically_allocated();
    native_type_out->AddToEventGroup();

    // In order to support functions which return and joining, a delegate is
    // deep copied into the context with a small wrapping function to actually
    // invoke the task with its arg.
    native_type_out->set_thread_routine(std::move(thread_fn));
    const BaseType_t result = xTaskCreate(Context::ThreadEntryPoint,
                                          options.name(),
                                          options.stack_size_words(),
                                          native_type_out,
                                          options.priority(),
                                          &task_handle);

    // Ensure it succeeded.
    PW_CHECK_UINT_EQ(result, pdPASS);
#endif  // !PW_THREAD_FREERTOS_CONFIG_DYNAMIC_ALLOCATION_ENABLED
  }
  PW_CHECK_NOTNULL(task_handle);  // Ensure it succeeded.
  native_type_out->set_task_handle(task_handle);

#if configNUM_THREAD_LOCAL_STORAGE_POINTERS > 0
  const auto& pointers = options.tls_pointers();
  for (uint32_t i = 0; i < configNUM_THREAD_LOCAL_STORAGE_POINTERS; ++i) {
    vTaskSetThreadLocalStoragePointer(
        task_handle, i, reinterpret_cast<void*>(pointers[i]));
  }
#endif
}

Thread::Thread(const thread::Options& facade_options, Function<void()>&& entry)
    : native_type_(nullptr) {
  // Cast the generic facade options to the backend specific option of which
  // only one type can exist at compile time.
  auto options = static_cast<const freertos::Options&>(facade_options);
  Context::CreateThread(options, std::move(entry), native_type_);
}

void Thread::detach() {
  PW_CHECK(joinable());

  // xTaskResumeAll() can only be used after the scheduler has been started.
  const bool scheduler_initialized =
      xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;

  if (scheduler_initialized) {
    // We don't want to individually suspend and resume this task using
    // vTaskResume() as that can cause tasks to prematurely wake up and return
    // from blocking APIs (b/303885539).
    vTaskSuspendAll();
  }
  native_type_->set_detached();
  const bool thread_done = native_type_->thread_done();
  if (scheduler_initialized) {
    xTaskResumeAll();
  }

  if (thread_done) {
    // The task finished (hit end of Context::ThreadEntryPoint) before we
    // invoked detach, clean up the thread.
    Context::TerminateThread(*native_type_);
  } else {
    // We're detaching before the task finished, defer cleanup to the task at
    // the end of Context::ThreadEntryPoint.
  }

  // Update to no longer represent a thread of execution.
  native_type_ = nullptr;
}

#if PW_THREAD_JOINING_ENABLED
void Thread::join() {
  PW_CHECK(joinable());
  PW_CHECK(this_thread::get_id() != get_id());

  // Wait indefinitely until kThreadDoneBit is set.
  while (xEventGroupWaitBits(reinterpret_cast<EventGroupHandle_t>(
                                 &native_type_->join_event_group()),
                             kThreadDoneBit,
                             pdTRUE,   // Clear the bits.
                             pdFALSE,  // Any bits is fine, N/A.
                             portMAX_DELAY) != kThreadDoneBit) {
  }

  // No need for a critical section here as the thread at this point is
  // waiting to be terminated.
  Context::TerminateThread(*native_type_);

  // Update to no longer represent a thread of execution.
  native_type_ = nullptr;
}
#endif  // PW_THREAD_JOINING_ENABLED

}  // namespace pw::thread
