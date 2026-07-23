# Copyright 2021 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Tests for pw_symbolizer's python tooling."""

import unittest
import pw_symbolizer


class TestSymbolFormatting(unittest.TestCase):
    """Tests Symbol objects to validate formatted output."""

    def test_blank_symbol(self):
        sym = pw_symbolizer.Symbol(address=0x00000000, name='', file='', line=0)
        self.assertEqual('??:?', sym.file_and_line())
        self.assertEqual('0x00000000 (??:?)', str(sym))

    def test_default_symbol(self):
        sym = pw_symbolizer.Symbol(address=0x0000A400)
        self.assertEqual('??:?', sym.file_and_line())
        self.assertEqual('0x0000A400 (??:?)', str(sym))

    def test_to_str(self):
        sym = pw_symbolizer.Symbol(
            address=0x12345678,
            name='idle_thread_context',
            file='device/system/threads.cc',
            line=59,
        )
        self.assertEqual('device/system/threads.cc:59', sym.file_and_line())
        self.assertEqual(
            'idle_thread_context (device/system/threads.cc:59)', str(sym)
        )

    def test_truncated_filename(self):
        sym = pw_symbolizer.Symbol(
            address=0x12345678,
            name='idle_thread_context',
            file='device/system/threads.cc',
            line=59,
        )
        self.assertEqual(
            'idle_thread_context ([...]stem/threads.cc:59)',
            sym.to_string(max_filename_len=15),
        )

    def test_64bit_blank_symbol(self):
        """Tests hex_width auto-detection and explicit overrides for Symbol."""
        sym_64 = pw_symbolizer.Symbol(
            address=0x00007FFF80001000, name='', file='', line=0
        )
        # Test auto-detection (64-bit -> 16 hex digits)
        self.assertEqual('0x00007FFF80001000 (??:?)', sym_64.to_string())
        self.assertEqual('0x00007FFF80001000 (??:?)', str(sym_64))

        # Test custom hex_width override
        self.assertEqual(
            '0x7FFF80001000 (??:?)', sym_64.to_string(hex_width=12)
        )

        # Test 32-bit auto-detection and explicit hex_width
        sym_32 = pw_symbolizer.Symbol(address=0x0000A400)
        self.assertEqual('0x0000A400 (??:?)', sym_32.to_string())
        self.assertEqual(
            '0x00000000A400 (??:?)', sym_32.to_string(hex_width=12)
        )


class TestFakeSymbolizer(unittest.TestCase):
    """Tests the FakeSymbolizer class."""

    def test_empty_db(self):
        symbolizer = pw_symbolizer.FakeSymbolizer()
        symbol = symbolizer.symbolize(0x404)
        self.assertEqual(symbol.address, 0x404)
        self.assertEqual(symbol.name, '')
        self.assertEqual(symbol.file, '')

    def test_db_with_entries(self):
        known_symbols = (
            pw_symbolizer.Symbol(
                0x404, 'do_a_flip(int n)', 'source/tricks.cc', 1403
            ),
            pw_symbolizer.Symbol(
                0xFFFFFFFF,
                'a_variable_here_would_be_funny',
                'source/globals.cc',
                21,
            ),
        )
        symbolizer = pw_symbolizer.FakeSymbolizer(known_symbols)

        symbol = symbolizer.symbolize(0x404)
        self.assertEqual(symbol.address, 0x404)
        self.assertEqual(symbol.name, 'do_a_flip(int n)')
        self.assertEqual(symbol.file, 'source/tricks.cc')
        self.assertEqual(symbol.line, 1403)

        symbol = symbolizer.symbolize(0xFFFFFFFF)
        self.assertEqual(symbol.address, 0xFFFFFFFF)
        self.assertEqual(symbol.name, 'a_variable_here_would_be_funny')
        self.assertEqual(symbol.file, 'source/globals.cc')
        self.assertEqual(symbol.line, 21)


class TestReturnAddressAdjustment(unittest.TestCase):
    """Tests architecture-aware return address adjustment for parent frames."""

    def test_adjust_return_address_thumb(self):
        """Tests Thumb mode LSB bit clearing and 2-byte offset subtraction."""
        symbolizer = pw_symbolizer.FakeSymbolizer()
        # ARM Thumb return address 0x00323625 (odd address LSB=1)
        # Should clear Thumb bit -> 0x00323624, subtract 2 -> 0x00323622
        adjusted = symbolizer.adjust_return_address(
            0x00323625, pw_symbolizer.CpuArchitecture.ARMV8M
        )
        self.assertEqual(adjusted, 0x00323622)

    def test_adjust_return_address_architectures(self):
        """Tests parent frame return address offsets across architectures."""
        symbolizer = pw_symbolizer.FakeSymbolizer()

        # ARM64: subtract 4
        self.assertEqual(
            symbolizer.adjust_return_address(
                0x4000, pw_symbolizer.CpuArchitecture.AARCH64
            ),
            0x3FFC,
        )

        # X86_64: subtract 1
        self.assertEqual(
            symbolizer.adjust_return_address(
                0x4000, pw_symbolizer.CpuArchitecture.X86_64
            ),
            0x3FFF,
        )

        # XTENSA: subtract 2
        self.assertEqual(
            symbolizer.adjust_return_address(
                0x4000, pw_symbolizer.CpuArchitecture.XTENSA
            ),
            0x3FFE,
        )

    def test_dump_stack_trace_parent_frame_adjustment(self):
        """Tests stack trace formatting with parent frame adjustment."""
        known_symbols = (
            # Active PC (Frame 1)
            pw_symbolizer.Symbol(0x084E3330, 'foo::Crash()', 'crash.cc', 15),
            # Caller address after adjustment (0x00323625 -> 0x00323622)
            pw_symbolizer.Symbol(
                0x00323622,
                'foo::HandleCrashInterrupt()',
                'crash_handler.cc',
                35,
            ),
        )
        symbolizer = pw_symbolizer.FakeSymbolizer(known_symbols)
        addresses = [0x084E3330, 0x00323625]

        stack_trace = symbolizer.dump_stack_trace(
            addresses,
            cpu_arch=pw_symbolizer.CpuArchitecture.ARMV8M,
            adjust_parent_frames=True,
        )

        # Verify frame 1 displays exact PC symbol
        self.assertIn('foo::Crash() (0x084E3330)', stack_trace)

        # Verify frame 2 displays raw address 0x00323625 but
        # symbolized caller function name
        self.assertIn('foo::HandleCrashInterrupt() (0x00323625)', stack_trace)
        self.assertIn('crash_handler.cc:35', stack_trace)

    def test_fallback_when_cpu_arch_unset(self):
        """Tests fallback behavior when cpu_arch is not set on symbolizer."""
        # Symbolizer with no cpu_arch set
        symbolizer = pw_symbolizer.FakeSymbolizer()

        # Odd address -> falls back to default 0 byte offset
        self.assertEqual(symbolizer.adjust_return_address(0x4001), 0x4001)

        # Even address -> falls back to default 0 byte offset
        self.assertEqual(symbolizer.adjust_return_address(0x4000), 0x4000)

    def test_instance_attribute_cpu_arch(self):
        """Tests setting cpu_arch as an instance attribute."""
        symbolizer = pw_symbolizer.FakeSymbolizer()

        # Set self.cpu_arch as string on instance
        symbolizer.cpu_arch = 'arm64'
        self.assertEqual(symbolizer.adjust_return_address(0x4000), 0x3FFC)

        # Set self.cpu_arch as enum on instance
        symbolizer.cpu_arch = pw_symbolizer.CpuArchitecture.XTENSA
        self.assertEqual(symbolizer.adjust_return_address(0x4000), 0x3FFE)

    def test_underflow_protection(self):
        """Tests protection against underflow for zero or small addresses."""
        symbolizer = pw_symbolizer.FakeSymbolizer()
        self.assertEqual(
            symbolizer.adjust_return_address(
                0x00000000, pw_symbolizer.CpuArchitecture.AARCH64
            ),
            0,
        )
        self.assertEqual(
            symbolizer.adjust_return_address(
                0x00000001, pw_symbolizer.CpuArchitecture.ARMV8M
            ),
            0,
        )

    def test_non_arm_odd_address(self):
        """Tests odd address handling on non-ARM architectures (e.g. RISC-V)."""
        symbolizer = pw_symbolizer.FakeSymbolizer()
        # RISCV32 with odd address 0x4001 should subtract 2 (0x3FFF)
        # (safe for 16-bit compressed instructions) without ARM Thumb clearing.
        self.assertEqual(
            symbolizer.adjust_return_address(
                0x4001, pw_symbolizer.CpuArchitecture.RISCV32
            ),
            0x3FFF,
        )

    def test_cpu_architecture_from_value(self):
        """Tests CpuArchitecture.from_value parsing strings and types."""
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('armv7-m'),
            pw_symbolizer.CpuArchitecture.ARMV7M,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('x86-64'),
            pw_symbolizer.CpuArchitecture.X86_64,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('CpuArchitecture.ARM64'),
            pw_symbolizer.CpuArchitecture.AARCH64,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('aarch64'),
            pw_symbolizer.CpuArchitecture.AARCH64,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('arm64'),
            pw_symbolizer.CpuArchitecture.AARCH64,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('arm'),
            pw_symbolizer.CpuArchitecture.AARCH32,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('aarch32'),
            pw_symbolizer.CpuArchitecture.AARCH32,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('armv7a'),
            pw_symbolizer.CpuArchitecture.ARMV7A,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('rv32e'),
            pw_symbolizer.CpuArchitecture.RISCV32,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('rv32i'),
            pw_symbolizer.CpuArchitecture.RISCV32,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('riscv32'),
            pw_symbolizer.CpuArchitecture.RISCV32,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('rv64e'),
            pw_symbolizer.CpuArchitecture.RISCV64,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('rv64i'),
            pw_symbolizer.CpuArchitecture.RISCV64,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value('riscv64'),
            pw_symbolizer.CpuArchitecture.RISCV64,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value(True),
            pw_symbolizer.CpuArchitecture.UNKNOWN,
        )
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value(False),
            pw_symbolizer.CpuArchitecture.UNKNOWN,
        )

        # Backwards-compatibility alias test
        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_str('armv7-m'),
            pw_symbolizer.CpuArchitecture.ARMV7M,
        )

        # Protobuf enum object carrying a .name attribute
        class FakeProtoEnum:
            name = 'ARMV8M'

        self.assertEqual(
            pw_symbolizer.CpuArchitecture.from_value(FakeProtoEnum()),
            pw_symbolizer.CpuArchitecture.ARMV8M,
        )

    def test_is_arm_32bit(self):
        """Tests CpuArchitecture.is_arm_32bit() helper method."""
        self.assertTrue(pw_symbolizer.CpuArchitecture.ARMV7M.is_arm_32bit())
        self.assertTrue(pw_symbolizer.CpuArchitecture.ARMV8M.is_arm_32bit())
        self.assertTrue(pw_symbolizer.CpuArchitecture.ARMV7A.is_arm_32bit())
        self.assertTrue(pw_symbolizer.CpuArchitecture.AARCH32.is_arm_32bit())
        self.assertFalse(pw_symbolizer.CpuArchitecture.AARCH64.is_arm_32bit())
        self.assertFalse(pw_symbolizer.CpuArchitecture.X86_64.is_arm_32bit())

    def test_adjust_return_address_aarch32(self):
        """Tests AArch32 and ARMv7A return address calculation."""
        symbolizer = pw_symbolizer.FakeSymbolizer()
        # AArch32 in ARM mode (even address LSB=0): subtracts 4 bytes
        adjusted_arm = symbolizer.adjust_return_address(
            0x00004000, pw_symbolizer.CpuArchitecture.AARCH32
        )
        self.assertEqual(adjusted_arm, 0x00003FFC)

        # AArch32 in Thumb mode (odd address LSB=1): clears Thumb bit and
        # subtracts 2 bytes
        adjusted_thumb = symbolizer.adjust_return_address(
            0x00004005, pw_symbolizer.CpuArchitecture.AARCH32
        )
        self.assertEqual(adjusted_thumb, 0x00004002)

        # ARMv7A in ARM mode (even address LSB=0): subtracts 4 bytes
        adjusted_armv7a = symbolizer.adjust_return_address(
            0x00004000, pw_symbolizer.CpuArchitecture.ARMV7A
        )
        self.assertEqual(adjusted_armv7a, 0x00003FFC)

        # ARMv7M (Cortex-M Thumb mode): clears Thumb bit and subtracts 2 bytes
        adjusted_armv7m = symbolizer.adjust_return_address(
            0x00004005, pw_symbolizer.CpuArchitecture.ARMV7M
        )
        self.assertEqual(adjusted_armv7m, 0x00004002)

    def test_dump_stack_trace_64bit_hex_formatting(self):
        """Tests 64-bit address hex formatting in dump_stack_trace."""
        known_symbols = (
            pw_symbolizer.Symbol(
                0x00007FFF80001000, 'foo::Main()', 'main.cc', 10
            ),
        )
        symbolizer = pw_symbolizer.FakeSymbolizer(known_symbols)
        addresses = [0x00007FFF80001000]

        stack_trace = symbolizer.dump_stack_trace(
            addresses,
            cpu_arch=pw_symbolizer.CpuArchitecture.X86_64,
        )
        # 64-bit addresses formatted as 16 hex digits
        self.assertIn('foo::Main() (0x00007FFF80001000)', stack_trace)

    def test_dump_stack_trace_adjust_parent_frames_false(self):
        """Tests disabling parent frame return address adjustments."""
        known_symbols = (
            # Symbol mapped to exact unadjusted return address 0x00323625
            pw_symbolizer.Symbol(
                0x00323625,
                'foo::UnadjustedCaller()',
                'caller.cc',
                99,
            ),
        )
        symbolizer = pw_symbolizer.FakeSymbolizer(known_symbols)
        addresses = [0x084E3330, 0x00323625]

        stack_trace = symbolizer.dump_stack_trace(
            addresses,
            cpu_arch=pw_symbolizer.CpuArchitecture.ARMV8M,
            adjust_parent_frames=False,
        )
        # Parent frame 2 uses raw 0x00323625 directly without subtraction
        self.assertIn('foo::UnadjustedCaller() (0x00323625)', stack_trace)
        self.assertIn('caller.cc:99', stack_trace)

    def test_most_recent_first_false(self):
        """Tests stack trace formatting when most_recent_first=False."""
        known_symbols = (
            pw_symbolizer.Symbol(
                0x00323622,
                'foo::HandleCrashInterrupt()',
                'crash_handler.cc',
                35,
            ),
            pw_symbolizer.Symbol(0x084E3330, 'foo::Crash()', 'crash.cc', 15),
        )
        symbolizer = pw_symbolizer.FakeSymbolizer(known_symbols)
        # Order: caller first (0x00323625), leaf last (0x084E3330)
        addresses = [0x00323625, 0x084E3330]

        stack_trace = symbolizer.dump_stack_trace(
            addresses,
            most_recent_first=False,
            cpu_arch=pw_symbolizer.CpuArchitecture.ARMV8M,
            adjust_parent_frames=True,
        )
        # Frame 1 is parent frame (adjusted 0x00323625 -> 0x00323622)
        self.assertIn('foo::HandleCrashInterrupt() (0x00323625)', stack_trace)
        # Frame 2 is leaf frame (raw 0x084E3330)
        self.assertIn('foo::Crash() (0x084E3330)', stack_trace)


if __name__ == '__main__':
    unittest.main()
