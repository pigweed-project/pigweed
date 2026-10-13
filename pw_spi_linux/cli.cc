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

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>

#include <cctype>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pw_log/log.h"
#include "pw_preprocessor/util.h"
#include "pw_result/result.h"
#include "pw_spi_linux/spi.h"
#include "pw_status/status.h"

namespace pw::spi {
namespace {

constexpr unsigned int kDefaultMode = 0;
constexpr unsigned int kDefaultBits = 8;

const struct option kLongOptions[] = {
    {"bits", required_argument, nullptr, 'b'},
    {"device", required_argument, nullptr, 'D'},
    {"freq", required_argument, nullptr, 'F'},
    {"human", no_argument, nullptr, 'h'},
    {"input", required_argument, nullptr, 'i'},
    {"lsb", no_argument, nullptr, 'l'},
    {"mode", required_argument, nullptr, 'm'},
    {"output", required_argument, nullptr, 'o'},
    {"rx-count", required_argument, nullptr, 'r'},
    {},  // terminator
};

// Starts with ':' to skip printing errors and return ':' for a missing option
// argument.
const char* kShortOptions = ":b:D:F:hi:lm:o:r:";

void Usage() {
  std::cerr << "Usage: pw_spi_linux_cli -D DEVICE -F FREQ [flags]" << std::endl;
  std::cerr << "Required flags:" << std::endl;
  std::cerr << "  -D/--device   SPI device path (e.g. /dev/spidev0.0"
            << std::endl;
  std::cerr << "  -F/--freq     SPI clock frequency in Hz (e.g. 24000000)"
            << std::endl;
  std::cerr << std::endl;
  std::cerr << "Optional flags:" << std::endl;
  std::cerr << "  -b/--bits     Bits per word, default: " << kDefaultBits
            << std::endl;
  std::cerr << "  -h/--human    Human-readable output (default: binary, "
               "unless output to stdout tty)"
            << std::endl;
  std::cerr << "  -i/--input    Input file, or - for stdin" << std::endl;
  std::cerr << "                If not given, no data is sent." << std::endl;
  std::cerr << "  -l/--lsb      LSB first (default: MSB first)" << std::endl;
  std::cerr << "  -m/--mode     SPI mode (0-3), default: " << kDefaultMode
            << std::endl;
  std::cerr << "  -o/--output   Output file (default: stdout)" << std::endl;
  std::cerr << "  -r/--rx-count Number of bytes to receive (defaults to size "
               "of input)"
            << std::endl;
}

struct Args {
  std::string device;
  unsigned int frequency = 0;
  std::optional<std::string> input_path;
  std::string output_path = "-";
  bool human_readable = false;
  std::optional<unsigned int> rx_count;

  unsigned int mode = kDefaultMode;
  unsigned int bits = kDefaultBits;
  bool lsb_first = false;

  Config GetSpiConfig() const {
    return {
        .polarity = (mode & 0b10) ? ClockPolarity::kActiveLow
                                  : ClockPolarity::kActiveHigh,
        .phase =
            (mode & 0b01) ? ClockPhase::kFallingEdge : ClockPhase::kRisingEdge,
        .bits_per_word = static_cast<uint8_t>(bits),
        .bit_order = lsb_first ? BitOrder::kLsbFirst : BitOrder::kMsbFirst,
    };
  }
};

template <class T>
std::optional<T> ParseNumber(std::string_view str) {
  T value{};
  const auto* str_end = str.data() + str.size();
  auto [ptr, ec] = std::from_chars(str.data(), str_end, value);
  if (ec == std::errc() && ptr == str_end) {
    return value;
  }
  return std::nullopt;
}

Result<Args> ParseArgs(int argc, char* argv[]) {
  Args args;
  bool human_readable_given;

  while (true) {
    int current_optind = optind;
    int c = getopt_long(argc, argv, kShortOptions, kLongOptions, nullptr);
    if (c == -1) {
      break;
    }

    switch (c) {
      case 'b': {
        auto bits = ParseNumber<unsigned int>(optarg);
        if (bits > 32) {
          PW_LOG_ERROR("Invalid bits : %s", optarg);
          return Status::InvalidArgument();
        }
        args.bits = bits.value();
        break;
      }
      case 'D':
        args.device = optarg;
        break;
      case 'F': {
        auto freq = ParseNumber<unsigned int>(optarg);
        if (!freq || freq.value() == 0) {
          PW_LOG_ERROR("Invalid frequency: %s", optarg);
          return Status::InvalidArgument();
        }
        args.frequency = freq.value();
        break;
      }
      case 'h':
        human_readable_given = true;
        break;
      case 'i':
        args.input_path = optarg;
        break;
      case 'l':
        args.lsb_first = true;
        break;
      case 'm': {
        auto mode = ParseNumber<unsigned int>(optarg);
        if (!mode || mode.value() > 3) {
          PW_LOG_ERROR("Invalid mode: %s", optarg);
          return Status::InvalidArgument();
        }
        args.mode = mode.value();
        break;
      }
      case 'o':
        args.output_path = optarg;
        break;
      case 'r': {
        auto count = ParseNumber<unsigned int>(optarg);
        if (!count) {
          PW_LOG_ERROR("Invalid count: %s", optarg);
          return Status::InvalidArgument();
        }
        args.rx_count = count;
        break;
      }
      case '?':
        if (optopt) {
          PW_LOG_ERROR("Invalid flag: -%c", optopt);
        } else {
          PW_LOG_ERROR("Invalid flag: %s", argv[current_optind]);
        }
        Usage();
        return Status::InvalidArgument();
      case ':':
        PW_LOG_ERROR("Missing argument to %s", argv[current_optind]);
        return Status::InvalidArgument();
    }
  }

  args.human_readable = human_readable_given ||
                        (args.output_path == "-" && isatty(STDOUT_FILENO));

  // Check for required flags
  if (args.device.empty()) {
    PW_LOG_ERROR("Missing required flag: -D/--device");
    Usage();
    return Status::InvalidArgument();
  }
  if (!args.frequency) {
    PW_LOG_ERROR("Missing required flag: -F/--frequency");
    Usage();
    return Status::InvalidArgument();
  }

  // Either input file or rx count must be provided
  if (!args.input_path && !args.rx_count) {
    PW_LOG_ERROR("Either -i/--input or -r/--rx must be provided.");
    return Status::InvalidArgument();
  }

  return args;
}

std::vector<std::byte> ReadInput(const std::string& path, size_t limit) {
  std::ifstream input_file;
  if (path != "-") {
    input_file.open(path, std::ifstream::in);
    if (!input_file.is_open()) {
      PW_LOG_ERROR("Failed to open %s", path.c_str());
      exit(2);
    }
  }
  std::istream& instream = input_file.is_open() ? input_file : std::cin;

  std::vector<std::byte> result;
  for (size_t i = 0; i < limit; i++) {
    int b = instream.get();
    if (b == EOF) {
      break;
    }
    result.push_back(static_cast<std::byte>(b));
  }

  return result;
}

void WriteOutput(const std::string& path,
                 std::vector<std::byte> data,
                 bool human_readable) {
  std::ofstream output_file;
  if (path != "-") {
    output_file.open(path, std::ifstream::out);
    if (!output_file.is_open()) {
      PW_LOG_ERROR("Failed to open %s", path.c_str());
      exit(2);
    }
  }
  std::ostream& out = output_file.is_open() ? output_file : std::cout;

  if (human_readable) {
    out << '"';
  }

  for (std::byte b : data) {
    char c = static_cast<char>(b);
    if (!human_readable || std::isprint(c)) {
      out.put(c);
    } else if (c == '\0') {
      out << "\\0";
    } else if (c == '\n') {
      out << "\\n";
    } else {
      out << "\\x" << std::hex << std::setfill('0') << std::setw(2)
          << static_cast<unsigned int>(c);
    }
  }

  if (human_readable) {
    out << '"' << std::endl;
  }
}

int MainInNamespace(int argc, char* argv[]) {
  auto maybe_args = ParseArgs(argc, argv);
  if (!maybe_args.ok()) {
    return 1;
  }
  auto args = std::move(maybe_args.value());

  int fd = open(args.device.c_str(), O_RDWR);
  if (fd < 0) {
    PW_LOG_ERROR("Failed to open %s: %s", args.device.c_str(), strerror(errno));
    return 1;
  }
  PW_LOG_DEBUG("Opened %s", args.device.c_str());

  // Set up SPI Initiator.
  LinuxInitiator initiator(fd, args.frequency);
  if (auto status = initiator.Configure(args.GetSpiConfig()); !status.ok()) {
    PW_LOG_ERROR(
        "Failed to configure %s: %s", args.device.c_str(), status.str());
    return 2;
  }
  PW_LOG_DEBUG("Configured %s", args.device.c_str());

  // Read input data for transmit.
  std::vector<std::byte> tx_data;
  if (args.input_path) {
    tx_data = ReadInput(args.input_path.value(), 1024);
  }

  // Set up receive buffer.
  std::vector<std::byte> rx_data(args.rx_count ? args.rx_count.value()
                                               : tx_data.size());

  // Perform a transfer!
  PW_LOG_DEBUG(
      "Ready to send %zu, receive %zu bytes", tx_data.size(), rx_data.size());
  if (auto status = initiator.WriteRead(tx_data, rx_data); !status.ok()) {
    PW_LOG_ERROR("Failed to send/recv data: %s", status.str());
    return 2;
  }
  PW_LOG_DEBUG("Transfer successful! (%zu bytes)", rx_data.size());

  WriteOutput(args.output_path, rx_data, args.human_readable);

  return 0;
}

}  // namespace
}  // namespace pw::spi

int main(int argc, char* argv[]) {
  return pw::spi::MainInNamespace(argc, argv);
}
