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
#pragma once

#include <math.h>
#include <stdint.h>

#include <cinttypes>
#include <cstdint>

/// Graphic display and framebuffer library
namespace pw::display {

/// @module{pw_display}

/// @defgroup pw_display_color Color
/// Color conversion functions used by the pw_display draw library and
/// tests.
/// @{

/// Base type for pixels in RGBA8888 format.
using ColorRgba8888 = uint32_t;

/// Base type for pixels in RGB565 format.
using ColorRgb565 = uint16_t;

/// Encode an RGB565 value from individual red, green, blue and alpha
/// values.
///
/// This will introduce some loss in color as values are mapped from 8
/// bits per color down to 5 for red, 6 for green, and 5 for blue.
constexpr ColorRgb565 EncodeRgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<ColorRgb565>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                                  ((b & 0xF8) >> 3));
}

/// Encode an RGBA8888 value into RGB565.
constexpr ColorRgb565 EncodeRgb565(ColorRgba8888 rgba8888) {
  uint8_t r = static_cast<uint8_t>(rgba8888 & 0xFF);
  uint8_t g = static_cast<uint8_t>((rgba8888 & 0xFF00) >> 8);
  uint8_t b = static_cast<uint8_t>((rgba8888 & 0xFF0000) >> 16);
  // Alpha is ignored for RGB565.
  return EncodeRgb565(r, g, b);
}

/// Encode an RGBA8888 value from individual red, green, blue and
/// alpha values.
constexpr ColorRgba8888 EncodeRgba8888(uint8_t r,
                                       uint8_t g,
                                       uint8_t b,
                                       uint8_t a) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
         (static_cast<uint32_t>(g) << 8) | r;
}

/// Encode an RGBA8888 value from RGB565.
///
/// This will scale each color up to 8 bits per pixel. Red and blue
/// are scaled from 5 bits to 8 bits. Green from 6 bits to 8
/// bits. There is no alpha channel in the RGB565 format so alpha is
/// set to 255 representing 100% opaque.
constexpr ColorRgba8888 EncodeRgba8888(ColorRgb565 rgb565) {
  uint8_t r = static_cast<uint8_t>(255 * ((rgb565 & 0xF800) >> 11) / 31);
  uint8_t g = static_cast<uint8_t>(255 * ((rgb565 & 0x7E0) >> 5) / 63);
  uint8_t b = static_cast<uint8_t>(255 * (rgb565 & 0x1F) / 31);
  uint8_t a = 255;
  return EncodeRgba8888(r, g, b, a);
}

/// @}

}  // namespace pw::display
