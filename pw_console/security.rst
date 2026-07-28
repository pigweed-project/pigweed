.. _module-pw_console-security:

=======================
pw_console threat model
=======================
This document supplements the
:ref:`general threat model <docs-security-threat_model>` to provide a threat
model specific to pw_console.

----------
Exclusions
----------
* Do not include vulnerabilities in ``pw_console`` that require inputs from a
  maliciously programmed target device. Host-side tools are assumed to only be
  used to develop and debug devices fully under the control of a developer with
  physical access, and the connected devices are assumed to be non-malicious.
