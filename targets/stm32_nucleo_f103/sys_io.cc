// Copyright 2026 The Pigweed Authors
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

#include "pw_sys_io/sys_io.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "stm32_nucleo_f103/init.h"

namespace {

constexpr uint32_t kBaudRate = 115200u;

constexpr uintptr_t kRccApb2EnrAddr = 0x40021018u;
constexpr uintptr_t kRccApb1EnrAddr = 0x4002101Cu;
constexpr uint32_t kRccApb2EnrIopaen = 1u << 2;
constexpr uint32_t kRccApb1EnrUsart2en = 1u << 17;

struct GpioBlock {
  volatile uint32_t crl;
  volatile uint32_t crh;
  volatile uint32_t idr;
  volatile uint32_t odr;
  volatile uint32_t bsrr;
  volatile uint32_t brr;
  volatile uint32_t lckr;
};

struct UsartBlock {
  volatile uint32_t sr;
  volatile uint32_t dr;
  volatile uint32_t brr;
  volatile uint32_t cr1;
  volatile uint32_t cr2;
  volatile uint32_t cr3;
  volatile uint32_t gtpr;
};

constexpr uintptr_t kGpioABase = 0x40010800u;
constexpr uintptr_t kUsart2Base = 0x40004400u;

// USART SR flags.
constexpr uint32_t kUsartSrOre = 1u << 3;
constexpr uint32_t kUsartSrRxne = 1u << 5;
constexpr uint32_t kUsartSrTxe = 1u << 7;

// USART CR1 flags.
constexpr uint32_t kUsartCr1Re = 1u << 2;
constexpr uint32_t kUsartCr1Te = 1u << 3;
constexpr uint32_t kUsartCr1Ue = 1u << 13;

GpioBlock& GpioA() { return *reinterpret_cast<GpioBlock*>(kGpioABase); }
UsartBlock& Usart2() { return *reinterpret_cast<UsartBlock*>(kUsart2Base); }

}  // namespace

namespace pw::stm32_nucleo_f103 {

void SysIoInit() {
  auto& apb2enr = *reinterpret_cast<volatile uint32_t*>(kRccApb2EnrAddr);
  auto& apb1enr = *reinterpret_cast<volatile uint32_t*>(kRccApb1EnrAddr);

  apb2enr |= kRccApb2EnrIopaen;
  // Errata ES096: read back RCC registers after enabling peripheral clocks.
  [[maybe_unused]] const uint32_t apb2enr_sync = apb2enr;
  apb1enr |= kRccApb1EnrUsart2en;
  [[maybe_unused]] const uint32_t apb1enr_sync = apb1enr;

  // PA2 = USART2_TX (Alternate Function Push-Pull, 50 MHz -> 0xB).
  // PA3 = USART2_RX (Input Pull-Up -> 0x8 with ODR3 = 1).
  GpioA().bsrr = (1u << 3);
  uint32_t crl = GpioA().crl;
  crl &= ~((0xFu << 8) | (0xFu << 12));
  crl |= (0xBu << 8) | (0x8u << 12);
  GpioA().crl = crl;

  Usart2().cr1 = 0u;
  Usart2().cr2 = 0u;
  Usart2().cr3 = 0u;

  const uint32_t apb1_hz = GetApb1ClockHz();
  Usart2().brr = (apb1_hz + (kBaudRate / 2u)) / kBaudRate;
  Usart2().cr1 = kUsartCr1Ue | kUsartCr1Te | kUsartCr1Re;
}

}  // namespace pw::stm32_nucleo_f103

namespace pw::sys_io {

Status ReadByte(std::byte* dest) {
  while (true) {
    if (TryReadByte(dest).ok()) {
      return OkStatus();
    }
  }
}

Status TryReadByte(std::byte* dest) {
  const uint32_t sr = Usart2().sr;
  if ((sr & kUsartSrRxne) != 0u) {
    *dest = static_cast<std::byte>(Usart2().dr & 0xFFu);
    return OkStatus();
  }
  if ((sr & kUsartSrOre) != 0u) {
    // Clear overrun error by reading DR after SR.
    static_cast<void>(Usart2().dr);
  }
  return Status::Unavailable();
}

Status WriteByte(std::byte b) {
  while ((Usart2().sr & kUsartSrTxe) == 0u) {
  }
  Usart2().dr = static_cast<uint32_t>(b);
  return OkStatus();
}

StatusWithSize WriteLine(std::string_view s) {
  size_t chars_written = 0;
  StatusWithSize result = WriteBytes(as_bytes(span(s)));
  if (!result.ok()) {
    return result;
  }
  chars_written += result.size();

  result = WriteBytes(as_bytes(span("\r\n", 2)));
  chars_written += result.size();

  return StatusWithSize(result.status(), chars_written);
}

}  // namespace pw::sys_io
