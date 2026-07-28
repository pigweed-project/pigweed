.. _docs-contributing-security:

================================
Contributing to Pigweed security
================================
Thank you for helping to keep Pigweed secure! We are serious about helping
Pigweed consumers keep their users safe, and appreciate the efforts of the
security community to find and report vulnerabilities responsibly.

-------------
How to report
-------------
**Please do not open pull requests for security vulnerabilities.**

To report a security issue, please use https://g.co/vulnz.

Pigweed uses Google's `Bug Hunters <https://g.co/vulnz>`_ program to handle
intake, coordination, and disclosure. Pigweed is an `OT2
<https://bughunters.google.com/about/rules/open-source/google-open-source-software-vulnerability-reward-program-rules#standard-oss-projects-ot2->`_
program. Please see the `OSS VRP rules
<https://bughunters.google.com/about/rules/open-source/google-open-source-software-vulnerability-reward-program-rules>`_
for more details.

The Google Security Team will respond within 5 working days of your report on
https://g.co/vulnz. If for any reason you don't receive a response, please
:ref:`connect with us <docs-contributing-help>`.

-----------------------------
Our security response process
-----------------------------
In general, the Bug Hunters team acts as an intermediary for external reporters
to ensure they are kept informed and their privacy maintained.

1. Once we receive a report from the Bug Hunters team, we will investigate,
   attempt to reproduce, and assign an initial severity.
2. We will coordinate with affected product teams and coordinate a public
   release date with them and you via Bug Hunters. This is typically within the
   Google standard target of 90 days.
3. Bug Hunters will ask your permission to credit you for the discovery in our
   disclosure.
4. On the public release date, we will publish the fix to our repository and a
   notice to our :ref:`docs-security-bulletins` page.

------------------------
Vulnerability disclosure
------------------------
Pigweed encourages its consumers to `live at HEAD
<https://abseil.io/about/philosophy#we-recommend-that-you-choose-to-live-at-head>`_.
Most defects will simply be fixed in the upstream repository without any special
notice, so staying up-to-date is the best way to get Pigweed fixes as soon as
they are available.

For critical and high severity vulnerabilities, Pigweed will publish a notice of
the vulnerability and fix on our :ref:`docs-security-bulletins` page. This
notice will include:

* The impact of exploiting the vulnerability.
* Technical details of the vulnerability.
* Credit to the finder, if externally reported and given permission.
* Additionally, for projects that cannot live at ``HEAD``:

  * Mitigations that may be applied by a consuming project, e.g. "ensure this
    parameter to this method is never more than 4096 bytes".
  * A link to the specific Git revisions that provide the fix and that you can
    use to create your own fix.
