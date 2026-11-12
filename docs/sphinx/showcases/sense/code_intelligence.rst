.. _showcase-sense-tutorial-intel:

================================
4. Explore C++ code intelligence
================================
Sense's integration with :ref:`module-pw_ide` enables fast and accurate
code navigation, autocompletion based on a deep understanding of the
code structure, and instant compiler warnings and errors. Try intelligent
code navigation features now:

.. _showcase-sense-tutorial-intel-nav:

-------------------------------
Navigate the code intelligently
-------------------------------
Suppose that you need to modify how your app logs messages. Your first task is
to understand how logging currently works. You know that your app uses
:ref:`module-pw_log` and that ``pw_log`` uses a special type of API called a
:ref:`facade <docs-facades>` where the implementation can be changed during
compilation. The plan is to use the intelligent code navigation features of
``pw_ide`` to determine the true, final implementation of the logging functions
used in your project.

.. _Command Palette: https://code.visualstudio.com/docs/getstarted/userinterface#_command-palette

.. tab-set::

   .. tab-item:: VS Code
      :sync: vsc

      #. Click the Pigweed icon to open the **Pigweed VSCode Extension** view.

         .. figure:: https://www.gstatic.com/pigweed/sense/extension_icon_v1.png

      #. Under **Select or generate compile commands** click the **Generate**
         button for **Host**.

      #. Press :kbd:`Control+Shift+P` (:kbd:`Command+Shift+P` on macOS) to open
         the `Command Palette`_.

      #. Start typing ``Pigweed: Select Code Analysis Target`` and press
         :kbd:`Enter` to start executing that command.

         .. figure:: https://www.gstatic.com/pigweed/sense/20240802/target.png

         .. tip::

            You can also select code analysis targets from the bottom-left
            of your VS Code GUI, in the status bar. In the next image,
            the mouse cursor (bottom-left of image) is hovering over the GUI
            element for selecting code analysis targets.

            .. figure:: https://www.gstatic.com/pigweed/sense/20240802/select_target_status_bar.png

      #. Select the ``Host (@targets//host:host)`` option.

         The code intelligence is now set up to help you understand how logging
         works when you build the firmware so that it can be simulated on your
         development host.

      #. Open ``//apps/blinky/main.cc``.

         .. figure:: https://www.gstatic.com/pigweed/sense/blinky_main_v1.png

      #. Right-click the ``PW_LOG_INFO()`` invocation and select
         **Go to Definition**.

         .. figure:: https://www.gstatic.com/pigweed/sense/20240802/go_to_definition.png
            :alt: Selecting "Go to Definition" after right-clicking PW_LOG_INFO()

         This should take you to a file called ``log.h`` Your cursor should be
         on the line that defines ``PW_LOG_INFO``:

         .. code-block:: c++
            :emphasize-lines: 2

            #ifndef PW_LOG_INFO
            #define PW_LOG_INFO(...) /* Your cursor should be on this line. */ \
              PW_LOG(PW_LOG_LEVEL_INFO, PW_LOG_MODULE_NAME, PW_LOG_FLAGS, __VA_ARGS__)
            #endif  // PW_LOG_INFO

      #. Right-click the ``PW_LOG()`` invocation (on the line below your cursor)
         and select **Go to Definition** again.

         Your cursor should jump to the line that defines :cc:`PW_LOG`:

         .. code-block:: c++
            :emphasize-lines: 2

            // Your cursor should be on the next line.
            #define PW_LOG(                                                            \
                level, verbosity, module, flags, /* format string and arguments */...) \
              do {                                                                     \
                if (PW_LOG_ENABLE_IF(level, verbosity, module, flags)) {               \
                  PW_HANDLE_LOG(level, module, flags, __VA_ARGS__);                    \
                }                                                                      \
              } while (0)

      #. Finally, right-click the ``PW_HANDLE_LOG()`` invocation
         and select **Go to Definition** one last time. You should jump to
         a :ref:`module-pw_log_tokenized` backend header. You can hover
         over the filename in the tab (``log_backend.h``) to see the full
         path to the header.

         .. code-block: c++

            #define PW_HANDLE_LOG PW_LOG_TOKENIZED_TO_GLOBAL_HANDLER_WITH_METADATA

   .. tab-item:: CLI
      :sync: cli

      This feature is only supported in VS Code.

.. _showcase-sense-tutorial-intel-explanation:

-----------
Explanation
-----------
``PW_LOG`` is a :ref:`facade <docs-facades>`. You can change out the
implementation of that logging function at compile-time. For example, on your
real hardware perhaps you use :ref:`module-pw_log_tokenized` to save code size,
but when simulating your firmware on your development host maybe you compile
with :ref:`module-pw_log_string` to make it easy to view debug log messages.
Let's call the hardware target in our example ``rp2040`` and the host target
``host_simulator``. The ``Select Code Analysis Target`` feature of Pigweed's
VS Code extension lets you investigate these different code paths. Selecting
``host_simulator`` as your target and drilling down via **Go to Definition**
would ultimately show you a ``PW_LOG`` implementation in ``pw_log_string``.
Selecting the ``rp2040`` target and drilling down again would show a completely
different implementation for ``PW_LOG`` in ``pw_log_tokenized``.

.. mermaid::

   flowchart LR

     a["main.cc"] --> b["log.h"]
     b["log.h"] -. rp2040 .-> c["pw_log_tokenized/.../log_backend.h"]
     b["log.h"] -. host_simulator .-> d["pw_log_string/.../log_backend.h"]

.. note::

   Currently all Sense targets use ``pw_log_tokenized`` as their backend so
   it's not possible to demonstrate this idea with the Sense repo.

.. _showcase-sense-tutorial-intel-summary:

-------
Summary
-------
Portable, hardware-agnostic software abstractions such as :ref:`module-pw_log`
make it easier to reuse code across projects and hardware platforms. But they
also make it more difficult to correctly navigate references in your codebase.
The Pigweed extension for VS Code can solve this problem; you just need to
tell it what target within your codebase to focus on.

Next, head over to :ref:`showcase-sense-tutorial-hosttests` to learn how to run
unit tests on your development host.
