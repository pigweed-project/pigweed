.. _module-pw_software_update-design:

------
Design
------
.. pigweed-module-subpage::
   :name: pw_software_update

This page explains the security framing, bundle format and update workflows of
``pw_software_update``.

Embedded TUF
------------

At the heart, the ``pw_software_update`` module leverages
`The Update Framework <https://theupdateframework.io/>`_ (TUF),
an industry-leading software update security framework that is open, flexible,
and offers a balanced security and privacy treatment.

The ``pw_software_update`` module implements the following building blocks
around TUF.

.. mermaid::

   flowchart TB
          A[/Source/] --> |Build| B[/Target files/]
          B --> |Assemble & Sign| C[(Update bundle)]
          C --> |Publish| D[(Available updates)]
          D --> |OTA| E[Device]

Update bundles
^^^^^^^^^^^^^^

Update bundles represent software releases packaged ready for delivery. A bundle
is essentially an archived folder matching the following structure:

.. code-block:: text

   /
   ├── root_metadata
   ├── targets_metadata
   └── targets
       ├── release_notes.txt
       ├── manifest.txt
       ├── rtos.bin
       └── app.bin

Bundles are encoded as serialized "protocol buffers".

Key structure
^^^^^^^^^^^^^

As an optimization and trade-off for embedded projects, ``pw_software_update``
only supports the "root" and "targets" roles, as represented by
``root_metadata`` and ``targets_metadata``.

.. mermaid::

   flowchart TB
          A[Verified boot] --> |Embed & Verify| B[/Root key/]
          B --> |Delegate & Rotate| C[/Targets key/]
          C --> |Sign| D[/Target files/]


The "root" role delegates the "targets" role to directly authorize each release.

The "root" role can regularly rotate the "targets" role, in effect revoking
older versions once a new release is available.

The "root" role is the "root of trust" for software update and tied into
verified boot. Due to security risks, ``pw_software_update`` does not use
persistent metadata caches that are not covered by verified boot.

Signing service
^^^^^^^^^^^^^^^
Production signing keys MUST be kept secure and clean. That means we must
carefully control access, log usage details, and revoke the key if it was
(accidentally) used to sign a "questionable" build.

This is easier with a signing server built around a key management service.

.. mermaid::

   sequenceDiagram
     actor Releaser

     Releaser->>Signer: Sign my bundle with my key, please.

     activate Signer

     Signer->>Signer: Check permission.
     Signer->>Signer: Validate & sign bundle.
     Signer->>Signer: Log action. Email alerts.
     Signer-->>Releaser: Done!
     deactivate Signer

We don't yet have a public-facing service. External users should source their
own solution.

Bundle verification
^^^^^^^^^^^^^^^^^^^

.. mermaid::

   flowchart LR
     A[(Incoming bundle)] --> |UpdateBundleAccessor| B[/Verified target files/]

The :cpp:type:`UpdateBundleAccessor` decodes, verifies, and exposes the target
files from an incoming bundle. This class hides the details of the bundle
format and verification flow from callers.

Update workflow
^^^^^^^^^^^^^^^

On the device side, :cpp:type:`BundledUpdateService` orchestrates an update
session end-to-end. It drives the backend via a :cpp:type:`BundledUpdateBackend`
interface.

:cpp:type:`BundledUpdateService` is invoked via :ref:`module-pw_rpc` after an
incoming bundle is staged via :ref:`module-pw_transfer`.

.. mermaid::

   stateDiagram-v2
   direction LR

   [*] --> Inactive

   Inactive --> Transferring: Start()
   Inactive --> Finished: Start() error

   Transferring --> Transferring: GetStatus()
   Transferring --> Transferred
   Transferring --> Aborting: Abort()
   Transferring --> Finished: Transfer error

   Transferred --> Transferred: GetStatus()
   Transferred --> Verifying: Verify()
   Transferred --> Verifying: Apply()
   Transferred --> Aborting: Abort()

   Verifying --> Verifying: GetStatus()
   Verifying --> Verified
   Verifying --> Aborting: Abort()

   Verified --> Verified: GetStatus()
   Verified --> Applying: Apply()
   Verified --> Aborting: Abort()

   Applying --> Applying: GetStatus()
   Applying --> Finished: Apply() OK
   Applying --> Finished: Apply() error

   Aborting --> Aborting: GetStatus()
   Aborting --> Finished: Abort() OK
   Aborting --> Finished: Abort() error

   Finished --> Finished: GetStatus()
   Finished --> Inactive: Reset()
   Finished --> Finished: Reset() error


Tooling
^^^^^^^

``pw_software_update`` provides the following tooling support for development
and integration.

The python package
~~~~~~~~~~~~~~~~~~
``pw_software_update`` comes with a python package of the same name, providing
the following functionalities.

- Local signing key generation for development.
- TUF root metadata generation and signing.
- Bundle generation, signing, and verification.
- Signing server integration.

A typical use of the package is for build system integration.

.. code-block:: text

   Help on package pw_software_update:

   NAME
          pw_software_update - pw_software_update

   PACKAGE CONTENTS
          bundled_update_pb2
          cli
          dev_sign
          generate_test_bundle
          keys
          metadata
          remote_sign
          root_metadata
          tuf_pb2
          update_bundle
          update_bundle_pb2
          verify


The command line utility
~~~~~~~~~~~~~~~~~~~~~~~~

The ``pw update ...`` CLI (Command Line Interface) is a user-friendly interface
to the ``pw_software_update`` python package.

You can use the CLI to quickly learn and prototype a software update system
based on ``pw_software_update`` on your development PC before productionizing
one. In the future you will be able to use the CLI to update a reference
target.

.. code-block:: text

   usage: pw update [sub-commands]

   sub-commands:

          generate-key
          create-root-metadata
          sign-root-metadata
          inspect-root-metadata
          create-empty-bundle
          add-root-metadata-to-bundle
          add-file-to-bundle
          sign-bundle
          inspect-bundle

   options:
          -h, --help            show this help message and exit


To learn more, see :ref:`module-pw_software_update-cli`.
