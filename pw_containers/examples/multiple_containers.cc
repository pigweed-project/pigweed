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

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_containers/intrusive_list.h"
#include "pw_containers/intrusive_map.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

// DOCSTAG: [pw_containers-multiple_containers]

// The base type for lists can be trivially derived.
struct ListItem : public pw::containers::future::IntrusiveList<ListItem>::Item {
};

// The base type for maps needs a constructor.
struct MapPair : public pw::IntrusiveMap<const uint32_t&, MapPair>::Pair {
  constexpr MapPair(const uint32_t& id)
      : pw::IntrusiveMap<const uint32_t&, MapPair>::Pair(id) {}
};

struct Task : public ListItem, public MapPair {
  uint32_t id = 0;
  constexpr explicit Task() : MapPair(id) {}
};

namespace examples {

class Scheduler {
 public:
  // Adds a task to the queue, and returns an opaque `id` that identifies it.
  // Returns INVALID_ARGUMENT if the task is already in the queue.
  pw::Result<uint32_t> ScheduleTask(Task& task) {
    if (task.id != 0) {
      return pw::Status::InvalidArgument();
    }
    task.id = ++num_ids_;
    by_id_.insert(task);
    queue_.push_back(task);
    return task.id;
  }

  // Removes a task associated with a given `id` from the queue.
  // Returns NOT_FOUND if the task is not in the queue.
  pw::Status CancelTask(uint32_t id) {
    auto iter = by_id_.find(id);
    if (iter == by_id_.end()) {
      return pw::Status::NotFound();
    }
    auto& task = static_cast<Task&>(*iter);
    by_id_.erase(iter);
    queue_.remove(task);
    task.id = 0;
    return pw::OkStatus();
  }

  // Runs the next task, if any, and returns its `id`.
  // Returns NOT_FOUND if the queue is empty.
  pw::Result<uint32_t> RunTask() {
    if (queue_.empty()) {
      return pw::Status::NotFound();
    }
    auto& task = static_cast<Task&>(queue_.front());
    queue_.pop_front();
    by_id_.erase(task.id);
    return task.id;
  }

 private:
  // NOTE! The containers must be templated on their specific item types, not
  // the composite `Task` type.
  pw::containers::future::IntrusiveList<ListItem> queue_;
  pw::IntrusiveMap<uint32_t, MapPair> by_id_;
  uint32_t num_ids_ = 0;
};

// DOCSTAG: [pw_containers-multiple_containers]

}  // namespace examples

namespace {

TEST(ListedAndMappedExampleTest, RunScheduler) {
  examples::Scheduler scheduler;
  constexpr size_t kNumTasks = 10;
  std::array<Task, kNumTasks> tasks;
  std::array<uint32_t, kNumTasks> ids;
  pw::Result<uint32_t> result;

  for (size_t i = 0; i < tasks.size(); ++i) {
    result = scheduler.ScheduleTask(tasks[i]);
    ASSERT_EQ(result.status(), pw::OkStatus());
    ids[i] = *result;
  }
  result = scheduler.ScheduleTask(tasks[0]);
  EXPECT_EQ(result.status(), pw::Status::InvalidArgument());

  EXPECT_EQ(scheduler.CancelTask(ids[3]), pw::OkStatus());
  EXPECT_EQ(scheduler.CancelTask(ids[7]), pw::OkStatus());
  EXPECT_EQ(scheduler.CancelTask(ids[7]), pw::Status::NotFound());

  for (size_t i = 0; i < tasks.size(); ++i) {
    if (i % 4 == 3) {
      continue;
    }
    result = scheduler.RunTask();
    ASSERT_EQ(result.status(), pw::OkStatus());
    EXPECT_EQ(*result, ids[i]);
  }
  result = scheduler.RunTask();
  EXPECT_EQ(result.status(), pw::Status::NotFound());
}

}  // namespace
