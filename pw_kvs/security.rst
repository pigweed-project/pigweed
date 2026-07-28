.. _module-pw_kvs-security:

===================
pw_kvs threat model
===================
This document supplements the
:ref:`general threat model <docs-security-threat_model>` to provide a threat
model specific to pw_kvs.

-----------------------
Environment and context
-----------------------
* ``pw_kvs`` is a key-value store (KVS) system intended for NOR flash. It
  supports writing arbitrary data entries indexed by a string key.
* It supports checksumming for data integrity purposes, but ``pw_kvs`` does not
  provide encryption or authentication.
* The key-value store is NOT a security boundary; i.e. it enforces no
  permissions on individual keys.

-----
Scope
-----
* Identify vulnerabilities that could corrupt or otherwise improperly affect
  the KVS contents through the public API.

----------
Exclusions
----------
* Do not include vulnerabilities using the public API whose effects could be
  achieved by different calls to the API, e.g. a ``Find`` call that deletes a
  key could also be achieved by simply calling ``Delete``.
* Do not include vulnerabilities that require direct flash access to change the
  value of entries, add new entries, or bypass checksumming. If an attacker has
  direct flash access, they already have full control over the KVS.
