.. _docs-os-zephyr-setup:

=====
Setup
=====
The quickest way to get started using Pigweed with Zephyr is to follow the
step-by-step :ref:`docs-quickstart-zephyr`.

------------------
Starter repository
------------------
You can also check out the `zephyr_pigweed`_ repository for an example of a
Zephyr starter project that has been set up to use Pigweed.

-------
Testing
-------
To run tests against Zephyr, first go through the `zephyr_pigweed`_ tutorial.
Once set up, activate the Pigweed environment and invoke ``west twister``:

.. code-block:: bash

   $ source ${PW_ROOT}/activate.sh
   $ west twister -T ${PW_ROOT}

.. attention::
   Testing has only been verified with the ``-p native_posix`` platform. Use
   caution on other platforms.

.. _zephyr_pigweed: https://github.com/yperess/zephyr-pigweed/
