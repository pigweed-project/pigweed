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

#include <fidl/fuchsia.diagnostics.types/cpp/fidl.h>
#include <fidl/fuchsia.logger/cpp/fidl.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/syslog/cpp/log_message_impl.h>
#include <lib/syslog/cpp/log_settings.h>
#include <zircon/process.h>

#include <cstdarg>
#include <cstdio>
#include <string_view>

#include "pw_assert/check.h"
#include "pw_log/levels.h"
#include "pw_log_fuchsia/log_backend.h"
#include "pw_string/string_builder.h"

namespace {

// This is an arbitrary size limit of logs.
constexpr size_t kBufferSize = 400;

// Returns the base name of a path including up to one parent directory.
constexpr const char* BaseName(const char* path) {
  const char* last_slash = nullptr;
  const char* second_last_slash = nullptr;
  for (const char* c = path; c && (*c != '\0'); c++) {
    if (*c == '/') {
      second_last_slash = last_slash;
      last_slash = c;
    }
  }
  if (second_last_slash != nullptr) {
    return second_last_slash + 1;
  }
  if (last_slash == path) {
    return last_slash + 1;
  }
  return path;
}

static_assert(std::string_view(BaseName("/a/b/c/file.cc")) == "c/file.cc");
static_assert(std::string_view(BaseName("c/file.cc")) == "c/file.cc");
static_assert(std::string_view(BaseName("/file.cc")) == "file.cc");
static_assert(std::string_view(BaseName("file.cc")) == "file.cc");

const char* LogLevelToString(int severity) {
  switch (severity) {
    case PW_LOG_LEVEL_ERROR:
      return "ERROR";
    case PW_LOG_LEVEL_WARN:
      return "WARN";
    case PW_LOG_LEVEL_INFO:
      return "INFO";
    case PW_LOG_LEVEL_DEBUG:
      return "DEBUG";
    default:
      return "UNKNOWN";
  }
}

fuchsia_logging::LogSeverity PigweedLevelToFuchsiaSeverity(int pw_level) {
  switch (pw_level) {
    case PW_LOG_LEVEL_ERROR:
      return fuchsia_logging::Error;
    case PW_LOG_LEVEL_WARN:
      return fuchsia_logging::Warn;
    case PW_LOG_LEVEL_INFO:
      return fuchsia_logging::Info;
    case PW_LOG_LEVEL_DEBUG:
      return fuchsia_logging::Debug;
    default:
      return fuchsia_logging::Error;
  }
}

}  // namespace

namespace pw::log_fuchsia {

void InitializeLogging(async_dispatcher_t* dispatcher) {
  fuchsia_logging::LogSettingsBuilder()
      .WithDispatcher(dispatcher)
      .BuildAndInitialize();
}

}  // namespace pw::log_fuchsia

extern "C" void pw_Log(int level,
                       const char* module_name,
                       unsigned int flags,
                       const char* file_name,
                       int line_number,
                       const char* message,
                       ...) {
  if (flags & PW_LOG_FLAG_IGNORE) {
    return;
  }

  fuchsia_logging::LogSeverity fuchsia_severity =
      PigweedLevelToFuchsiaSeverity(level);
  if (!(flags & PW_LOG_FLAG_USE_PRINTF) &&
      !fuchsia_logging::IsSeverityEnabled(fuchsia_severity)) {
    return;
  }

  pw::StringBuffer<kBufferSize> formatted;

  va_list args;
  va_start(args, message);
  formatted.FormatVaList(message, args);
  va_end(args);

  if (flags & PW_LOG_FLAG_USE_PRINTF) {
    printf("%s: [%s:%s:%d] %s\n",
           LogLevelToString(level),
           module_name,
           BaseName(file_name),
           line_number,
           formatted.c_str());
    return;
  }

  auto buffer = fuchsia_logging::LogBufferBuilder(fuchsia_severity)
                    .WithFile(BaseName(file_name), line_number)
                    .WithMsg(formatted)
                    .Build();
  buffer.WriteKeyValue("tag", module_name);
  [[maybe_unused]] zx::result<> result =
      fuchsia_logging::FlushToGlobalLogger(buffer);
}
