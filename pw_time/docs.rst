.. _module-pw_time:

=======
pw_time
=======
.. pigweed-module::
   :name: pw_time

``pw_time`` is a Rust Pigweed module that provides time types and clock domain
facilities specifically tailored for embedded systems, with a strong focus on portability,
correctness, and type safety.

The module allows type-safe handling of systems with multiple clock domains. Instants and
durations are associated with a specific clock domain at compile-time, preventing developers
from accidentally mixing times from different clocks.

-------------
API reference
-------------
Refer to the `rustdoc API docs </rustdoc/pw_time/>`_ for detailed documentation of the
``pw_time`` Rust API.

-----------
Get started
-----------
To use ``pw_time``, add it as a dependency in your ``BUILD.bazel`` file:

.. code-block:: starlark

   deps = [
       "//pw_time/rust:pw_time",
   ]

And import the types in your Rust source code:

.. code-block:: rust

   use pw_time::{Clock, Instant, Duration};
