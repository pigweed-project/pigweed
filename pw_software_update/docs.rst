.. _module-pw_software_update:

.. rst-class:: with-subtitle

==================
pw_software_update
==================
.. pigweed-module::
   :name: pw_software_update

   The ``pw_software_update`` module offers the following building blocks for
   setting up your own end-to-end software delivery system.

   - **TUF embedded**: An underlying TUF_-based security framework tailored
     for embedded use cases that enable safe and resilient software delivery.
   - **One bundle**: A standard update bundle format for assembling all build
     artifacts and release information.
   - **Two keys**: Each product has two keys dedicated to software updates. The
     ``targets`` key directly signs a versioned manifest of target files and
     can be regularly rotated by the ``root`` key. The ``root`` keys are in
     turn rotated by verified boot. No provisioning is required.
   - **Frameworked client**: An update client that takes care of all the logic
     of checking, staging, verifying, and installing an incoming update. The
     framework calls into the downstream backend only when needed.
   - **Signing service**: Integration support for your favorite production
     signing service.
   - **Tooling**: Python modules that assemble, sign, and inspect bundles,
     ready to be integrated into your build and release pipeline. Plus a CLI
     with which you can try out ``pw_software_update`` before buying into it.
   - **Extensive guidance**: All software update systems are not equal. We
     are building out extensive guidance for representative scenarios.

-------------
Who is it for
-------------

The ``pw_software_update`` module is still in early stages. It works best if
your software update needs checks the following boxes.

✅ **I want security-by-design**!

The ``pw_software_update`` module is built with security in mind from
day 0. It leverages the state-of-the-art and widely used TUF_ framework.
With relatively little expertise, you can set up and operate a software
building, release, and delivery pipeline that is resiliently secure and
private.

✅ **My project has verified boot.**

Software update is an extension of verified boot. Security measures in
``pw_software_update`` CANNOT replace verified boot.

.. note::

   Verified boot, also known as secure boot, refers to the generic security
   feature that ensures no software component is run without passing
   integrity and authentication verification.  In particular, verified boot
   ensures the software update stack has not been tampered with.

✅ **My project DOES NOT require delta updates.**

``pw_software_update`` packages every new software release in a single opaque
bundle. The bundle is the smallest granularity transferred between endpoints.

✅ **I can manage signing keys myself.**

We don't yet have a public-facing signing service.

✅ **I can store and serve my own updates.**

We don't yet have a public-facing end-to-end software delivery solution.

If your project doesn't check all the boxes above but you still wish to use
``pw_software_update``. Please `email <https://groups.google.com/g/pigweed>`_
or `chat <https://discord.gg/M9NSeTA>`_ with us for potential workarounds.

.. _TUF: https://theupdateframework.io/

--------
Security
--------
See :ref:`module-pw_software_update-security` for the module-specific threat
model.

.. toctree::
   :hidden:
   :maxdepth: 1

   get_started
   design
   guides
   cli
   security
