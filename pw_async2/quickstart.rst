.. _module-pw_async2-quickstart:

==========
Quickstart
==========
.. pigweed-module-subpage::
   :name: pw_async2

``pw_async2`` is Pigweed's cooperatively scheduled async framework.
``pw_async2`` makes it easier to build correct, efficient asynchronous
firmware. This quickstart sets you up with a minimal but complete ``pw_async2``
project and explains key concepts along the way.

* For a detailed conceptual overview of ``pw_async2``, check out
  :ref:`module-pw_async2-informed-poll`.
* For a more in-depth, hands-on introduction, try the
  :ref:`module-pw_async2-codelab`.

.. tab-set::

   .. tab-item:: Post task to dispatcher

      .. literalinclude:: examples/quickstart.cc
         :language: cpp
         :linenos:
         :start-after: // DOCSTAG: [main]
         :end-before: // DOCSTAG: [main]

   .. tab-item:: Implement task

      Create a subclass of :cc:`Task <pw::async2::Task>`:

      .. literalinclude:: examples/quickstart.cc
         :language: cpp
         :linenos:
         :start-after: // DOCSTAG: [decl]
         :end-before: // DOCSTAG: [decl]

      Implement an override of :cc:`DoPend() <pw::async2::Task::DoPend>`:

      .. literalinclude:: examples/quickstart.cc
         :language: cpp
         :linenos:
         :start-after: // DOCSTAG: [impl]
         :end-before: // DOCSTAG: [impl]

   .. tab-item:: Set up build rules

      .. literalinclude:: examples/BUILD.bazel
         :language: py
         :linenos:
         :start-after: # DOCSTAG: [quickstart]
         :end-before: # DOCSTAG: [quickstart]

.. tip::

   The quickstart is based on the following files in upstream Pigweed:

   * :cs:`pw_async2/examples/quickstart.cc`
   * :cs:`pw_async2/examples/BUILD.bazel`

   You can build and test it yourself with the following command:

   .. code-block:: console

      bazelisk test //pw_async2/examples:quickstart

   See :ref:`docs-contributing` for help with setting up the upstream
   Pigweed repo.

.. _module-pw_async2-quickstart-build:

------------------
Set up build rules
------------------
Add a dependency on ``pw_async2``. If you instantiate a concrete dispatcher
such as ``basic_dispatcher``, also depend on that dispatcher implementation.
The ``pw_async2`` dependency pulls in most of the core ``pw_async2`` and
dispatcher APIs that you'll always need.

.. tab-set::

   .. tab-item:: Bazel

      .. literalinclude:: examples/BUILD.bazel
         :language: py
         :linenos:
         :emphasize-lines: 5-6
         :start-after: # DOCSTAG: [quickstart]
         :end-before: # DOCSTAG: [quickstart]

      See :ref:`docs-build-bazel` for help with creating a new Bazel project
      or integrating Pigweed into an existing Bazel project.

      .. note::

         The code above uses ``pw_cc_test`` because we unit test this example in
         upstream Pigweed. In your project you'll want to use ``cc_binary`` or
         :ref:`module-pw_build-bazel-pw_cc_binary` instead.

   .. tab-item:: GN

      .. code-block:: py

         pw_executable("quickstart") {
           sources = [
             "quickstart.cc",
           ]
           deps = [
             "$dir_pw_async2",
             # …
           ]
         }

      See :ref:`docs-build-gn` for help with integrating Pigweed into an
      existing GN project.

   .. tab-item:: CMake

      .. code-block:: py

         add_executable(quickstart quickstart.cc)
         target_link_libraries(quickstart PRIVATE pw_async2)

      See :ref:`docs-build-cmake` for help with integrating Pigweed into an
      existing CMake project.

.. _module-pw_async2-quickstart-dispatcher:

-----------------------------------
Create a dispatcher and post a task
-----------------------------------
The :ref:`dispatcher <module-pw_async2-informed-poll-components-dispatcher>` is
the cooperative scheduler of ``pw_async2``. :ref:`Tasks
<module-pw_async2-informed-poll-components-task>` are logical collections of
async work. You post tasks to the dispatcher, and the dispatcher drives the
tasks to completion.

The highlighted lines below demonstrate creating a task, posting a task to
a dispatcher, and running the tasks with a dispatcher:

.. literalinclude:: examples/quickstart.cc
   :language: cpp
   :linenos:
   :emphasize-lines: 11,13,15,17
   :start-after: // DOCSTAG: [main]
   :end-before: // DOCSTAG: [main]

The next section dives into the ``TimerTask`` implementation in detail.

.. _module-pw_async2-quickstart-task:

----------------
Implement a task
----------------
Each task represents a logical collection of async work. For example, when
writing the firmware for a vending machine, you might dedicate one task to
handling user input, another task to driving the item dispenser machinery,
and yet another task to sending time series data to the cloud.

First, let's study the main API of a single task. Your task class must be a
subclass of :cc:`pw::async2::Task` and it must implement an override of
:cc:`DoPend() <pw::async2::Task::DoPend>`, as ``TimerTask`` demonstrates:

.. literalinclude:: examples/quickstart.cc
   :language: cpp
   :linenos:
   :emphasize-lines: 8,15
   :start-after: // DOCSTAG: [decl]
   :end-before: // DOCSTAG: [decl]

.. note::

   The dispatcher drives a task forward by invoking the task's ``Pend()``
   method, which is a non-virtual wrapper around ``DoPend()``.

The ``DoPend()`` implementation is where ``TimerTask`` progresses through its
work asynchronously. Our ``DoPend()`` waits on a ``TimeFuture`` to complete.
``time_future_`` finishes after 100ms and then never runs again. Futures like
``time_future_`` will be described more in the next section. For now, focus on
the return values of the implementation. Notice how the implementation returns
``Pending()`` in one case, and ``Ready()`` in another case:

.. literalinclude:: examples/quickstart.cc
   :language: cpp
   :linenos:
   :emphasize-lines: 11,16
   :start-after: // DOCSTAG: [impl]
   :end-before: // DOCSTAG: [impl]

All ``DoPend()`` implementations follow this general pattern:

* The task is not able to progress for some reason so it returns
  :cc:`pw::async2::Pending` to notify the dispatcher that it's stalled. The
  dispatcher sleeps the task.

  .. tip::

     Helpers like :cc:`PW_TRY_READY` and :cc:`PW_TRY_READY_ASSIGN` can reduce
     the boilerplate of handling a stalled task.

* Something informs the dispatcher that the task is able to make more progress.
  This will be explained in the next section. The dispatcher runs the task
  again.

* On the second run, ``TimerTask`` completes all of its work. The task returns
  :cc:`pw::async2::Ready` to signal to the dispatcher that it has finished and
  no longer needs to be polled.

.. _module-pw_async2-quickstart-future:

-----------------
Wait for a future
-----------------
If you run this example, you'll see ``Waiting…`` logged only once:

.. code-block:: none

   INF  Waiting…
   INF  Done!

This suggests that there's no busy polling. So how does the polling work?
``time_future_`` is the key to understanding the flow. ``time_future_`` is a
:ref:`future <module-pw_async2-informed-poll-components-future>`, the main
async primitive in ``pw_async2``. A future is a result of an asynchronous
operation that may not be ready yet.  Notice how ``cx`` is passed to the
future's ``Pend()`` method:

.. literalinclude:: examples/quickstart.cc
   :language: cpp
   :linenos:
   :emphasize-lines: 8
   :start-after: // DOCSTAG: [impl]
   :end-before: // DOCSTAG: [impl]

When the future is ready, it informs the dispatcher that its parent task can
make more progress and therefore should be polled again. This is why the
``pw_async2`` programming model is called :ref:`module-pw_async2-informed-poll`.

.. note::

   When the task invokes the future's ``Pend()`` method, the task provides
   a :cc:`pw::async2::Context` instance. This ``Context`` instance is how the
   future informs the dispatcher.

.. _module-pw_async2-quickstart-more:

----------
Learn more
----------
Posting tasks to the dispatcher and implementing tasks that wait on futures is
the core of all ``pw_async2`` systems. Of course, in a real-world project,
you may need to handle more complex interactions, such as:

* Tasks that communicate with each other via :ref:`module-pw_async2-channels`.

* :ref:`Custom futures <module-pw_async2-futures-implementing>` that
  :ref:`integrate with hardware
  <module-pw_async2-futures-implementing-example>`.

* Tasks that :ref:`wait on multiple futures
  <module-pw_async2-futures-combinators>`.

* :ref:`Custom dispatchers <module-pw_async2-dispatcher>`.

Browse the :ref:`module-pw_async2-more` section on the ``pw_async2`` homepage
for more guidance on building correct, efficient firmware with ``pw_async2``.
