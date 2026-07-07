.. _stl-backends:

===================
STL backend modules
===================
Pigweed provides native STL backends for several of its core OS abstraction
layers.

.. list-table:: OS backend modules
   :header-rows: 1

   * - **API group**
     - **Backend module**
     - **Description**
   * - **Time**
     - :ref:`module-pw_chrono_stl`
     - C++ standard library clock and timer backends.
   * - **Synchronization**
     - :ref:`module-pw_sync_stl`
     - C++ standard library mutex and semaphore backends.
   * - **Threading**
     - :ref:`module-pw_thread_stl`
     - C++ standard library threading backends.

.. toctree::
   :maxdepth: 1
   :hidden:

   Chrono <pw://pw_chrono_stl/docs.html>
   Sync <pw://pw_sync_stl/docs.html>
   Thread <pw://pw_thread_stl/docs.html>
