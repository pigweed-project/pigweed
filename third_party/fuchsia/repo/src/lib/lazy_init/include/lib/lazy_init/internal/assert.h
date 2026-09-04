// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The implementation here in this file is kept separate intentionally to
// enable lazy_init library to be imported in different contexts with
// custom assert implementations.
#ifndef SRC_LIB_LAZY_INIT_INCLUDE_LIB_LAZY_INIT_INTERNAL_ASSERT_H_
#define SRC_LIB_LAZY_INIT_INCLUDE_LIB_LAZY_INIT_INTERNAL_ASSERT_H_

#include "pw_assert/assert.h"

namespace lazy_init {
namespace internal {

constexpr void Assert(bool condition) {
  if (!condition) {
    PW_ASSERT(false);
  }
}

}  // namespace internal
}  // namespace lazy_init

#endif  // SRC_LIB_LAZY_INIT_INCLUDE_LIB_LAZY_INIT_INTERNAL_ASSERT_H_
