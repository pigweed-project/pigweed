.. _module-pw_preprocessor:

---------------
pw_preprocessor
---------------
.. pigweed-module::
   :name: pw_preprocessor

The preprocessor module provides various helpful preprocessor macros.

Compatibility
=============
C and C++

API reference
=============
Moved: :cc:`pw_preprocessor`

Headers
=======
The preprocessor module provides several headers.

pw_preprocessor/apply.h
-----------------------
Defines the :cc:`PW_APPLY` macro, which expands to a macro invocation for each
argument it is passed, with optional separators.

pw_preprocessor/arguments.h
---------------------------
Defines macros for handling variadic arguments to function-like macros. Macros
include the following:

.. c:macro:: PW_DELEGATE_BY_ARG_COUNT(name, ...)

  Selects and invokes a macro based on the number of arguments provided. Expands
  to ``<name><arg_count>(...)``. For example,
  ``PW_DELEGATE_BY_ARG_COUNT(foo_, 1, 2, 3)`` expands to ``foo_3(1, 2, 3)``.

  This example shows how :c:macro:`PW_DELEGATE_BY_ARG_COUNT` could be used to log a
  customized message based on the number of arguments provided.

  .. code-block:: cpp

     #define ARG_PRINT(...) PW_DELEGATE_BY_ARG_COUNT(_ARG_PRINT, __VA_ARGS__)
     #define _ARG_PRINT0(a) LOG_INFO("nothing!")
     #define _ARG_PRINT1(a) LOG_INFO("1 arg: %s", a)
     #define _ARG_PRINT2(a, b) LOG_INFO("2 args: %s, %s", a, b)
     #define _ARG_PRINT3(a, b, c) LOG_INFO("3 args: %s, %s, %s", a, b, c)

  When used, ``ARG_PRINT`` expands to the ``_ARG_PRINT#`` macro corresponding
  to the number of arguments.

  .. code-block:: cpp

     ARG_PRINT();               // Outputs: nothing!
     ARG_PRINT("a");            // Outputs: 1 arg: a
     ARG_PRINT("a", "b");       // Outputs: 2 args: a, b
     ARG_PRINT("a", "b", "c");  // Outputs: 3 args: a, b, c

.. c:macro:: PW_COMMA_ARGS(...)

  Expands to a comma followed by the arguments if any arguments are provided.
  Otherwise, expands to nothing. If the final argument is empty, it is omitted.
  This is useful when passing ``__VA_ARGS__`` to a variadic function or template
  parameter list, since it removes the extra comma when no arguments are
  provided. ``PW_COMMA_ARGS`` must NOT be used when invoking a macro from
  another macro.

  For example. ``PW_COMMA_ARGS(1, 2, 3)``, expands to ``, 1, 2, 3``, while
  ``PW_COMMA_ARGS()`` expands to nothing. ``PW_COMMA_ARGS(1, 2, )`` expands to
  ``, 1, 2``.

pw_preprocessor/boolean.h
-------------------------
Defines macros for boolean logic on literal 1s and 0s. This is useful for
situations when a literal is needed to build the name of a function or macro.

pw_preprocessor/compiler.h
--------------------------
Macros for compiler-specific features, such as attributes (for example,
:cc:`PW_ATTRIBUTE_LIFETIME_BOUND`, :cc:`PW_ATTRIBUTE_PREFERRED_NAME`,
and :cc:`PW_NO_UNIQUE_ADDRESS`) or builtins.

Modifying compiler diagnostics
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
``pw_preprocessor/compiler.h`` provides macros for enabling or disabling
compiler diagnostics (warnings or errors) for sections of code.

:cc:`PW_MODIFY_DIAGNOSTICS_PUSH` and :cc:`PW_MODIFY_DIAGNOSTICS_POP`
are used to turn off or on diagnostics (warnings or errors) for a section of
code. Use :cc:`PW_MODIFY_DIAGNOSTICS_PUSH`, use
:cc:`PW_MODIFY_DIAGNOSTIC` as many times as needed, then use
:cc:`PW_MODIFY_DIAGNOSTICS_POP` to restore the previous settings.

.. code-block:: c

   PW_MODIFY_DIAGNOSTICS_PUSH();
   PW_MODIFY_DIAGNOSTIC(ignored, "-Wunused-variable");

   static int this_variable_is_never_used;

   PW_MODIFY_DIAGNOSTICS_POP();

.. tip::

  :cc:`PW_MODIFY_DIAGNOSTIC` and related macros should rarely be used.
  Whenever possible, fix the underlying issues about which the compiler is
  warning, rather than silencing the diagnostics.

.. _module-pw_preprocessor-integer-overflow:

pw_preprocessor/concat.h
------------------------
Defines the ``PW_CONCAT(...)`` macro, which expands its arguments if they are
macros and token pastes the results. This can be used for building names of
classes, variables, macros, etc.

pw_preprocessor/util.h
----------------------
General purpose, useful macros.

* ``PW_ARRAY_SIZE(array)`` -- calculates the size of a C array
* ``PW_STRINGIFY(...)`` -- expands its arguments as macros and converts them to
  a string literal
* ``PW_EXTERN_C`` -- declares a name to be ``extern "C"`` in C++; expands to
  nothing in C
* ``PW_EXTERN_C_START`` / ``PW_EXTERN_C_END`` -- declares an ``extern "C" { }``
  block in C++; expands to nothing in C

Zephyr
======
To enable ``pw_preprocessor`` for Zephyr add ``CONFIG_PIGWEED_PREPROCESSOR=y``
to the project's configuration.
