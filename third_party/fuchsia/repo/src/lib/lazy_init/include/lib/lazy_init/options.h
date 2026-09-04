// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_LAZY_INIT_INCLUDE_LIB_LAZY_INIT_OPTIONS_H_
#define SRC_LIB_LAZY_INIT_INCLUDE_LIB_LAZY_INIT_OPTIONS_H_

namespace lazy_init {

// Enum that specifies what kind of debug init checks to perform for a
// lazy-initialized global variable.
enum class CheckType {
  // No checks are performed.
  None,

  // Initialization checks are performed. If multiple threads will access the
  // global variable, initialization must be manually serialized with respect
  // to the guard variable.
  Basic,

  // Initialization checks are performed using atomic operations. Checks are
  // guaranteed to be consistent, even when races occur over initialization.
  Atomic,

  // The default check type as specified by the build. This is the check type
  // used when not explicitly specified. It may also be specified explicitly
  // to defer to the build configuration when setting other options.
  // TODO(eieio): Add the build arg and conditional logic.
  Default = None,
};

// Enum that specifies whether to enable a lazy-initialized global variable's
// destructor. Disabling global destructors avoids destructor registration.
// However, destructors can be conditionally enabled on builds that require
// them, such as ASAN.
enum class Destructor {
  Disabled,
  Enabled,

  // The default destructor enablement as specified by the build. This is the
  // enablement used when not explicitly specified. It may also be specified
  // explicitly to defer to the build configuration when setting other
  // options.
  // TODO(eieio): Add the build arg and conditional logic.
  Default = Disabled,
};

}  // namespace lazy_init

#endif  // SRC_LIB_LAZY_INIT_INCLUDE_LIB_LAZY_INIT_OPTIONS_H_
