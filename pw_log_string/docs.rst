.. _module-pw_log_string:

=============
pw_log_string
=============
.. pigweed-module::
   :name: pw_log_string

``pw_log_string`` is a partial backend for ``pw_log``. This backend forwards the
``PW_LOG_*`` macros to the ``pw_log_string:handler`` facade which is backed by
a C API. ``pw_log_string:handler`` does not implement the full C API, leaving
projects to provide their own implementation of
``pw_log_string_HandleMessageVaList``. See ``pw_log_basic`` for a similar
``pw_log`` backend that also provides an implementation.

As this module passes the log message, file name, and module name as a string to
the handler function, it's relatively expensive and not well suited for
space-constrained devices. This module is oriented towards usage on a host
(e.g. a simulated device).

Note that ``pw_log_string:handler`` may be used even when it's not used
as the backend for ``pw_log`` via ``pw_log_string``. For example it can be
useful to mix tokenized and string based logging in case you have a C ABI where
tokenization can not be used on the other side.

.. _module-pw_log_string-get-started-gn:

----------------
Get started (GN)
----------------
This section outlines how to implement a ``pw_log_string`` backend in a
GN-based project.

.. note::
   The example code was written for a :ref:`host <target-host>` target running
   on Linux.

Invoke a logging macro
======================
Call one of the :ref:`pw_log macros <module-pw_log-macros>` in your project
code:

.. code-block:: cpp
   :emphasize-lines: 9

   /* //src/app.cc */

   #include <unistd.h>

   #include "pw_log/log.h"

   int main() {
     while (true) {
       PW_LOG_INFO("Hello, world!");
       sleep(5);
     }
     return 0;
   }

Implement the logging function
==============================
Implement :cpp:func:`pw_log_string_HandleMessageVaList()` in C. Macros like
:cpp:func:`PW_LOG()` hand off the actual logging implementation to this
function.

The function signature of your implementation must match the one specified by
Pigweed.

The example code below just logs most of the available information to
``stdout``:

.. code-block:: c

   /* //src/pw_log_string_backend.c */

   #include <stdio.h>
   #include <stdarg.h>

   void pw_log_string_HandleMessageVaList(int level,
                                          unsigned int flags,
                                          const char* module_name,
                                          const char* file_name,
                                          int line_number,
                                          const char* message,
                                          va_list args) {
       printf("Entering custom pw_log_string backend...\n");
       printf("%d\n", level);
       printf("%u\n", flags);
       printf("%s\n", module_name);
       printf("%s\n", file_name);
       printf("%d\n", line_number);
       printf("%s\n", message);
       if (args) { /* Do something with your args here... */ }
       printf("Exiting custom pw_log_string backend...\n\n");
   }

What exactly ``pw_log_string_HandleMessageVaList()`` should do is entirely up to
the implementation. The log handler in ``pw_log_basic`` is one example, but it's
also possible to encode as protobuf and send over a TCP port, write to a file,
or even blink an LED to log as morse code.

Create source sets
==================
.. _source set: https://gn.googlesource.com/gn/+/main/docs/reference.md#c_language-source_sets

Use ``pw_source_set`` to create a `source set`_ for your logging
implementation. Do not use GN's built-in ``source_set`` feature.

.. code-block:: python

   # //src/BUILD.gn

   ...

   pw_source_set("pw_log_string_backend") {
       sources = [ "pw_log_string_backend.c" ]
   }

   pw_source_set("pw_log_string_backend.impl") {
       sources = []
   }

   ...

The empty ``pw_log_string_backend.impl`` source set prevents circular
dependencies. See the comment for ``group("impl")`` in :cs:`pw_log/BUILD.gn`
for more context.

Configure backends
==================
Update your target toolchain configuration file:

* Set ``pw_log_BACKEND`` to ``dir_pw_log_string``
* Point ``pw_log_string_HANDLER_BACKEND`` to your source set that implements
  :cpp:func:`pw_log_string_HandleMessageVaList()`
* Update :ref:`pw_build_LINK_DEPS <module-pw_build-link-deps>` to include
  ``"$dir_pw_log:impl"`` and ``"$dir_pw_log_string:handler:impl"``

.. code-block:: python
   :emphasize-lines: 11,12,14,15

   # //targets/my_target/target_toolchains.gni

   ...

   my_target = {
     ...
     my_toolchain = {
       name = "my_toolchain"
       defaults = {
         ...
         pw_log_BACKEND = dir_pw_log_string
         pw_log_string_HANDLER_BACKEND = "//src:pw_log_string_backend"
         pw_build_LINK_DEPS = [
           "$dir_pw_log:impl",
           "$dir_pw_log_string:handler.impl",
           ...
         ]
         ...
       }
     }
   }

   ...


(Optional) Implement message handler
====================================
Optionally provide your own implementation of ``PW_LOG_STRING_HANDLE_MESSAGE``
which invokes ``pw_log_string_HANDLER_BACKEND`` with your selected arguments.

``assert`` wrapper
==================
A wrapper for ``assert`` is provided that redirects calls to the ``pw_log_string``
handler. This can be used to replace all usage of ``assert`` in a Newlib binary
at link time.

-------------
API reference
-------------
Moved: :cc:`pw_log_string`
