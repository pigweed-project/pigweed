.. TODO: b/442224704 - Migrate the C/C++ API reference content to Doxygen

.. _module-pw_protobuf:

===========
pw_protobuf
===========
.. pigweed-module::
   :name: pw_protobuf

``pw_protobuf`` provides an expressive interface for encoding and decoding
the Protocol Buffer wire format with a lightweight code and data footprint.

--------
Overview
--------
Unlike protobuf libraries which require protobuf messages be represented by
in-memory data structures, ``pw_protobuf`` provides a progressive flexible API
that allows the user to choose the data storage format and tradeoffs most
suitable for their product on top of the implementation.

The API is designed in three layers, which can be freely intermixed with each
other in your code, depending on point of use requirements:

1. Message Structures,
2. Per-Field Writers and Readers,
3. Direct Writers and Readers.

This has a few benefits. The primary one is that it allows the core proto
serialization and deserialization libraries to be relatively small.

.. include:: size_report/protobuf_overview

To demonstrate these layers, we use the following protobuf message definition
in the examples:

.. code-block:: protobuf

   message Customer {
     enum Status {
       UNKNOWN = 0;
       NEW = 1;
       ACTIVE = 2;
       INACTIVE = 3;
     }
     int32 age = 1;
     string name = 2;
     Status status = 3;
   }

And the following accompanying options file:

.. code-block:: text

   Customer.name max_size:32

.. toctree::
   :maxdepth: 1

   size_report

Message Structures
==================
The highest level API is based around message structures created through C++
code generation, integrated with Pigweed's build system.

.. warning::

   Message structures only support a subset of protobuf field types. Before
   continuing, refer to :ref:`pw_protobuf-message-limitations` to understand
   what types of protobuf messages can and cannot be represented, and whether
   or not message structures are a suitable choice.

This results in the following generated structure:

.. code-block:: c++

   enum class Customer::Status : uint32_t {
     UNKNOWN = 0,
     NEW = 1,
     ACTIVE = 2,
     INACTIVE = 3,

     kUnknown = UNKNOWN,
     kNew = NEW,
     kActive = ACTIVE,
     kInactive = INACTIVE,
   };

   struct Customer::Message {
     int32_t age;
     pw::InlineString<32> name;
     Customer::Status status;
   };

Which can be encoded with the code:

.. code-block:: c++

   #include "example_protos/customer.pwpb.h"

   pw::Status EncodeCustomer(Customer::StreamEncoder& encoder) {
     return encoder.Write(
         {age = 33, name = "Joe Bloggs", status = Customer::Status::INACTIVE});
   }

And decoded into a struct with the code:

.. code-block:: c++

   #include "example_protos/customer.pwpb.h"

   pw::Status DecodeCustomer(Customer::StreamDecoder& decoder) {
     Customer::Message customer{};
     PW_TRY(decoder.Read(customer));
     // Read fields from customer
     return pw::OkStatus();
   }

These structures can be moved, copied, and compared with each other for
equality.

The encoder and decoder code is generic and implemented in the core C++ module.
A small overhead for each message type used in your code describes the structure
to the generic encoder and decoders.

Message comparison
------------------
Message structures implement ``operator==`` and ``operator!=`` for equality and
inequality comparisons. However, these can only work on trivial scalar fields
and fixed-size strings within the message. Fields using a callback are not
considered in the comparison.

To check if the equality operator of a generated message covers all fields,
``pw_protobuf`` provides an ``IsTriviallyComparable`` function.

.. code-block:: c++

   template <typename Message>
   constexpr bool IsTriviallyComparable<Message>();

For example, given the following protobuf definitions:

.. code-block:: protobuf

   message Point {
     int32 x = 1;
     int32 y = 2;
   }

   message Label {
     Point point = 1;
     string label = 2;
   }

And the accompanying options file:

.. code-block:: text

   Label.label use_callback:true

The ``Point`` message can be fully compared for equality, but ``Label`` cannot.
``Label`` still defines an ``operator==``, but it ignores the ``label`` string.

.. code-block:: c++

   Point::Message one = {.x = 5, .y = 11};
   Point::Message two = {.x = 5, .y = 11};

   static_assert(pw::protobuf::IsTriviallyComparable<Point::Message>());
   ASSERT_EQ(one, two);
   static_assert(!pw::protobuf::IsTriviallyComparable<Label::Message>());

Buffer Sizes
------------
Initializing a ``MemoryEncoder`` requires that you specify the size of the
buffer to encode to. The code generation includes constants which assist with
sizing your buffers, listed below:

- ``kMaxEncodedSizeBytes``: In messages where the size of every field is known,
  either due to them being scalar fields or having size/count limits specified
  through an options file, this constant defines the maximum size of the message
  if every field is set to its largest possible value.

  If the size of the message cannot be statically determined, typically due to
  callback fields within the message, this constant will not be generated.

- ``kMaxEncodedSizeBytesWithoutValues``: The maximum size of the encoded message
  excluding any dynamic variable-length fields such as those requiring
  callbacks. If the message contains only statically known field sizes, meeting
  the criteria to generate ``kMaxEncodedSizeBytes``, this value will be
  identical to it.

- Field-specific size constants (``k{FieldName}MaxSize``): String or bytes
  fields specifying a maximum or fixed size option define those sizes as
  constants.

In the below example, because the ``name`` field has a ``max_size`` specified
in the accompanying options file, ``kMaxEncodedSizeBytes`` includes the maximum
length of the value for that field.

.. code-block:: c++

   #include "example_protos/customer.pwpb.h"

   std::byte buffer[Customer::kMaxEncodedSizeBytes];
   Customer::MemoryEncoder encoder(buffer);
   const auto status = encoder.Write(
       {age = 22, name = "Wolfgang Bjornson", status = Customer::Status::ACTIVE});

   // Always check the encoder status or return values from Write calls.
   if (!status.ok()) {
     PW_LOG_INFO("Failed to encode proto; %s", encoder.status().str());
   }

Where the maximum length of a field value is not known, indicated by the
structure requiring a callback for that field, ``kMaxEncodedSizeBytes`` will
not be defined to indicate that additional computation is required from the
user.

For example if a ``bytes`` field length is not specified in the options file,
but is known to your code (``kMaxImageDataSize`` in this example being a
constant in your own code), you can simply add it to the generated
``kMaxEncodedSizeBytesWithoutValues`` constant:

.. code-block:: c++

   #include "example_protos/store.pwpb.h"

   const std::byte image_data[kMaxImageDataSize] = {/*...*/};

   Store::Message store{};
   // Calling SetEncoder means we must always extend the buffer size.
   store.image_data.SetEncoder([](Store::StreamEncoder& encoder) {
     return encoder.WriteImageData(image_data);
   });

   std::byte buffer[Store::kMaxEncodedSizeBytesWithoutValues + kMaxImageDataSize];
   Store::MemoryEncoder encoder(buffer);
   const auto status = encoder.Write(store);

   // Always check the encoder status or return values from Write calls.
   if (!status.ok()) {
     PW_LOG_INFO("Failed to encode proto; %s", encoder.status().str());
   }

Or when using a variable number of repeated submessages, where the maximum
number is known to your code but not to the proto, you can add the constants
from one message type to another:

.. code-block:: c++

   #include "example_protos/person.pwpb.h"

   Person::Message grandchild{};
   // Calling SetEncoder means we must always extend the buffer size.
   grandchild.grandparent.SetEncoder([](Person::StreamEncoder& encoder) {
     PW_TRY(encoder.GetGrandparentEncoder().Write(maternal_grandma));
     PW_TRY(encoder.GetGrandparentEncoder().Write(maternal_grandpa));
     PW_TRY(encoder.GetGrandparentEncoder().Write(paternal_grandma));
     PW_TRY(encoder.GetGrandparentEncoder().Write(paternal_grandpa));
     return pw::OkStatus();
   });

   std::byte buffer[Person::kMaxEncodedSizeBytesWithoutValues +
                    Grandparent::kMaxEncodedSizeBytes * 4];
   Person::MemoryEncoder encoder(buffer);
   const auto status = encoder.Write(grandchild);

   // Always check the encoder status or return values from Write calls.
   if (!status.ok()) {
     PW_LOG_INFO("Failed to encode proto; %s", encoder.status().str());
   }

.. warning::
  Encoding to a buffer that is insufficiently large will return
  ``Status::ResourceExhausted()`` from ``Write`` calls, and from the
  encoder's ``status()`` call. Always check the status of calls or the encoder,
  as in the case of error, the encoded data will be invalid.

.. _pw_protobuf-message-limitations:

Limitations
-----------
``pw_protobuf``'s message structure API is incomplete. Generally speaking, it is
reasonable to use for basic messages containing simple inline fields (scalar
types, bytes, and strings) and nested messages of similar form. Beyond this,
certain other types of protobuf specifiers can be used, but may be limited in
how and when they are supported. These cases are described below.

If an object representation of protobuf messages is desired, the Pigweed team
recommends using `Nanopb`_, which is fully supported within Pigweed's build and
RPC systems.

Message structures are eventually intended to be replaced with an alternative
object model. See `SEED-0103 <http://pwrev.dev/133971>`_ for additional
information about how message structures came to be and our future plans.

Protobuf versioning
^^^^^^^^^^^^^^^^^^^
Message structures are generated in accordance to ``proto3`` semantics only.
Unset fields default to their zero values. Explicit field presence is supported
through the use of the ``optional`` specifier. Proto2-only keywords such as
``required`` have no effect.

There is limited preliminary support for Protobuf editions, primarily to allow
editions-based proto files to compile. At this time, the only editions feature
supported by the code generator is ``field_presence``.

``oneof`` fields
^^^^^^^^^^^^^^^^
``oneof`` protobuf fields cannot be inlined within a message structure: they
must be encoded and decoded using callbacks.

``optional`` fields
^^^^^^^^^^^^^^^^^^^
Only scalar fields generate a ``std::optional`` wrapper in the resulting message
structure. Optional submessages or variable-length fields
(``string`` / ``bytes``) must be processed through callbacks, requiring manual
tracking of presence depending on whether or not the callback is invoked.

.. _pw_protobuf-per-field-apis:

Per-Field Writers and Readers
=============================
The middle level API is based around typed methods to write and read each
field of the message directly to the final serialized form, again created
through C++ code generation.

Encoding
--------
Given the same message structure, in addition to the ``Write()`` method that
accepts a message structure, the following additional methods are also
generated in the typed ``StreamEncoder`` class.

There are lightweight wrappers around the core implementation, calling the
underlying methods with the correct field numbers and value types, and result
in no additional binary code over correctly using the core implementation.

.. code-block:: c++

   class Customer::StreamEncoder : pw::protobuf::StreamEncoder {
    public:
     // Message Structure Writer.
     pw::Status Write(const Customer::Message&);

     // Per-Field Typed Writers.
     pw::Status WriteAge(int32_t);

     pw::Status WriteName(std::string_view);
     pw::Status WriteName(const char*, size_t);

     pw::Status WriteStatus(Customer::Status);
   };

So the same encoding method could be written as:

.. code-block:: c++

   #include "example_protos/customer.pwpb.h"

   Status EncodeCustomer(Customer::StreamEncoder& encoder) {
     PW_TRY(encoder.WriteAge(33));
     PW_TRY(encoder.WriteName("Joe Bloggs"sv));
     PW_TRY(encoder.WriteStatus(Customer::Status::INACTIVE));
   }

Pigweed's protobuf encoders encode directly to the wire format of a proto rather
than staging information to a mutable datastructure. This means any writes of a
value are final, and can't be referenced or modified as a later step in the
encode process.

Casting between generated StreamEncoder types
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
pw_protobuf guarantees that all generated ``StreamEncoder`` classes can be
converted among each other. It's also safe to convert any ``MemoryEncoder`` to
any other ``StreamEncoder``.

This guarantee exists to facilitate usage of protobuf overlays. Protobuf
overlays are protobuf message definitions that deliberately ensure that
fields defined in one message will not conflict with fields defined in other
messages.

For example:

.. code-block:: protobuf

   // The first half of the overlaid message.
   message BaseMessage {
     uint32 length = 1;
     reserved 2;  // Reserved for Overlay
   }

   // OK: The second half of the overlaid message.
   message Overlay {
     reserved 1;  // Reserved for BaseMessage
     uint32 height = 2;
   }

   // OK: A message that overlays and bundles both types together.
   message Both {
     uint32 length = 1;  // Defined independently by BaseMessage
     uint32 height = 2;  // Defined independently by Overlay
   }

   // BAD: Diverges from BaseMessage's definition, and can cause decode
   // errors/corruption.
   message InvalidOverlay {
     fixed32 length = 1;
   }

The ``StreamEncoderCast<>()`` helper template reduces very messy casting into
a much easier to read syntax:

.. code-block:: c++

   #include "pw_protobuf/encoder.h"
   #include "pw_protobuf_test_protos/full_test.pwpb.h"

   Result<ConstByteSpan> EncodeOverlaid(uint32_t height,
                                        uint32_t length,
                                        ConstByteSpan encode_buffer) {
     BaseMessage::MemoryEncoder base(encode_buffer);

     // Without StreamEncoderCast<>(), this line would be:
     //   Overlay::StreamEncoder& overlay =
     //       *static_cast<Overlay::StreamEncoder*>(
     //           static_cast<pw::protobuf::StreamEncoder*>(&base)
     Overlay::StreamEncoder& overlay =
         StreamEncoderCast<Overlay::StreamEncoder>(base);
     if (!overlay.WriteHeight(height).ok()) {
       return overlay.status();
     }
     if (!base.WriteLength(length).ok()) {
       return base.status();
     }
     return ConstByteSpan(base);
   }

While this use case is somewhat uncommon, it's a core supported use case of
pw_protobuf.

.. warning::

   Using this to convert one stream encoder to another when the messages
   themselves do not safely overlay will result in corrupt protos. Be careful
   when doing this as there's no compile-time way to detect whether or not two
   messages are meant to overlay.

Decoding
--------
For decoding, in addition to the ``Read()`` method that populates a message
structure, the following additional methods are also generated in the typed
``StreamDecoder`` class.

.. code-block:: c++

   class Customer::StreamDecoder : pw::protobuf::StreamDecoder {
    public:
     // Message Structure Reader.
     pw::Status Read(Customer::Message&);

     // Returns the identity of the current field.
     ::pw::Result<Fields> Field();

     // Per-Field Typed Readers.
     pw::Result<int32_t> ReadAge();

     pw::StatusWithSize ReadName(pw::span<char>);
     BytesReader GetNameReader();  // Read name as a stream of bytes.

     pw::Result<Customer::Status> ReadStatus();
   };

Complete and correct decoding requires looping through the fields, so is more
complex than encoding or using the message structure.

.. code-block:: c++

   pw::Status DecodeCustomer(Customer::StreamDecoder& decoder) {
     // Initialize variables to their default values as they will not be written
     // if they don't appear in the serialized message.
     uint32_t age = 0;
     char name[32] = "";
     Customer::Status status = Customer::Status::UNKNOWN;
     pw::Status decode_status;

     while ((decode_status = decoder.Next()).ok()) {
       switch (decoder.Field().value()) {
         case Customer::Fields::kAge: {
           PW_TRY_ASSIGN(age, decoder.ReadAge());
           break;
         }
         case Customer::Fields::kName: {
           PW_TRY(decoder.ReadName(name));
           break;
         }
         case Customer::Fields::kStatus: {
           PW_TRY_ASSIGN(status, decoder.ReadStatus());
           break;
         }
       }
     }

     return decode_status.IsOutOfRange() ? OkStatus() : decode_status;
   }

.. warning:: ``Fields::SNAKE_CASE`` is deprecated. Use ``Fields::kCamelCase``.

   Transitional support for ``Fields::SNAKE_CASE`` will soon only be available by
   explicitly setting the following GN variable in your project:
   ``pw_protobuf_compiler_GENERATE_LEGACY_ENUM_SNAKE_CASE_NAMES=true``

   This support will be removed after downstream projects have been migrated.

.. _module-pw_protobuf-read:

Reading a single field
----------------------
Sometimes, only a single field from a serialized message needs to be read. In
these cases, setting up a decoder and iterating through the message is a lot of
boilerplate. ``pw_protobuf`` generates convenient ``Find*()`` functions for
most fields in a message which handle this for you.

.. code-block:: c++

   pw::Status ReadCustomerData(pw::ConstByteSpan serialized_customer) {
     pw::Result<uint32_t> age = Customer::FindAge(serialized_customer);
     if (!age.ok()) {
       return age.status();
     }

     // This will scan the buffer again from the start, which is less efficient
     // than writing a custom decoder loop.
     pw::Result<std::string_view> name = Customer::FindName(serialized_customer);
     if (!age.ok()) {
       return age.status();
     }

     DoStuff(age, name);
     return pw::OkStatus();
   }

The ``Find`` APIs also work with streamed data, as shown below.

.. code-block:: c++

   pw::Status ReadCustomerData(pw::stream::Reader& customer_stream) {
     pw::Result<uint32_t> age = Customer::FindAge(customer_stream);
     if (!age.ok()) {
       return age.status();
     }

     // This will begin scanning for `name` from the current position of the
     // stream (following the `age` field). If `name` appeared before `age` in
     // the serialized data, it will not be found.
     //
     // Note that unlike with the buffer APIs, stream Find methods copy `string`
     // and `bytes` fields into a user-provided buffer.
     char name[32];
     pw::StatusWithSize sws = Customer::FindName(serialized_customer, name);
     if (!sws.ok()) {
       return sws.status();
     }
     if (sws.size() >= sizeof(name)) {
       return pw::Status::OutOfRange();
     }
     name[sws.size()] = '\0';

     DoStuff(age, name);
     return pw::OkStatus();
   }

``Find`` APIs can also be used to iterate over occurrences of a repeated field.
For example, given the protobuf definition:

.. code-block:: proto

   message Samples {
     repeated uint32 raw_values = 1;
   }

The ``raw_values`` field can be iterated over as follows:

.. code-block:: c++

   pw::Status ReadSamples(pw::ConstByteSpan serialized_samples) {
     pw::protobuf::Uint32Finder raw_values =
         Samples::FindRawValues(serialized_samples);
     for (Result<int32_t> val = finder.Next(); val.ok(); val = finder.Next()) {
       ConsumeValue(*val);
     }
   }

.. note::

   Each call to ``Find*()`` linearly scans through the message. If you have to
   read multiple fields, it is more efficient to instantiate your own decoder as
   described above.


Direct Writers and Readers
==========================
The lowest level API is provided by the core C++ implementation, and requires
the caller to provide the correct field number and value types for encoding, or
check the same when decoding.

Encoding
--------
The two fundamental classes are ``MemoryEncoder`` which directly encodes a proto
to an in-memory buffer, and ``StreamEncoder`` that operates on
``pw::stream::Writer`` objects to serialize proto data.

``StreamEncoder`` allows you encode a proto to something like ``pw::sys_io``
without needing to build the complete message in memory

To encode the same message we've used in the examples thus far, we would use
the following parts of the core API:

.. code-block:: c++

   class pw::protobuf::StreamEncoder {
    public:
     Status WriteInt32(uint32_t field_number, int32_t);
     Status WriteUint32(uint32_t field_number, uint32_t);

     Status WriteString(uint32_t field_number, std::string_view);
     Status WriteString(uint32_t field_number, const char*, size_t);

     // And many other methods, see pw_protobuf/encoder.h
   };

Encoding the same message requires that we specify the field numbers, which we
can hardcode, or supplement using the C++ code generated ``Fields`` enum, and
cast the enumerated type.

.. code-block:: c++

   #include "example_protos/customer.pwpb.h"
   #include "pw_protobuf/encoder.h"

   Status EncodeCustomer(pw::protobuf::StreamEncoder& encoder) {
     PW_TRY(encoder.WriteInt32(static_cast<uint32_t>(Customer::Fields::kAge), 33));
     PW_TRY(encoder.WriteString(static_cast<uint32_t>(Customer::Fields::kName),
                                "Joe Bloggs"sv));
     PW_TRY(
         encoder.WriteUint32(static_cast<uint32_t>(Customer::Fields::kStatus),
                             static_cast<uint32_t>(Customer::Status::INACTIVE)));
   }

Decoding
--------
``StreamDecoder`` reads data from a ``pw::stream::Reader`` and mirrors the API
of the encoders.

To decode the same message we would use the following parts of the core API:

.. code-block:: c++

   class pw::protobuf::StreamDecoder {
    public:
     // Returns the identity of the current field.
     ::pw::Result<uint32_t> FieldNumber();

     Result<int32_t> ReadInt32();
     Result<uint32_t> ReadUint32();

     StatusWithSize ReadString(pw::span<char>);

     // And many other methods, see pw_protobuf/stream_decoder.h
   };

As with the typed per-field API, complete and correct decoding requires looping
through the fields and checking the field numbers, along with casting types.

.. code-block:: c++

   pw::Status DecodeCustomer(pw::protobuf::StreamDecoder& decoder) {
     uint32_t age;
     char name[32];
     Customer::Status status;
     pw::Status decode_status;

     while ((decode_status = decoder.Next()).ok()) {
       switch (decoder.FieldNumber().value()) {
         case static_cast<uint32_t>(Customer::Fields::kAge): {
           PW_TRY_ASSIGN(age, decoder.ReadInt32());
           break;
         }
         case static_cast<uint32_t>(Customer::Fields::kName): {
           PW_TRY(decoder.ReadString(name));
           break;
         }
         case static_cast<uint32_t>(Customer::Fields::kStatus): {
           uint32_t status_value;
           PW_TRY_ASSIGN(status_value, decoder.ReadUint32());
           status = static_cast<Customer::Status>(status_value);
           break;
         }
       }
     }

     return decode_status.IsOutOfRange() ? OkStatus() : decode_status;
   }

Find APIs
---------
Sometimes, only a single field from a serialized message needs to be read. In
these cases, setting up a decoder and iterating through the message is a lot of
boilerplate. ``pw_protobuf`` provides convenient :cc:`Find APIs
<pw_protobuf_find>` which handle this for you.

.. note::

   Each call to ``Find*()`` linearly scans through the message. If you
   have to read multiple fields, it is more efficient to instantiate your own
   decoder as described above.

.. code-block:: cpp

   pw::Status PrintCustomerAge(pw::ConstByteSpan serialized_customer) {
     pw::Result<uint32_t> age =
         pw::protobuf::FindUint32(serialized_customer, Customer::Fields::kAge);
     if (!age.ok()) {
       return age.status();
     }

     PW_LOG_INFO("Customer's age is %u", *age);
     return pw::OkStatus();
   }

Handling of packages
====================

Package declarations in ``.proto`` files are converted to namespace
declarations. Unlike ``protoc``'s native C++ codegen, pw_protobuf appends an
additional ``::pwpb`` namespace after the user-specified package name: for
example, ``package my.cool.project`` becomes ``namespace
my::cool::project::pwpb``. We emit a different package name than stated, in
order to avoid clashes for projects that link against multiple C++ proto
libraries in the same library.

..
  TODO: b/258832150 - Remove this section, if possible

In some cases, pw_protobuf codegen may encounter external message references
during parsing, where it is unable to resolve the package name of the message.
In these situations, the codegen is instead forced to emit the package name as
``pw::pwpb_xxx::my::cool::project``, where "pwpb_xxx" is the name of some
unspecified private namespace. Users are expected to manually identify the
intended namespace name of that symbol, as described above, and must not rely
on any such private namespaces, even if they appear in codegen output.

-------
Codegen
-------
pw_protobuf codegen integration is supported in GN, Bazel, and CMake.

``pw_protobuf`` only supports targeting ``proto3`` syntax and its semantics.
Files which specify ``syntax = "proto2"`` will be treated as proto3, and any
proto2-specific options will be ignored.

.. admonition:: Protobuf edition support

   `Protobuf editions <https://protobuf.dev/editions/overview>`_ are the future
   of Protobuf versioning, replacing proto2 and proto3 with more granular code
   generation options. ``pw_protobuf`` has some basic support for editions, but
   it is still a work in progress.

This module's codegen is available through the ``*.pwpb`` sub-target of a
``pw_proto_library`` in GN, CMake, and Bazel. See :ref:`pw_protobuf_compiler's
documentation <module-pw_protobuf_compiler>` for more information on build
system integration for pw_protobuf codegen.

Example ``BUILD.gn``:

.. code-block::

   import("//build_overrides/pigweed.gni")

   import("$dir_pw_build/target_types.gni")
   import("$dir_pw_protobuf_compiler/proto.gni")

   # This target controls where the *.pwpb.h headers end up on the include path.
   # In this example, it's at "pet_daycare_protos/client.pwpb.h".
   pw_proto_library("pet_daycare_protos") {
     sources = [
       "pet_daycare_protos/client.proto",
     ]
   }

   pw_source_set("example_client") {
     sources = [ "example_client.cc" ]
     deps = [
       ":pet_daycare_protos.pwpb",
       dir_pw_bytes,
       dir_pw_stream,
     ]
   }

-------------
Configuration
-------------
``pw_protobuf`` supports the following configuration options.

* ``PW_PROTOBUF_CFG_MAX_VARINT_SIZE``:
  When encoding nested messages, the number of bytes to reserve for the varint
  submessage length. Nested messages are limited in size to the maximum value
  that can be varint-encoded into this reserved space.

  The values that can be set, and their corresponding maximum submessage
  lengths, are outlined below.

  +-------------------+----------------------------------------+
  | MAX_VARINT_SIZE   | Maximum submessage length              |
  +===================+========================================+
  | 1 byte            | 127                                    |
  +-------------------+----------------------------------------+
  | 2 bytes           | 16,383 or < 16KiB                      |
  +-------------------+----------------------------------------+
  | 3 bytes           | 2,097,151 or < 2048KiB                 |
  +-------------------+----------------------------------------+
  | 4 bytes (default) | 268,435,455 or < 256MiB                |
  +-------------------+----------------------------------------+
  | 5 bytes           | 4,294,967,295 or < 4GiB (max uint32_t) |
  +-------------------+----------------------------------------+

Field Options
=============
``pw_protobuf`` supports the following field options for specifying
protocol-level limitations, rather than code generation parameters (although
they do influence code generation):


* ``max_count``:
  Maximum number of entries for repeated fields.

* ``max_size``:
  Maximum size of `bytes` or `string` fields.

Even though other proto codegen implementations do not respect these field
options, they can still compile protos which use these options. This is
especially useful for host builds using upstream protoc code generation, where
host software can use the reflection API to query for the options and validate
messages comply with the specified limitations.

.. code-block:: text

   import "pw_protobuf_protos/field_options.proto";

   message Demo {
     string size_limited_string = 1 [(pw.protobuf.pwpb).max_size = 16];
   };

Options Files
=============
Code generation can be configured using a separate ``.pwpb_options`` file placed
alongside the relevant ``.proto`` file.

The format of this file is a series of fully qualified field names, or patterns,
followed by one or more options. Lines starting with ``#`` or ``//`` are
comments, and blank lines are ignored.

Example:

.. code-block::

   // Set an option for a specific field.
   fuzzy_friends.Client.visit_dates max_count:16

   // Set options for multiple fields by wildcard matching.
   fuzzy_friends.Pet.* max_size:32

   // Set multiple options in one go.
   fuzzy_friends.Dog.paws max_count:4 fixed_count:true

Options files should be listed as ``inputs`` when defining ``pw_proto_library``,
e.g.

.. code-block::

   pw_proto_library("pet_daycare_protos") {
     sources = [
       "pet_daycare_protos/client.proto",
     ]
     inputs = [
       "pet_daycare_protos/client.pwpb_options",
     ]
   }

Valid options are:

* ``max_count``:
  Maximum number of entries for repeated fields. When set, repeated scalar
  fields will use the ``pw::Vector`` container type instead of a callback.

* ``fixed_count``:
  Specified with ``max_count`` to use a fixed length ``std::array`` container
  instead of ``pw::Vector``.

* ``max_size``:
  Maximum size of `bytes` or `string` fields. When set, `bytes` fields use
  ``pw::Vector`` and `string` fields use ``pw::InlineString`` instead of a
  callback.

* ``fixed_size``:
  Specified with ``max_size`` to use a fixed length ``std::array`` container
  instead of ``pw::Vector`` for `bytes` fields.

* ``use_callback``:
  Replaces the structure member for the field with a callback function even
  where a simpler type could be used. This can be useful to ignore fields, to
  stop decoding of complex structures if certain values are not as expected, or
  to provide special handling for nested messages.

.. admonition:: Rationale

  The choice of a separate options file, over embedding options within the proto
  file, are driven by the need for proto files to be shared across multiple
  contexts.

  A typical product would require the same proto be used on a hardware
  component, running Pigweed; a server-side component, running on a cloud
  platform; and an app component, running on a Phone OS.

  While related, each of these will likely have different source projects and
  build systems.

  Were the Pigweed options embedded in the protos, it would be necessary for
  both the cloud platform and Phone OS to be able to ``"import pigweed"`` ---
  and equivalently for options relevant to their platforms in the embedded
  software project.

------------------
Message Structures
------------------
The C++ code generator creates a ``struct Message`` for each protobuf message
that can hold the set of values encoded by it, following these rules.

* Scalar fields are represented by their appropriate C++ type.

  .. code-block:: protobuf

     message Customer {
       int32 age = 1;
       uint32 birth_year = 2;
       sint64 rating = 3;
       bool is_active = 4;
     }

  .. code-block:: c++

     struct Customer::Message {
       int32_t age;
       uint32_t birth_year;
       int64_t rating;
       bool is_active;
     };

* Enumerations are represented by a code generated namespaced proto enum.

  .. code-block:: protobuf

     message Award {
       enum Service {
         BRONZE = 1;
         SILVER = 2;
         GOLD = 3;
       }
       Service service = 1;
     }

  .. code-block:: c++

     enum class Award::Service : uint32_t {
       BRONZE = 1,
       SILVER = 2,
       GOLD = 3,

       kBronze = BRONZE,
       kSilver = SILVER,
       kGold = GOLD,
     };

     struct Award::Message {
       Award::Service service;
     };

  Aliases to the enum values are also included in the "constant" style to match
  your preferred coding style. These aliases have any common prefix to the
  enumeration values removed, such that:

  .. code-block:: protobuf

     enum Activity {
       ACTIVITY_CYCLING = 1;
       ACTIVITY_RUNNING = 2;
       ACTIVITY_SWIMMING = 3;
     }

  .. code-block:: c++

     enum class Activity : uint32_t {
       ACTIVITY_CYCLING = 1,
       ACTIVITY_RUNNING = 2,
       ACTIVITY_SWIMMING = 3,

       kCycling = ACTIVITY_CYCLING,
       kRunning = ACTIVITY_RUNNING,
       kSwimming = ACTIVITY_SWIMMING,
     };


* Nested messages are represented by their own ``struct Message`` provided that
  a reference cycle does not exist.

  .. code-block:: protobuf

     message Sale {
       Customer customer = 1;
       Product product = 2;
     }

  .. code-block:: c++

     struct Sale::Message {
       Customer::Message customer;
       Product::Message product;
     };

* Optional scalar fields are represented by the appropriate C++ type wrapped in
  ``std::optional``. Optional fields are not encoded when the value is not
  present.

  .. code-block:: protobuf

     message Loyalty {
       optional int32 points = 1;
     }

  .. code-block:: c++

     struct Loyalty::Message {
       std::optional<int32_t> points;
     };

* Repeated scalar fields are represented by ``pw::Vector`` when the
  ``max_count`` option is set for that field, or by ``std::array`` when both
  ``max_count`` and ``fixed_count:true`` are set.

  The max count is exposed as an UpperCamelCase constant ``k{FieldName}MaxSize``.

  .. code-block:: protobuf

     message Register {
       repeated int32 cash_in = 1;
       repeated int32 cash_out = 2;
     }

  .. code-block:: text

     Register.cash_in max_count:32 fixed_count:true
     Register.cash_out max_count:64

  .. code-block:: c++

     namespace Register {
     inline constexpr size_t kCashInMaxSize = 32;
     inline constexpr size_t kCashOutMaxSize = 64;
     }  // namespace Register

     struct Register::Message {
       std::array<int32_t, kCashInMaxSize> cash_in;
       pw::Vector<int32_t, kCashOutMaxSize> cash_out;
     };

* `bytes` fields are represented by ``pw::Vector`` when the ``max_size`` option
  is set for that field, or by ``std::array`` when both ``max_size`` and
  ``fixed_size:true`` are set.

  The max size is exposed as an UpperCamelCase constant ``k{FieldName}MaxSize``.

  .. code-block:: protobuf

     message Product {
       bytes sku = 1;
       bytes serial_number = 2;
     }

  .. code-block:: text

     Product.sku max_size:8 fixed_size:true
     Product.serial_number max_size:64

  .. code-block:: c++

     namespace Product {
     inline constexpr size_t kSkuMaxSize = 8;
     inline constexpr size_t kSerialNumberMaxSize = 64;
     }  // namespace Product

     struct Product::Message {
       std::array<std::byte, kSkuMaxSize> sku;
       pw::Vector<std::byte, kSerialNumberMaxSize> serial_number;
     };

* `string` fields are represented by a :cpp:type:`pw::InlineString` when the
  ``max_size`` option is set for that field. The string can hold up to
  ``max_size`` characters, and is always null terminated. The null terminator is
  not counted in ``max_size``.

  The max size is exposed as an UpperCamelCase constant ``k{FieldName}MaxSize``.

  .. code-block:: protobuf

     message Employee {
       string name = 1;
     }

  .. code-block:: text

     Employee.name max_size:128

  .. code-block:: c++

     namespace Employee {
     inline constexpr size_t kNameMaxSize = 128;
     }

     struct Employee::Message {
       pw::InlineString<kNameMaxSize> name;
     };

* Nested messages with a dependency cycle, repeated scalar fields without a
  ``max_count`` option set, `bytes` and `strings` fields without a ``max_size``
  option set, and repeated nested messages, repeated `bytes`, and repeated
  `strings` fields, are represented by a callback.

  You set the callback to a custom function for encoding or decoding
  before passing the structure to ``Write()`` or ``Read()`` appropriately.

  .. code-block:: protobuf

     message Store {
       Store nearest_store = 1;
       repeated int32 employee_numbers = 2;
       string directions = 3;
       repeated string address = 4;
       repeated Employee employees = 5;
     }

  .. code-block::

     // No options set.

  .. code-block:: c++

     struct Store::Message {
       pw::protobuf::Callback<Store::StreamEncoder, Store::StreamDecoder>
           nearest_store;
       pw::protobuf::Callback<Store::StreamEncoder, Store::StreamDecoder>
           employee_numbers;
       pw::protobuf::Callback<Store::StreamEncoder, Store::StreamDecoder> directions;
       pw::protobuf::Callback<Store::StreamEncoder, Store::StreamDecoder> address;
       pw::protobuf::Callback<Store::StreamEncoder, Store::StreamDecoder> employees;
     };

  A Callback object can be converted to a ``bool`` indicating whether a callback
  is set.

* Fields defined within a ``oneof`` group are represented by a ``OneOf``
  callback.

  .. code-block:: protobuf

     message OnlineOrder {
       Product product = 1;
       Customer customer = 2;

       oneof delivery {
         Address shipping_address = 3;
         Date pickup_date = 4;
       }
     }

  .. code-block::

     // No options set.

  .. code-block:: c++

     struct OnlineOrder::Message {
       Product::Message product;
       Customer::Message customer;
       pw::protobuf::OneOf<OnlineOrder::StreamEncoder,
                           OnlineOrder::StreamDecoder,
                           OnlineOrder::Fields>
           delivery;
     };

  Encoding a ``oneof`` field is identical to using a regular field callback.
  The encode callback will be invoked once when the message is written. Users
  must ensure that only a single field is written to the encoder within the
  callback.

  .. code-block:: c++

     OnlineOrder::Message message;
     message.delivery.SetEncoder(
         [&pickup_date](OnlineOrder::StreamEncoder& encoder) {
           return encoder.GetPickupDateEncoder().Write(pickup_date);
         });

  The ``OneOf`` decoder callback is invoked when reading a message structure
  when a field within the ``oneof`` group is encountered. The found field is
  passed to the callback.

  If multiple fields from the ``oneof`` group are encountered within a ``Read``,
  it will fail with a ``DATA_LOSS`` status.

  .. code-block:: c++

     OnlineOrder::Message message;
     message.delivery.SetDecoder([this](OnlineOrder::Fields field,
                                        OnlineOrder::StreamDecoder& decoder) {
       switch (field) {
         case OnlineOrder::Fields::kShippingAddress:
           PW_TRY(decoder.GetShippingAddressDecoder().Read(&this->shipping_address));
           break;
         case OnlineOrder::Fields::kPickupDate:
           PW_TRY(decoder.GetPickupDateDecoder().Read(&this->pickup_date));
           break;
         default:
           return pw::Status::DataLoss();
       }

       return pw::OkStatus();
     });

Message structures can be copied, but doing so will clear any assigned
callbacks. To preserve functions applied to callbacks, ensure that the message
structure is moved.

Message structures can also be compared with each other for equality. This
includes all repeated and nested fields represented by value types, but does not
compare any field represented by a callback.

Reserved-Word Conflicts
=======================
Generated symbols whose names conflict with reserved C++ keywords or
standard-library macros are suffixed with underscores to avoid compilation
failures. This can be seen below in ``Channel.operator``, which is mapped to
``Channel::Message::operator_`` to avoid conflicting with the ``operator``
keyword.

.. code-block:: protobuf

   message Channel {
     int32 bitrate = 1;
     float signal_to_noise_ratio = 2;
     Company operator = 3;
   }

.. code-block:: c++

   struct Channel::Message {
     int32_t bitrate;
     float signal_to_noise_ratio;
     Company::Message operator_;
   };

Similarly, as shown in the example below, some POSIX-signal names conflict with
macros defined by the standard-library header ``<csignal>`` and therefore
require underscore suffixes in the generated code. Note, however, that some
signal names are left alone. This is because ``<csignal>`` only defines a subset
of the POSIX signals as macros; the rest are perfectly valid identifiers that
won't cause any problems unless the user defines custom macros for them. Any
naming conflicts caused by user-defined macros are the user's responsibility
(https://google.github.io/styleguide/cppguide.html#Preprocessor_Macros).

.. code-block:: protobuf

   enum PosixSignal {
     NONE = 0;
     SIGHUP = 1;
     SIGINT = 2;
     SIGQUIT = 3;
     SIGILL = 4;
     SIGTRAP = 5;
     SIGABRT = 6;
     SIGFPE = 8;
     SIGKILL = 9;
     SIGSEGV = 11;
     SIGPIPE = 13;
     SIGALRM = 14;
     SIGTERM = 15;
   }

.. code-block:: c++

   enum class PosixSignal : uint32_t {
     NONE = 0,
     SIGHUP = 1,
     SIGINT_ = 2,
     SIGQUIT = 3,
     SIGILL_ = 4,
     SIGTRAP = 5,
     SIGABRT_ = 6,
     SIGFPE_ = 8,
     SIGKILL = 9,
     SIGSEGV_ = 11,
     SIGPIPE = 13,
     SIGALRM = 14,
     SIGTERM_ = 15,

     kNone = NONE,
     kSighup = SIGHUP,
     kSigint = SIGINT_,
     kSigquit = SIGQUIT,
     kSigill = SIGILL_,
     kSigtrap = SIGTRAP,
     kSigabrt = SIGABRT_,
     kSigfpe = SIGFPE_,
     kSigkill = SIGKILL,
     kSigsegv = SIGSEGV_,
     kSigpipe = SIGPIPE,
     kSigalrm = SIGALRM,
     kSigterm = SIGTERM_,
   };

Much like reserved words and macros, the names ``Message`` and ``Fields`` are
suffixed with underscores in generated C++ code. This is to prevent name
conflicts with the codegen internals if they're used in a nested context as in
the example below.

.. code-block:: protobuf

   message Function {
     message Message {
       string content = 1;
     }

     enum Fields {
       NONE = 0;
       COMPLEX_NUMBERS = 1;
       INTEGERS_MOD_5 = 2;
       MEROMORPHIC_FUNCTIONS_ON_COMPLEX_PLANE = 3;
       OTHER = 4;
     }

     Message description = 1;
     Fields domain = 2;
     Fields codomain = 3;
   }

.. code-block::

   Function.Message.content max_size:128

.. code-block:: c++

   struct Function::Message_::Message {
     pw::InlineString<128> content;
   };

   enum class Function::Message_::Fields : uint32_t {
     CONTENT = 1,
   };

   enum class Function::Fields_ uint32_t{
       NONE = 0,
       COMPLEX_NUMBERS = 1,
       INTEGERS_MOD_5 = 2,
       MEROMORPHIC_FUNCTIONS_ON_COMPLEX_PLANE = 3,
       OTHER = 4,

       kNone = NONE,
       kComplexNumbers = COMPLEX_NUMBERS,
       kIntegersMod5 = INTEGERS_MOD_5,
       kMeromorphicFunctionsOnComplexPlane =
           MEROMORPHIC_FUNCTIONS_ON_COMPLEX_PLANE,
       kOther = OTHER,
   };

   struct Function::Message {
     Function::Message_::Message description;
     Function::Fields_ domain;
     Function::Fields_ codomain;
   };

   enum class Function::Fields : uint32_t {
     DESCRIPTION = 1,
     DOMAIN = 2,
     CODOMAIN = 3,
   };

.. warning::
   Note that the C++ spec also reserves two categories of identifiers for the
   compiler to use in ways that may conflict with generated code:

   * Any identifier that contains two consecutive underscores anywhere in it.
   * Any identifier that starts with an underscore followed by a capital letter.

   Appending underscores to symbols in these categories wouldn't change the fact
   that they match patterns reserved for the compiler, so the codegen does not
   currently attempt to fix them. Such names will therefore result in
   non-portable code that may or may not work depending on the compiler. These
   naming patterns are of course strongly discouraged in any protobufs that will
   be used with ``pw_protobuf`` codegen.

Overhead
========
A single encoder and decoder is used for these structures, with a one-time code
cost. When the code generator creates the ``struct Message``, it also creates
a description of this structure that the shared encoder and decoder use.

The cost of this description is a shared table for each protobuf message
definition used, with four words per field within the protobuf message, and an
addition word to store the size of the table.

--------
Encoding
--------
The simplest way to use ``MemoryEncoder`` to encode a proto is from its code
generated ``Message`` structure into an in-memory buffer.

.. code-block:: c++

   #include "my_protos/my_proto.pwpb.h"
   #include "pw_bytes/span.h"
   #include "pw_protobuf/encoder.h"
   #include "pw_status/status_with_size.h"

   // Writes a proto response to the provided buffer, returning the encode
   // status and number of bytes written.
   pw::StatusWithSize WriteProtoResponse(pw::ByteSpan response) {
     MyProto::Message message {}
     message.magic_number = 0x1a1a2b2b;
     message.favorite_food = "cookies";
     message.calories = 600;

     // All proto writes are directly written to the `response` buffer.
     MyProto::MemoryEncoder encoder(response);
     encoder.Write(message);

     return pw::StatusWithSize(encoder.status(), encoder.size());
   }

All fields of a message are written, including those initialized to their
default values.

Alternatively, for example if only a subset of fields are required to be
encoded, fields can be written a field at a time through the code generated
or lower-level APIs. This can be more convenient if finer grained control or
other custom handling is required.

.. code-block:: c++

   #include "my_protos/my_proto.pwpb.h"
   #include "pw_bytes/span.h"
   #include "pw_protobuf/encoder.h"
   #include "pw_status/status_with_size.h"

   // Writes a proto response to the provided buffer, returning the encode
   // status and number of bytes written.
   pw::StatusWithSize WriteProtoResponse(pw::ByteSpan response) {
     // All proto writes are directly written to the `response` buffer.
     MyProto::MemoryEncoder encoder(response);
     encoder.WriteMagicNumber(0x1a1a2b2b);
     encoder.WriteFavoriteFood("cookies");
     // Only conditionally write calories.
     if (on_diet) {
       encoder.WriteCalories(600);
     }
     return pw::StatusWithSize(encoder.status(), encoder.size());
   }

StreamEncoder
=============
``StreamEncoder`` is constructed with the destination stream, and a scratch
buffer used to handle nested submessages.

.. code-block:: c++

   #include "my_protos/my_proto.pwpb.h"
   #include "pw_bytes/span.h"
   #include "pw_protobuf/encoder.h"
   #include "pw_stream/sys_io_stream.h"

   pw::stream::SysIoWriter sys_io_writer;
   MyProto::StreamEncoder encoder(sys_io_writer, pw::ByteSpan());

   // Once this line returns, the field has been written to the Writer.
   encoder.WriteTimestamp(system::GetUnixEpoch());

   // There's no intermediate buffering when writing a string directly to a
   // StreamEncoder.
   encoder.WriteWelcomeMessage("Welcome to Pigweed!");

   if (!encoder.status().ok()) {
     PW_LOG_INFO("Failed to encode proto; %s", encoder.status().str());
   }

Callbacks
=========
When using the ``Write()`` method with a ``struct Message``, certain fields may
require a callback function be set to encode the values for those fields.
Otherwise the values will be treated as an empty repeated field and not encoded.

The callback is called with the cursor at the field in question, and passed
a reference to the typed encoder that can write the required values to the
stream or buffer.

Callback implementations may use any level of API. For example a callback for a
nested submessage (with a dependency cycle, or repeated) can be implemented by
calling ``Write()`` on a nested encoder.

.. code-block:: c++

   Store::Message store{};
   store.employees.SetEncoder([](Store::StreamEncoder& encoder) {
     Employee::Message employee{};
     // Populate `employee`.
     return encoder.GetEmployeesEncoder().Write(employee);
   ));

Nested submessages
==================
The examples in this section use the following protobuf definition:

.. code-block:: protobuf

   message Owner {
     Animal pet = 1;
   }

There are two methods for encoding nested submessages:

1. :ref:`pw_protobuf-encoding-nested_submessages-nested_encoder` -- This is
   the original method of writing submessages. It performs encoding in a
   single pass and requires an appropriately sized scratch buffer.

2. :ref:`pw_protobuf-encoding-nested_submessages-multipass_encoder` -- This
   method performs encoding in two passes to avoid the need for a scratch
   buffer.


.. _pw_protobuf-encoding-nested_submessages-nested_encoder:

Nested encoder
--------------
Code-generated ``GetFieldEncoder`` methods are provided that return a
correctly-typed ``StreamEncoder`` or ``MemoryEncoder`` for the message.
The accessor method is named for the field, while the returned encoder
is named for the message type. For example:

.. code-block:: c++

   Owner::StreamEncoder owner(writer, temp_buffer);

   {
     Animal::StreamEncoder pet = owner.GetPetEncoder();
     pet.WriteName("Rufus");
     pet.WriteAge(8);
     // submessage is closed when the nested encoder is destroyed.
   }

A lower-level API method returns an untyped encoder, which only provides the
lower-level API methods. This can be cast to a typed encoder if needed.

.. cpp:function:: pw::protobuf::StreamEncoder pw::protobuf::StreamEncoder::GetNestedEncoder(uint32_t field_number, EmptyEncoderBehavior empty_encoder_behavior = EmptyEncoderBehavior::kWriteFieldNumber)

(The optional ``empty_encoder_behavior`` parameter allows the user to disable
writing the tag number for the nested encoder, if no data was written to that
nested decoder.)

.. warning::
   When a nested submessage is created, any use of the parent encoder that
   created the nested encoder will trigger a crash. To resume using the parent
   encoder, destroy the submessage encoder first.

Buffering
^^^^^^^^^

.. note::
   This section applies only to the original "Nested encoder" method of
   encoding submessages.

Writing proto messages with nested submessages requires buffering due to
limitations of the proto format. Every proto submessage must know the size of
the submessage before its final serialization can begin. A streaming encoder can
be passed a scratch buffer to use when constructing nested messages. All
submessage data is buffered to this scratch buffer until the submessage is
finalized. Note that the contents of this scratch buffer is not necessarily
valid proto data, so don't try to use it directly.

The code generation includes a ``kScratchBufferSizeBytes`` constant that
represents the size of the largest submessage and all necessary overhead,
excluding the contents of any field values which require a callback.

If a submessage field requires a callback, due to a dependency cycle, or a
repeated field of unknown length, the size of the submessage cannot be included
in the ``kScratchBufferSizeBytes`` constant. If you encode a submessage of this
type (which you'll know you're doing because you set an encoder callback for it)
simply add the appropriate structure's ``kMaxEncodedSizeBytes`` constant to the
scratch buffer size to guarantee enough space.

When calculating yourself, the ``MaxScratchBufferSize()`` helper function can
also be useful in estimating how much space to allocate to account for nested
submessage encoding overhead.

.. code-block:: c++

   #include "my_protos/pets.pwpb.h"
   #include "pw_bytes/span.h"
   #include "pw_protobuf/encoder.h"
   #include "pw_stream/sys_io_stream.h"

   pw::stream::SysIoWriter sys_io_writer;
   // The scratch buffer should be at least as big as the largest nested
   // submessage. It's a good idea to be a little generous.
   std::byte submessage_scratch_buffer[Owner::kScratchBufferSizeBytes];

   // Provide the scratch buffer to the proto encoder. The buffer's lifetime must
   // match the lifetime of the encoder.
   Owner::StreamEncoder owner_encoder(sys_io_writer, submessage_scratch_buffer);

   {
     // Note that the parent encoder, owner_encoder, cannot be used until the
     // nested encoder, pet_encoder, has been destroyed.
     Animal::StreamEncoder pet_encoder = owner_encoder.GetPetEncoder();

     // There's intermediate buffering when writing to a nested encoder.
     pet_encoder.WriteName("Spot");
     pet_encoder.WriteType(Pet::Type::DOG);

     // When this scope ends, the nested encoder is serialized to the Writer.
     // In addition, the parent encoder, owner_encoder, can be used again.
   }

   // If an encode error occurs when encoding the nested messages, it will be
   // reflected at the root encoder.
   if (!owner_encoder.status().ok()) {
     PW_LOG_INFO("Failed to encode proto; %s", owner_encoder.status().str());
   }

MemoryEncoder objects use the final destination buffer rather than relying on a
scratch buffer.  The ``kMaxEncodedSizeBytes`` constant takes into account the
overhead required for nesting submessages. If you calculate the buffer size
yourself, your destination buffer might need additional space.

.. warning::
   If the scratch buffer size is not sufficient, the encoding will fail with
   ``Status::ResourceExhausted()``. Always check the results of ``Write`` calls
   or the encoder status to ensure success, as otherwise the encoded data will
   be invalid.

.. _pw_protobuf-encoding-nested_submessages-multipass_encoder:

Two-pass encoder
----------------
The two-pass encoder method accepts a lambda (or other callable) which is
passed a typed encoder for writing the submessage fields:

.. code-block:: c++

   Owner::StreamEncoder owner(writer, {});  // No scratch buffer needed.
   owner.WritePetMessage([](Animal::StreamEncoder& pet) {
     // This lambda is invoked twice, so it must
     // write the same fields each invocation.
     pet.WriteName("Rufus");
     pet.WriteAge(8);
   });

.. tip::
   * You can use ``auto``  in the lambda parameter to avoid referencing the
     potentially verbose submessage encoder type.

   * The callable argument is defined in such a way that normal
     ``pw::Function`` limitations are bypassed, so you are free to capture as
     many values in the lambda as needed to write the submessage.

   .. code-block:: c++

      std::string_view name = "Rufus";
      uint32_t age = 8;

      Owner::StreamEncoder owner(writer, {});
      // Using by-ref capture default [&] and auto& stream ref.
      owner.WritePetMessage([&](auto& pet) {
        pet.WriteName(name);
        pet.WriteAge(age);
      });

The two-pass encoder works as follows to avoid the need for a scratch buffer:

1. The callable is invoked once with an encoder that only counts the number of
   encoded bytes in the entire submessage. No data is actually written to the
   underlying stream during this pass.

2. The submessage tag (field number and size) are written to the underlying
   stream.

3. The callable is invoked again with a different encoder that actually writes
   the encoded data to the underlying stream.

.. warning::

   The callable must write the exact same fields, with the exact same values,
   in the same order, on both invocations.

   If the data being written might change during writing, the caller is
   expected to capture a snapshot of the data prior to encoding, or implement
   some type of explicit synchronization (e.g. mutex/semaphore/etc).

   Failure to do so may silently corrupt the output data and/or produce an
   error result.

Scalar Fields
=============
As shown, scalar fields are written using code generated ``WriteFoo``
methods that accept the appropriate type and automatically writes the correct
field number.

.. cpp:function:: Status MyProto::StreamEncoder::WriteFoo(T)

These can be freely intermixed with the lower-level API that provides a method
per field type, requiring that the field number be passed in. The code
generation includes a ``Fields`` enum to provide the field number values.

.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteUint64(uint32_t field_number, uint64_t)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteSint64(uint32_t field_number, int64_t)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteInt64(uint32_t field_number, int64_t)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteUint32(uint32_t field_number, uint32_t)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteSint32(uint32_t field_number, int32_t)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteInt32(uint32_t field_number, int32_t)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteFixed64(uint32_t field_number, uint64_t)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteFixed32(uint32_t field_number, uint64_t)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteDouble(uint32_t field_number, double)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteFloat(uint32_t field_number, float)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteBool(uint32_t field_number, bool)

The following two method calls are equivalent, where the first is using the
code generated API, and the second implemented by hand.

.. code-block:: c++

   my_proto_encoder.WriteAge(42);
   my_proto_encoder.WriteInt32(static_cast<uint32_t>(MyProto::Fields::kAge), 42);

Repeated Fields
---------------
For repeated scalar fields, multiple code generated ``WriteFoos`` methods
are provided.

.. cpp:function:: Status MyProto::StreamEncoder::WriteFoos(T)

   This writes a single unpacked value.

.. cpp:function:: Status MyProto::StreamEncoder::WriteFoos(pw::span<const T>)
.. cpp:function:: Status MyProto::StreamEncoder::WriteFoos(const pw::Vector<T>&)

   These write a packed field containing all of the values in the provided span
   or vector.

These too can be freely intermixed with the lower-level API methods, both to
write a single value, or to write packed values from either a ``pw::span`` or
``pw::Vector`` source.

.. cpp:function:: Status pw::protobuf::StreamEncoder::WritePackedUint64(uint32_t field_number, pw::span<const uint64_t>)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteRepeatedUint64(uint32_t field_number, const pw::Vector<uint64_t>&)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WritePackedSint64(uint32_t field_number, pw::span<const int64_t>)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteRepeatedSint64(uint32_t field_number, const pw::Vector<int64_t>&)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WritePackedInt64(uint32_t field_number, pw::span<const int64_t>)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteRepeatedInt64(uint32_t field_number, const pw::Vector<int64_t>&)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WritePackedUint32(uint32_t field_number, pw::span<const uint32_t>)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteRepeatedUint32(uint32_t field_number, const pw::Vector<uint32_t>&)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WritePackedSint32(uint32_t field_number, pw::span<const int32_t>)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteRepeatedSint32(uint32_t field_number, const pw::Vector<int32_t>&)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WritePackedInt32(uint32_t field_number, pw::span<const int32_t>)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteRepeatedInt32(uint32_t field_number, const pw::Vector<int32_t>&)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WritePackedFixed64(uint32_t field_number, pw::span<const uint64_t>)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteRepeatedFixed64(uint32_t field_number, const pw::Vector<uint64_t>&)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WritePackedFixed32(uint32_t field_number, pw::span<const uint64_t>)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteRepeatedFixed32(uint32_t field_number, const pw::Vector<uint64_t>&)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WritePackedDouble(uint32_t field_number, pw::span<const double>)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteRepeatedDouble(uint32_t field_number, const pw::Vector<double>&)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WritePackedFloat(uint32_t field_number, pw::span<const float>)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteRepeatedFloat(uint32_t field_number, const pw::Vector<float>&)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WritePackedBool(uint32_t field_number, pw::span<const bool>)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteRepeatedBool(uint32_t field_number, const pw::Vector<bool>&)

The following two method calls are equivalent, where the first is using the
code generated API, and the second implemented by hand.

.. code-block:: c++

   constexpr std::array<int32_t, 5> numbers = {4, 8, 15, 16, 23, 42};

   my_proto_encoder.WriteNumbers(numbers);
   my_proto_encoder.WritePackedInt32(
       static_cast<uint32_t>(MyProto::Fields::kNumbers), numbers);

Enumerations
============
Enumerations are written using code generated ``WriteEnum`` methods that
accept the code generated enumeration as the appropriate type and automatically
writes both the correct field number and corresponding value.

.. cpp:function:: Status MyProto::StreamEncoder::WriteEnum(MyProto::Enum)

To write enumerations with the lower-level API, you would need to cast both
the field number and value to the ``uint32_t`` type.

The following two methods are equivalent, where the first is code generated,
and the second implemented by hand.

.. code-block:: c++

   my_proto_encoder.WriteAward(MyProto::Award::SILVER);
   my_proto_encoder.WriteUint32(static_cast<uint32_t>(MyProto::Fields::kAward),
                                static_cast<uint32_t>(MyProto::Award::SILVER));

Repeated Fields
---------------
For repeated enum fields, multiple code generated ``WriteEnums`` methods
are provided.

.. cpp:function:: Status MyProto::StreamEncoder::WriteEnums(MyProto::Enums)

   This writes a single unpacked value.

.. cpp:function:: Status MyProto::StreamEncoder::WriteEnums(pw::span<const MyProto::Enums>)
.. cpp:function:: Status MyProto::StreamEncoder::WriteEnums(const pw::Vector<MyProto::Enums>&)

   These write a packed field containing all of the values in the provided span
   or vector.

Their use is as scalar fields.

Strings
=======
Strings fields have multiple code generated methods provided.

.. cpp:function:: Status MyProto::StreamEncoder::WriteName(std::string_view)
.. cpp:function:: Status MyProto::StreamEncoder::WriteName(const char*, size_t)

These can be freely intermixed with the lower-level API methods.

.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteString(uint32_t field_number, std::string_view)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteString(uint32_t field_number, const char*, size_t)

A lower level API method is provided that can write a string from another
stream.

.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteStringFromStream(uint32_t field_number, stream::Reader& bytes_reader, size_t num_bytes, ByteSpan stream_pipe_buffer)

   The payload for the value is provided through the stream::Reader
   ``bytes_reader``. The method reads a chunk of the data from the reader using
   the ``stream_pipe_buffer`` and writes it to the encoder.

Bytes
=====
Bytes fields have multiple code generated methods provided.
The ``write_func`` callback version allows direct writes to the underlying
stream, eliminating the need for a temporary copy buffer.

.. cpp:function:: Status MyProto::StreamEncoder::WriteFoo(ConstByteSpan)
.. cpp:function:: Status MyProto::StreamEncoder::WriteFoo(size_t num_bytes, const pw::Function<pw::Status(pw::stream::Writer&)> write_func);

These can be freely intermixed with the lower-level API method.

.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteBytes(uint32_t field_number, ConstByteSpan)
.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteBytes(size_t num_bytes, const pw::Function<pw::Status(pw::stream::Writer&)> write_func);

And with the API method that can write bytes from another stream.

.. cpp:function:: Status pw::protobuf::StreamEncoder::WriteBytesFromStream(uint32_t field_number, stream::Reader& bytes_reader, size_t num_bytes, ByteSpan stream_pipe_buffer)

   The payload for the value is provided through the stream::Reader
   ``bytes_reader``. The method reads a chunk of the data from the reader using
   the ``stream_pipe_buffer`` and writes it to the encoder.

Error Handling
==============
While individual write calls on a proto encoder return ``pw::Status`` objects,
the encoder tracks all status returns and "latches" onto the first error
encountered. This status can be accessed via ``StreamEncoder::status()``.

Proto map encoding utils
========================
Some additional helpers for encoding more complex but common protobuf
submessages (e.g. ``map<string, bytes>``) are provided in
``pw_protobuf/map_utils.h``.

.. note::
   The helper API are currently in-development and may not remain stable.

--------
Decoding
--------
The simplest way to use ``StreamDecoder`` is to decode a proto from the stream
into its code generated ``Message`` structure.

.. code-block:: c++

   #include "my_protos/my_proto.pwpb.h"
   #include "pw_protobuf/stream_decoder.h"
   #include "pw_status/status.h"
   #include "pw_stream/stream.h"

   pw::Status DecodeProtoFromStream(pw::stream::Reader& reader) {
     MyProto::Message message{};
     MyProto::StreamDecoder decoder(reader);
     decoder.Read(message);
     return decoder.status();
   }

In the case of errors, the decoding will stop and return with the cursor on the
field that caused the error. It is valid in some cases to inspect the error and
continue decoding by calling ``Read()`` again on the same structure, or fall
back to using the lower-level APIs.

Unknown fields in the wire encoding are skipped.

If finer-grained control is required, the ``StreamDecoder`` class provides an
iterator-style API for processing a message a field at a time where calling
``Next()`` advances the decoder to the next proto field.

.. cpp:function:: Status pw::protobuf::StreamDecoder::Next()

In the code generated classes the ``Field()`` method returns the current field
as a typed ``Fields`` enumeration member, while the lower-level API provides a
``FieldNumber()`` method that returns the number of the field.

.. cpp:function:: Result<MyProto::Fields> MyProto::StreamDecoder::Field()
.. cpp:function:: Result<uint32_t> pw::protobuf::StreamDecoder::FieldNumber()

.. code-block:: c++

   #include "my_protos/my_proto.pwpb.h"
   #include "pw_protobuf/strema_decoder.h"
   #include "pw_status/status.h"
   #include "pw_status/try.h"
   #include "pw_stream/stream.h"

   pw::Status DecodeProtoFromStream(pw::stream::Reader& reader) {
     MyProto::StreamDecoder decoder(reader);
     pw::Status status;

     uint32_t age;
     char name[16];

     // Iterate over the fields in the message. A return value of OK indicates
     // that a valid field has been found and can be read. When the decoder
     // reaches the end of the message, Next() will return OUT_OF_RANGE.
     // Other return values indicate an error trying to decode the message.
     while ((status = decoder.Next()).ok()) {
       // Field() returns a Result<Fields> as it may fail sometimes.
       // However, Field() is guaranteed to be valid after a call to Next()
       // that returns OK, so the value can be used directly here.
       switch (decoder.Field().value()) {
         case MyProto::Fields::kAge: {
           PW_TRY_ASSIGN(age, decoder.ReadAge());
           break;
         }
         case MyProto::Fields::kName:
           // The string field is copied into the provided buffer. If the buffer
           // is too small to fit the string, RESOURCE_EXHAUSTED is returned and
           // the decoder is not advanced, allowing the field to be re-read.
           PW_TRY(decoder.ReadName(name));
           break;
       }
     }

     // Do something with the fields...

     return status.IsOutOfRange() ? OkStatus() : status;
   }

Callbacks
=========
When using the ``Read()`` method with a ``struct Message`` containing callback
fields, the callback is called with the cursor at the field in question, and
passed a reference to the typed decoder that can examine the field and be used
to decode it.

Callback implementations may use any level of API. For example a callback for a
nested submessage (with a dependency cycle, or repeated) can be implemented by
calling ``Read()`` on a nested decoder.

.. code-block:: c++

   Store::Message store{};
   store.employees.SetDecoder([](Store::StreamDecoder& decoder) {
     PW_ASSERT(decoder.Field().value() == Store::Fields::kEmployees);

     Employee::Message employee{};
     // Set any callbacks on `employee`.
     PW_TRY(decoder.GetEmployeesDecoder().Read(employee));
     // Do things with `employee`.
     return OkStatus();
   ));

Nested submessages
==================
Code generated ``GetFieldDecoder`` methods are provided that return a correctly
typed ``StreamDecoder`` for the message.

.. code-block:: protobuf

   message Owner {
     Animal pet = 1;
   }

As with encoding, note that the accessor method is named for the field, while
the returned decoder is named for the message type.

.. cpp:function:: Animal::StreamDecoder Owner::StreamDecoder::GetPetDecoder()

A lower-level API method returns an untyped decoder, which only provides the
lower-level API methods. This can be moved to a typed decoder later.

.. cpp:function:: pw::protobuf::StreamDecoder pw::protobuf::StreamDecoder::GetNestedDecoder()

.. warning::
   When a nested submessage is being decoded, any use of the parent decoder that
   created the nested decoder will trigger a crash. To resume using the parent
   decoder, destroy the submessage decoder first.


.. code-block:: c++

   case Owner::Fields::kPet: {
     // Note that the parent decoder, owner_decoder, cannot be used until the
     // nested decoder, pet_decoder, has been destroyed.
     Animal::StreamDecoder pet_decoder = owner_decoder.GetPetDecoder();

     while ((status = pet_decoder.Next()).ok()) {
       switch (pet_decoder.Field().value()) {
         // Decode pet fields...
       }
     }

     // When this scope ends, the nested decoder is destroyed and the
     // parent decoder, owner_decoder, can be used again.
     break;
   }

Scalar Fields
=============
Scalar fields are read using code generated ``ReadFoo`` methods that return the
appropriate type and assert that the correct field number ie being read.

.. cpp:function:: Result<T> MyProto::StreamDecoder::ReadFoo()

These can be freely intermixed with the lower-level API that provides a method
per field type, requiring that the caller first check the field number.

.. cpp:function:: Result<uint64_t> pw::protobuf::StreamDecoder::ReadUint64()
.. cpp:function:: Result<int64_t> pw::protobuf::StreamDecoder::ReadSint64()
.. cpp:function:: Result<int64_t> pw::protobuf::StreamDecoder::ReadInt64()
.. cpp:function:: Result<uint32_t> pw::protobuf::StreamDecoder::ReadUint32()
.. cpp:function:: Result<int32_t> pw::protobuf::StreamDecoder::ReadSint32()
.. cpp:function:: Result<int32_t> pw::protobuf::StreamDecoder::ReadInt32()
.. cpp:function:: Result<uint64_t> pw::protobuf::StreamDecoder::ReadFixed64()
.. cpp:function:: Result<uint64_t> pw::protobuf::StreamDecoder::ReadFixed32()
.. cpp:function:: Result<double> pw::protobuf::StreamDecoder::ReadDouble()
.. cpp:function:: Result<float> pw::protobuf::StreamDecoder::ReadFloat()
.. cpp:function:: Result<bool> pw::protobuf::StreamDecoder::ReadBool()

The following two code snippets are equivalent, where the first uses the code
generated API, and the second implemented by hand.

.. code-block:: c++

   pw::Result<int32_t> age = my_proto_decoder.ReadAge();

.. code-block:: c++

   PW_ASSERT(my_proto_decoder.FieldNumber().value() ==
       static_cast<uint32_t>(MyProto::Fields::kAge));
   pw::Result<int32_t> my_proto_decoder.ReadInt32();

Repeated Fields
---------------
For repeated scalar fields, multiple code generated ``ReadFoos`` methods
are provided.

.. cpp:function:: Result<T> MyProto::StreamDecoder::ReadFoos()

   This reads a single unpacked value.

.. cpp:function:: StatusWithSize MyProto::StreamDecoder::ReadFoos(pw::span<T>)

   This reads a packed field containing all of the values into the provided span.

.. cpp:function:: Status MyProto::StreamDecoder::ReadFoos(pw::Vector<T>&)

   Protobuf encoders are permitted to choose either repeating single unpacked
   values, or a packed field, including splitting repeated fields up into
   multiple packed fields.

   This method supports either format, appending values to the provided
   ``pw::Vector``.

These too can be freely intermixed with the lower-level API methods, to read a
single value, a field of packed values into a ``pw::span``, or support both
formats appending to a ``pw::Vector`` source.

.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadPackedUint64(pw::span<uint64_t>)
.. cpp:function:: Status pw::protobuf::StreamDecoder::ReadRepeatedUint64(pw::Vector<uint64_t>&)
.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadPackedSint64(pw::span<int64_t>)
.. cpp:function:: Status pw::protobuf::StreamDecoder::ReadRepeatedSint64(pw::Vector<int64_t>&)
.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadPackedInt64(pw::span<int64_t>)
.. cpp:function:: Status pw::protobuf::StreamDecoder::ReadRepeatedInt64(pw::Vector<int64_t>&)
.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadPackedUint32(pw::span<uint32_t>)
.. cpp:function:: Status pw::protobuf::StreamDecoder::ReadRepeatedUint32(pw::Vector<uint32_t>&)
.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadPackedSint32(pw::span<int32_t>)
.. cpp:function:: Status pw::protobuf::StreamDecoder::ReadRepeatedSint32(pw::Vector<int32_t>&)
.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadPackedInt32(pw::span<int32_t>)
.. cpp:function:: Status pw::protobuf::StreamDecoder::ReadRepeatedInt32(pw::Vector<int32_t>&)
.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadPackedFixed64(pw::span<uint64_t>)
.. cpp:function:: Status pw::protobuf::StreamDecoder::ReadRepeatedFixed64(pw::Vector<uint64_t>&)
.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadPackedFixed32(pw::span<uint64_t>)
.. cpp:function:: Status pw::protobuf::StreamDecoder::ReadRepeatedFixed32(pw::Vector<uint64_t>&)
.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadPackedDouble(pw::span<double>)
.. cpp:function:: Status pw::protobuf::StreamDecoder::ReadRepeatedDouble(pw::Vector<double>&)
.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadPackedFloat(pw::span<float>)
.. cpp:function:: Status pw::protobuf::StreamDecoder::ReadRepeatedFloat(pw::Vector<float>&)
.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadPackedBool(pw::span<bool>)
.. cpp:function:: Status pw::protobuf::StreamDecoder::ReadRepeatedBool(pw::Vector<bool>&)

The following two code blocks are equivalent, where the first uses the code
generated API, and the second is implemented by hand.

.. code-block:: c++

   pw::Vector<int32_t, 8> numbers;

   my_proto_decoder.ReadNumbers(numbers);

.. code-block:: c++

   pw::Vector<int32_t, 8> numbers;

   PW_ASSERT(my_proto_decoder.FieldNumber().value() ==
       static_cast<uint32_t>(MyProto::Fields::kNumbers));
   my_proto_decoder.ReadRepeatedInt32(numbers);

Enumerations
============
``pw_protobuf`` generates a few functions for working with enumerations.
Most importantly, enumerations are read using generated ``ReadEnum`` methods
that return the enumeration as the appropriate generated type.

.. cpp:function:: Result<MyProto::Enum> MyProto::StreamDecoder::ReadEnum()

   Decodes an enum from the stream.

.. cpp:function:: constexpr bool MyProto::IsValidEnum(MyProto::Enum value)

   Validates the value encoded in the wire format against the known set of
   enumerators.

.. cpp:function:: constexpr const char* MyProto::EnumToString(MyProto::Enum value, const char* invalid = "")

   Returns the string representation of the enum value, in UPPER_SNAKE_CASE.
   Returns ``invalid`` (which defaults to an empty string) if the value is not a
   valid enumerator.

   Examples:

   * ``FooToString(Foo::kBarBaz)`` returns ``"BAR_BAZ"``.

   * ``FooToString(static_cast<Foo>(666), "cheese")`` returns ``"cheese"``
     (assuming 666 is an invalid Foo enumerator).

.. cpp:var:: inline constexpr std::array<Enum, 7> kEnumValues

   An array of all enumerator values.

   Example usage:

   .. code-block:: c++

      for (const Enum value : MyProto::kEnumValues) {
      }

   These can be used with ``EnumToString()`` to produce the enumerator names as
   strings, with no overhead due to its ``constexpr`` implementation.

To read enumerations with the lower-level API, you would need to cast the
retured value from the ``uint32_t``.

The following two code blocks are equivalent, where the first is using the code
generated API, and the second implemented by hand.

.. code-block:: c++

   pw::Result<MyProto::Award> award = my_proto_decoder.ReadAward();
   if (!MyProto::IsValidAward(award)) {
     PW_LOG_DBG("Unknown award");
   }

.. code-block:: c++

   PW_ASSERT(my_proto_decoder.FieldNumber().value() ==
       static_cast<uint32_t>(MyProto::Fields::kAward));
   pw::Result<uint32_t> award_value = my_proto_decoder.ReadUint32();
   if (award_value.ok()) {
     MyProto::Award award = static_cast<MyProto::Award>(award_value);
   }

Repeated Fields
---------------
For repeated enum fields, multiple code generated ``ReadEnums`` methods
are provided.

.. cpp:function:: Result<MyProto::Enums> MyProto::StreamDecoder::ReadEnums()

   This reads a single unpacked value.

.. cpp:function:: StatusWithSize MyProto::StreamDecoder::ReadEnums(pw::span<MyProto::Enums>)

   This reads a packed field containing all of the checked values into the
   provided span.

.. cpp:function:: Status MyProto::StreamDecoder::ReadEnums(pw::Vector<MyProto::Enums>&)

   This method supports either repeated unpacked or packed formats, appending
   checked values to the provided ``pw::Vector``.

Their use is as scalar fields.

Strings
=======
Strings fields provide a code generated method to read the string into the
provided span. Since the span is updated with the size of the string, the string
is not automatically null-terminated. :ref:`module-pw_string` provides utility
methods to copy string data from spans into other targets.

.. cpp:function:: StatusWithSize MyProto::StreamDecoder::ReadName(pw::span<char>)

An additional code generated method is provided to return a nested
``BytesReader`` to access the data as a stream. As with nested submessage
decoders, any use of the parent decoder that created the bytes reader will
trigger a crash. To resume using the parent decoder, destroy the bytes reader
first.

.. cpp:function:: pw::protobuf::StreamDecoder::BytesReader MyProto::StreamDecoder::GetNameReader()

These can be freely intermixed with the lower-level API method:

.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadString(pw::span<char>)

The lower-level ``GetBytesReader()`` method can also be used to read string data
as bytes.

Bytes
=====
Bytes fields provide the ``WriteData`` code generated method to read the bytes
into the provided span.

.. cpp:function:: StatusWithSize MyProto::StreamDecoder::ReadData(ByteSpan)

An additional code generated method is provided to return a nested
``BytesReader`` to access the data as a stream. As with nested submessage
decoders, any use of the parent decoder that created the bytes reader will
trigger a crash. To resume using the parent decoder, destroy the bytes reader
first.

.. cpp:function:: pw::protobuf::StreamDecoder::BytesReader MyProto::StreamDecoder::GetDataReader()

These can be freely intermixed with the lower-level API methods.

.. cpp:function:: StatusWithSize pw::protobuf::StreamDecoder::ReadBytes(ByteSpan)
.. cpp:function:: pw::protobuf::StreamDecoder::BytesReader pw::protobuf::StreamDecoder::GetBytesReader()

The ``BytesReader`` supports seeking only if the ``StreamDecoder``'s reader
supports seeking.

Error Handling
==============
While individual read calls on a proto decoder return ``pw::Result``,
``pw::StatusWithSize``, or ``pw::Status`` objects, the decoder tracks all status
returns and "latches" onto the first error encountered. This status can be
accessed via ``StreamDecoder::status()``.

Length Limited Decoding
=======================
Where the length of the protobuf message is known in advance, the decoder can
be prevented from reading from the stream beyond the known bounds by specifying
the known length to the decoder:

.. code-block:: c++

   pw::protobuf::StreamDecoder decoder(reader, message_length);

When a decoder constructed in this way goes out of scope, it will consume any
remaining bytes in ``message_length`` allowing the next ``Read()`` on the stream
to be data after the protobuf, even when it was not fully parsed.

-----------------
In-memory Decoder
-----------------
The separate ``Decoder`` class operates on an protobuf message located in a
buffer in memory. It is more efficient than the ``StreamDecoder`` in cases
where the complete protobuf data can be stored in memory. The tradeoff of this
efficiency is that no code generation is provided, so all decoding must be
performed by hand.

As ``StreamDecoder``, it provides an iterator-style API for processing a
message. Calling ``Next()`` advances the decoder to the next proto field, which
can then be read by calling the appropriate ``Read*`` function for the field
number.

When reading ``bytes`` and ``string`` fields, the decoder returns a view of that
field within the buffer; no data is copied out.

.. code-block:: c++

   #include "pw_protobuf/decoder.h"
   #include "pw_status/try.h"

   pw::Status DecodeProtoFromBuffer(pw::span<const std::byte> buffer) {
     pw::protobuf::Decoder decoder(buffer);
     pw::Status status;

     uint32_t uint32_field;
     std::string_view string_field;

     // Iterate over the fields in the message. A return value of OK indicates
     // that a valid field has been found and can be read. When the decoder
     // reaches the end of the message, Next() will return OUT_OF_RANGE.
     // Other return values indicate an error trying to decode the message.
     while ((status = decoder.Next()).ok()) {
       switch (decoder.FieldNumber()) {
         case 1:
           PW_TRY(decoder.ReadUint32(&uint32_field));
           break;
         case 2:
           // The passed-in string_view will point to the contents of the string
           // field within the buffer.
           PW_TRY(decoder.ReadString(&string_field));
           break;
       }
     }

     // Do something with the fields...

     return status.IsOutOfRange() ? OkStatus() : status;
   }

---------------
Message Decoder
---------------

.. note::

   ``pw::protobuf::Message`` is unrelated to the codegen ``struct Message``
   used with ``StreamDecoder``.

The module implements a message parsing helper class ``Message``, in
``pw_protobuf/message.h``, to faciliate proto message parsing and field access.
The class provides interfaces for searching fields in a proto message and
creating helper classes for it according to its interpreted field type, i.e.
uint32, bytes, string, map<>, repeated etc. The class works on top of
``StreamDecoder`` and thus requires a ``pw::stream::SeekableReader`` for proto
message access. The following gives examples for using the class to process
different fields in a proto message:

.. code-block:: c++

   // Consider the proto messages defined as follows:
   //
   // message Nested {
   //   string nested_str = 1;
   //   bytes nested_bytes = 2;
   // }
   //
   // message {
   //   uint32 integer = 1;
   //   string str = 2;
   //   bytes bytes = 3;
   //   Nested nested = 4;
   //   repeated string rep_str = 5;
   //   repeated Nested rep_nested  = 6;
   //   map<string, bytes> str_to_bytes = 7;
   //   map<string, Nested> str_to_nested = 8;
   // }

   // Given a seekable `reader` that reads the top-level proto message, and
   // a <proto_size> that gives the size of the proto message:
   Message message(reader, proto_size);

   // Parse a proto integer field
   Uint32 integer = messasge_parser.AsUint32(1);
   if (!integer.ok()) {
     // handle parsing error. i.e. return integer.status().
   }
   uint32_t integer_value = integer.value();  // obtained the value

   // Parse a string field
   String str = message.AsString(2);
   if (!str.ok()) {
     // handle parsing error. i.e. return str.status();
   }

   // check string equal
   Result<bool> str_check = str.Equal("foo");

   // Parse a bytes field
   Bytes bytes = message.AsBytes(3);
   if (!bytes.ok()) {
     // handle parsing error. i.e. return bytes.status();
   }

   // Get a reader to the bytes.
   stream::IntervalReader bytes_reader = bytes.GetBytesReader();

   // Parse nested message `Nested nested = 4;`
   Message nested = message.AsMessage(4);
   // Get the fields in the nested message.
   String nested_str = nested.AsString(1);
   Bytes nested_bytes = nested.AsBytes(2);

   // Parse repeated field `repeated string rep_str = 5;`
   RepeatedStrings rep_str = message.AsRepeatedString(5);
   // Iterate through the entries. If proto is malformed when
   // iterating, the next element (`str` in this case) will be invalid
   // and loop will end in the iteration after.
   for (String element : rep_str) {
     // Check status
     if (!str.ok()) {
       // In the case of error, loop will end in the next iteration if
       // continues. This is the chance for code to catch the error.
     }
     // Process str
   }

   // Parse repeated field `repeated Nested rep_nested = 6;`
   RepeatedStrings rep_str = message.AsRepeatedString(6);
   // Iterate through the entries. For iteration
   for (Message element : rep_rep_nestedstr) {
     // Check status
     if (!element.ok()) {
       // In the case of error, loop will end in the next iteration if
       // continues. This is the chance for code to catch the error.
     }
     // Process element
   }

   // Parse map field `map<string, bytes> str_to_bytes = 7;`
   StringToBytesMap str_to_bytes = message.AsStringToBytesMap(7);
   // Access the entry by a given key value
   Bytes bytes_for_key = str_to_bytes["key"];
   // Or iterate through map entries
   for (StringToBytesMapEntry entry : str_to_bytes) {
     // Check status
     if (!entry.ok()) {
       // In the case of error, loop will end in the next iteration if
       // continues. This is the chance for code to catch the error.
     }
     String key = entry.Key();
     Bytes value = entry.Value();
     // process entry
   }

   // Parse map field `map<string, Nested> str_to_nested = 8;`
   StringToMessageMap str_to_nested = message.AsStringToBytesMap(8);
   // Access the entry by a given key value
   Message nested_for_key = str_to_nested["key"];
   // Or iterate through map entries
   for (StringToMessageMapEntry entry : str_to_nested) {
     // Check status
     if (!entry.ok()) {
       // In the case of error, loop will end in the next iteration if
       // continues. This is the chance for code to catch the error.
       // However it is still recommended that the user breaks here.
       break;
     }
     String key = entry.Key();
     Message value = entry.Value();
     // process entry
   }

The methods in ``Message`` for parsing a single field, i.e. everty `AsXXX()`
method except AsRepeatedXXX() and AsStringMapXXX(), internally performs a
linear scan of the entire proto message to find the field with the given
field number. This can be expensive if performed multiple times, especially
on slow reader. The same applies to the ``operator[]`` of StringToXXXXMap
helper class. Therefore, for performance consideration, whenever possible, it
is recommended to use the following for-range style to iterate and process
single fields directly.


.. code-block:: c++

   for (Message::Field field : message) {
     // Check status
     if (!field.ok()) {
       // In the case of error, loop will end in the next iteration if
       // continues. This is the chance for code to catch the error.
     }
     if (field.field_number() == 1) {
       Uint32 integer = field.As<Uint32>();
       ...
     } else if (field.field_number() == 2) {
       String str = field.As<String>();
       ...
     } else if (field.field_number() == 3) {
       Bytes bytes = field.As<Bytes>();
       ...
     } else if (field.field_number() == 4) {
       Message nested = field.As<Message>();
       ...
     }
   }

.. Note::
  The helper API are currently in-development and may not remain stable.

-----------
Size report
-----------

Full size report
================

This report demonstrates the size of using the entire decoder with all of its
decode methods and a decode callback for a proto message containing each of the
protobuf field types.

.. include:: size_report/decoder_partial


Incremental size report
=======================

This report is generated using the full report as a base and adding some int32
fields to the decode callback to demonstrate the incremental cost of decoding
fields in a message.

.. include:: size_report/decoder_incremental

---------------------------
Serialized size calculation
---------------------------
``pw_protobuf/serialized_size.h`` provides a set of functions for calculating
how much memory serialized protocol buffer fields require. The
``kMaxSizeBytes*`` variables provide the maximum encoded sizes of each field
type. The ``SizeOfField*()`` functions calculate the encoded size of a field of
the specified type, given a particular key and, for variable length fields
(varint or delimited), a value. The ``SizeOf*Field`` functions calculate the
encoded size of fields with a particular wire format (delimited, varint).

In the rare event that you need to know the serialized size of a field's tag
(field number and wire type), you can use ``TagSizeBytes()`` to calculate the
tag size for a given field number.

--------------------------
Available protobuf modules
--------------------------
There are a handful of messages ready to be used in Pigweed projects. These are
located in ``pw_protobuf/pw_protobuf_protos``.

common.proto
============
Contains Empty message proto used in many RPC calls.


status.proto
============
Contains the enum for pw::Status.

.. note::
   ``pw::protobuf::StatusCode`` values should not be used outside of a .proto
   file. Instead, the StatusCodes should be converted to the Status type in the
   language. In C++, this would be:

   .. code-block:: c++

      // Reading from a proto
      pw::Status status = static_cast<pw::Status::Code>(proto.status_field));
      // Writing to a proto
      proto.status_field = static_cast<pw::protobuf::StatusCode>(status.code()));

----------------------------------------
Comparison with other protobuf libraries
----------------------------------------

protobuf-lite
=============
protobuf-lite is the official reduced-size C++ implementation of protobuf. It
uses a restricted subset of the protobuf library's features to minimize code
size. However, is is still around 150K in size and requires dynamic memory
allocation, making it unsuitable for many embedded systems.

nanopb
======
`Nanopb`_ is a commonly used embedded protobuf library with very small code size
and full code generation. It provides both encoding/decoding functionality and
in-memory C structs representing protobuf messages.

nanopb works well for many embedded products; however, using its generated code
can run into RAM usage issues when processing nontrivial protobuf messages due
to the necessity of defining a struct capable of storing all configurations of
the message, which can grow incredibly large. In one project, Pigweed developers
encountered an 11K struct statically allocated for a single message---over twice
the size of the final encoded output! (This was what prompted the development of
``pw_protobuf``.)

To avoid this issue, it is possible to use nanopb's low-level encode/decode
functions to process individual message fields directly, but this loses all of
the useful semantics of code generation. ``pw_protobuf`` is designed to optimize
for this use case; it allows for efficient operations on the wire format with an
intuitive user interface.

Depending on the requirements of a project, either of these libraries could be
suitable.

.. _Nanopb: https://jpa.kapsi.fi/nanopb/
