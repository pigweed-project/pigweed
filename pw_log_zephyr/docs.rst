.. _module-pw_log_zephyr:

=============
pw_log_zephyr
=============
.. pigweed-module::
   :name: pw_log_zephyr

--------
Overview
--------
This interrupt backend implements the ``pw_log`` facade. Currently, two
separate Pigweed backends are implemented. One that uses the plain Zephyr
logging framework and routes Pigweed's logs to Zephyr. While another maps
the Zephyr logging macros to Pigweed's tokenized logging.

Using Zephyr logging
--------------------
To enable, set ``CONFIG_PIGWEED_LOG_ZEPHYR=y``. After that, logging can be
controlled via the standard `Kconfig options`_. All logs made through
`PW_LOG_*` are logged to the Zephyr logging module ``pigweed``. In this
model, the Zephyr logging is set as ``pw_log``'s backend.

Using Pigweed tokenized logging
-------------------------------
Using the pigweed logging can be done by enabling
``CONFIG_PIGWEED_LOG_TOKENIZED=y``. At that point ``pw_log_tokenized`` is set
as the backend for ``pw_log`` and all Zephyr logs are routed to Pigweed's
logging facade. This means that any logging statements made in Zephyr itself
are also tokenized.

When enabled, a few extra configurations are available to control the tokenized
metadata bits such as log level bits, line number bits, custom flag bits, and
module string bits.
The log format string may also be modified by defining your own ``PW_LOG_TOKENIZED_FORMAT_STRING``.
This can be done in your cmake by including your own header that defines it.

.. code-block::

   add_library(log_tokenized_config INTERFACE)
   target_compile_options(log_tokenized_config INTERFACE -include header_file_that_sets_that_macro.h)
   pw_set_module_config(pw_log_tokenized_CONFIG log_tokenized_config)

Where tokenized log bytes go
-----------------------------
Enabling tokenized logging (``CONFIG_PIGWEED_LOG_TOKENIZED`` or
``CONFIG_PIGWEED_LOG_TOKENIZED_RPC``) only decides that log messages get
tokenized -- a separate facade, ``pw_log_tokenized.handler``, decides where
the resulting bytes actually go. It's controlled by its own ``choice
PIGWEED_LOG_TOKENIZED_HANDLER_BACKEND`` (see :ref:`zephyr-backend-selection`
for the general pattern):

* ``CONFIG_PIGWEED_LOG_TOKENIZED_HANDLER_ZEPHYR`` -- route tokenized bytes
  into Zephyr's own log subsystem.
* ``CONFIG_PIGWEED_LOG_TOKENIZED_HANDLER_SYSTEM`` -- route tokenized bytes
  into :ref:`module-pw_system`'s log backend (its multisink/RPC pipeline)
  instead.
* ``CONFIG_PIGWEED_LOG_TOKENIZED_HANDLER_NONE`` -- don't link a handler;
  you're supplying your own via ``pw_set_backend()``.

Defaults to Zephyr; to ``SYSTEM`` when ``CONFIG_PIGWEED_SYSTEM_LOG_BACKEND``
is set (``CONFIG_PIGWEED_LOG_TOKENIZED_RPC`` sets it), since ``pw_system``
then handles logs; and to ``NONE`` with ``CONFIG_PIGWEED_LOG_TOKENIZED_LIB``,
where the application provides the handler.

Setting the log level
---------------------
In order to remain compatible with existing Pigweed code, the logging backend
respects ``PW_LOG_LEVEL``. If set, the backend will translate the Pigweed log
levels to their closest Zephyr counterparts:

+---------------------------+-------------------+
| Pigweed                   | Zephyr            |
+===========================+===================+
| ``PW_LOG_LEVEL_DEBUG``    | ``LOG_LEVEL_DBG`` |
+---------------------------+-------------------+
| ``PW_LOG_LEVEL_INFO``     | ``LOG_LEVEL_INF`` |
+---------------------------+-------------------+
| ``PW_LOG_LEVEL_WARN``     | ``LOG_LEVEL_WRN`` |
+---------------------------+-------------------+
| ``PW_LOG_LEVEL_ERROR``    | ``LOG_LEVEL_ERR`` |
|                           |                   |
| ``PW_LOG_LEVEL_CRITICAL`` |                   |
|                           |                   |
| ``PW_LOG_LEVEL_FATAL``    |                   |
+---------------------------+-------------------+

Alternatively, it is also possible to set the Zephyr logging level directly via
``CONFIG_PIGWEED_LOG_LEVEL``.

.. _`Kconfig options`: https://docs.zephyrproject.org/latest/reference/logging/index.html#global-kconfig-options
