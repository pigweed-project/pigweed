.. _module-pw_bluetooth_proxy-security:

===============================
pw_bluetooth_proxy threat model
===============================
This document supplements the
:ref:`general threat model <docs-security-threat_model>` to provide a threat
model specific to pw_bluetooth_proxy.

-----------------------
Environment and context
-----------------------
* Handles H4 and HCI Bluetooth (BT) packets containing data from the BT host
  on the user’s device or from other devices via the BT controller.
* Can be configured to intercept traffic coming from the BT controller to the
  BT host, in order to avoid waking the AP.
* Does not apply any security policies to traffic.

----------
Exclusions
----------
* Do not include issues which cause packets to be incorrectly forwarded to the
  host, rather than intercepted, as the proxy is not a security enforcement
  point.
* Do not include issues where the client’s event or receive callbacks might be
  called after the client frees memory used by the callbacks. Destroying the
  client channel object does not mean these callbacks won’t be called again.
* Do not include vulnerabilities that require destroying ``ProxyHost`` to
  cause a use-after-free (UAF).
