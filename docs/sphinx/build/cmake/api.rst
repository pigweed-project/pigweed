.. _cmake-api:

===================
CMake API reference
===================
.. pigweed-module-subpage::
   :name: pw_build

Pigweed provides numerous CMake helper functions. Testing-related
functions are defined and documented in :cs:`pw_unit_test/test.cmake`.
Everything else comes from :cs:`pw_build/pigweed.cmake`. This page
lists all of the helper functions alphabetically. The usage information
is automatically pulled from the comments in :cs:`pw_unit_test/test.cmake`
and :cs:`pw_build/pigweed.cmake`.

.. _cmake-api-pw_add_error_target:

-------------------
pw_add_error_target
-------------------
.. literalinclude:: ../../pw_build/pigweed.cmake
   :language: text
   :start-at: # pw_add_error_target:
   :end-before: function(pw_add_error_target

.. _cmake-api-pw_add_facade:

-------------
pw_add_facade
-------------
.. literalinclude:: ../../pw_build/pigweed.cmake
   :language: text
   :start-at: # Declares a module as a facade
   :end-before: function(pw_add_facade NAME

.. _cmake-api-pw_add_facade_generic:

---------------------
pw_add_facade_generic
---------------------
.. literalinclude:: ../../pw_build/pigweed.cmake
   :language: text
   :start-at: # pw_add_facade_generic:
   :end-before: function(pw_add_facade_generic NAME

.. _cmake-api-pw_add_global_compile_options:

-----------------------------
pw_add_global_compile_options
-----------------------------
.. literalinclude:: ../../pw_build/pigweed.cmake
   :language: text
   :start-at: # Adds compiler options
   :end-before: function(pw_add_global_compile_options

.. _cmake-api-pw_add_library:

--------------
pw_add_library
--------------
.. literalinclude:: ../../pw_build/pigweed.cmake
   :language: text
   :start-at: # Creates a pw module library
   :end-before: function(pw_add_library NAME

.. _cmake-api-pw_add_library_generic:

----------------------
pw_add_library_generic
----------------------
.. literalinclude:: ../../pw_build/pigweed.cmake
   :language: text
   :start-at: # pw_add_library_generic:
   :end-before: function(pw_add_library_generic NAME

.. _cmake-api-pw_add_test:

-----------
pw_add_test
-----------
.. literalinclude:: ../../pw_unit_test/test.cmake
   :language: text
   :start-at: # pw_add_test:
   :end-before: function(pw_add_test

.. _cmake-api-pw_add_test_generic:

-------------------
pw_add_test_generic
-------------------
.. literalinclude:: ../../pw_unit_test/test.cmake
   :language: text
   :start-at: # pw_add_test_generic:
   :end-before: function(pw_add_test_generic

.. _cmake-api-pw_add_test_group:

-----------------
pw_add_test_group
-----------------
.. literalinclude:: ../../pw_unit_test/test.cmake
   :language: text
   :start-at: # pw_add_test_group:
   :end-before: function(pw_add_test_group

.. _cmake-api-pw_parse_arguments:

------------------
pw_parse_arguments
------------------
.. literalinclude:: ../../pw_build/pigweed.cmake
   :language: text
   :start-at: # Wrapper around cmake_parse_arguments
   :end-before: macro(pw_parse_arguments)

.. _cmake-api-pw_rebase_paths:

---------------
pw_rebase_paths
---------------
.. literalinclude:: ../../pw_build/pigweed.cmake
   :language: text
   :start-at: # Rebases a set of files
   :end-before: function(pw_rebase_paths VAR

.. _cmake-api-pw_set_backend:

--------------
pw_set_backend
--------------
.. literalinclude:: ../../pw_build/pigweed.cmake
   :language: text
   :start-at: # Sets which backend
   :end-before: function(pw_set_backend NAME

.. _cmake-api-pw_target_link_targets:

----------------------
pw_target_link_targets
----------------------
.. literalinclude:: ../../pw_build/pigweed.cmake
   :language: text
   :start-at: # pw_target_link_targets:
   :end-before: function(pw_target_link_targets
