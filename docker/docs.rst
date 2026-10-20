.. docker is annotated with a module reference label because it is listed in
.. //PIGWEED_MODULES and the docs categorization sync logic expects every
.. module in //PIGWEED_MODULES to define a `module-<name>` target.

.. _module-docker:

------
Docker
------
``pw_env_setup`` reliably initializes a working environment for Pigweed, but
can take awhile to run. It intelligently caches where it can, but that caching
is designed around a particular execution environment. That environment
assumption is poor when running tests with Docker. To help out teams using
Docker for automated testing, the Pigweed team has a publicly-available Docker
image with a cache of some of the Pigweed environment. The current tag of this
image is in ``docker/tag`` under the Pigweed checkout.

We're still trying to improve this process, so if you have any ideas for
improvements please `create an issue`_.

.. _create an issue: https://issues.pigweed.dev/issues?q=status:open

Build your own Docker image
===========================

To build your own Docker image, start with ``docker/Dockerfile.cache`` and
run ``docker build --file docker/Dockerfile.cache .`` from the root of your
Pigweed checkout.

Use the publicly available Docker image
=======================================

To use the publicly-available Docker image, run
``docker build --file docker/Dockerfile.run --build-arg from=$(cat docker/tag) .``
from the root of your Pigweed checkout. You still need to run
``. ./bootstrap.sh`` within the Docker image, but it should complete much
faster than on a vanilla Docker image.
