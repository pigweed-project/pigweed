.. _module-pw_interrupt_fake_backend:

-------------------------
pw_interrupt fake backend
-------------------------
The fake backend is meant for use in unit tests when run on the host.

The fake backend implementation can be found in
:cs:`pw_interrupt/fake_context.cc`, and also has a corresponding header
:cs:`pw_interrupt/fake_context.h` for use by the test.

With the fake backend, the ``InInterruptContext()`` call will by default return
false.

The fake backend function ``SetInInterruptContextCallback()`` can be used to
set a lambda function which allows the test to return an arbitrary value.

See the example unit test in :cs:`pw_interrupt/examples/context_usage_test.cc`.
