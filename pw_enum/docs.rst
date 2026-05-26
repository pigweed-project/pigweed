.. _module-pw_enum:

=======
pw_enum
=======
.. pigweed-module-subpage::
   :name: pw_enum

------------------
Stringifying enums
------------------
:cc:`pw::EnumToString` returns a string version of an enum. It uses a FTADLE
extension point ``PwEnumToString(enum)``. FTADLE is a pattern that enables
customization by searching for a matching function via Argument-Dependent Lookup
(ADL). For more information, see `Designing Extension Points With FTADLE
<https://abseil.io/tips/218>`_.

The :cc:`PW_TOKENIZE_ENUM` macro in :cs:`pw_tokenizer/enum.h
<pw_tokenizer/public/pw_tokenizer/enum.h>` implements ``PwEnumToString`` for
you.
