// Copyright 2025 The Pigweed Authors
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

#include "pw_containers/vector.h"

#include <cstddef>
#include <utility>

#include "pw_function/function.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace examples {

struct Message {};

// DOCSTAG: [pw_containers-vector]

class Publisher {
 public:
  using Subscriber = pw::Function<void(const Message&)>;
  static constexpr size_t kMaxSubscribers = 10;

  pw::Status Subscribe(Subscriber&& subscriber) {
    // Check if the vector's fixed capacity has been exhausted.
    if (subscribers_.full()) {
      return pw::Status::ResourceExhausted();
    }

    // Add the subscriber to the vector.
    subscribers_.emplace_back(std::move(subscriber));
    return pw::OkStatus();
  }

  void Publish(const Message& message) {
    // Iterate over the vector.
    for (auto& subscriber : subscribers_) {
      subscriber(message);
    }
  }

 private:
  pw::Vector<Subscriber, kMaxSubscribers> subscribers_;
};

// DOCSTAG: [pw_containers-vector]

}  // namespace examples

namespace {

TEST(VectorExampleTest, PublishMessages) {
  examples::Publisher publisher;
  examples::Message ignored;
  size_t count1 = 0;
  size_t count2 = 0;
  auto status = pw::OkStatus();

  status =
      publisher.Subscribe([&count1](const examples::Message&) { ++count1; });
  EXPECT_EQ(status, pw::OkStatus());
  publisher.Publish(ignored);
  EXPECT_EQ(count1, 1U);
  EXPECT_EQ(count2, 0U);

  status =
      publisher.Subscribe([&count2](const examples::Message&) { ++count2; });
  EXPECT_EQ(status, pw::OkStatus());
  publisher.Publish(ignored);
  publisher.Publish(ignored);
  EXPECT_EQ(count1, 3U);
  EXPECT_EQ(count2, 2U);
}

}  // namespace
