.. _docs-facades:

==================
Facades & backends
==================
This page explains what "facades" and "backends" mean in the context of Pigweed
and provides guidelines on when to use them.

.. _docs-facades-definition:

------------------------------
What are facades and backends?
------------------------------
.. _top-down approach: https://en.wikipedia.org/wiki/Bottom%E2%80%93up_and_top%E2%80%93down_design

Let's take a `top-down approach`_, starting with high-level definitions.
In the context of a Pigweed module:

* A facade is an API contract of a module that must be satisfied at compile-time,
  i.e. a swappable dependency that changes the implementation of an API at
  compile-time.
* A backend is an implementation of a facade's contract.

Usually, the facade and the backend are in different modules. One module
exposes the facade contract, and another module is the backend that implements
the facade contract. The naming pattern that Pigweed follows is
``{facade_name}_{backend_name}``.

Facades, by design, don't need to be configured until they're actually used.
This makes it significantly easier to bring up features piece-by-piece.

Example: pw_log and pw_log_string
=================================
Here's a step-by-step walkthrough of how a real backend module implements
another module's facade.

* :ref:`module-pw_log` is a module that exposes a facade. The macros listed in
  :ref:`module-pw_log-macros` represent the API of the module.
* The ``#include "pw_log_backend/log_backend.h"`` line in
  :cs:`pw_log/public/pw_log/log.h` represents the facade contract of ``pw_log``.
* :ref:`module-pw_log_string` is a backend module, It implements the
  ``pw_log_backend/log_backend.h`` facade contract in
  :cs:`pw_log_string/public_overrides/pw_log_backend/log_backend.h`.
* In the build system there is a variable of some sort that specifies the
  backend. In Bazel there's a `label
  flag <https://bazel.build/extending/config#label-typed-build-settings>`_
  at ``//pw_log:backend``. In CMake there's a ``pw_log_BACKEND`` variable set
  to ``pw_log_string``. In GN there's a ``pw_log_BACKEND`` variable set to
  ``dir_pw_log_string``.

.. note::

   There are a few more steps needed to get ``pw_log_string`` hooked up as the
   backend for ``pw_log`` but they aren't essential for the current discussion.
   See :ref:`module-pw_log_string-get-started-gn` for the details.

Example: Swappable OS libraries
===============================
The facade and backend system is similar to swappable OS libraries: you can
write code against ``libc`` (for example) and it will work so long as you have
an object to link against that has the symbols you're depending on. You can
swap in different versions of ``libc`` and your code doesn't need to know about
it.

A similar example from Windows is ``d3d9.dll``. Developers often swap this DLL
with a different library like ReShade to customize shading behavior.

Can a module have multiple facades?
===================================
Yes. The module-to-facade relationship is one-to-many. A module can expose
0, 1, or many facades. There can be (and usually are) multiple backend modules
implementing the same facade.

Is the module facade the same thing as its API?
===============================================
No. It's best to think of them as different concepts. The API is the interface
that downstream projects interact with. There's always a one-to-one relationship
between a module and its API. A facade represents the build system "glue" that
binds a backend to an API.

This is a common point of confusion because there are a few modules that
blur this distinction. Historically, the facade comprised the API as well as
the build system concept. We're moving away from this perspective.

Are Pigweed facades the same as the GoF facade design pattern?
==============================================================
.. _facade pattern: https://en.wikipedia.org/wiki/Facade_pattern

There are some loose similarities but it's best to think of them as different
things. The goal of the Gang of Four `facade pattern`_ is to compress some
ugly, complex API behind a much smaller API that is more aligned with your
narrow business needs. The motivation behind some Pigweed facades is loosely
the same: we don't want a device HAL to leak out into your include paths when
a facade is implemented.

------------------------------
Why does Pigweed have facades?
------------------------------
Pigweed's facades are basically a pattern that builds off the ideas of
`link-time substitution <https://bramtertoolen.medium.com/91ffd4ef8687>`_.
Link-time substitution only allows you to replace one source file with another,
whereas facades enable substituting program elements defined in *header files*.

Pigweed facades enable implementation flexibility with zero performance cost.
There are two ways to look at this. Continuing with the ``pw_log`` example:

* Organizations can reuse more code across projects by adopting the ``pw_log``
  API as their logging layer. Facades enable each project to customize how
  its logs are handled.
* For projects that want to integrate with ``pw_log`` but cannot adopt its
  API, facades can wrap the existing API which enables Pigweed to be compatible
  with the existing system.

Two of the major aspects of "implementation flexibility" enabled by facades are:

* Portability. For example, ``pw_sync`` needs platform-specific
  implementations to work correctly.
* Customized behavior. For example, you can make a fully portable ``pw_log``
  implementation, but what makes it special is the ability to tune it to your
  needs.

Why compile-time?
=================
Resolving facades and backends at compile-time enables:

* Call-site control from backends.
* Static allocation of backend-provided types.
* Explicit backend includes so it’s visually obvious you’re poking through
  abstraction.

--------------------------------
When to use facades and backends
--------------------------------
If you're trying to use a Pigweed module, and that module exposes a facade,
then you've got no choice: you've got to hook up a backend to fulfill that
facade contract or else the module won't work.

When to roll your own facades and backends
==========================================
* You need a global function or macro.
* You absolutely must avoid the overhead of virtual functions.

When to NOT roll your own facades and backends
==============================================
* If you can afford the runtime cost of dependency injection, use that.
  In all other cases where link-time subsitution will work, use that.
  Only if the API contract requires a backend to provide a header (which
  link-time substitution doesn't let you do) should you reach for a facade.
* You're trying to use globals to avoid dependency injection. Use
  the dependency injection! It makes testing much easier.
* Your needs can be served by a standard mechanism like virtual interfaces.
  Use the standard mechanism.
