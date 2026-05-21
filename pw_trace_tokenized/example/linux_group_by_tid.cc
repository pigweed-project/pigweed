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
//==============================================================================
//

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "pw_log/log.h"
#include "pw_trace/trace.h"
#include "pw_trace_tokenized/example/trace_to_file.h"
#include "pw_trace_tokenized/trace_callback.h"
#include "pw_trace_tokenized/trace_tokenized.h"

// Example for annotating trace events with thread id.
// The platform annotates instants and duration events with the thread id if the
// caller does not explicitly provide a group. The thread id is written in
// the trace_id field.
//
// This example requires linux_config_overrides.h to define
// PW_TRACE_HAS_TRACE_ID. Set pw_trace_CONFIG to
// "$dir_pw_trace_tokenized:linux_config_overrides" in target_toolchains.gni to
// enable the override before building this example.
//
// TODO(ykyyip): update trace_tokenized.py to handle the trace_id.

pw_trace_TraceEventReturnFlags TraceEventCallback(
    void* /*user_data*/, pw_trace_tokenized_TraceEvent* event) {
  // Instant and duration events with no group means group by pid/tid.
  if ((event->event_type == PW_TRACE_EVENT_TYPE_INSTANT) ||
      (event->event_type == PW_TRACE_EVENT_TYPE_DURATION_START) ||
      (event->event_type == PW_TRACE_EVENT_TYPE_DURATION_END)) {
    event->trace_id = static_cast<uint32_t>(syscall(__NR_gettid));
  }
  return PW_TRACE_EVENT_RETURN_FLAGS_NONE;
}

void ExampleTask(void* /*arg*/) {
  int times_to_run = 10;
  while (times_to_run--) {
    PW_TRACE_START("Processing");
    //  Fake processing time.
    std::this_thread::sleep_for(std::chrono::milliseconds(42));
    PW_TRACE_END("Processing");
    //  Sleep for a random amount before running again.
    int sleep_time = 1 + std::rand() % 20;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
  }
}

void RunThreadedTraceSampleApp() {
  std::srand(static_cast<unsigned int>(std::time(nullptr)));

  // Start threads to show parallel processing.
  int num_threads = 5;
  while (num_threads--) {
    PW_TRACE_INSTANT("CreateThread");
    std::thread thread(ExampleTask, nullptr);
    thread.detach();
  }
}

int main(int argc, char** argv) {
  if (argc != 2) {
    PW_LOG_ERROR("Expected output file name as argument.\n");
    return -1;
  }

  // Enable tracing.
  PW_TRACE_SET_ENABLED(true);

  // Dump trace data to the file passed in.
  pw::trace::TraceToFile trace_to_file(pw::trace::GetCallbacks(), argv[1]);

  // Register platform callback
  pw::trace::RegisterCallbackWhenCreated(pw::trace::GetCallbacks(),
                                         TraceEventCallback);

  PW_LOG_INFO("Running threaded trace example...\n");
  RunThreadedTraceSampleApp();

  // Sleep forever
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
  }
  return 0;
}
