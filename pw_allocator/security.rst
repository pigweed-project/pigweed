.. _module-pw_allocator-security:

=========================
pw_allocator threat model
=========================
This document supplements the
:ref:`general threat model <docs-security-threat_model>` to provide a threat
model specific to pw_allocator.

-----------------------
Environment and context
-----------------------
* This module is used to manage dynamically allocated memory.
* Each memory allocation has a requested size, which is less than or equal
  to the usable size that may include padding, which is less than or equal to the
  allocated size that includes metadata.
* The ``pw_allocator`` module includes tunable assertions, which allows module
  consumers to trade between performance and code size against greater safety.

-----
Scope
-----
* Include vulnerabilities where an attacker is able to get a write-what-where
  (W3) primitive using one or more allocated pointers.

----------
Exclusions
----------
* Do not include denials of service (DoS) where an attacker induces a crash by
  deliberately misusing allocated memory.
* Do not include disabled assertions as “mitigation bypasses” since these are
  tunable.
