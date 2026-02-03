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

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "pw_assert/assert.h"
#include "pw_clock_tree/internal/deferred_init.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/mutex.h"

/// Clock tree management library
namespace pw::clock_tree {

/// @module{pw_clock_tree}

/// Abstract base class for a clock tree element of a clock tree.
///
/// Class implementations of `Element` must implement `DoAcquireLocked` and
/// `DoReleaseLocked` functions. For clock tree elements that only get enabled /
/// configured, it is sufficient to only override the `DoEnable` function,
/// otherwise it is required to override the `DoDisable` function to disable the
/// respective clock tree element.
///
/// Note: Clock tree element classes shouldn't be directly derived from the
/// `Element` class, but from the `ElementBlocking`,
/// `ElementNonBlockingCannotFail` or `ElementNonBlockingMightFail` class.
class Element {
 public:
  constexpr Element(bool may_block) : may_block_(may_block) {}
  virtual ~Element() = default;

  /// Get reference count for this clock tree element.
  uint32_t ref_count() const { return ref_count_; }

  /// Check whether acquiring or releasing the element may block.
  bool may_block() const { return may_block_; }

  // Not copyable or movable
  Element(const Element&) = delete;
  Element(const Element&&) = delete;
  Element& operator=(const Element&) = delete;
  Element& operator=(const Element&&) = delete;

  /// Acquire a reference to this clock tree element.
  ///
  /// Note: Calling this method called from interrupt context or with
  /// interrupts disabled is only permitted on an `ElementNonBlockingMightFail`
  /// or `ElementNonBlockingCannotFail` instance, and not on a
  /// `ElementBlocking` or generic `Element` instance.
  Status Acquire() { return DoAcquire(); }

  /// Release a reference to this clock tree element.
  ///
  /// Note: Calling this method called from interrupt context or with
  /// interrupts disabled is only permitted on an `ElementNonBlockingMightFail`
  /// or `ElementNonBlockingCannotFail` instance, and not on a
  /// `ElementBlocking` or generic `Element` instance.
  Status Release() { return DoRelease(); }

  /// Acquire a reference to this clock tree element while `element_with`
  /// clock tree is enabled.
  /// Acquiring the clock tree element might fail.
  ///
  /// This is useful when dealing with synchronized clock muxes where, in order
  /// to switch to a new clock source, both the old and new clock must be
  /// running. This ensures that the old clock source (`element_with`) is
  /// running before attempting to activate the new clock source (`element`).
  ///
  /// Note: May not be called from inside an interrupt context or with
  /// interrupts disabled.
  Status AcquireWith(Element& element_with) {
    PW_TRY(element_with.Acquire());
    Status status = Acquire();
    // Ignore any error on release because it should not fail if the acquire
    // succeeded, and there's no reasonable alternative.
    element_with.Release().IgnoreError();
    return status;
  }

 protected:
  /// Acquire a reference to the clock tree element.
  ///
  /// Acquiring a reference to a clock tree element ensures that the
  /// clock tree element is configured and enabled.
  ///
  /// If the clock tree element depends on another clock tree element,
  /// a reference to the dependent clock tree element will get
  /// acquired when the first reference to this clock tree element
  /// gets acquired. This ensures that all dependent clock tree
  /// elements have been enabled before this clock tree element gets
  /// configured and enabled.
  virtual Status DoAcquireLocked() = 0;

  /// Release a reference to the clock tree element.
  ///
  /// Releasing the last reference to the clock tree element will disable
  /// the clock tree element.
  ///
  /// When the last reference to the clock tree element gets released,
  /// the clock tree element gets disabled if the `DoDisable` function
  /// is overridden.
  ///
  /// If the clock tree element depends on another clock tree element,
  /// a reference to the dependent clock tree element will get
  /// released once the last reference to this clock tree element has
  /// been released and the clock tree element has been disabled.
  /// This ensures that the clock tree element gets disabled before
  /// all dependent clock tree elements have been disabled.
  virtual Status DoReleaseLocked() = 0;

  /// Increment reference count and return incremented value.
  uint32_t IncRef() { return ++ref_count_; }

  /// Decrement reference count and return decremented value.
  uint32_t DecRef() {
    PW_ASSERT(ref_count_ > 0);
    return --ref_count_;
  }

  /// Function called when the clock tree element needs to get enabled.
  virtual Status DoEnable() = 0;

  /// Function called when the clock tree element can get disabled.
  ///
  /// Can be overridden by child class in case the clock tree element
  /// can be disabled to save power.
  virtual Status DoDisable() { return OkStatus(); }

 private:
  /// Reference count for this tree element.
  uint32_t ref_count_ = 0;

  /// Whether acquiring or releasing the element may block.
  const bool may_block_;

  /// Handle Acquire(), deferring locking the child class.
  virtual Status DoAcquire() = 0;

  /// Handle Release(), deferring locking the child class.
  virtual Status DoRelease() = 0;
};

/// Abstract class of a clock tree element that might need to block to perform
/// element updates.
class ElementBlocking : public Element {
 public:
  static constexpr bool kMayBlock = true;
  static constexpr bool kMayFail = true;
  constexpr ElementBlocking() : Element(kMayBlock) {}

 protected:
  sync::Mutex& lock() { return *mutex_; }

 private:
  // sync::Mutex is not constexpr-constructible so use DeferredInit so we can
  // keep our constexpr constructor.
  internal::DeferredInit<sync::Mutex> mutex_;

  Status DoAcquire() final {
    std::lock_guard lock(this->lock());
    return DoAcquireLocked();
  }
  Status DoRelease() final {
    std::lock_guard lock(this->lock());
    return DoReleaseLocked();
  }
};

/// Abstract class of a clock tree element that will not block to perform
/// element updates and might fail when performing clock updates.
class ElementNonBlockingMightFail : public Element {
 public:
  static constexpr bool kMayBlock = false;
  static constexpr bool kMayFail = true;
  constexpr ElementNonBlockingMightFail() : Element(kMayBlock) {}

 protected:
  sync::InterruptSpinLock& lock() { return spin_lock_; }

 private:
  sync::InterruptSpinLock spin_lock_;

  Status DoAcquire() final {
    std::lock_guard lock(this->lock());
    return DoAcquireLocked();
  }
  Status DoRelease() final {
    std::lock_guard lock(this->lock());
    return DoReleaseLocked();
  }
};

/// Abstract class of a clock tree element that will not block to perform
/// element updates and will not fail when performing clock updates.
class ElementNonBlockingCannotFail : public ElementNonBlockingMightFail {
 public:
  static constexpr bool kMayFail = false;

  /// Acquire a reference to this clock tree element.
  void Acquire() { PW_DASSERT(ElementNonBlockingMightFail::Acquire().ok()); }

  /// Release a reference to this clock tree element.
  void Release() { PW_DASSERT(ElementNonBlockingMightFail::Release().ok()); }
};

/// Abstract class template of a clock tree element that provides a clock
/// source.
///
/// A `ClockSource` clock tree element does not depend on any other
/// clock tree element, but provides a clock to the system that is not
/// derived from another clock.
///
/// Class implementations of `ClockSource` must implement
/// `DoAcquireLocked` and `DoReleaseLocked` functions. For clock sources that
/// only get enabled / configured, it is sufficient to only override the
/// `DoEnable` function, otherwise it is required to override the `DoDisable`
/// function to disable the clock source.
///
/// Template argument `ElementType` can be of class `ElementBlocking`,
/// `ElementNonBlockingCannotFail` or
/// `ElementNonBlockingMightFail.`
template <typename ElementType>
class ClockSource : public ElementType {
 private:
  /// Acquire a reference to the clock source.
  ///
  /// When the first reference gets acquired, the clock source gets enabled.
  Status DoAcquireLocked() final {
    if (this->IncRef() > 1) {
      // This clock tree element is already enabled.
      return OkStatus();
    }

    // Enable clock source.
    Status status = this->DoEnable();
    if (!status.ok()) {
      this->DecRef();
    }
    return status;
  }

  /// Release a reference to the clock source.
  ///
  /// When the last reference gets released, the clock source gets disabled.
  Status DoReleaseLocked() final {
    if (this->DecRef() > 0) {
      // The clock tree element remains enabled.
      return OkStatus();
    }

    // Disable the clock source.
    Status status = this->DoDisable();
    if (!status.ok()) {
      this->IncRef();
    }
    return status;
  }
};

/// Class that represents a no-op clock source clock tree element that can
/// be used to satisfy the dependent source clock tree element dependency
/// for clock source classes that expect a source clock tree element.
class ClockSourceNoOp : public ClockSource<ElementNonBlockingCannotFail> {
 private:
  pw::Status DoEnable() final { return pw::OkStatus(); }

  pw::Status DoDisable() final { return pw::OkStatus(); }
};

/// Class that represents a no-op clock source clock tree element that can
/// be used to satisfy the dependent source clock tree element dependency
/// for clock source classes that expect a source clock tree element.
class ClockSourceNoOpBlocking : public ClockSource<ElementBlocking> {
 private:
  pw::Status DoEnable() final { return pw::OkStatus(); }

  pw::Status DoDisable() final { return pw::OkStatus(); }
};

/// Abstract class template of a clock tree element that depends on another
/// clock tree element.
///
/// A `DependentElement` clock tree element depends on another
/// clock tree element.
///
/// Class implementations of `DependentElement` must override the
/// `DoEnable` function, the `DoDisable` function can be overridden to disable
/// the dependent clock tree element to save power.
///
/// Template argument `ElementType` can be of class `ElementBlocking`,
/// `ElementNonBlockingCannotFail` or
/// `ElementNonBlockingMightFail.`
template <typename ElementType>
class DependentElement : public ElementType {
 public:
  /// Create a dependent clock tree element that depends on `source`.
  constexpr DependentElement(ElementType& source) : source_(&source) {}

 private:
  /// Acquire a reference to the dependent clock tree element.
  ///
  /// When the first reference gets acquired, a reference to the source
  /// element gets acquired, before the dependent clock tree element gets
  /// enabled.
  Status DoAcquireLocked() final {
    if (this->IncRef() > 1) {
      // This clock tree element is already enabled.
      return OkStatus();
    }

    // Acquire a reference to the dependent clock tree element before
    // enabling this clock tree element.
    if (Status status = source_->Acquire(); !status.ok()) {
      this->DecRef();
      return status;
    }

    Status status = this->DoEnable();
    if (!status.ok()) {
      source_->Release().IgnoreError();
      this->DecRef();
    }
    return status;
  }

  /// Release a reference to the dependent clock tree element.
  ///
  /// When the last reference gets released, the dependent clock tree
  /// element gets disabled (if implemented), before the reference to the
  /// `source` element gets released.
  Status DoReleaseLocked() final {
    if (this->DecRef() > 0) {
      // The clock tree element remains enabled.
      return OkStatus();
    }

    // Disable the clock tree element.
    if (Status status = this->DoDisable(); !status.ok()) {
      this->IncRef();
      return status;
    }
    // Even if releasing the dependent source references fails,
    // we won't re-enable the clock source, and instead just return
    // the error code to the caller.
    return source_->Release();
  }

 private:
  // NOTE: This is an Element* not ElementType* to allow the Acquire() and
  // Release() calls above to consistently use a Status return type.
  // We still use ElementType& in the constructor to ensure a nonblocking clock
  // element cannot depend on a blocking one.
  // TODO: https://pwbug.dev/434801926 - Change this this to ElementType and
  // use something like:
  //   if constexpr (std::remove_pointer_t<decltype(source_)>::kMayFail)
  // to handle the optional Status return value.
  Element* source_;
};

/// Abstract class of the clock divider specific interface.
///
/// The clock divider interface allows APIs to accept a `ClockDivider`
/// element, if they want to use the `SetDivider` method.
/// They can use the `element` method to call the `Acquire` and `Release`
/// methods.
class ClockDivider {
 public:
  constexpr ClockDivider(Element& element) : element_(element) {}

  virtual ~ClockDivider() = default;

  /// Set `divider` value.
  ///
  /// The `divider` value will get updated as part of this method if the clock
  /// divider is currently active, otherwise the new divider value will be
  /// configured when the clock divider gets enabled next.
  Status SetDivider(uint32_t divider) { return DoSetDivider(divider); }

  /// Return the element implementing this interface.
  Element& element() const { return element_; }

 private:
  /// Reference to element implementing this interface.
  Element& element_;

  virtual Status DoSetDivider(uint32_t divider) = 0;
};

/// Abstract class template of a clock divider element.
///
/// A `ClockDivider` clock tree element depends on another clock tree element
/// and has a divider value that gets configured when the clock divider gets
/// enabled.
///
/// Class implementations of `ClockDivider` must override the `DoEnable`
/// function.
///
/// Template argument `ElementType` can be of class `ElementBlocking`,
/// `ElementNonBlockingCannotFail` or
/// `ElementNonBlockingMightFail.`
template <typename ElementType>
class ClockDividerElement : public DependentElement<ElementType>,
                            public ClockDivider {
 public:
  /// Create a clock divider element that depends on `source` and gets
  /// configured with `divider` value when enabled.
  constexpr ClockDividerElement(ElementType& source, uint32_t divider)
      : DependentElement<ElementType>(source),
        ClockDivider(static_cast<Element&>(*this)),
        divider_(divider) {}

  /// Set `divider` value.
  ///
  /// The `divider` value will get updated as part of this method if the clock
  /// divider is currently active, otherwise the new divider value will be
  /// configured when the clock divider gets enabled next.
  template <typename T = ElementType>
  typename std::enable_if_t<T::kMayFail, pw::Status> SetDivider(
      uint32_t divider) {
    return ClockDivider::SetDivider(divider);
  }

  /// Set `divider` value.
  ///
  /// The `divider` value will get updated as part of this method if the clock
  /// divider is currently active, otherwise the new divider value will be
  /// configured when the clock divider gets enabled next.
  template <typename T = ElementType>
  typename std::enable_if_t<!T::kMayFail, void> SetDivider(uint32_t divider) {
    PW_ASSERT_OK(ClockDivider::SetDivider(divider));
  }

 protected:
  /// Get current divider value.
  uint32_t divider() const { return divider_; }

 private:
  /// Configured divider value.
  uint32_t divider_;

  Status DoSetDivider(uint32_t divider) final {
    std::lock_guard lock(this->lock());
    return DoSetDividerLocked(divider);
  }

  Status DoSetDividerLocked(uint32_t divider) {
    uint32_t old_divider = divider_;
    divider_ = divider;
    if (this->ref_count() == 0) {
      return OkStatus();
    }

    Status status = this->DoEnable();
    if (!status.ok()) {
      // Restore old divider value.
      divider_ = old_divider;
    }
    return status;
  }
};

/// Alias for a blocking clock divider tree element.
using ClockDividerBlocking = ClockDividerElement<ElementBlocking>;

/// Alias for a non-blocking clock divider tree element where updates cannot
/// fail.
using ClockDividerNonBlockingCannotFail =
    ClockDividerElement<ElementNonBlockingCannotFail>;

/// Alias for a non-blocking clock divider tree element where updates might
/// fail.
using ClockDividerNonBlockingMightFail =
    ClockDividerElement<ElementNonBlockingMightFail>;

/// An optional reference to an Element which can be Acquired and Released.
///
/// This avoids the verbosity of checking if an element pointer is null in
/// e.g. a driver which accepts an optional Element* argument.
class OptionalElement {
 public:
  explicit constexpr OptionalElement(Element* element = nullptr)
      : element_(element) {}

  explicit constexpr OptionalElement(Element& element)
      : OptionalElement(&element) {}

  /// Acquire a reference to the optional clock tree element.
  ///
  /// If the optional element pointer is null, the function just returns
  /// `pw::OkStatus()`.
  Status Acquire() {
    if (element_ != nullptr) {
      return element_->Acquire();
    }
    return OkStatus();
  }

  /// Release a reference to the optional clock tree element.
  ///
  /// If the optional element pointer is null, the function just returns
  /// `pw::OkStatus()`.
  Status Release() {
    if (element_ != nullptr) {
      return element_->Release();
    }
    return OkStatus();
  }

 private:
  Element* element_ = nullptr;
};

}  // namespace pw::clock_tree
