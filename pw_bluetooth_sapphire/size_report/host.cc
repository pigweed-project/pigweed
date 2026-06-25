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

#include "pw_bloat/bloat_this_binary.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/gap/adapter.h"
#include "pw_bluetooth_sapphire/null_lease_provider.h"
#include "pw_random/random.h"

namespace {
class BloatRandomGenerator final : public pw::random::RandomGenerator {
 public:
  void Get([[maybe_unused]] pw::ByteSpan dest) override {}
  void InjectEntropyBits(uint32_t /*data*/,
                         uint_fast8_t /*num_bits*/) override {}
};

class BloatDispatcher final : public pw::async::Dispatcher {
 public:
  void Post([[maybe_unused]] pw::async::Task& task) override {}
  void PostAt(
      [[maybe_unused]] pw::async::Task& task,
      [[maybe_unused]] pw::chrono::SystemClock::time_point time) override {}
  bool Cancel([[maybe_unused]] pw::async::Task& task) override { return true; }
  pw::chrono::SystemClock::time_point now() override {
    return pw::chrono::SystemClock::time_point::min();
  }
};
}  // namespace

int main() {
  pw::bloat::BloatThisBinary();

  BloatDispatcher dispatcher;
  BloatRandomGenerator random_generator;
  bt::set_random_generator(&random_generator);
  std::unique_ptr<bt::hci::Transport> transport;
  std::unique_ptr<bt::gatt::GATT> gatt;
  pw::bluetooth_sapphire::NullLeaseProvider lease_provider;

  bt::gap::Adapter::Config config{};
  std::unique_ptr<bt::gap::Adapter> adapter =
      bt::gap::Adapter::Create(dispatcher,
                               transport->GetWeakPtr(),
                               gatt->GetWeakPtr(),
                               config,
                               lease_provider,
                               std::nullopt);
  auto gap_init_cb = []([[maybe_unused]] bool success) {};
  auto transport_closed_cb = []() {};
  adapter->Initialize(std::move(gap_init_cb), std::move(transport_closed_cb));
  return 0;
}
