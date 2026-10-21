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
// clang-format on

#include <cstring>

#include "pw_log/log.h"
#include "pw_rpc/fuzz/argparse.h"
#include "pw_rpc/fuzz/engine.h"
#include "pw_rpc/integration_testing.h"

namespace pw::rpc::fuzz {
namespace {

int FuzzClient(int argc, char** argv) {
  // TODO(aarongreen): Incorporate descriptions into usage message.
  Vector<ArgParserVariant, 5> parsers{
      // Enables additional logging.
      BoolParser("-v", "--verbose").set_default(false),

      // The number of actions to perform as part of the test. A value of 0 runs
      // indefinitely.
      UnsignedParser<size_t>("-n", "--num-actions").set_default(256),

      // The seed value for the PRNG. A value of 0 generates a seed.
      UnsignedParser<uint64_t>("-s", "--seed").set_default(0),

      // The time, in milliseconds, that can elapse without triggering an error.
      UnsignedParser<size_t>("-t", "--timeout").set_default(5000),

      // The port use to connect to the `test_rpc_server`.
      UnsignedParser<uint16_t>("port").set_default(48000)};

  if (!ParseArgs(parsers, argc, argv).ok()) {
    PrintUsage(parsers, argv[0]);
    return 1;
  }

  bool verbose;
  size_t num_actions;
  uint64_t seed;
  size_t timeout_ms;
  uint16_t port;
  if (!GetArg(parsers, "--verbose", &verbose).ok() ||
      !GetArg(parsers, "--num-actions", &num_actions).ok() ||
      !GetArg(parsers, "--seed", &seed).ok() ||
      !GetArg(parsers, "--timeout", &timeout_ms).ok() ||
      !GetArg(parsers, "port", &port).ok()) {
    return 1;
  }

  if (!seed) {
    seed = static_cast<uint64_t>(
        chrono::SystemClock::now().time_since_epoch().count());
  }

  if (auto status = integration_test::InitializeClient(port); !status.ok()) {
    PW_LOG_ERROR("Failed to initialize client: %s", pw_StatusString(status));
    return 1;
  }

  if (num_actions == 0) {
    num_actions = std::numeric_limits<size_t>::max();
  }

  Fuzzer fuzzer(integration_test::client(), integration_test::kChannelId);
  fuzzer.set_verbose(verbose);
  fuzzer.set_timeout(std::chrono::milliseconds(timeout_ms));
  fuzzer.Run(seed, num_actions);
  integration_test::TerminateClient();
  return 0;
}

}  // namespace
}  // namespace pw::rpc::fuzz

int main(int argc, char** argv) {
  return pw::rpc::fuzz::FuzzClient(argc, argv);
}
