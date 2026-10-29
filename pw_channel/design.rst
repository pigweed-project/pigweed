.. _module-pw_channel-design:

======
Design
======
.. pigweed-module-subpage::
   :name: pw_channel

.. admonition:: ``pw_channel`` is deprecated
   :class: warning

   ``pw_channel`` is deprecated. Do not use it for new code. Use
   :cc:`pw::transport::Socket` or
   :cc:`pw::transport::ReliableSocket` in :ref:`module-pw_transport`
   instead.

   Note that :ref:`pw_async2 Channels <module-pw_async2-channels>` are not
   related to ``pw_channel``.

------------------
Channel attributes
------------------
Channels may be reliable, readable, or writable. A channel may be
substituted for another as long as it provides at least the same set of
capabilities; additional capabilities are okay. The channel's data type
(datagram or byte) implies different read/write semantics, so datagram/byte
channels cannot be used interchangeably in general.

For datagram channels, the exact bytes provided to a write call will appear in a
read call on the other end. A zero-byte datagram write results in a zero-byte
datagram read, so empty datagrams may convey information.

For byte channels, bytes written may be grouped differently when read. A
zero-length byte write is meaningless and will not result in a zero-length byte
read. If a zero-length byte read occurs, it is ignored.

.. _module-pw_channel-design-hourglass_inheritance_pattern:

-----------------------------
Hourglass inheritance pattern
-----------------------------
:cc:`pw::channel::Channel` uses an uncommon, hourglass-like inheritance
pattern. This pattern offers the advantages of multiple inheritance without the
downsides (overhead, potential for the diamond problem).

Empty base classes define the public interface with strongly typed capability
guarantees. A shared core class privately inherits from all of the empty bases.
This core class is virtual and stores common state variables. A series of
implementation classes inherit from the core class. These correspond with the
empty bases at the top of the hierarchy, expressing their capabilities in the
type system once again.

This pattern is hourglass-like because the hierarchy starts with several types
at the top, narrows to a single type in the middle, then expands out to the
implementation classes at the bottom.

Advantages of this pattern:

- Express capabilities in the type system, with support for optional
  capabilities.
- No multiple virtual inheritance. All supported functionality is in a single
  vtable.
- Trivial and safe conversions between related types. Any type can be used
  through a reference to a compatible type without indirection or memory
  aliasing.

The drawback of this pattern is implementation complexity. While the core
implementation is complicated, the resulting classes are straightforward to use
or extend.

.. mermaid::

   classDiagram
       class TypeA {
           +CommonFunctions()
           +FunctionForA()
       }

       class TypeB {
           +CommonFunctions()
           +FunctionForB()
       }

       class TypeAB {
           +CommonFunctions()
           +FunctionForA()
           +FunctionForB()
       }

       class Core {
           -common_state
           -capabilities
           +CommonFunctions()
           +MaybeFunctionForA()
           +MaybeFunctionForB()

           virtual -DoFunctionForA()
           virtual -DoFunctionForB()
       }

       class ImplementTypeA {
           +CommonFunctions()
           +FunctionForA()
       }

       class ImplementTypeB {
           +CommonFunctions()
           +FunctionForB()
       }

       class ImplementTypeAB {
           +CommonFunctions()
           +FunctionForA()
           +FunctionForB()
       }

       TypeA <|-- Core
       TypeB <|-- Core
       TypeAB <|-- Core


       Core <|-- ImplementTypeA
       Core <|-- ImplementTypeB
       Core <|-- ImplementTypeAB
