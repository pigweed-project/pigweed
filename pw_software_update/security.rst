.. _module-pw_software_update-security:

===============================
pw_software_update threat model
===============================
This document supplements the
:ref:`general threat model <docs-security-threat_model>` to provide a threat
model specific to pw_software_update.

-----
Scope
-----
* Software-level bypasses of integrity checks (such as signature verification
  bypasses in the ``pw_software_update`` module) are **fully in-scope**,
  regardless of how the payload is delivered to flash.
