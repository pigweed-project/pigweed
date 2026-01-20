.. TODO: b/442238478 - Migrate C/C++ API reference content to Doxygen

.. _module-pw_log:

======
pw_log
======
.. pigweed-module::
   :name: pw_log

Pigweed's logging module provides facilities for applications to log
information about the execution of their application. The module is split into
two components:

1. The facade (this module) which is only a macro interface layer
2. The backend, provided elsewhere, that implements the low level logging

``pw_log`` also defines a logging protobuf, helper utilities, and an RPC
service for efficiently storing and transmitting log messages. See
:ref:`module-pw_log-protobuf` for details.

--------------
Usage examples
--------------
Here is a typical usage example, showing setting the module name, and using the
long-form names.

.. code-block:: cpp

   #define PW_LOG_MODULE_NAME "BLE"

   #include "pw_log/log.h"

   int main() {
     PW_LOG_INFO("Booting...");
     PW_LOG_DEBUG("CPU temp: %.2f", cpu_temperature);
     if (BootFailed()) {
       PW_LOG_CRITICAL("Had trouble booting due to error %d", GetErrorCode());
       ReportErrorsAndHalt();
     }
     PW_LOG_INFO("Successfully booted");
   }

In ``.cc`` files, it is possible to dispense with the ``PW_`` part of the log
names and go for shorter log macros. Include ``pw_log/short.h`` or
``pw_log/shorter.h`` for shorter versions of the macros.

.. warning::
   The shorter log macros collide with `Abseil's logging API
   <https://abseil.io/docs/cpp/guides/logging>`_. Do not use them in upstream
   Pigweed modules, or any code that may depend on Abseil.

.. code-block:: cpp

   #define PW_LOG_MODULE_NAME "BLE"

   #include "pw_log/shorter.h"

   int main() {
     INF("Booting...");
     DBG("CPU temp: %.2f", cpu_temperature);
     if (BootFailed()) {
       CRT("Had trouble booting due to error %d", GetErrorCode());
       ReportErrorsAndHalt();
     }
     INF("Successfully booted");
   }

The ``pw_log`` facade also exposes a handful of macros that only apply
specifically to tokenized logging. See :ref:`module-pw_log-tokenized-args` for
details.

Layer diagram example: ``stm32f429i-disc1``
===========================================
Below is an example diagram showing how the modules connect together for the
``stm32f429i-disc1`` target, where the ``pw_log`` backend is ``pw_log_basic``.
``pw_log_basic`` uses the ``pw_sys_io`` module to log in plaintext, which in
turn outputs to the STM32F429 bare metal backend for ``pw_sys_io``, which is
``pw_sys_io_baremetal_stm32f429i``.

.. image:: https://www.gstatic.com/pigweed/pw_log/example_layer_diagram.svg

.. _module-pw_log-macros:

Logging macros
==============
These are the primary macros for logging information about the functioning of a
system, intended to be used directly.

.. c:macro:: PW_LOG(level, verbosity, module, flags, fmt, ...)

   This is the primary mechanism for logging.

   *level* - An integer level as defined by ``pw_log/levels.h`` for this log.

   *verbosity* - An integer level as defined by ``pw_log/levels.h`` which is the
   minimum level which is enabled.

   *module* - A string literal for the module name.

   *flags* - Arbitrary flags the backend can leverage. The semantics of these
   flags are not defined in the facade, but are instead meant as a general
   mechanism for communication bits of information to the logging backend.
   ``pw_log`` reserves 2 flag bits by default, but log backends may provide for
   more or fewer flag bits.

   Here are some ideas for what a backend might use flags for:

   - Example: ``HAS_PII`` - A log has personally-identifying data
   - Example: ``HAS_DII`` - A log has device-identifying data
   - Example: ``RELIABLE_DELIVERY`` - Ask the backend to ensure the log is
     delivered; this may entail blocking other logs.
   - Example: ``BEST_EFFORT`` - Don't deliver this log if it would mean blocking
     or dropping important-flagged logs

   *fmt* - The message to log, which may contain format specifiers like ``%d``
   or ``%0.2f``.

   Example:

   .. code-block:: cpp

      PW_LOG(PW_LOG_LEVEL_INFO,
             PW_LOG_LEVEL_DEBUG,
             PW_LOG_MODULE_NAME,
             PW_LOG_FLAGS,
             "Temp is %d degrees",
             temp);
      PW_LOG(PW_LOG_LEVEL_ERROR,
             PW_LOG_LEVEL_DEBUG,
             PW_LOG_MODULE_NAME,
             UNRELIABLE_DELIVERY,
             "It didn't work!");

   .. note::

      ``PW_LOG()`` should not be used frequently; typically only when adding
      flags to a particular message to mark PII or to indicate delivery
      guarantees.  For most cases, prefer to use the direct ``PW_LOG_INFO`` or
      ``PW_LOG_DEBUG`` style macros, which are often implemented more efficiently
      in the backend.

.. _module-pw_log-levels:

.. c:macro:: PW_LOG_DEBUG(fmt, ...)
.. c:macro:: PW_LOG_INFO(fmt, ...)
.. c:macro:: PW_LOG_WARN(fmt, ...)
.. c:macro:: PW_LOG_ERROR(fmt, ...)
.. c:macro:: PW_LOG_CRITICAL(fmt, ...)

   Shorthand for ``PW_LOG(<level>, PW_LOG_MODULE_NAME, PW_LOG_FLAGS, fmt, ...)``.

.. c:macro:: PW_LOG_EVERY_N(level, rate, ...)

   A simple rate limit logger which will simply log one out of every N logs.

   *level* - An integer level as defined by ``pw_log/levels.h``.

   *rate* - Rate to reduce logs to, every ``rate``-th log will complete, others
   will be suppressed.

.. c:macro:: PW_LOG_EVERY_N_DURATION(level, min_interval_between_logs, msg, ...)

   This is a rate-limited form of logging, especially useful for progressive
   or chatty logs that should be logged periodically, but not on each instance
   of the logger being called.

   *level* - An integer level as defined by ``pw_log/levels.h``.

   *min_interval_between_logs* - A ``std::chrono::duration`` of the minimum time
   between logs. Logs attempted before this time duration will be completely
   dropped.
   Dropped logs will be counted to add a drop count and calculated rate of the
   logs.

   *msg* - Formattable log message, as you would pass to the above ``PW_LOG``
   macro.

   .. note::

      ``PW_LOG_EVERY_N`` is simpler, if you simply need to reduce uniformly
      periodic logs by a fixed or variable factor not based explicitly on a
      duration. Each call to the macro will incur a static ``uint32_t`` within
      the calling context.

      ``PW_LOG_EVERY_N_DURATION`` is able to suppress all logs based on a time
      interval, suppressing logs logging faster than the desired time interval.
      Each call to the duration macro will incur a static 16 byte object to
      track the time interval within the calling context.

   Example:

   .. code-block:: cpp

      // Ensure at least 500ms between transfer parameter logs.
      chrono::SystemClock::duration rate_limit_ =
          chrono::SystemClock::for_at_least(std::chrono::milliseconds(500));

      PW_LOG_EVERY_N_DURATION(
          PW_LOG_LEVEL_INFO,
          rate_limit_,
          "Transfer %u sending transfer parameters!" static_cast<unsigned>(
              session_id_));

--------------------
Module configuration
--------------------
This module has configuration options that globally affect the behavior of
pw_log via compile-time configuration of this module, see the
:ref:`module documentation <module-structure-compile-time-configuration>` for
more details.

.. c:macro:: PW_LOG_LEVEL_DEFAULT

   Controls the default value of ``PW_LOG_LEVEL``. Setting
   ``PW_LOG_LEVEL_DEFAULT`` will change the behavior of all source files that
   have not explicitly set ``PW_LOG_LEVEL``. Defaults to ``PW_LOG_LEVEL_INFO``.

.. c:macro:: PW_LOG_FLAGS_DEFAULT

   Controls the default value of ``PW_LOG_FLAGS``. Setting
   ``PW_LOG_FLAGS_DEFAULT`` will change the behavior of all source files that
   have not explicitly set ``PW_LOG_FLAGS``. Defaults to ``0``.

.. c:macro:: PW_LOG_ENABLE_IF(level, verbosity, flags)

   Filters logs by an arbitrary expression based on ``level``, ``verbosity``,
   and ``flags``. Source files that define
   ``PW_LOG_ENABLE_IF(level, verbosity, flags)`` will display if the given
   expression evaluates true. Defaults to
   ``((int32_t)(level) >= (int32_t)(verbosity))``.

.. attention::

   At this time, only compile time filtering is supported. In the future, we
   plan to add support for runtime filtering.


Per-source file configuration
=============================
This module defines macros that can be overridden to independently control the
behavior of ``pw_log`` statements for each C or C++ source file. To override
these macros, add ``#define`` statements for them before including headers.

The option macro definitions must be visible to ``pw_log/log.h`` the first time
it is included. To handle potential transitive includes, place these
``#defines`` before all ``#include`` statements. This should only be done in
source files, not headers. For example:

.. code-block:: cpp

   // Set the pw_log option macros here, before ALL of the #includes.
   #define PW_LOG_MODULE_NAME "Calibration"
   #define PW_LOG_LEVEL PW_LOG_LEVEL_WARN

   #include <array>
   #include <random>

   #include "devices/hal9000.h"
   #include "pw_log/log.h"
   #include "pw_rpc/server.h"

   int MyFunction() { PW_LOG_INFO("hello???"); }

.. c:macro:: PW_LOG_MODULE_NAME

   A string literal module name to use in logs. Log backends may attach this
   name to log messages or use it for runtime filtering. Defaults to ``""``.

.. c:macro:: PW_LOG_FLAGS

   Log flags to use for the ``PW_LOG_<level>`` macros. Different flags may be
   applied when using the ``PW_LOG`` macro directly.

   Log backends use flags to change how they handle individual log messages.
   Potential uses include assigning logs priority or marking them as containing
   personal information. Defaults to ``PW_LOG_FLAGS_DEFAULT``.

.. c:macro:: PW_LOG_LEVEL

   Filters logs by level. Source files that define ``PW_LOG_LEVEL`` will display
   only logs at or above the chosen level. Log statements below this level will
   be compiled out of optimized builds. Defaults to ``PW_LOG_LEVEL_DEFAULT``.

   Example:

   .. code-block:: cpp

      #define PW_LOG_LEVEL PW_LOG_LEVEL_INFO

      #include "pw_log/log.h"

      void DoSomething() {
        PW_LOG_DEBUG("This won't be logged at all");
        PW_LOG_INFO("This is INFO level, and will display");
        PW_LOG_WARN("This is above INFO level, and will display");
      }

.. _module-pw_log-logging_attributes:

------------------
Logging attributes
------------------
The logging facade in Pigweed is designed to facilitate the capture of at least
the following attributes:

- *Level* - The log level; for example, INFO, DEBUG, ERROR, etc. Typically an
  integer
- *Flags* - Bitset for e.g. RELIABLE_DELIVERY, or HAS_PII, or BEST_EFFORT
- *File* - The file where the log was triggered
- *Line* - The line number in the file where the log line occured
- *Function* - What function the log comes from. This is expensive in binary
  size to use!
- *Module* - The user-defined module name for the log statement; e.g. “BLE” or
  “BAT”
- *Message* - The message itself; with % format arguments
- *Arguments* - The format arguments to message
- *Thread* - For devices running with an RTOS, capturing the thread is very
  useful
- *Others* - Processor security level? Maybe Thread is a good proxy for this

Each backend may decide to capture different attributes to balance the tradeoff
between call site code size, call site run time, wire format size, logging
complexity, and more.

.. _module-pw_log-circular-deps:

----------------------------------------------
Avoiding circular dependencies with ``PW_LOG``
----------------------------------------------
Because logs are so widely used, including in low-level libraries, it is
common for the ``pw_log`` backend to cause circular dependencies. Because of
this, log backends may avoid declaring explicit dependencies, instead relying
on include paths to access header files.

GN
==
In GN, the ``pw_log`` backend's full implementation with true dependencies is
made available through the ``$dir_pw_log:impl`` group. When ``pw_log_BACKEND``
is set, ``$dir_pw_log:impl`` must be listed in the ``pw_build_LINK_DEPS``
variable. See :ref:`module-pw_build-link-deps`.

In the ``pw_log``, the backend's full implementation is placed in the
``$pw_log_BACKEND.impl`` target. ``$dir_pw_log:impl`` depends on this
backend target. The ``$pw_log_BACKEND.impl`` target may be an empty group if
the backend target can use its dependencies directly without causing circular
dependencies.

In order to break dependency cycles, the ``pw_log_BACKEND`` target may need
to directly provide dependencies through include paths only, rather than GN
``public_deps``. In this case, GN header checking can be disabled with
``check_includes = false``.

.. _module-pw_log-bazel-backend_impl:

Bazel
=====
In Bazel, log backends may avoid cyclic dependencies by placing the full
implementation in an ``impl`` target, like ``//pw_log_tokenized:impl``. The
``//pw_log:backend_impl`` label flag should be set to the ``impl`` target
required by the log backend used by the platform.

You must add a dependency on the ``@pigweed//pw_log:backend_impl`` target to
any binary using ``pw_log``.

See :ref:`docs-build_system-bazel_link-extra-lib` for a general discussion of
cyclic dependencies in low-level libraries in Bazel.

----------------------
Google Logging Adapter
----------------------
Pigweed provides a minimal C++ stream-style Google Log set of adapter
macros around PW_LOG under ``pw_log/glog_adapter.h`` for compatibility with
non-embedded code. While it is effective for porting server code to
microcontrollers quickly, we do not advise embedded projects use that approach
unless absolutely necessary.

Configuration
==============

.. c:macro:: PW_LOG_CFG_GLOG_BUFFER_SIZE_BYTES

   The size of the stack-allocated buffer used by the Google Logging (glog)
   macros. This only affects the glog macros provided through pw_log/glog.h.

   Pigweed strongly recommends sticking to printf-style logging instead
   of C++ stream-style Google Log logging unless absolutely necessary. The glog
   macros are only provided for compatibility with non-embedded code. See
   :ref:`module-pw_log-design-discussion` for more details.

   Undersizing this buffer will result in truncated log messages.

-----------------
Design discussion
-----------------

.. _module-pw_log-design-discussion:

Why not use C++ style stream logging operators like Google Log?
===============================================================
There are multiple reasons to avoid the C++ stream logging style in embedded,
but the biggest reason is that C++ stream logging defeats log tokenization. By
having the string literals broken up between ``<<`` operators, tokenization
becomes impossible with current language features.

Consider this example use of Google Log:

.. code-block:: cpp

   LOG(INFO) << "My temperature is " << temperature << ". State: " << state;

This log statement has two string literals. It might seem like one could convert
move to tokenization:

.. code-block:: cpp

   LOG(INFO) << TOKEN("My temperature is ") << temperature << TOKEN(". State: ")
             << state;

However, this doesn't work. The key problem is that the tokenization system
needs to allocate the string in a linker section that is excluded from the
final binary, but is in the final ELF executable (and so can be extracted).
Since there is no way to declare a string or array in a different section in
the middle of an experession in C++, it is not possible to tokenize an
expression like the above.

In contrast, the ``printf``-style version is a single statement with a single
string constant, which can be expanded by the preprocessor (as part of
``pw_tokenizer``) into a constant array in a special section.

.. code-block:: cpp

   // Note: LOG_INFO can be tokenized behind the macro; transparent to users.
   PW_LOG_INFO("My temperature is %d. State: %s", temperature, state);

Additionally, while Pigweed is mostly C++, it a practical reality that at times
projects using Pigweed will need to log from third-party libraries written in
C. Thus, we also wanted to retain C compatibility.

In summary, printf-style logging is better for Pigweed's target audience
because it:

- works with tokenization
- is C compatibile
- has smaller call sites

See also :ref:`module-pw_log_tokenized` for details on leveraging Pigweed's
tokenizer module for logging.

See also :ref:`module-pw_tokenizer` for details on Pigweed's tokenizer,
which is useful for more than just logging.

Why does the facade use header redirection instead of C functions?
==================================================================
Without header redirection, it is not possible to do sophisticated macro
transforms in the backend. For example, to apply tokenization to log strings,
the backend must define the handling macros. Additionally, compile-time
filtering by log level or flags is not possible without header redirection.
While it may be possible to do the filtering in the facade, that would imply
having the same filtering implementation for all log handling, which is a
restriction we want to avoid.

Why is the module name done as a preprocessor define rather than an argument?
=============================================================================
APIs are a balance between power and ease of use. In the practical cases we
have seen over the years, most translation units only need to log to one
module, like ``"BLE"``, ``"PWR"``, ``"BAT"`` and so on. Thus, adding the
argument to each macro call seemed like too much. On the other hand, flags are
something that are typically added on a per-log-statement basis, and is why the
flags are added on a per-call basis (though hidden through the high-level
macros).

--------------
pw_log in Java
--------------
``pw_log`` provides a thin Java logging class that uses Google's `Flogger
<https://google.github.io/flogger/>`_ API. The purpose of this wrapper is to
support logging on platforms that do not support Flogger. The main
implementation in ``pw_log/java/main`` simply wraps a
``com.google.common.flogger.FluentLogger``. An implementation that logs to
Android's ``android.util.Log`` instead is provided in
``pw_log/java/android_main``.

----------------
pw_log in Python
----------------
``pw_log`` provides utilities to decode ``LogEntries`` and the encapsulated
``LogEntry`` proto messages.

The ``Log`` class represents a decoded ``LogEntry`` in a human-readable and
consumable fashion.

The ``LogStreamDecoder`` offers APIs to decode ``LogEntries`` and ``LogEntry``
while tracking and logging log drops. It requires a ``decoded_log_handler`` to
pass decoded logs to. This class can also be customized to use an optional token
database if the message, module and thread names are tokenized; a custom
timestamp parser; and optional message parser for any extra message parsing.
``pw_log`` includes examples for customizing the ``LogStreamDecoder``:
``timestamp_parser_ns_since_boot`` parses the timestamp number from nanoseconds
since boot to an HH:MM::SS string, ``log_decoded_log`` emits a decoded ``Log``
to the provided logger in a format known to ``pw console``, and
``pw_status_code_to_name`` searches the decoded log message for a matching
pattern encapsulating the status code number and replaces it with the status
name.

Python API
==========

pw_log.log_decoder
------------------
.. automodule:: pw_log.log_decoder
    :members: Log, LogStreamDecoder
    :undoc-members:
    :show-inheritance:

.. toctree::
   :hidden:
   :maxdepth: 1

   protobuf
   tokenized_args
   backends
