.. _module-pw_tokenizer:

============
pw_tokenizer
============
.. pigweed-module::
   :name: pw_tokenizer

Logging is critical, but developers are often forced to choose between
additional logging or saving crucial flash space. The ``pw_tokenizer`` module
enables **extensive logging with substantially less memory usage** by replacing
printf-style strings with binary tokens during compilation. It is designed to
integrate easily into existing logging systems.

Although the most common application of ``pw_tokenizer`` is binary logging,
**the tokenizer is general purpose and can be used to tokenize any strings**,
with or without printf-style arguments.

Why tokenize strings?

* **Dramatically reduce binary size** by removing string literals from binaries.
* **Reduce I/O traffic, RAM, and flash usage** by sending and storing compact tokens
  instead of strings. We've seen over 50% reduction in encoded log contents.
* **Reduce CPU usage** by replacing snprintf calls with simple tokenization code.
* **Remove potentially sensitive log, assert, and other strings** from binaries.

.. grid:: 1

   .. grid-item-card:: :octicon:`rocket` Get started
      :link: module-pw_tokenizer-get-started
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Integrate pw_tokenizer into your project.

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Tokenization
      :link: module-pw_tokenizer-tokenization
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Convert strings and arguments to tokens.

   .. grid-item-card:: :octicon:`code-square` Token databases
      :link: module-pw_tokenizer-token-databases
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Store a mapping of tokens to the strings and arguments they represent.

.. grid:: 2

   .. grid-item-card:: :octicon:`code-square` Detokenization
      :link: module-pw_tokenizer-detokenization
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Expand tokens back to the strings and arguments they represent.

   .. grid-item-card:: :octicon:`info` API reference
      :link: module-pw_tokenizer-api
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Detailed reference information about the pw_tokenizer API.

.. _module-pw_tokenizer-tokenized-logging-example:

---------------------------
Tokenized logging in action
---------------------------
Here's an example of how ``pw_tokenizer`` enables you to store
and send the same logging information using significantly less
resources:

.. raw:: html

   <style>
     #example > svg { height: 750px; }
   </style>

.. mermaid::
   :name: example

   %%{init: {
     'flowchart': {
       'subGraphTitleMargin': {'bottom': 15}
     }
   }}%%

   flowchart LR

     subgraph after["After: Tokenized Logs (37 bytes saved!)"]
       direction TB
       after_log["LOG(#quot;Battery Voltage: %d mV#quot;, voltage)"] -- 4 bytes stored on-device as… -->
       after_encoding["d9 28 47 8e"] -- 6 bytes sent over the wire as… -->
       after_transmission["d9 28 47 8e aa 3e"] -- Displayed in logs as… -->
       after_display["#quot;Battery Voltage: 3989 mV#quot;"]
     end

     subgraph before["Before: No Tokenization"]
       direction TB
       before_log["LOG(#quot;Battery Voltage: %d mV#quot;, voltage)"] -- 41 bytes stored on-device as… -->
       before_encoding["#quot;Battery Voltage: %d mV#quot;"] -- 43 bytes sent over the wire as… -->
       before_transmission["#quot;Battery Voltage: 3989 mV#quot;"] -- Displayed in logs as… -->
       before_display["#quot;Battery Voltage: 3989 mV#quot;"]
     end

     before ~~~ after

A quick overview of how the tokenized version works:

* You tokenize ``"Battery Voltage: %d mV"`` with a macro like
  :cc:`PW_TOKENIZE_STRING`. You can use :ref:`module-pw_log_tokenized`
  to handle the tokenization automatically.
* After tokenization, ``"Battery Voltage: %d mV"`` becomes ``d9 28 47 8e``.
* The first 4 bytes sent over the wire is the tokenized version of
  ``"Battery Voltage: %d mV"``. The last 2 bytes are the value of ``voltage``
  converted to a varint using :ref:`module-pw_varint`.
* The logs are converted back to the original, human-readable message
  via the :ref:`Detokenization API <module-pw_tokenizer-detokenization>`
  and a :ref:`token database <module-pw_tokenizer-token-databases>`.

.. toctree::
   :hidden:
   :maxdepth: 1

   Get started <get_started>
   tokenization
   token_databases
   detokenization
   api
