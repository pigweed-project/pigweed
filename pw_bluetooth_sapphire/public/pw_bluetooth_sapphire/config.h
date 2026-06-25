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

// Configuration macros for the pw_bluetooth_sapphire module.
#pragma once

// Disable Fuchsia inspect code by default.
#ifndef PW_BLUETOOTH_SAPPHIRE_INSPECT_ENABLED
#define NINSPECT 1
#endif  // PW_BLUETOOTH_SAPPHIRE_INSPECT_ENABLED

// Disable Fuchsia tracing code by default.
#ifndef PW_BLUETOOTH_SAPPHIRE_TRACE_ENABLED
#define NTRACE 1
#endif  // PW_BLUETOOTH_SAPPHIRE_TRACE_ENABLED

#ifdef PW_SAPPHIRE_LEASE_TOKENIZED

#include "pw_tokenizer/tokenize.h"
#define PW_SAPPHIRE_LEASE_TOKEN_TYPE pw_tokenizer_Token
#define PW_SAPPHIRE_LEASE_TOKEN_EXPR PW_TOKENIZE_STRING_EXPR

#else

#define PW_SAPPHIRE_LEASE_TOKEN_TYPE const char*
#define PW_SAPPHIRE_LEASE_TOKEN_EXPR(name) name

#endif  // PW_SAPPHIRE_LEASE_TOKENIZED

#ifdef PW_SAPPHIRE_WAKE_ALARM_TOKENIZED

#include "pw_tokenizer/tokenize.h"
#define PW_SAPPHIRE_WAKE_ALARM_TOKEN_TYPE pw_tokenizer_Token
#define PW_SAPPHIRE_WAKE_ALARM_TOKEN_EXPR PW_TOKENIZE_STRING_EXPR

#else

#define PW_SAPPHIRE_WAKE_ALARM_TOKEN_TYPE const char*
#define PW_SAPPHIRE_WAKE_ALARM_TOKEN_EXPR(name) name

#endif  // PW_SAPPHIRE_WAKE_ALARM_TOKENIZED
