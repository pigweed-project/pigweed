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

#include <cstddef>

namespace pw::multibuf::v2 {

/// @submodule{pw_multibuf,v2}

/// Base class for a subscriber that can be notified when a MultiBuf changes.
///
/// As an example, one possible usage is as part of a flow control scheme:
/// An observer tracks how many bytes have been received and added to or removed
/// and sent from one or more MultiBufs. It uses this information to update
/// peers on how much more to send, and to update local tasks how much they may
/// send.
class Observer {
 public:
  /// A notification from a MultiBuf.
  ///
  /// Each Event is paired with a value with an Event-specifc meaning.
  enum class Event {
    /// The associated value gives the number of bytes added to the MultiBuf, or
    /// added to the top layer if the MultiBuf is layerable.
    kBytesAdded,

    /// The associated value gives the number of bytes removed from the
    /// MultiBuf, or removed from the top layer if the MultiBuf is layerable.
    kBytesRemoved,

    /// The associated value gives the number of fragments in the previous top
    /// layer that have been coaleasced into a single fragment in the new top
    /// layer.
    kLayerAdded,

    /// The associated value gives the number of fragments in the previous top
    /// layer that was removed.
    kLayerRemoved,
  };

  virtual ~Observer() = default;

  /// Notifies the observer that an event has occurred.
  void Notify(Event event, size_t value) { DoNotify(event, value); }

 private:
  virtual void DoNotify(Event event, size_t value) = 0;
};

/// @}

}  // namespace pw::multibuf::v2
