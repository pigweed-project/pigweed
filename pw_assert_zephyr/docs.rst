.. _module-pw_assert_zephyr:

================
pw_assert_zephyr
================
.. pigweed-module::
   :name: pw_assert_zephyr

--------
Overview
--------
This assert backend implements the ``pw_assert`` facade, by routing the assert
message to the Zephyr assert subsystem. Failed asserts will call:
1) ``__ASSERT_LOC(condition)``
2) If and only if there's a message ``__ASSERT_MSG_INFO(message, ...)``
3) ``__ASSERT_POST_ACTION()``

To enable the assert module, set ``CONFIG_PIGWEED_ASSERT_ZEPHYR=y``. After that,
Zephyr's assert configs can be used to control the behavior via CONFIG_ASSERT_
and CONFIG_ASSERT_LEVEL_.

.. _CONFIG_ASSERT: https://docs.zephyrproject.org/latest/kconfig.html#CONFIG_ASSERT
.. _CONFIG_ASSERT_LEVEL: https://docs.zephyrproject.org/latest/kconfig.html#CONFIG_ASSERT_LEVEL

Using Pigweed tokenized asserts
-------------------------------
Using the pigweed tokenized assert can be done by enabling
``CONFIG_PIGWEED_ASSERT_TOKENIZED=y``. At that point ``pw_assert_tokenized`` is
set as the backend for ``pw_assert`` and all Zephyr asserts are routed to
Pigweed's assert/check facade. This means that any assert statements made in
Zephyr itself are also tokenized.

When enabled, the tokenized crash is forwarded to the log system where
``LOG_LEVEL_FATAL`` will need to be handled by the user. The assert will then
need to be properly diverted to a crash handler for processing.

Zephyr asserts need to be turned on with ``CONFIG_ASSERT=y``. Without asserts
enabled in zephyr, the zephyr asserts will not be active and only the
application calls to ``pw_assert`` will be active.
