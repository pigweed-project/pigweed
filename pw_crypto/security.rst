.. _module-pw_crypto-security:

======================
pw_crypto threat model
======================
This document supplements the
:ref:`general threat model <docs-security-threat_model>` to provide a threat
model specific to pw_crypto.

-----
Scope
-----
* For ``pw_crypto`` (which acts as a thin wrapper), implementation bugs that
  *introduce* new side-channel attacks are in-scope, though side-channel
  resistance of the underlying cryptographic algorithms is delegated to the
  configured backend (e.g., Mbed TLS).
