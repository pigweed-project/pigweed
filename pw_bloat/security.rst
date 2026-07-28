.. _module-pw_bloat-security:

=====================
pw_bloat threat model
=====================
This document supplements the
:ref:`general threat model <docs-security-threat_model>` to provide a threat
model specific to pw_bloat.

-----------------------
Environment and context
-----------------------
* This module is used to generate size reports and is not included on
  downstream devices.

----------
Exclusions
----------
* Do not include vulnerabilities in this module, as we are only interested in
  issues that affect end users.
