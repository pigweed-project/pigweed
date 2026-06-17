.. _module-pw_sync:

=======
pw_sync
=======
.. pigweed-module::
   :name: pw_sync

The ``pw_sync`` module contains utilities for synchronizing between threads
and/or interrupts through signaling primitives and critical section lock
primitives.

.. Note::

   The objects in this module do not have an Init() style public API which is
   common in many RTOS C APIs. Instead, they rely on being able to invoke the
   native initialization APIs for synchronization primitives during C++
   construction.

   In order to support global statically constructed synchronization without
   constexpr constructors, the user and/or backend **MUST** ensure that any
   initialization required in your environment is done prior to the creation
   and/or initialization of the native synchronization primitives
   (e.g. kernel initialization).

--------------------------------
Critical Section Lock Primitives
--------------------------------
The critical section lock primitives provided by this module comply with
`BasicLockable <https://en.cppreference.com/w/cpp/named_req/BasicLockable>`_,
`Lockable <https://en.cppreference.com/w/cpp/named_req/Lockable>`_, and where
relevant
`TimedLockable <https://en.cppreference.com/w/cpp/named_req/TimedLockable>`_ C++
named requirements. This means that they are compatible with existing helpers in
the STL's ``<mutex>`` thread support library. For example `std::lock_guard
<https://en.cppreference.com/w/cpp/thread/lock_guard>`_ and `std::unique_lock
<https://en.cppreference.com/w/cpp/thread/unique_lock>`_ can be directly used.

Mutex
=====
The Mutex is a synchronization primitive that can be used to protect shared data
from being simultaneously accessed by multiple threads. It offers exclusive,
non-recursive ownership semantics where priority inheritance is used to solve
the classic priority-inversion problem.

The Mutex's API is C++11 STL
`std::mutex <https://en.cppreference.com/w/cpp/thread/mutex>`_ like,
meaning it is a
`BasicLockable <https://en.cppreference.com/w/cpp/named_req/BasicLockable>`_
and `Lockable <https://en.cppreference.com/w/cpp/named_req/Lockable>`_.

.. list-table::
   :header-rows: 1

   * - Supported on
     - Backend module
   * - FreeRTOS
     - :ref:`module-pw_sync_freertos`
   * - Zephyr
     - :ref:`module-pw_sync_zephyr`
   * - ThreadX
     - :ref:`module-pw_sync_threadx`
   * - embOS
     - :ref:`module-pw_sync_embos`
   * - STL
     - :ref:`module-pw_sync_stl`
   * - Baremetal
     - Planned
   * - CMSIS-RTOS API v2 & RTX5
     - Planned

C++
---
.. list-table::
   :header-rows: 1
   :widths: 70 10 10 10

   * - Safe to use in context
     - Thread
     - Interrupt
     - NMI
   * - Constructor
     - ✔
     -
     -
   * - Destructor
     - ✔
     -
     -
   * - :cc:`lock <pw::sync::Mutex::lock>`
     - ✔
     -
     -
   * - :cc:`try_lock <pw::sync::Mutex::try_lock>`
     - ✔
     -
     -
   * - :cc:`unlock <pw::sync::Mutex::unlock>`
     - ✔
     -
     -

Examples in C++
^^^^^^^^^^^^^^^
.. code-block:: cpp

   #include "pw_sync/mutex.h"

   pw::sync::Mutex mutex;

   void ThreadSafeCriticalSection() {
     mutex.lock();
     NotThreadSafeCriticalSection();
     mutex.unlock();
   }

Alternatively you can use C++'s RAII helpers to ensure you always unlock.

.. code-block:: cpp

   #include <mutex>

   #include "pw_sync/mutex.h"

   pw::sync::Mutex mutex;

   void ThreadSafeCriticalSection() {
     std::lock_guard lock(mutex);
     NotThreadSafeCriticalSection();
   }

C
-
The Mutex must be created in C++, however it can be passed into C using the
``pw_sync_Mutex`` opaque struct alias.

.. list-table::
   :header-rows: 1
   :widths: 70 10 10 10

   * - Safe to use in context
     - Thread
     - Interrupt
     - NMI
   * - :cc:`pw_sync_Mutex_Lock`
     - ✔
     -
     -
   * - :cc:`pw_sync_Mutex_TryLock`
     - ✔
     -
     -
   * - :cc:`pw_sync_Mutex_Unlock`
     - ✔
     -
     -

Example in C
^^^^^^^^^^^^
.. code-block:: cpp

   #include "pw_sync/mutex.h"

   pw::sync::Mutex mutex;

   extern pw_sync_Mutex mutex;  // This can only be created in C++.

   void ThreadSafeCriticalSection(void) {
     pw_sync_Mutex_Lock(&mutex);
     NotThreadSafeCriticalSection();
     pw_sync_Mutex_Unlock(&mutex);
   }

TimedMutex
==========
The :cc:`TimedMutex <pw::sync::TimedMutex>` is an extension of the Mutex
which offers timeout and deadline based semantics.

The :cc:`TimedMutex <pw::sync::TimedMutex>`'s API is C++11 STL
`std::timed_mutex <https://en.cppreference.com/w/cpp/thread/timed_mutex>`_ like,
meaning it is a
`BasicLockable <https://en.cppreference.com/w/cpp/named_req/BasicLockable>`_,
`Lockable <https://en.cppreference.com/w/cpp/named_req/Lockable>`_, and
`TimedLockable <https://en.cppreference.com/w/cpp/named_req/TimedLockable>`_.

Note that the :cc:`TimedMutex <pw::sync::TimedMutex>` is a derived
:cc:`Mutex <pw::sync::Mutex>` class, meaning that a :cc:`TimedMutex
<pw::sync::TimedMutex>` can be used by someone who needs the basic
:cc:`Mutex <pw::sync::Mutex>`. This is in contrast to the C++ STL's
`std::timed_mutex <https://en.cppreference.com/w/cpp/thread/timed_mutex>`_.

.. list-table::
   :header-rows: 1

   * - Supported on
     - Backend module
   * - FreeRTOS
     - :ref:`module-pw_sync_freertos`
   * - ThreadX
     - :ref:`module-pw_sync_threadx`
   * - embOS
     - :ref:`module-pw_sync_embos`
   * - STL
     - :ref:`module-pw_sync_stl`
   * - Zephyr
     - Planned
   * - CMSIS-RTOS API v2 & RTX5
     - Planned

C++
---
.. list-table::
   :header-rows: 1
   :widths: 70 10 10 10

   * - Safe to use in context
     - Thread
     - Interrupt
     - NMI
   * - Constructor
     - ✔
     -
     -
   * - Destructor
     - ✔
     -
     -
   * - ``lock``
     - ✔
     -
     -
   * - ``try_lock``
     - ✔
     -
     -
   * - :cc:`try_lock_for <pw::sync::TimedMutex::try_lock_for>`
     - ✔
     -
     -
   * - :cc:`try_lock_until <pw::sync::TimedMutex::try_lock_until>`
     - ✔
     -
     -
   * - ``unlock``
     - ✔
     -
     -

Examples in C++
^^^^^^^^^^^^^^^
.. code-block:: cpp

   #include "pw_chrono/system_clock.h"
   #include "pw_sync/timed_mutex.h"

   pw::sync::TimedMutex mutex;

   bool ThreadSafeCriticalSectionWithTimeout(const SystemClock::duration timeout) {
     if (!mutex.try_lock_for(timeout)) {
       return false;
     }
     NotThreadSafeCriticalSection();
     mutex.unlock();
     return true;
   }

Alternatively you can use C++'s RAII helpers to ensure you always unlock.

.. code-block:: cpp

   #include <mutex>

   #include "pw_chrono/system_clock.h"
   #include "pw_sync/timed_mutex.h"

   pw::sync::TimedMutex mutex;

   bool ThreadSafeCriticalSectionWithTimeout(const SystemClock::duration timeout) {
     std::unique_lock lock(mutex, std::defer_lock);
     if (!lock.try_lock_for(timeout)) {
       return false;
     }
     NotThreadSafeCriticalSection();
     return true;
   }

C
-
The TimedMutex must be created in C++, however it can be passed into C using the
``pw_sync_TimedMutex`` opaque struct alias.

.. list-table::
   :header-rows: 1
   :widths: 70 10 10 10

   * - Safe to use in context
     - Thread
     - Interrupt
     - NMI
   * - :cc:`pw_sync_TimedMutex_Lock`
     - ✔
     -
     -
   * - :cc:`pw_sync_TimedMutex_TryLock`
     - ✔
     -
     -
   * - :cc:`pw_sync_TimedMutex_TryLockFor`
     - ✔
     -
     -
   * - :cc:`pw_sync_TimedMutex_TryLockUntil`
     - ✔
     -
     -
   * - :cc:`pw_sync_TimedMutex_Unlock`
     - ✔
     -
     -

Example in C
^^^^^^^^^^^^
.. code-block:: cpp

   #include "pw_chrono/system_clock.h"
   #include "pw_sync/timed_mutex.h"

   pw::sync::TimedMutex mutex;

   extern pw_sync_TimedMutex mutex;  // This can only be created in C++.

   bool ThreadSafeCriticalSectionWithTimeout(
       const pw_chrono_SystemClock_Duration timeout) {
     if (!pw_sync_TimedMutex_TryLockFor(&mutex, timeout)) {
       return false;
     }
     NotThreadSafeCriticalSection();
     pw_sync_TimedMutex_Unlock(&mutex);
     return true;
   }

RecursiveMutex
==============
``pw_sync`` provides ``pw::sync::RecursiveMutex``, a recursive mutex
implementation. At this time, this facade can only be used internally by
Pigweed.

InterruptSpinLock
=================
:cc:`InterruptSpinLock <pw::sync::InterruptSpinLock>` is a synchronization
primitive that can be used to protect shared data from being simultaneously
accessed by multiple threads and/or interrupts as a targeted global lock, with
the exception of Non-Maskable Interrupts (NMIs). It offers exclusive,
non-recursive ownership semantics where IRQs up to a backend defined level of
"NMIs" will be masked to solve priority-inversion.

This InterruptSpinLock relies on built-in local interrupt masking to make it
interrupt safe without requiring the caller to separately mask and unmask
interrupts when using this primitive.

Unlike global interrupt locks, this also works safely and efficiently on SMP
systems. On systems which are not SMP, spinning is not required but some state
may still be used to detect recursion.

The InterruptSpinLock is a
`BasicLockable <https://en.cppreference.com/w/cpp/named_req/BasicLockable>`_
and
`Lockable <https://en.cppreference.com/w/cpp/named_req/Lockable>`_.

.. list-table::
   :header-rows: 1

   * - Supported on
     - Backend module
   * - FreeRTOS
     - :ref:`module-pw_sync_freertos`
   * - ThreadX
     - :ref:`module-pw_sync_threadx`
   * - embOS
     - :ref:`module-pw_sync_embos`
   * - STL
     - :ref:`module-pw_sync_stl`
   * - Baremetal
     - Planned, not ready for use
   * - Zephyr
     - Planned
   * - CMSIS-RTOS API v2 & RTX5
     - Planned

C++
---
.. list-table::
   :widths: 70 10 10 10
   :header-rows: 1

   * - Safe to use in context
     - Thread
     - Interrupt
     - NMI
   * - Constructor
     - ✔
     - ✔
     -
   * - Destructor
     - ✔
     - ✔
     -
   * - :cc:`lock <pw::sync::InterruptSpinLock::lock>`
     - ✔
     - ✔
     -
   * - :cc:`try_lock <pw::sync::InterruptSpinLock::try_lock>`
     - ✔
     - ✔
     -
   * - :cc:`unlock <pw::sync::InterruptSpinLock::unlock>`
     - ✔
     - ✔
     -

Examples in C++
^^^^^^^^^^^^^^^
.. code-block:: cpp

   #include "pw_sync/interrupt_spin_lock.h"

   pw::sync::InterruptSpinLock interrupt_spin_lock;

   void InterruptSafeCriticalSection() {
     interrupt_spin_lock.lock();
     NotThreadSafeCriticalSection();
     interrupt_spin_lock.unlock();
   }

Alternatively you can use C++'s RAII helpers to ensure you always unlock.

.. code-block:: cpp

   #include <mutex>

   #include "pw_sync/interrupt_spin_lock.h"

   pw::sync::InterruptSpinLock interrupt_spin_lock;

   void InterruptSafeCriticalSection() {
     std::lock_guard lock(interrupt_spin_lock);
     NotThreadSafeCriticalSection();
   }

C
-
The InterruptSpinLock must be created in C++, however it can be passed into C
using the ``pw_sync_InterruptSpinLock`` opaque struct alias.

.. list-table::
   :widths: 70 10 10 10
   :header-rows: 1

   * - Safe to use in context
     - Thread
     - Interrupt
     - NMI
   * - :cc:`pw_sync_InterruptSpinLock_Lock`
     - ✔
     - ✔
     -
   * - :cc:`pw_sync_InterruptSpinLock_TryLock`
     - ✔
     - ✔
     -
   * - :cc:`pw_sync_InterruptSpinLock_Unlock`
     - ✔
     - ✔
     -

Example in C
^^^^^^^^^^^^
.. code-block:: cpp

   #include "pw_chrono/system_clock.h"
   #include "pw_sync/interrupt_spin_lock.h"

   pw::sync::InterruptSpinLock interrupt_spin_lock;

   extern pw_sync_InterruptSpinLock
       interrupt_spin_lock;  // This can only be created in C++.

   void InterruptSafeCriticalSection(void) {
     pw_sync_InterruptSpinLock_Lock(&interrupt_spin_lock);
     NotThreadSafeCriticalSection();
     pw_sync_InterruptSpinLock_Unlock(&interrupt_spin_lock);
   }

Optional locking
================
:cc:`NoLock <pw::sync::NoLock>` is a no-op lock that can be used to
satisfy a lock interface without providing any synchronization. This can be
useful for templated code that is lock-agnostic, but may be used in a context
that does not require any synchronization. ``NoLock`` is a `BasicLockable
<https://en.cppreference.com/w/cpp/named_req/BasicLockable>`_.

:cc:`MaybeLock <pw::sync::MaybeLock>` selects between a real lock type and
``NoLock`` based on a boolean template parameter. This may be helpful when
locking is conditionally enabled by a config macro.

.. _module-pw_sync-thread-safety-lock-annotations:

Thread Safety Lock Annotations
==============================
Pigweed's critical section lock primitives support Clang's thread safety
analysis extension for C++. The analysis is completely static at compile-time.
This is only supported when building with Clang. The annotations are no-ops when
using different compilers.

Pigweed provides the ``pw_sync/lock_annotations.h`` header file with macro
definitions to allow developers to document the locking policies of
multi-threaded code. The annotations can also help program analysis tools to
identify potential thread safety issues.

More information on Clang's thread safety analysis system can be found
`here <https://clang.llvm.org/docs/ThreadSafetyAnalysis.html>`_.

Enabling Clang's Analysis
-------------------------
In order to enable the analysis, Clang requires that the ``-Wthread-safety``
compilation flag be used. To also enable :cc:`PW_ACQUIRED_AFTER` and/or
:cc:`PW_ACQUIRED_BEFORE`, it also requires the ``-Wthread-safety-beta``
compilation flag. And if any STL components like ``std::lock_guard`` are used,
the STL's built in annotations should be manually enabled, typically by setting
the ``_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS`` macro.

If using GN, the ``pw_build:clang_thread_safety_warnings`` config is provided
to do all of the above for you, when added to your clang toolchain definition's
default configs.

Why use lock annotations?
-------------------------
Lock annotations can help warn you about potential race conditions in your code
when using locks: you have to remember to grab lock(s) before entering a
critical section, yuou have to remember to unlock it when you leave, and you
have to avoid deadlocks.

Clang's lock annotations let you inform the compiler and anyone reading your
code which variables are guarded by which locks, which locks should or cannot be
held when calling which function, which order locks should be acquired in, etc.

Using Lock Annotations
----------------------
When referring to locks in the arguments of the attributes, you should
use variable names or more complex expressions (e.g. ``my_object->lock_``)
that evaluate to a concrete lock object whenever possible. If the lock
you want to refer to is not in scope, you may use a member pointer
(e.g. ``&MyClass::lock_``) to refer to a lock in some (unknown) object.

Annotating Lock Usage
^^^^^^^^^^^^^^^^^^^^^
* :cc:`PW_GUARDED_BY`
* :cc:`PW_PT_GUARDED_BY`
* :cc:`PW_ACQUIRED_AFTER`
* :cc:`PW_ACQUIRED_BEFORE`
* :cc:`PW_EXCLUSIVE_LOCKS_REQUIRED`
* :cc:`PW_SHARED_LOCKS_REQUIRED`
* :cc:`PW_LOCKS_EXCLUDED`
* :cc:`PW_LOCK_RETURNED`
* :cc:`PW_LOCKABLE`
* :cc:`PW_SCOPED_LOCKABLE`
* :cc:`PW_EXCLUSIVE_LOCK_FUNCTION`
* :cc:`PW_SHARED_LOCK_FUNCTION`
* :cc:`PW_UNLOCK_FUNCTION`
* :cc:`PW_EXCLUSIVE_TRYLOCK_FUNCTION`
* :cc:`PW_SHARED_TRYLOCK_FUNCTION`
* :cc:`PW_ASSERT_EXCLUSIVE_LOCK`
* :cc:`PW_ASSERT_SHARED_LOCK`
* :cc:`PW_NO_LOCK_SAFETY_ANALYSIS`

Annotating Lock Objects
^^^^^^^^^^^^^^^^^^^^^^^
In order of lock usage annotation to work, the lock objects themselves need to
be annotated as well. In case you are providing your own lock or psuedo-lock
object, you can use the macros in this section to annotate it.

As an example we've annotated a Lock and a RAII ScopedLocker object for you, see
the macro documentation after for more details:

.. code-block:: cpp

   class PW_LOCKABLE("Lock") Lock {
    public:
     void Lock() PW_EXCLUSIVE_LOCK_FUNCTION();

     void ReaderLock() PW_SHARED_LOCK_FUNCTION();

     void Unlock() PW_UNLOCK_FUNCTION();

     void ReaderUnlock() PW_SHARED_TRYLOCK_FUNCTION();

     bool TryLock() PW_EXCLUSIVE_TRYLOCK_FUNCTION(true);

     bool ReaderTryLock() PW_SHARED_TRYLOCK_FUNCTION(true);

     void AssertHeld() PW_ASSERT_EXCLUSIVE_LOCK();

     void AssertReaderHeld() PW_ASSERT_SHARED_LOCK();
   };

   // Tag types for selecting a constructor.
   struct adopt_lock_t {};
   inline constexpr adopt_lock = {};
   struct defer_lock_t {};
   inline constexpr defer_lock = {};
   struct shared_lock_t {};
   inline constexpr shared_lock = {};

   class PW_SCOPED_LOCKABLE ScopedLocker {
     // Acquire lock, implicitly acquire *this and associate it with lock.
     ScopedLocker(Lock* lock) PW_EXCLUSIVE_LOCK_FUNCTION(lock)
         : lock_(lock), locked(true) {
       lock->Lock();
     }

     // Assume lock is held, implicitly acquire *this and associate it with lock.
     ScopedLocker(Lock* lock, adopt_lock_t) PW_EXCLUSIVE_LOCKS_REQUIRED(lock)
         : lock_(lock), locked(true) {}

     // Acquire lock in shared mode, implicitly acquire *this and associate it
     // with lock.
     ScopedLocker(Lock* lock, shared_lock_t) PW_SHARED_LOCK_FUNCTION(lock)
         : lock_(lock), locked(true) {
       lock->ReaderLock();
     }

     // Assume lock is held in shared mode, implicitly acquire *this and associate
     // it with lock.
     ScopedLocker(Lock* lock, adopt_lock_t, shared_lock_t)
         PW_SHARED_LOCKS_REQUIRED(lock)
         : lock_(lock), locked(true) {}

     // Assume lock is not held, implicitly acquire *this and associate it with
     // lock.
     ScopedLocker(Lock* lock, defer_lock_t) PW_LOCKS_EXCLUDED(lock)
         : lock_(lock), locked(false) {}

     // Release *this and all associated locks, if they are still held.
     // There is no warning if the scope was already unlocked before.
     ~ScopedLocker() PW_UNLOCK_FUNCTION() {
       if (locked)
         lock_->GenericUnlock();
     }

     // Acquire all associated locks exclusively.
     void Lock() PW_EXCLUSIVE_LOCK_FUNCTION() {
       lock_->Lock();
       locked = true;
     }

     // Try to acquire all associated locks exclusively.
     bool TryLock() PW_EXCLUSIVE_TRYLOCK_FUNCTION(true) {
       return locked = lock_->TryLock();
     }

     // Acquire all associated locks in shared mode.
     void ReaderLock() PW_SHARED_LOCK_FUNCTION() {
       lock_->ReaderLock();
       locked = true;
     }

     // Try to acquire all associated locks in shared mode.
     bool ReaderTryLock() PW_SHARED_TRYLOCK_FUNCTION(true) {
       return locked = lock_->ReaderTryLock();
     }

     // Release all associated locks. Warn on double unlock.
     void Unlock() PW_UNLOCK_FUNCTION() {
       lock_->Unlock();
       locked = false;
     }

     // Release all associated locks. Warn on double unlock.
     void ReaderUnlock() PW_UNLOCK_FUNCTION() {
       lock_->ReaderUnlock();
       locked = false;
     }

    private:
     Lock* lock_;
     bool locked_;
   };

-----------------------------
Critical Section Lock Helpers
-----------------------------

Virtual Lock Interfaces
=======================
Virtual lock interfaces can be useful when lock selection cannot be templated.

Why use virtual locks?
----------------------
Virtual locks enable depending on locks without templating implementation code
on the type, while retaining flexibility with respect to the concrete lock type.
Pigweed tries to avoid pushing policy on to users, and virtual locks are one way
to accomplish that without templating everything.

A case when virtual locks are useful is when the concrete lock type changes at
run time. For example, access to flash may be protected at run time by an
internal mutex, however at crash time we may want to switch to a no-op lock. A
virtual lock interface could be used here to minimize the code-size cost that
would occur otherwise if the flash driver were templated.

VirtualBasicLockable
--------------------
The ``VirtualBasicLockable`` interface meets the
`BasicLockable <https://en.cppreference.com/w/cpp/named_req/BasicLockable>`_ C++
named requirement. Our critical section lock primitives offer optional virtual
versions, including:

* :cc:`VirtualMutex <pw::sync::VirtualMutex>`
* :cc:`VirtualTimedMutex <pw::sync::VirtualTimedMutex>`
* :cc:`VirtualInterruptSpinLock <pw::sync::VirtualInterruptSpinLock>`

.. _module-pw_sync-genericbasiclockable:

GenericBasicLockable
--------------------
``GenericBasicLockable`` is a helper construct that can be used to declare
virtual versions of a critical section lock primitive that meets the
`BasicLockable <https://en.cppreference.com/w/cpp/named_req/BasicLockable>`_
C++ named requirement. For example, given a ``Mutex`` type with ``lock()`` and
``unlock()`` methods, a ``VirtualMutex`` type that derives from
``VirtualBasicLockable`` can be declared as follows:

.. code-block:: cpp

   class VirtualMutex : public GenericBasicLockable<Mutex> {};

Borrowable
==========
``Borrowable`` is a helper construct that enables callers to borrow an object
which is guarded by a lock, enabling a containerized style of external locking.

Users who need access to the guarded object can ask to acquire a
:cc:`BorrowedPointer <pw::sync::BorrowedPointer>` which permits access
while the lock is held.

This class is compatible with locks which comply with
`BasicLockable <https://en.cppreference.com/w/cpp/named_req/BasicLockable>`_,
`Lockable <https://en.cppreference.com/w/cpp/named_req/Lockable>`_, and
`TimedLockable <https://en.cppreference.com/w/cpp/named_req/TimedLockable>`_
C++ named requirements.

By default the selected lock type is a ``pw::sync::VirtualBasicLockable``. If
this virtual interface is used, the templated lock parameter can be skipped.

External vs Internal locking
----------------------------
Before we explain why Borrowable is useful, it's important to understand the
trade-offs when deciding on using internal and/or external locking.

Internal locking is when the lock is hidden from the caller entirely and is used
internally to the API. For example:

.. code-block:: cpp

   class BankAccount {
    public:
     void Deposit(int amount) {
       std::lock_guard lock(mutex_);
       balance_ += amount;
     }

     void Withdraw(int amount) {
       std::lock_guard lock(mutex_);
       balance_ -= amount;
     }

     void Balance() const {
       std::lock_guard lock(mutex_);
       return balance_;
     }

    private:
     int balance_ PW_GUARDED_BY(mutex_);
     pw::sync::Mutex mutex_;
   };

Internal locking guarantees that any concurrent calls to its public member
functions don't corrupt an instance of that class. This is typically ensured by
having each member function acquire a lock on the object upon entry. This way,
for any instance, there can only be one member function call active at any
moment, serializing the operations.

One common issue that pops up is that member functions may have to call other
member functions which also require locks. This typically results in a
duplication of the public API into an internal mirror where the lock is already
held. This along with having to modify every thread-safe public member function
may results in an increased code size.

However, with the per-method locking approach, it is not possible to perform a
multi-method thread-safe transaction. For example, what if we only wanted to
withdraw money if the balance was high enough? With the current API there would
be a risk that money is withdrawn after we've checked the balance.

This is usually why external locking is used. This is when the lock is exposed
to the caller and may be used externally to the public API. External locking
can take may forms which may even include mixing internal and external locking.
In its most simplistic form it is an external lock used along side each
instance, e.g.:

.. code-block:: cpp

   class BankAccount {
    public:
     void Deposit(int amount) { balance_ += amount; }

     void Withdraw(int amount) { balance_ -= amount; }

     void Balance() const { return balance_; }

    private:
     int balance_;
   };

   pw::sync::Mutex bobs_account_mutex;
   BankAccount bobs_account PW_GUARDED_BY(bobs_account_mutex);

The lock is acquired before the bank account is used for a transaction. In
addition, we do not have to modify every public function and its trivial to
call other public member functions from a public member function. However, as
you can imagine instantiating and passing around the instances and their locks
can become error prone.

This is why ``Borrowable`` exists.

Why use Borrowable?
-------------------
:cc:`Borrowable <pw::sync::Borrowable>` offers code-size efficient way to
enable external locking that is easy and safe to use. It is effectively a
container which holds references to a protected instance and its lock which
provides RAII-style access.

.. code-block:: cpp

   pw::sync::Mutex bobs_account_mutex;
   BankAccount bobs_account PW_GUARDED_BY(bobs_account_mutex);
   pw::sync::Borrowable<BankAccount, pw::sync::Mutex> bobs_acount(
       bobs_account, bobs_account_mutex);

This construct is useful when sharing objects or data which are transactional in
nature where making individual operations threadsafe is insufficient. See the
section on internal vs external locking tradeoffs above.

It can also offer a code-size and stack-usage efficient way to separate timeout
constraints between the acquiring of the shared object and timeouts used for the
shared object's API. For example, imagine you have an I2c bus which is used by
several threads and you'd like to specify an ACK timeout of 50ms. It'd be ideal
if the duration it takes to gain exclusive access to the I2c bus does not eat
into the ACK timeout you'd like to use for the transaction. Borrowable can help
you do exactly this if you provide access to the I2c bus through a
``Borrowable``.

.. note::

   ``Borrowable`` has semantics similar to a pointer and should be passed by
   value. Furthermore, a ``Borrowable<U>`` can be assigned to a
   ``Borrowable<T>`` if ``U`` is a subclass of ``T``.

Example in C++
^^^^^^^^^^^^^^

.. code-block:: cpp

   #include <chrono>

   #include "pw_bytes/span.h"
   #include "pw_i2c/initiator.h"
   #include "pw_status/result.h"
   #include "pw_status/try.h"
   #include "pw_sync/borrow.h"
   #include "pw_sync/mutex.h"

   class ExampleI2c : public pw::i2c::Initiator;

   pw::sync::VirtualMutex i2c_mutex;
   ExampleI2c i2c;
   pw::sync::Borrowable<ExampleI2c> borrowable_i2c(i2c, i2c_mutex);

   pw::Result<ConstByteSpan> ReadI2cData(ByteSpan buffer) {
     // Block indefinitely waiting to borrow the i2c bus.
     pw::sync::BorrowedPointer<ExampleI2c> borrowed_i2c = borrowable_i2c.acquire();

     // Execute a sequence of transactions to get the needed data.
     PW_TRY(borrowed_i2c->WriteFor(kFirstWrite, std::chrono::milliseconds(50)));
     PW_TRY(borrowed_i2c->WriteReadFor(
         kSecondWrite, buffer, std::chrono::milliseconds(10)));

     // Borrowed i2c pointer is returned when the scope exits.
     return buffer;
   }

InlineBorrowable
=================
:cc:`InlineBorrowable <pw::sync::InlineBorrowable>` is a helper to
simplify the common use case where an object is wrapped in a
:cc:`Borrowable <pw::sync::Borrowable>` for its entire lifetime. The
``InlineBorrowable`` owns the guarded object and the lock object.

``InlineBorrowable`` has a separate parameter for the concrete lock type that is
instantiated and a (possibly virtual) lock interface type that is referenced by
users of the guarded object. The default lock is :cc:`VirtualMutex
<pw::sync::VirtualMutex>` and the default lock interface is
:cc:`VirtualBasicLockable <pw::sync::VirtualBasicLockable>`.

An ``InlineBorrowable`` is a ``Borrowable`` with the same guarded object and
lock interface types, and it can be passed directly to APIs that expect a
Borrowable reference.

Why use InlineBorrowable?
-------------------------
It is a safer and simpler way to guard an object for its entire lifetime. The
unguarded object is never exposed and doesn't need to be stored in a separate
variable or data member. The guarded object and its lock are guaranteed to have
the same lifetime, and the lock cannot be re-used for any other purpose.

Constructing objects in-place
-----------------------------
The guarded object and its lock are constructed in-place by the
InlineBorrowable, and any constructor parameters required by the object or
its lock must be passed through the :cc:`InlineBorrowable
<pw::sync::InlineBorrowable>` constructor. There are several ways to do this:

* Pass the parameters for the guarded object inline to the constructor. This is
  the recommended way to construct the object when the lock does not require any
  constructor parameters. Use the ``std::in_place`` marker to invoke the inline
  constructor.

  .. code-block:: cpp

     InlineBorrowable<Foo> foo(std::in_place, foo_arg1, foo_arg2);
     InlineBorrowable<std::array<int, 2>> foo_array(std::in_place, 1, 2);

* Pass the parameters inside tuples:

  .. code-block:: cpp

     InlineBorrowable<Foo> foo(std::forward_as_tuple(foo_arg1, foo_arg2));

     InlineBorrowable<Foo, MyLock> foo_lock(std::forward_as_tuple(foo_arg1,
                                                                  foo_arg2),
                                            std::forward_as_tuple(lock_arg1,
                                                                  lock_arg2));

  .. note:: This approach only supports list initialization starting with C++20.

* Use callables to construct the guarded object and lock object:

  .. code-block:: cpp

     InlineBorrowable<Foo> foo([&] { return Foo{foo_arg1, foo_arg2}; });

     InlineBorrowable<Foo, MyLock> foo_lock([&] { return Foo{foo_arg1, foo_arg2}; },
                                            [&] {
                                              return MyLock{lock_arg1, lock_arg2};
                                            })

  .. note:: It is possible to construct and return objects that are not copyable
    or movable, thanks to mandatory copy ellision (return value optimization).

Example in C++
^^^^^^^^^^^^^^
.. code-block:: cpp

   #include <utility>

   #include "pw_bytes/span.h"
   #include "pw_i2c/initiator.h"
   #include "pw_status/result.h"
   #include "pw_sync/inline_borrowable.h"

   struct I2cOptions;

   class ExampleI2c : public pw::i2c::Initiator {
    public:
     ExampleI2c(int bus_id, I2cOptions options);
     // ...
   };

   int kBusId;
   I2cOptions opts;

   pw::sync::InlineBorrowable<ExampleI2c> i2c(std::in_place, kBusId, opts);

   pw::Result<ConstByteSpan> ReadI2cData(
       pw::sync::Borrowable<pw::i2c::Initiator> initiator, ByteSpan buffer);

   pw::Result<ConstByteSpan> ReadData(ByteSpan buffer) {
     return ReadI2cData(i2c, buffer);
   }

ScopedLocker
============
:cc:`pw::ScopedLocker` is an RAII helper to use ``BasicLockable``
locks with thread safety lock annotations with more capabilities than
``std::lock_guard``.

.. Note::
   The simpler ``std::lock_guard`` RAII helper is recommended by default.
   due to additional member overhead. ``pw::ScopedLocker`` should only be used
   if more complex lock management is required where RAII alone is insufficient.

Unlike a ``std::lock_guard`` and ``std::scoped_lock``, this helper can be
constructed with the lock deferred. In addition, this does not support the
``std::lock`` deadlock avoidance algorithm nor lock adoption from
``std::scoped_lock``.

Lastly this supports explicit ``lock()`` and ``unlock()`` like
``std::unique_lock``, however unlike ``std::unique_lock``, ``pw::ScopedLocker``
does not support the use of ``Lockables``'s conditional lock acquisition,
``try_lock()``. This means that this supports thread safety lock annotations
unlike ``std::unique_lock``.

--------------------
Signaling Primitives
--------------------
Native signaling primitives tend to vary more compared to critial section locks
across different platforms. For example, although common signaling primtives
like semaphores are in most if not all RTOSes and even POSIX, it was not in the
STL before C++20. Likewise many C++ developers are surprised that conditional
variables tend to not be natively supported on RTOSes. Although you can usually
build any signaling primitive based on other native signaling primitives, this
may come with non-trivial added overhead in ROM, RAM, and execution efficiency.

For this reason, Pigweed intends to provide some simpler signaling primitives
which exist to solve a narrow programming need but can be implemented as
efficiently as possible for the platform that it is used on.

This simpler but highly portable class of signaling primitives is intended to
ensure that a portability efficiency tradeoff does not have to be made up front.
Today this is class of simpler signaling primitives is limited to the
:cc:`ThreadNotification <pw::sync::ThreadNotification>` and
:cc:`TimedThreadNotification <pw::sync::TimedThreadNotification>`.

ThreadNotification
==================
The :cc:`ThreadNotification <pw::sync::ThreadNotification>` is a
synchronization primitive that can be used to permit a SINGLE thread to block
and consume a latching, saturating notification from multiple notifiers.

.. Note::
   Although only a single thread can block on a :cc:`ThreadNotification
   <pw::sync::ThreadNotification>` at a time, many instances may be used by a
   single thread just like binary semaphores.  This is in contrast to some
   native RTOS APIs, such as direct task notifications, which re-use the same
   state within a thread's context.

.. Warning::
   This is a single consumer/waiter, multiple producer/notifier API!
   The acquire APIs must only be invoked by a single consuming thread. As a
   result, having multiple threads receiving notifications via the acquire API
   is unsupported.

This is effectively a subset of the :cc:`BinarySemaphore
<pw::sync::BinarySemaphore>` API, except that only a single thread can be
notified and block at a time.

The single consumer aspect of the API permits the use of a smaller and/or
faster native APIs such as direct thread signaling. This should be
backed by the most efficient native primitive for a target, regardless of
whether that is a semaphore, event flag group, condition variable, or something
else.

The :cc:`ThreadNotification <pw::sync::ThreadNotification>` is initialized
to being empty (latch is not set).

Generic BinarySemaphore-based Backend
-------------------------------------
This module provides a generic backend for :cc:`ThreadNotification
<pw::sync::ThreadNotification>` via
``pw_sync:binary_semaphore_thread_notification`` which uses a
:cc:`BinarySemaphore <pw::sync::BinarySemaphore>` as the backing
primitive. See :ref:`BinarySemaphore <module-pw_sync-binary-semaphore>` for
backend availability.

Optimized Backend
-----------------
.. list-table::
   :header-rows: 1

   * - Supported on
     - Optimized backend module
   * - FreeRTOS
     - ``pw_sync_freertos:thread_notification``
   * - ThreadX
     - Not possible, use ``pw_sync:binary_semaphore_thread_notification``
   * - embOS
     - Not needed, use ``pw_sync:binary_semaphore_thread_notification``
   * - STL
     - Not planned, use ``pw_sync:binary_semaphore_thread_notification``
   * - Baremetal
     - Planned
   * - Zephyr
     - Planned
   * - CMSIS-RTOS API v2 & RTX5
     - Planned

C++
---
.. list-table::
   :widths: 70 10 10 10
   :header-rows: 1

   * - Safe to use in context
     - Thread
     - Interrupt
     - NMI
   * - Constructor
     - ✔
     -
     -
   * - Destructor
     - ✔
     -
     -
   * - :cc:`acquire <pw::sync::ThreadNotification::acquire>`
     - ✔
     -
     -
   * - :cc:`try_acquire <pw::sync::ThreadNotification::try_acquire>`
     - ✔
     -
     -
   * - :cc:`release <pw::sync::ThreadNotification::release>`
     - ✔
     - ✔
     -

Examples in C++
^^^^^^^^^^^^^^^
.. code-block:: cpp

   #include "pw_sync/thread_notification.h"
   #include "pw_thread/thread_core.h"

   class FooHandler() {
    public:
     // Public API invoked by other threads and/or interrupts.
     void NewFooAvailable() { new_foo_notification_.release(); }

     // Thread function.
     void Run() {
       while (true) {
         new_foo_notification_.acquire();
         HandleFoo();
       }
     }

    private:
     void HandleFoo();

     pw::sync::ThreadNotification new_foo_notification_;
   };

TimedThreadNotification
=======================
The :cc:`TimedThreadNotification <pw::sync::TimedThreadNotification>` is
an extension of the :cc:`ThreadNotification
<pw::sync::ThreadNotification>` which offers timeout and deadline based
semantics.

The :cc:`TimedThreadNotification <pw::sync::TimedThreadNotification>` is
initialized to being empty (latch is not set).

.. Warning::
   This is a single consumer/waiter, multiple producer/notifier API!  The
   acquire APIs must only be invoked by a single consuming thread. As a result,
   having multiple threads receiving notifications via the acquire API is
   unsupported.

Generic BinarySemaphore-based Backend
-------------------------------------
This module provides a generic backend for :cc:`TimedThreadNotification
<pw::sync::TimedThreadNotification>` via
``pw_sync:binary_semaphore_timed_thread_notification`` which uses a
:cc:`BinarySemaphore <pw::sync::BinarySemaphore>` as the backing
primitive. See :ref:`BinarySemaphore <module-pw_sync-binary-semaphore>` for
backend availability.

Optimized Backend
-----------------
.. list-table::
   :header-rows: 1

   * - Supported on
     - Backend module
   * - FreeRTOS
     - ``pw_sync_freertos:timed_thread_notification``
   * - ThreadX
     - Not possible, use ``pw_sync:binary_semaphore_timed_thread_notification``
   * - embOS
     - Not needed, use ``pw_sync:binary_semaphore_timed_thread_notification``
   * - STL
     - Not planned, use ``pw_sync:binary_semaphore_timed_thread_notification``
   * - Zephyr
     - Planned
   * - CMSIS-RTOS API v2 & RTX5
     - Planned

C++
---
.. list-table::
   :widths: 70 10 10 10
   :header-rows: 1

   * - Safe to use in context
     - Thread
     - Interrupt
     - NMI
   * - Constructor
     - ✔
     -
     -
   * - Destructor
     - ✔
     -
     -
   * - ``acquire``
     - ✔
     -
     -
   * - ``try_acquire``
     - ✔
     -
     -
   * - :cc:`try_acquire_for
       <pw::sync::TimedThreadNotification::try_acquire_for>`
     - ✔
     -
     -
   * - :cc:`try_acquire_until
       <pw::sync::TimedThreadNotification::try_acquire_until>`
     - ✔
     -
     -
   * - ``release``
     - ✔
     - ✔
     -

Examples in C++
^^^^^^^^^^^^^^^
.. code-block:: cpp

   #include "pw_sync/timed_thread_notification.h"
   #include "pw_thread/thread_core.h"

   class FooHandler() {
    public:
     // Public API invoked by other threads and/or interrupts.
     void NewFooAvailable() { new_foo_notification_.release(); }

     // Thread function.
     void Run() {
       while (true) {
         if (new_foo_notification_.try_acquire_for(kNotificationTimeout)) {
           HandleFoo();
         }
         DoOtherStuff();
       }
     }

    private:
     void HandleFoo();
     void DoOtherStuff();

     pw::sync::TimedThreadNotification new_foo_notification_;
   };

CountingSemaphore
=================
The :cc:`CountingSemaphore <pw::sync::CountingSemaphore>` is a
synchronization primitive that can be used for counting events and/or resource
management where receiver(s) can block on acquire until notifier(s) signal by
invoking release.

Note that unlike :cc:`Mutex <pw::sync::Mutex>`, priority inheritance is
not used by semaphores meaning semaphores are subject to unbounded priority
inversions. Due to this, Pigweed does not recommend semaphores for mutual
exclusion.

The :cc:`CountingSemaphore <pw::sync::CountingSemaphore>` is initialized
to being empty or having no tokens.

The entire API is thread safe, but only a subset is interrupt safe.

.. Note::
   If there is only a single consuming thread, use a
   :cc:`ThreadNotification <pw::sync::ThreadNotification>` instead which
   can be much more efficient on some RTOSes such as FreeRTOS.

.. Warning::
   Releasing multiple tokens is often not natively supported, meaning you may
   end up invoking the native kernel API many times, i.e. once per token you
   are releasing!

.. list-table::
   :header-rows: 1

   * - Supported on
     - Backend module
   * - FreeRTOS
     - :ref:`module-pw_sync_freertos`
   * - ThreadX
     - :ref:`module-pw_sync_threadx`
   * - embOS
     - :ref:`module-pw_sync_embos`
   * - STL
     - :ref:`module-pw_sync_stl`
   * - Zephyr
     - Planned
   * - CMSIS-RTOS API v2 & RTX5
     - Planned

C++
---
.. list-table::
   :widths: 70 10 10 10
   :header-rows: 1

   * - Safe to use in context
     - Thread
     - Interrupt
     - NMI
   * - Constructor
     - ✔
     -
     -
   * - Destructor
     - ✔
     -
     -
   * - :cc:`acquire <pw::sync::CountingSemaphore::acquire>`
     - ✔
     -
     -
   * - :cc:`try_acquire <pw::sync::CountingSemaphore::try_acquire>`
     - ✔
     - ✔
     -
   * - :cc:`try_acquire_for
       <pw::sync::CountingSemaphore::try_acquire_for>`
     - ✔
     -
     -
   * - :cc:`try_acquire_until
       <pw::sync::CountingSemaphore::try_acquire_until>`
     - ✔
     -
     -
   * - :cc:`release <pw::sync::CountingSemaphore::release>`
     - ✔
     - ✔
     -
   * - :cc:`max <pw::sync::CountingSemaphore::max>`
     - ✔
     - ✔
     - ✔

Examples in C++
^^^^^^^^^^^^^^^
As an example, a counting sempahore can be useful to run periodic tasks at
frequencies near or higher than the system clock tick rate in a way which lets
you detect whether you ever fall behind.

.. code-block:: cpp

   #include "pw_sync/counting_semaphore.h"
   #include "pw_thread/thread_core.h"

   class PeriodicWorker() : public pw::thread::ThreadCore {
     // Public API invoked by a higher frequency timer interrupt.
     void TimeToExecute() { periodic_run_semaphore_.release(); }

    private:
     pw::sync::CountingSemaphore periodic_run_semaphore_;

     // Thread function.
     void Run() override {
       while (true) {
         size_t behind_by_n_cycles = 0;
         periodic_run_semaphore_.acquire();  // Wait to run until it's time.
         while (periodic_run_semaphore_.try_acquire()) {
           ++behind_by_n_cycles;
         }
         if (behind_by_n_cycles > 0) {
           PW_LOG_WARNING("Not keeping up, behind by %d cycles",
                          behind_by_n_cycles);
         }
         DoPeriodicWork();
       }
     }

     void DoPeriodicWork();
   }

.. _module-pw_sync-binary-semaphore:

BinarySemaphore
===============
:cc:`BinarySemaphore <pw::sync::BinarySemaphore>` is a specialization of
:cc:`CountingSemaphore <pw::sync::CountingSemaphore>` with an
arbitrary token limit of 1. Note that that ``max()`` is >= 1, meaning it may be
released up to ``max()`` times but only acquired once for those N releases.

Implementations of ``BinarySemaphore`` are typically more
efficient than the default implementation of ``CountingSemaphore``.

``BinarySemaphore`` is initialized to being empty or having no
tokens.

The entire API is thread safe, but only a subset is interrupt safe.

.. Note::
   If there is only a single consuming thread, use a
   :cc:`ThreadNotification <pw::sync::ThreadNotification>` instead which
   can be much more efficient on some RTOSes such as FreeRTOS.

.. list-table::
   :header-rows: 1

   * - Supported on
     - Backend module
   * - FreeRTOS
     - :ref:`module-pw_sync_freertos`
   * - ThreadX
     - :ref:`module-pw_sync_threadx`
   * - embOS
     - :ref:`module-pw_sync_embos`
   * - STL
     - :ref:`module-pw_sync_stl`
   * - Zephyr
     - Planned
   * - CMSIS-RTOS API v2 & RTX5
     - Planned

C++
---
.. list-table::
   :widths: 70 10 10 10
   :header-rows: 1

   * - Safe to use in context
     - Thread
     - Interrupt
     - NMI
   * - Constructor
     - ✔
     -
     -
   * - Destructor
     - ✔
     -
     -
   * - :cc:`acquire <pw::sync::BinarySemaphore::acquire>`
     - ✔
     -
     -
   * - :cc:`try_acquire <pw::sync::BinarySemaphore::try_acquire>`
     - ✔
     - ✔
     -
   * - :cc:`try_acquire_for <pw::sync::BinarySemaphore::try_acquire_for>`
     - ✔
     -
     -
   * - :cc:`try_acquire_until
       <pw::sync::BinarySemaphore::try_acquire_until>`
     - ✔
     -
     -
   * - :cc:`release <pw::sync::BinarySemaphore::release>`
     - ✔
     - ✔
     -
   * - :cc:`max <pw::sync::BinarySemaphore::max>`
     - ✔
     - ✔
     - ✔

Examples in C++
^^^^^^^^^^^^^^^
.. code-block:: cpp

   #include "pw_sync/binary_semaphore.h"
   #include "pw_thread/thread_core.h"

   class FooHandler() {
    public:
     // Public API invoked by other threads and/or interrupts.
     void NewFooAvailable() { new_foo_semaphore_.release(); }

     // Thread function.
     void Run() {
       while (true) {
         if (new_foo_semaphore_.try_acquire_for(kNotificationTimeout)) {
           HandleFoo();
         }
         DoOtherStuff();
       }
     }

    private:
     void HandleFoo();
     void DoOtherStuff();

     pw::sync::BinarySemaphore new_foo_semaphore_;
   };

.. _module-pw_sync-condition-variables:

Condition Variables
=====================
`ConditionVariable <main:pw_sync/public/pw_sync/condition_variable.h>`
provides a condition variable implementation that provides semantics and an API
very similar to `std::condition_variable
<https://en.cppreference.com/w/cpp/thread/condition_variable>`_ in the C++
Standard Library.

.. warning::
   Condition variables are not a good abstraction for embedded due to spurious
   wakeups. As a result, the only ``pw_sync`` backend provided by Pigweed that
   supports condition variables is :ref:`module-pw_sync_stl`. Consider using
   a ``ThreadNotification`` instead, as these do not cause spurious wakeups and
   can be used in an interrupt context.

Limitations
-----------
As a blocking operation, condition variables should not be waited on in an
interrupt context. Less intuitively, condition variables should not be notified
in an interrupt context. Notifying a condition variable involves checking the
corresponding condition to decide whether to resume waiting threads. This check
can happen either on the signaling thread or the waiting thread:

- If the signaling thread checks the condition, it needs to exclusively access
  the waiters and their associated conditions. Access to this list must be
  synchronized with calls to wait on the variable. Additional state checked by
  the conditions may also need to be synchronized. As a result, checking the
  conditions on the signaling thread may involve blocking and is not suitable
  for a interrupt context.
- If the waiting threads check their conditions, access to the list of waiters
  still needs to be synchronized. Additionally, a thread may find that its
  condition is not satisfied, and that it needs to resume waiting. Waking
  threads only to resume waiting is costly in terms of both power and
  performance.

The second approach leads to spurious wakeups in a thread context as well. The
first approach may also have spurious wakeups if the condition changes between
signaling the waiter and the waiter reacquiring its lock.

-------------
API reference
-------------
Moved: :cc:`pw_sync`

.. toctree::
   :hidden:
   :maxdepth: 1

   backends
