.. _module-pw_build-security:

=====================
pw_build threat model
=====================
This document supplements the
:ref:`general threat model <docs-security-threat_model>` to provide a threat
model specific to pw_build.

-----------------------
Environment and context
-----------------------
* This module is used to build software and is not included on downstream
  devices.

----------
Exclusions
----------
* Do not include vulnerabilities in this module, as we are only interested in
  issues that affect end users.
