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
#pragma once

#include <type_traits>

#include "pw_assert/check.h"
#include "pw_random/random.h"

namespace bt {

// Returns the global random number generator. This returns nullptr until it is
// configured by initialization code.
pw::random::RandomGenerator* random_generator();

// Sets the global random number generator used by the host stack. To prevent
// overriding, the current generator must be nullptr or an assert will fail.
void set_random_generator(pw::random::RandomGenerator* generator);

template <typename T>
T Random() {
  static_assert(std::is_trivial_v<T> && !std::is_pointer_v<T>,
                "Type cannot be filled with random bytes");
  PW_DCHECK(random_generator());
  T t = T{};
  random_generator()->Get({reinterpret_cast<std::byte*>(&t), sizeof(T)});
  return t;
}

}  // namespace bt
