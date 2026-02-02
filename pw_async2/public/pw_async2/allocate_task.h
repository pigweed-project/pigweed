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
#pragma once

#include "pw_allocator/allocator.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/internal/owned_task.h"

namespace pw::async2 {
namespace internal {

template <typename Pendable>
class AllocatedTask final : public OwnedTask {
 public:
  template <typename... Args>
  AllocatedTask(pw::allocator::Deallocator& deallocator, Args&&... args)
      : deallocator_(deallocator), pendable_(std::forward<Args>(args)...) {}

 private:
  Poll<> DoPend(Context& cx) override { return pendable_.Pend(cx); }

  void DoDestroy() override { deallocator_.Delete(this); }

  pw::allocator::Deallocator& deallocator_;
  Pendable pendable_;
};

}  // namespace internal

/// @submodule{pw_async2,alloc}

/// Creates a ``Task`` by dynamically allocating ``Task`` memory from
/// ``allocator``.
///
/// Returns ``nullptr`` on allocation failure.
/// ``Pendable`` must have a ``Poll<> Pend(Context&)`` method.
/// ``allocator`` must outlive the resulting ``Task``.
template <typename Pendable>
Task* AllocateTask(pw::allocator::Allocator& allocator, Pendable&& pendable) {
  return allocator.New<internal::AllocatedTask<Pendable>>(
      allocator, std::forward<Pendable>(pendable));
}

/// Creates a ``Task`` by dynamically allocating ``Task`` memory from
/// ``allocator``.
///
/// Returns ``nullptr`` on allocation failure.
/// ``Pendable`` must have a ``Poll<> Pend(Context&)`` method.
/// ``allocator`` must outlive the resulting ``Task``.
template <typename Pendable, typename... Args>
Task* AllocateTask(pw::allocator::Allocator& allocator, Args&&... args) {
  return allocator.New<internal::AllocatedTask<Pendable>>(
      allocator, std::forward<Args>(args)...);
}

/// @endsubmodule

}  // namespace pw::async2
