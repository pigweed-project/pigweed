// Copyright 2022 The Pigweed Authors
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

// IWYU pragma: private, include "pw_log/log.h"

#include "pw_log_string/handler.h"  // IWYU pragma: export

// Log a message with many attributes included. This is a backend implementation
// for the logging facade in pw_log/log.h.
//
// This is the log macro frontend that funnels everything into the C-based
// message hangler facade, i.e. pw_log_string_HandleMessage. It's not efficient
// at the callsite, since it passes many arguments.
//
// Users can configure exactly what is passed to pw_log_string_HandleMessage by
// providing their own PW_LOG_STRING_HANDLE_MESSAGE implementation.
#define PW_HANDLE_LOG PW_LOG_STRING_HANDLE_MESSAGE
