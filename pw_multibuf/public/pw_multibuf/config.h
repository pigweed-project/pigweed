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
#pragma once

/// @module{pw_multibuf}

#ifndef PW_MULTIBUF_VERSION
/// Sets the version of MultiBuf provided by this module.
///
/// This module is undergoing refactoring to improve usability and
/// performance. Some portions of the version 1 API are in use by downstream
/// consumers. These legacy interfaces are preserved for now, but deprecated.
///
/// Both versions are intended to minimize copying when working with sequences
/// of memory buffers. These buffers are typically used to hold datagrams for
/// use with I/O, and may be discontiguous.
///
/// Notable differences between the two versions include:
/// * V1 required the use of a MultiBufAllocator, whereas V2 defers memory
///   allocation strategies to the consumer.
/// * V1 could provide views of higher level protocols be releasing and claiming
///   prefixes and suffixes. V2 instead describes "layers" of span-like views of
///   the underlying data.
///
/// Versions higher than 2 are currently unsupported.
///
/// Initially, this setting defaults to 1. Eventually, this will default to 2.
/// Downstream projects may still use version 1 by overriding this
/// configuration, but must be aware that version 1 will eventually be removed.
#define PW_MULTIBUF_VERSION 1
#endif  // PW_MULTIBUF_VERSION

#ifndef PW_MULTIBUF_WARN_DEPRECATED
/// Enables warnings about using legacy MultiBuf.
///
/// This module is undergoing refactoring to improve usability and
/// performance. Some portions of the version 1 API are in use by downstream
/// consumers. These legacy interfaces are preserved for now, but deprecated.
///
/// Initially, this setting defaults to 0 and pw_multibuf.v1 may still be
/// consumed without warning.  At some point, this will default to 1. Downstream
/// projects may still suppress the warning by overriding this configuration,
/// but must be aware that legacy interfaces will eventually be removed.
///
/// See b/418013384 for background and details.
#define PW_MULTIBUF_WARN_DEPRECATED 0
#endif  // PW_MULTIBUF_WARN_DEPRECATED

#if PW_MULTIBUF_WARN_DEPRECATED
#define PW_MULTIBUF_DEPRECATED \
  [[deprecated("See b/418013384 for background and workarounds.")]]
#else
#define PW_MULTIBUF_DEPRECATED
#endif  // PW_MULTIBUF_WARN_DEPRECATED

#ifndef PW_MULTIBUF_INCLUDE_V1_ADAPTERS
/// Includes v1_adapter for v2.
///
/// When true and `PW_MULTIBUF_VERSION` is 2, pw_multibuf will include adapter
/// types that match the v1 API and wrap corresponding v2 types. These types
/// facilitate migrating between versions. When project has migrated to v2, it
/// can set this to 0 to verify that no code is still using the v1 API.
#define PW_MULTIBUF_INCLUDE_V1_ADAPTERS 1
#endif  // PW_MULTIBUF_INCLUDE_V1_ADAPTERS

/// @}
