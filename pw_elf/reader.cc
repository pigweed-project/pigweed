// Copyright 2024 The Pigweed Authors
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

#include "pw_elf/reader.h"

#include <string>

#include "pw_bytes/endian.h"
#include "pw_log/log.h"
#include "pw_status/try.h"

namespace pw::elf {
namespace {

using ElfIdent = std::array<unsigned char, EI_NIDENT>;

Result<pw::endian> ElfIdentGetEndian(const ElfIdent& e_ident) {
  switch (e_ident[EI_DATA]) {
    case ELFDATA2LSB:
      // Encoding ELFDATA2LSB specifies 2's complement values, with the least
      // significant byte occupying the lowest address.
      return pw::endian::little;
    case ELFDATA2MSB:
      // Encoding ELFDATA2MSB specifies 2's complement values, with the most
      // significant byte occupying the lowest address.
      return pw::endian::big;
    default:
      return Status::OutOfRange();
  }
}

enum class ElfClass {
  kElf32,
  kElf64,
};

Result<ElfClass> ElfIdentGetElfClass(const ElfIdent& e_ident) {
  switch (e_ident[EI_CLASS]) {
    case ELFCLASS32:
      return ElfClass::kElf32;
    case ELFCLASS64:
      return ElfClass::kElf64;
    default:
      return Status::OutOfRange();
  }
}

Result<internal::ElfReaderImpls> MakeReaderImpl(
    ElfClass elf_class, stream::SeekableReader& stream) {
  switch (elf_class) {
    case ElfClass::kElf32:
      return internal::ElfReaderImpl32::FromStream(stream);
#if UINTPTR_MAX == UINT64_MAX
    case ElfClass::kElf64:
      return internal::ElfReaderImpl64::FromStream(stream);
#endif
    default:
      return Status::Unimplemented();
  }
}

}  // namespace

Result<ElfReader> ElfReader::FromStream(stream::SeekableReader& stream) {
  PW_TRY(stream.Seek(0));

  // Read the e_ident field of the ELF header.
  PW_TRY_ASSIGN(ElfIdent e_ident, internal::ReadObject<ElfIdent>(stream));
  PW_TRY(stream.Seek(0));

  // Validate e_ident.
  if (!(e_ident[EI_MAG0] == ELFMAG0 && e_ident[EI_MAG1] == ELFMAG1 &&
        e_ident[EI_MAG2] == ELFMAG2 && e_ident[EI_MAG3] == ELFMAG3)) {
    PW_LOG_ERROR("Invalid ELF magic bytes");
    return Status::DataLoss();
  }

  auto endian = ElfIdentGetEndian(e_ident);
  if (!endian.ok()) {
    return Status::DataLoss();
  }
  if (endian.value() != pw::endian::native) {
    // Only native endian is supported.
    PW_LOG_ERROR("Non-native ELF endian not supported");
    return Status::Unimplemented();
  }

  auto elf_class = ElfIdentGetElfClass(e_ident);
  if (!elf_class.ok()) {
    return Status::DataLoss();
  }

  PW_TRY_ASSIGN(auto impl, MakeReaderImpl(elf_class.value(), stream));
  return ElfReader(std::move(impl));
}

Result<std::vector<std::byte>> ElfReader::ReadSection(std::string_view name) {
  StatusWithSize size = SeekToSection(name);
  if (!size.ok()) {
    return size.status();
  }

  std::vector<std::byte> data;
  data.resize(size.size());
  PW_TRY(stream().ReadExact(data));

  return data;
}

namespace internal {

// TODO(jrreinhart): Move to pw_stream
Result<std::string> ReadNullTermString(stream::Reader& stream) {
  std::string result;
  while (true) {
    PW_TRY_ASSIGN(char c, ReadObject<char>(stream));
    if (c == '\0') {
      break;
    }
    result += c;
  }
  return result;
}

}  // namespace internal
}  // namespace pw::elf
