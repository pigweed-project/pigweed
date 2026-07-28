.. _module-pw_boot-security:

====================
pw_boot threat model
====================
This document supplements the
:ref:`general threat model <docs-security-threat_model>` to provide a threat
model specific to pw_boot.

-----
Scope
-----
* Software-level bypasses of integrity checks (such as signature verification
  bypasses in the ``pw_boot`` module) are **fully in-scope**, regardless of how
  the payload is delivered to flash.
