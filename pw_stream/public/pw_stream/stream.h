// Copyright 2021 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_memory/internal/sibling_cast.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::stream {

/// @submodule{pw_stream,interface}

/// A generic stream that may support reading, writing, and seeking, but makes
/// no guarantees about whether any operations are supported. Unsupported
/// functions return `Status::Unimplemented()`. Stream serves as the base for
/// the Reader, Writer, and ReaderWriter interfaces.
///
/// Stream cannot be extended directly. Instead, work with one of the derived
/// classes that explicitly supports the required functionality. Stream should
/// almost never be used in APIs; accept a derived class with the required
/// capabilities instead.
///
/// All Stream methods are blocking. They return when the requested operation
/// completes.
class Stream {
 public:
  /// Positions from which to seek.
  enum Whence : uint8_t {
    /// Seek from the beginning of the stream. The offset is a direct offset
    /// into the data.
    kBeginning = 0b001,

    /// Seek from the current position in the stream. The offset is added to the
    /// current position. Use a negative offset to seek backwards.
    ///
    /// Implementations may only support seeking within a limited range from the
    /// current position.
    kCurrent = 0b010,

    /// Seek from the end of the stream. The offset is added to the end
    /// position. Use a negative offset to seek backwards from the end.
    kEnd = 0b100,
  };

  /// Value returned from read/write limit if unlimited.
  static constexpr size_t kUnlimited = std::numeric_limits<size_t>::max();

  /// Returned by Tell() if getting the position is not supported.
  static constexpr size_t kUnknownPosition = std::numeric_limits<size_t>::max();

  virtual ~Stream() = default;

  /// @retval True If reading is supported.
  /// @retval False If Read() returns UNIMPLEMENTED.
  constexpr bool readable() const { return readable_; }

  /// @retval True If writing is supported.
  /// @retval False If Write() returns UNIMPLEMENTED.
  constexpr bool writable() const { return writable_; }

  /// @retval True If the stream supports seeking.
  /// @retval False If the stream does not supports seeking.
  constexpr bool seekable() const { return seekability_ != Seekability::kNone; }

  /// True if the stream supports seeking from the specified origin.
  constexpr bool seekable(Whence origin) const {
    return (static_cast<uint8_t>(seekability_) & origin) != 0u;
  }

  /// Reads data from the stream into the provided buffer, if supported. As many
  /// bytes as are available up to the buffer size are copied into the buffer.
  /// Remaining bytes may by read in subsequent calls. If any number of bytes
  /// are read returns OK with a span of the bytes read.
  ///
  /// If the reader has been exhausted and is and can no longer read additional
  /// bytes it will return `OUT_OF_RANGE`. This is similar to end-of-file (EOF).
  /// Read will only return `OUT_OF_RANGE` if ConservativeReadLimit() is and
  /// will remain zero. A Read operation that is successful and also exhausts
  /// the reader returns OK, with all following calls returning `OUT_OF_RANGE`.
  ///
  /// Derived classes should NOT try to override these public read methods.
  /// Instead, provide an implementation by overriding DoRead().
  ///
  /// @returns
  /// * @OK: Between 1 and `dest.size_bytes()` were successfully
  ///   read. `Result` contains the span of read bytes.
  /// * @UNIMPLEMENTED: This stream does not support reading.
  /// * @FAILED_PRECONDITION: The `Reader` is not in state to read data.
  /// * @RESOURCE_EXHAUSTED: Unable to read any bytes at this time. No bytes
  ///   read. Try again once bytes become available.
  /// * @OUT_OF_RANGE: `Reader` has been exhausted, similar to EOF. No bytes
  ///   were read, no more will be read.
  Result<ByteSpan> Read(ByteSpan dest) {
    PW_DASSERT(dest.empty() || dest.data() != nullptr);
    StatusWithSize result = DoRead(dest);

    if (result.ok()) {
      return dest.first(result.size());
    }
    return result.status();
  }
  /// @overload
  Result<ByteSpan> Read(void* dest, size_t size_bytes) {
    return Read(span(static_cast<std::byte*>(dest), size_bytes));
  }

  /// Reads exactly the number of bytes requested into the provided buffer, if
  /// supported. Internally, the stream is read as many times as necessary to
  /// fill the destination buffer. If fewer bytes than requested are available,
  /// `OUT_OF_RANGE` is returned.
  ///
  /// Other errors may be returned, as documented on Read().
  Result<ByteSpan> ReadExact(ByteSpan const buffer) {
    ByteSpan dest = buffer;
    while (!dest.empty()) {
      auto result = Read(dest);
      if (!result.ok()) {
        return result.status();
      }
      const size_t bytes_read = result->size();
      PW_DASSERT(bytes_read != 0);  // Should never happen, according to API.
      dest = dest.subspan(bytes_read);
    }
    return buffer;
  }

  /// Writes data to this stream. Data is not guaranteed to be fully written out
  /// to final resting place on Write return.
  ///
  /// If the writer is unable to fully accept the input data size it will abort
  /// the write and return `RESOURCE_EXHAUSTED`.
  ///
  /// If the writer has been exhausted and is and can no longer accept
  /// additional bytes it will return `OUT_OF_RANGE`. This is similar to
  /// end-of-file (EOF).  Write will only return `OUT_OF_RANGE` if
  /// ConservativeWriteLimit() is and will remain zero. A Write operation that
  /// is successful and also exhausts the writer returns OK, with all following
  /// calls returning `OUT_OF_RANGE`. When ConservativeWriteLimit() is greater
  /// than zero, a Write that is a number of bytes beyond what will exhaust the
  /// Write will abort and return `RESOURCE_EXHAUSTED` rather than OUT_OF_RANGE
  /// because the writer is still able to write bytes.
  ///
  /// Derived classes should NOT try to override the public Write methods.
  /// Instead, provide an implementation by overriding DoWrite().
  ///
  /// @returns
  /// * @OK: Data was successfully accepted by the stream.
  /// * @UNIMPLEMENTED: This stream does not support writing.
  /// * @FAILED_PRECONDITION: The writer is not in a state to accept data.
  /// * @RESOURCE_EXHAUSTED: The writer was unable to write all of requested
  ///   data at this time. No data was written.
  /// * @OUT_OF_RANGE: The writer has been exhausted, similar to EOF. No
  ///   data was written; no more will be written.
  Status Write(ConstByteSpan data) {
    PW_DASSERT(data.empty() || data.data() != nullptr);
    return DoWrite(data);
  }
  /// @overload
  Status Write(const void* data, size_t size_bytes) {
    return Write(span(static_cast<const std::byte*>(data), size_bytes));
  }
  /// @overload
  Status Write(const std::byte b) { return Write(&b, 1); }

  /// Changes the current position in the stream for both reading and writing,
  /// if supported.
  ///
  /// Seeking to a negative offset is invalid. The behavior when seeking beyond
  /// the end of a stream is determined by the implementation. The
  /// implementation could fail with OUT_OF_RANGE or append bytes to the stream.
  ///
  /// @returns
  /// * @OK: Successfully updated the position.
  /// * @UNIMPLEMENTED: Seeking is not supported for this stream.
  /// * @OUT_OF_RANGE: Attempted to seek beyond the bounds of the stream. The
  ///   position is unchanged.
  Status Seek(ptrdiff_t offset, Whence origin = kBeginning) {
    return DoSeek(offset, origin);
  }

  /// Returns the current position in the stream, if supported. The position is
  /// the offset from the beginning of the stream. Returns
  /// Stream::kUnknownPosition (`size_t(-1)`) if the position is unknown.
  ///
  /// Streams that support seeking from the beginning always support Tell().
  /// Other streams may or may not support Tell().
  size_t Tell() { return DoTell(); }

  /// Liklely (not guaranteed) minimum bytes available to read at this time.
  /// This number is advisory and not guaranteed to read full number of
  /// requested bytes or without a `RESOURCE_EXHAUSTED` or `OUT_OF_RANGE`. As
  /// Reader processes/handles/receives enqueued data or other contexts read
  /// data this number can go up or down for some Readers.
  ///
  /// @retval zero if, in the current state, Read() would not return OkStatus().
  /// @retval kUnlimited if the implementation imposes no limits on read sizes.
  size_t ConservativeReadLimit() const {
    return ConservativeLimit(LimitType::kRead);
  }

  /// Likely (not guaranteed) minimum bytes available to write at this time.
  /// This number is advisory and not guaranteed to write without a
  /// `RESOURCE_EXHAUSTED` or `OUT_OF_RANGE`. As Writer processes/handles
  /// enqueued of other contexts write data this number can go up or down for
  /// some Writers.  Returns zero if, in the current state, Write() would not
  /// return OkStatus().
  ///
  /// Returns kUnlimited if the implementation has no limits on write sizes.
  size_t ConservativeWriteLimit() const {
    return ConservativeLimit(LimitType::kWrite);
  }

 protected:
  // Used to indicate the type of limit being queried in ConservativeLimit.
  enum class LimitType : bool { kRead, kWrite };

 private:
  // The Stream class should not be extended directly, so its constructor is
  // private. To implement a new Stream, extend one of its derived classes.
  friend class Reader;
  friend class RelativeSeekableReader;
  friend class SeekableReader;
  friend class NonSeekableReader;

  friend class Writer;
  friend class RelativeSeekableWriter;
  friend class SeekableWriter;
  friend class NonSeekableWriter;

  friend class ReaderWriter;
  friend class RelativeSeekableReaderWriter;
  friend class SeekableReaderWriter;
  friend class NonSeekableReaderWriter;

  /// Seekability expresses the origins from which the stream always supports
  /// seeking. Seeking from other origins may work, but is not guaranteed.
  ///
  /// Seekability is implemented as a bitfield of Whence values.
  enum class Seekability : uint8_t {
    /// No type of seeking is supported.
    kNone = 0,

    /// Seeking from the current position is supported, but the range may be
    /// limited. For example, a buffered stream might support seeking within the
    /// buffered data, but not before or after.
    kRelative = kCurrent,

    /// The stream supports random access anywhere within the stream.
    kAbsolute = kBeginning | kCurrent | kEnd,
  };

  // These are the virtual methods that are implemented by derived classes. The
  // public API functions call these virtual functions.

  constexpr Stream(bool readable, bool writable, Seekability seekability)
      : readable_(readable), writable_(writable), seekability_(seekability) {}

  /// Virtual Read() function implemented by derived classes.
  virtual StatusWithSize DoRead(ByteSpan destination) = 0;

  /// Virtual Write() function implemented by derived classes.
  virtual Status DoWrite(ConstByteSpan data) = 0;

  /// Virtual Seek() function implemented by derived classes.
  virtual Status DoSeek(ptrdiff_t offset, Whence origin) = 0;

  /// Virtual Tell() function optionally implemented by derived classes.
  /// The default implementation always returns kUnknownPosition.
  virtual size_t DoTell() { return kUnknownPosition; }

  /// Virtual function optionally implemented by derived classes that is used
  /// for ConservativeReadLimit() and ConservativeWriteLimit().
  ///
  /// The default implementation returns kUnlimited or ``0`` depending on
  /// whether the stream is readable/writable.
  virtual size_t ConservativeLimit(LimitType limit_type) const {
    if (limit_type == LimitType::kRead) {
      return readable() ? kUnlimited : 0;
    }
    return writable() ? kUnlimited : 0;
  }

  bool readable_;
  bool writable_;
  Seekability seekability_;
};

/// @}

/// @submodule{pw_stream,interface_reader}

/// A Stream that supports reading but not writing. The Write() method is
/// hidden.
///
/// Use in APIs when:
///   * Must read from, but not write to, a stream.
///   * May or may not need seeking. Use a SeekableReader& if seeking is
///     required.
///
/// Inherit from when:
///   * Reader cannot be extended directly. Instead, extend SeekableReader,
///     NonSeekableReader, or (rarely) RelativeSeekableReader, as appropriate.
///
/// A Reader may or may not support seeking. Check seekable() or try calling
/// Seek() to determine if the stream is seekable.
class Reader : public Stream {
 private:
  friend class RelativeSeekableReader;
  friend class NonSeekableReader;

  constexpr Reader(Seekability seekability)
      : Stream(true, false, seekability) {}

  using Stream::Write;

  Status DoWrite(ConstByteSpan) final { return Status::Unimplemented(); }
};

/// A Reader that supports at least relative seeking within some range of the
/// current position. Seeking beyond that or from other origins may or may not
/// be supported. The extent to which seeking is possible is NOT exposed by this
/// API.
///
/// Use in APIs when:
///   * Relative seeking is required. Usage in APIs should be rare; generally
///     Reader should be used instead.
///
/// Inherit from when:
///   * Implementing a Reader that can only support seeking near the current
///     position.
///
/// A buffered Reader that only supports seeking within its buffer is a good
/// example of a RelativeSeekableReader.
class RelativeSeekableReader : public Reader {
 protected:
  constexpr RelativeSeekableReader()
      : RelativeSeekableReader(Seekability::kRelative) {}

 private:
  friend class SeekableReader;

  constexpr RelativeSeekableReader(Seekability seekability)
      : Reader(seekability) {}
};

/// A Reader that fully supports seeking.
///
/// Use in APIs when:
///   * Absolute seeking is required. Use Reader& if seeking is not required or
///     seek failures can be handled gracefully.
///
/// Inherit from when:
///   * Implementing a reader that supports absolute seeking.
///
class SeekableReader : public RelativeSeekableReader {
 protected:
  constexpr SeekableReader() : RelativeSeekableReader(Seekability::kAbsolute) {}
};

/// A Reader that does not support seeking. The Seek() method is hidden.
///
/// Use in APIs when:
///   * Do NOT use in APIs! If seeking is not required, use Reader& instead.
///
/// Inherit from when:
///   * Implementing a Reader that does not support seeking.
///
class NonSeekableReader : public Reader {
 protected:
  constexpr NonSeekableReader() : Reader(Seekability::kNone) {}

 private:
  using Reader::Seek;

  Status DoSeek(ptrdiff_t, Whence) final { return Status::Unimplemented(); }
};

/// @}

/// @submodule{pw_stream,interface_writer}

/// A Stream that supports writing but not reading. The Read() method is hidden.
///
/// Use in APIs when:
///   * Must write to, but not read from, a stream.
///   * May or may not need seeking. Use a SeekableWriter& if seeking is
///     required.
///
/// Inherit from when:
///   * Writer cannot be extended directly. Instead, extend SeekableWriter,
///     NonSeekableWriter, or (rarely) RelativeSeekableWriter, as appropriate.
///
/// A Writer may or may not support seeking. Check seekable() or try calling
/// Seek() to determine if the stream is seekable.
class Writer : public Stream {
 private:
  friend class RelativeSeekableWriter;
  friend class NonSeekableWriter;

  constexpr Writer(Seekability seekability)
      : Stream(false, true, seekability) {}

  using Stream::Read;

  StatusWithSize DoRead(ByteSpan) final {
    return StatusWithSize::Unimplemented();
  }
};

/// A Writer that supports at least relative seeking within some range of the
/// current position. Seeking beyond that or from other origins may or may not
/// be supported. The extent to which seeking is possible is NOT exposed by this
/// API.
///
/// Use in APIs when:
///   * Relative seeking is required. Usage in APIs should be rare; generally
///     Writer should be used instead.
///
/// Inherit from when:
///   * Implementing a Writer that can only support seeking near the current
///     position.
///
/// A buffered Writer that only supports seeking within its buffer is a good
/// example of a RelativeSeekableWriter.
class RelativeSeekableWriter : public Writer {
 protected:
  constexpr RelativeSeekableWriter()
      : RelativeSeekableWriter(Seekability::kRelative) {}

 private:
  friend class SeekableWriter;

  constexpr RelativeSeekableWriter(Seekability seekability)
      : Writer(seekability) {}
};

/// A Writer that fully supports seeking.
///
/// Use in APIs when:
///   * Absolute seeking is required. Use Writer& if seeking is not required or
///     seek failures can be handled gracefully.
///
/// Inherit from when:
///   * Implementing a writer that supports absolute seeking.
///
class SeekableWriter : public RelativeSeekableWriter {
 protected:
  constexpr SeekableWriter() : RelativeSeekableWriter(Seekability::kAbsolute) {}
};

/// A Writer that does not support seeking. The Seek() method is hidden.
///
/// Use in APIs when:
///   * Do NOT use in APIs! If seeking is not required, use Writer& instead.
///
/// Inherit from when:
///   * Implementing a Writer that does not support seeking.
///
class NonSeekableWriter : public Writer {
 protected:
  constexpr NonSeekableWriter() : Writer(Seekability::kNone) {}

 private:
  using Writer::Seek;

  Status DoSeek(ptrdiff_t, Whence) final { return Status::Unimplemented(); }
};

/// @}

/// @submodule{pw_stream,interface_readerwriter}

/// A Stream that supports both reading and writing.
///
/// Use in APIs when:
///   * Must both read from and write to a stream.
///   * May or may not need seeking. Use a SeekableReaderWriter& if seeking is
///     required.
///
/// Inherit from when:
///   * Cannot extend ReaderWriter directly. Instead, extend
///     SeekableReaderWriter, NonSeekableReaderWriter, or (rarely)
///     RelativeSeekableReaderWriter, as appropriate.
///
/// A ReaderWriter may or may not support seeking. Check seekable() or try
/// calling Seek() to determine if the stream is seekable.
class ReaderWriter : public Stream {
 public:
  // ReaderWriters may be used as Readers.
  Reader& as_reader() { return internal::SiblingCast<Reader&, Stream>(*this); }
  const Reader& as_reader() const {
    return internal::SiblingCast<const Reader&, Stream>(*this);
  }

  operator Reader&() { return as_reader(); }
  operator const Reader&() const { return as_reader(); }

  // ReaderWriters may be used as Writers.
  Writer& as_writer() { return internal::SiblingCast<Writer&, Stream>(*this); }
  const Writer& as_writer() const {
    return internal::SiblingCast<const Writer&, Stream>(*this);
  }

  operator Writer&() { return as_writer(); }
  operator const Writer&() const { return as_writer(); }

 private:
  friend class RelativeSeekableReaderWriter;
  friend class NonSeekableReaderWriter;

  constexpr ReaderWriter(Seekability seekability)
      : Stream(true, true, seekability) {}
};

/// A ReaderWriter that supports at least relative seeking within some range of
/// the current position. Seeking beyond that or from other origins may or may
/// not be supported. The extent to which seeking is possible is NOT exposed by
/// this API.
///
/// Use in APIs when:
///   * Relative seeking is required. Usage in APIs should be rare; generally
///     ReaderWriter should be used instead.
///
/// Inherit from when:
///   * Implementing a ReaderWriter that can only support seeking near the
///     current position.
///
/// A buffered ReaderWriter that only supports seeking within its buffer is a
/// good example of a RelativeSeekableReaderWriter.
class RelativeSeekableReaderWriter : public ReaderWriter {
 public:
  // RelativeSeekableReaderWriters may be used as RelativeSeekableReaders or
  // RelativeSeekableWriters.
  operator RelativeSeekableReader&() {
    return internal::SiblingCast<RelativeSeekableReader&, Stream>(*this);
  }
  operator const RelativeSeekableReader&() const {
    return internal::SiblingCast<const RelativeSeekableReader&, Stream>(*this);
  }
  operator RelativeSeekableWriter&() {
    return internal::SiblingCast<RelativeSeekableWriter&, Stream>(*this);
  }
  operator const RelativeSeekableWriter&() const {
    return internal::SiblingCast<const RelativeSeekableWriter&, Stream>(*this);
  }

 protected:
  constexpr RelativeSeekableReaderWriter()
      : ReaderWriter(Seekability::kRelative) {}

 private:
  friend class SeekableReaderWriter;

  constexpr RelativeSeekableReaderWriter(Seekability seekability)
      : ReaderWriter(seekability) {}
};

/// A ReaderWriter that fully supports seeking.
///
/// Use in APIs when:
///   * Absolute seeking is required. Use ReaderWriter& if seeking is not
///     required or seek failures can be handled gracefully.
///
/// Inherit from when:
///   * Implementing a writer that supports absolute seeking.
///
class SeekableReaderWriter : public RelativeSeekableReaderWriter {
 public:
  // SeekableReaderWriters may be used as SeekableReaders.
  SeekableReader& as_seekable_reader() {
    return internal::SiblingCast<SeekableReader&, Stream>(*this);
  }
  const SeekableReader& as_seekable_reader() const {
    return internal::SiblingCast<const SeekableReader&, Stream>(*this);
  }

  operator SeekableReader&() { return as_seekable_reader(); }
  operator const SeekableReader&() const { return as_seekable_reader(); }

  // SeekableReaderWriters may be used as SeekableWriters.
  SeekableWriter& as_seekable_writer() {
    return internal::SiblingCast<SeekableWriter&, Stream>(*this);
  }
  const SeekableWriter& as_seekable_writer() const {
    return internal::SiblingCast<const SeekableWriter&, Stream>(*this);
  }

  operator SeekableWriter&() { return as_seekable_writer(); }
  operator const SeekableWriter&() const { return as_seekable_writer(); }

 protected:
  constexpr SeekableReaderWriter()
      : RelativeSeekableReaderWriter(Seekability::kAbsolute) {}
};

/// A ReaderWriter that does not support seeking. The Seek() method is hidden.
///
/// Use in APIs when:
///   * Do NOT use in APIs! If seeking is not required, use ReaderWriter&
///     instead.
///
/// Inherit from when:
///   * Implementing a ReaderWriter that does not support seeking.
///
class NonSeekableReaderWriter : public ReaderWriter {
 public:
  // NonSeekableReaderWriters may be used as either NonSeekableReaders or
  // NonSeekableWriters. Note that NonSeekableReaderWriter& generally should not
  // be used in APIs, which should accept ReaderWriter& instead.
  operator NonSeekableReader&() {
    return internal::SiblingCast<NonSeekableReader&, Stream>(*this);
  }
  operator const NonSeekableReader&() const {
    return internal::SiblingCast<const NonSeekableReader&, Stream>(*this);
  }
  operator NonSeekableWriter&() {
    return internal::SiblingCast<NonSeekableWriter&, Stream>(*this);
  }
  operator const NonSeekableWriter&() const {
    return internal::SiblingCast<const NonSeekableWriter&, Stream>(*this);
  }

 protected:
  constexpr NonSeekableReaderWriter() : ReaderWriter(Seekability::kNone) {}

 private:
  using ReaderWriter::Seek;

  Status DoSeek(ptrdiff_t, Whence) final { return Status::Unimplemented(); }
};

/// @}

}  // namespace pw::stream
