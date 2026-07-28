.. _docs-security-threat_model:

====================
Agentic threat model
====================
This document contains a general threat model that applies globally to Pigweed.

Individual modules may provide a supplementary ``security.rst`` in their
respective directories to detail module-specific assets, threat vectors, and
safety assumptions.

This threat model and the supplementary per-module threat models are intended
to be consumed by agents. As a result, this document has many contextual
details and direct instructions.

-----------------------
Environment and context
-----------------------
Pigweed includes libraries, called “modules”, which are prefixed by ``pw_``.
These libraries are used in MCU environments that typically:

* Lack an MMU, and have deterministic or nearly-deterministic memory layouts.
* Operate with few or no trust levels. Some may use MPUs to distinguish between
  privileged and unprivileged code, while others may treat all code as equally
  privileged.
* Persist any state or logs in the clear without encryption.
* Interact with peripherals via MMIO registers and occasionally DMA.
* Do not have any notion of internal security principals, but may authenticate
  connections to other devices.

Some modules are “facades” which specify behavior to be implemented by “backend”
modules, many of which are platform-specific. Backend module names are prefixed
by their facade name, e.g. “pw_uart_mcuxpresso” implements “pw_uart”.

Platform-specific backend modules often integrate with an RTOS that provides:

* MPU managements to enforce privileges, limit DMA by devices, etc.
* Thread creation, scheduling, and reaping.
* ISR context switching

The actual devices and their complete software images are assembled by
downstream consumers of Pigweed. Pigweed also contains build environments and
developer tooling that are used by downstream consumers but that are not
included on downstream devices.

Target Deployment and Safety
============================
Pigweed is intended for consumer electronics and other non-safety-critical
applications. It has **not** undergone evaluation or certification for
safety-critical or high-threat applications, and makes no claims or guarantees
in these environments.

While downstream product teams may consider physical tampering or bus sniffing
in-scope for their specific products, these hardware-level attacks are
out-of-scope for the Pigweed project itself.

MPU and Privilege Assumptions
=============================
Pigweed does not assume an MPU is always present and supports configurations
without one. The final security posture depends on the integration decisions
made by the downstream product team. For threat evaluation purposes:

* **MPU Bypass:** For cases where a vulnerability is claimed to bypass MPU
  protections, the MPU is assumed to be **present** and active.
* **MPU Mitigation:** For cases where an MPU would be required to mitigate a
  vulnerability, the MPU is assumed to be **absent** (ensuring Pigweed remains
  resilient by default).
* MPUs are explicitly supported by the RTOS backends (e.g., ``_embos``,
  ``_freertos``, ``_threadx``, ``_zephyr``).

---------------
Attack surfaces
---------------
The primary trust boundary on a Pigweed device is between the MCU and other
hardware devices or components. The most sensitive attack surfaces are any
sections of code that handle untrusted input in the form of messages between
devices and components, including:

* Bluetooth communications
* RPC messages
* SPI, UART, and DigitalIO data

Another very important trust boundary is between privileged and unprivileged
modes of execution when an MPU is present. The data flows across this boundary
may be calls to RTOS or specific hardware events such as interrupts.

The boundary between Pigweed code and other device code is important. It is not
a true trust boundary as both sides represent the same level of privilege, but
it provides an opportunity for Pigweed's APIs to be safe by default. This
attack surfaces requires an attacker to have first gained some control over
other device code. Since Pigweed cannot ensure its consumers' code is free of
vulnerabilities, it strives to be resilient against any valid call or
sequence of calls. Devices interact with Pigweed code in multiple ways,
including:

* By direct calls to a module's public API, which can usually be found in the
  public functions and public and protected methods of types defined in header
  files somewhere under a "public/" directory.
* Via previously registered callbacks or handlers responding to hardware
  events, timers, interrupts, etc.

Finally Pigweed is concerned with keeping developers safe in addition to users.
The development tools provided by Pigweed and used for writing, compiling, and
debugging device code are also a valid attack surface.

------
Assets
------
An attacker may value any of the following objectives on a Pigweed device:

* Gaining code execution: This allows an attacker to achieve any other goal.
* User data: This includes secrets like tokens, credentials, or keys, as well
  as sensitive data like identifiers, location data, microphone data, etc.
* Cryptographic materials: This includes private/secret keys, key-derivation
  states, and cryptographically secure random seeds.
* Integrity of firmware: The ability to run unsigned or unauthorized firmware.
* Access to other devices or services: This may allow an attacker to move
  laterally through a user's devices.
* Log suppression: This may allow an attacker to exploit other vulnerabilities
  without being detected.
* MPU bypass: This may allow an attacker to escalate into a more privileged mode
  on some RTOSes, e.g. FreeRTOS.
* Remote denial of service: An attacker may wish to render all nearby devices
  inoperable.
* Cross-connection leaks: There is rarely any “authentication” in terms of
  security principals, but such leaks may facilitate lateral movement.
* Fine-grained control of memory allocations: While not an exploit in and of
  itself, the ability to "groom" the heap may make other vulnerabilities such
  as use-after-frees or buffer overflows more severe.

-----
Scope
-----
When examining potential software defects, include the following in the set of
issues considered to be vulnerabilities:

* Include arbitrary memory writes (write-what-where, or W3) as more severe. Due
  to the lack of fine-grain privileges or virtual memory, arbitrary memory
  writes very often provide arbitrary code execution (CodeExec).
* Include use-after-free (UAF) vulnerabilities as severe, especially if the
  type in question contains vtables or function pointers.

  * Memory allocation may be moderately to highly deterministic.
  * If an attacker can control allocations from the same allocator, consider
    this more severe as they can likely groom the heap.

* Include any buffer overflows (BO) of the stack, of statically-allocated
  data, or of dynamically-allocated data.
* Include any out-of-bounds reads (OOBR) or writes (OOBW).
* Include any integer overflows (IOF) or underflows (IUF), especially if the
  results are used to calculate indices, offsets, or sizes.
* Include any unauthenticated, remote means of exfiltrating data.
* Include crashes and other denials of service (DoS) but only if they result
  from data sent by an attacker to a device.
* Include any data races (Race) or time-of-check-time-of-use (TOCTOU) bugs in
  code that includes synchronization (i.e. locks) or is documented as being
  thread-safe or ISR-safe.
* Include any accesses of uninitialized memory (UMA).
* Include type conversion errors (TCE) if the resulting value is something an
  attacker could not have otherwise set.
* Include type confusion errors (Type).
* Include cryptographic failures in modules providing cryptographic, update, or
  verification functionality (e.g., bypasses of signature checks,
  predictability in random number generators).
* Include vulnerabilities in examples, as those are often copied and pasted.

----------
Exclusions
----------
When examining potential software defects, exclude the following in the set of
issues considered to be vulnerabilities:

* Do not include them if they require other software on the device to
  deliberately misuse the module’s interface according to the module’s
  documentation or comments.

  * For example, do not include data races or TOCTOU bugs in code documented
    as being single-threaded.
  * However, API design flaws that make the safe path highly error-prone or
    introduce implicit safety risks under standard usage may still be
    evaluated as design vulnerabilities.

* Do not include attacks that require physical presence to probe hardware
  buses, extract keys via physical side-channels (e.g., power analysis), or
  physically tamper with internal silicon.

  * However, software-level bypasses of integrity checks (such as signature
    verification bypasses in update or boot modules) are **fully in-scope**,
    regardless of how the payload is delivered to flash. Detailed
    module-specific threat models are located in their respective module
    directories.

* Do not include vulnerabilities in host-side developer tools that require
  inputs from a maliciously programmed target device. Host-side tools are
  assumed to only be used to develop and debug devices fully under the control
  of a developer with physical access, and the connected devices are assumed
  to be non-malicious.
* Do not include any partial, temporary DoS where the system can fail and
  recover gracefully.

  * For example, do not include a DoS caused by an attacker exhausting the
    memory used to handle messages if new messages can be handled again some
    time after the attacker stops flooding the device.

* Do not include any attacks which assume the attacker already has the ability
  to execute arbitrary code.
* Do not include vulnerabilities in code that is only reachable by calling
  functions or methods within an "internal" namespace or defined in header
  files within an "internal/" directory.
* Do not include any vulnerabilities in the "environment/" directory.
* Do not include any vulnerabilities in tests or size reports.
* Do not include flaws found in third-party dependencies; please report those
  directly to the respective upstream maintainers.
* Do not include simple compiler warnings or static analysis tool noise that do
  not result in a practical security risk.
