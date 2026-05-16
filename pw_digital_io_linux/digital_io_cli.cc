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

// Usage: pw_digital_io_linux_cli COMMAND ...
//  Commands:
//    get   [-i] CHIP LINE
//      Configure the GPIO as an input and read its value.
//
//    set   [-i] CHIP LINE VALUE
//      Configure the GPIO as an output and set its value.
//
//    watch [-i] [{-ta,-tb,-td}] CHIP LINE
//      Configure the GPIO as an input and watch for interrupt events.
//
//      Options:
//        -t  Trigger for an interrupt:
//            -ta - activating edge
//            -tb - both edges (default)
//            -td - deactivating edge
//
//  Args:
//   CHIP:  gpiochip path (e.g. /dev/gpiochip0)
//   LINE:  line number (e.g. 1)
//   VALUE: the value to set (0 or 1)
//
// Options:
//   -i    Invert; configure as active-low.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <list>
#include <optional>
#include <string>

#include "pw_digital_io_linux/digital_io.h"
#include "pw_log/log.h"
#include "pw_status/try.h"

using pw::digital_io::InterruptTrigger;
using pw::digital_io::LinuxDigitalIoChip;
using pw::digital_io::LinuxGpioNotifier;
using pw::digital_io::LinuxInputConfig;
using pw::digital_io::LinuxOutputConfig;
using pw::digital_io::Polarity;
using pw::digital_io::State;

namespace {

pw::Status SetOutput(LinuxDigitalIoChip& chip,
                     const LinuxOutputConfig& config) {
  auto maybe_output = chip.GetOutputLine(config);
  if (!maybe_output.ok()) {
    PW_LOG_ERROR("Failed to get output line: %s", maybe_output.status().str());
    return pw::Status::Unavailable();
  }
  auto output = std::move(maybe_output.value());

  if (auto status = output.Enable(); !status.ok()) {
    PW_LOG_ERROR("Failed to enable output line: %s", status.str());
    return status;
  }

  // Nothing to do... default value applied.
  PW_LOG_INFO("Set line %u to %s\n",
              config.index,
              config.default_state == State::kActive ? "active" : "inactive");

  // NOTE: When this function returns and `output` goes out of scope, its
  // file descriptor is closed. Depending on the GPIO driver, this could
  // result in the pin being immediately returned to its default state.
  // See https://manpages.debian.org/bookworm/gpiod/gpioset.1.en.html.

  return pw::OkStatus();
}

const char* InterruptTriggerStr(InterruptTrigger trigger) {
  switch (trigger) {
    case InterruptTrigger::kActivatingEdge:
      return "activating edge";
    case InterruptTrigger::kBothEdges:
      return "both edges";
    case InterruptTrigger::kDeactivatingEdge:
      return "deactivating edge";
    default:
      return "?";
  }
}

pw::Status WatchInput(LinuxDigitalIoChip& chip,
                      const LinuxInputConfig& config,
                      InterruptTrigger trigger) {
  PW_TRY_ASSIGN(auto notifier, LinuxGpioNotifier::Create());

  auto maybe_input = chip.GetInterruptLine(config, notifier);
  if (!maybe_input.ok()) {
    PW_LOG_ERROR("Failed to get input line: %s", maybe_input.status().str());
    return pw::Status::Unavailable();
  }
  auto input = std::move(maybe_input.value());

  auto handler = [](State state) {
    if (state == State::kActive) {
      std::cout << "Activated" << std::endl;
    } else {
      std::cout << "Deactivated" << std::endl;
    }
  };

  PW_TRY(input.SetInterruptHandler(trigger, handler));

  if (auto status = input.EnableInterruptHandler(); !status.ok()) {
    PW_LOG_ERROR("Failed to enable input interrupt: %s", status.str());
    return status;
  }

  if (auto status = input.Enable(); !status.ok()) {
    PW_LOG_ERROR("Failed to enable input line: %s", status.str());
    return status;
  }

  PW_LOG_INFO("Watching for events (%s)", InterruptTriggerStr(trigger));

  // Process events
  notifier->Run();

  return pw::OkStatus();
}

pw::Status GetInput(LinuxDigitalIoChip& chip, const LinuxInputConfig& config) {
  auto maybe_input = chip.GetInputLine(config);
  if (!maybe_input.ok()) {
    PW_LOG_ERROR("Failed to get input line: %s", maybe_input.status().str());
    return pw::Status::Unavailable();
  }
  auto input = std::move(maybe_input.value());

  if (auto status = input.Enable(); !status.ok()) {
    PW_LOG_ERROR("Failed to enable input line: %s", status.str());
    return status;
  }

  auto maybe_state = input.GetState();
  if (!maybe_state.ok()) {
    PW_LOG_ERROR("Failed to get input line state: %s",
                 maybe_state.status().str());
    return pw::Status::Unavailable();
  }
  std::cout << (maybe_state.value() == State::kActive ? "active" : "inactive")
            << std::endl;
  return pw::OkStatus();
}

void UsageError(const std::string& error) {
  PW_LOG_ERROR("%s", error.c_str());
  std::cerr << "Usage: pw_digital_io_linux_cli COMMAND ..." << std::endl;
  std::cerr << std::endl;
  std::cerr << "  Commands:" << std::endl;
  std::cerr << "    get   [-i] CHIP LINE" << std::endl;
  std::cerr << "    set   [-i] CHIP LINE VALUE" << std::endl;
  std::cerr << "    watch [-i] [{-ta,-tb,-td}] CHIP LINE" << std::endl;
  std::cerr << "        Options:" << std::endl;
  std::cerr << "          -t  Trigger for an interrupt:" << std::endl;
  std::cerr << "              -ta - activating edge" << std::endl;
  std::cerr << "              -tb - both edges (default)" << std::endl;
  std::cerr << "              -td - deactivating edge" << std::endl;
  std::cerr << std::endl;
  std::cerr << "  Common Options:" << std::endl;
  std::cerr << "    -i    Invert; configure as active-low." << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::list<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  // The first argument is the command name.
  if (args.empty()) {
    UsageError("Missing command");
    return 1;
  }
  std::string command = args.front();
  args.pop_front();

  // These are currently the only commands, and they take the same options (-i)
  // and first two arguments (chip and line).
  if (!(command == "get" || command == "set" || command == "watch")) {
    UsageError("Invalid command: \"" + command + "\"");
    return 1;
  }

  // Process options
  Polarity polarity = Polarity::kActiveHigh;
  InterruptTrigger trigger = InterruptTrigger::kBothEdges;
  for (auto argi = args.begin(); argi != args.end(); /* Advance in body. */) {
    std::string option = *argi;
    if (!(option.size() >= 2 && option[0] == '-')) {
      // Not an option.
      ++argi;  // Advance.
      continue;
    }
    option = option.substr(1);
    argi = args.erase(argi);  // Advance.

    if (option == "i") {
      polarity = Polarity::kActiveLow;
      continue;
    }
    if (command == "watch") {
      if (option == "ta") {
        trigger = InterruptTrigger::kActivatingEdge;
        continue;
      } else if (option == "tb") {
        trigger = InterruptTrigger::kBothEdges;
        continue;
      } else if (option == "td") {
        trigger = InterruptTrigger::kDeactivatingEdge;
        continue;
      }
    }
    UsageError("Invalid option: \"-" + option + "\"");
    return 1;
  }

  // Process args
  if (args.size() < 2) {
    UsageError("Missing arguments: CHIP, LINE");
    return 1;
  }
  std::string path = args.front();
  args.pop_front();
  // std::stoi
  const int raw_index = std::stoi(args.front());
  if (raw_index < 0) {
    PW_LOG_ERROR("The index must be non-negative; %d is negative", raw_index);
    return 1;
  }
  const unsigned index = static_cast<unsigned>(raw_index);
  args.pop_front();

  // "set" also takes a value argument.
  std::optional<State> set_value = std::nullopt;
  if (command == "set") {
    if (args.empty()) {
      UsageError("Missing argument: VALUE");
      return 1;
    }
    set_value =
        (std::stoi(args.front()) > 0) ? State::kActive : State::kInactive;
    args.pop_front();
  }

  if (!args.empty()) {
    UsageError("Unexpected argument: \"" + args.front() + "\"");
    return 1;
  }

  // Open the chip.
  auto maybe_chip = LinuxDigitalIoChip::Open(path.c_str());
  if (!maybe_chip.ok()) {
    PW_LOG_ERROR("Failed to open %s: %s", path.c_str(), std::strerror(errno));
    return 2;
  }
  auto chip = std::move(maybe_chip.value());
  PW_LOG_INFO("Opened %s", path.c_str());

  // Handle the get or set.
  pw::Status status;
  if (command == "get") {
    LinuxInputConfig config(
        /* gpio_index= */ index,
        /* gpio_polarity= */ polarity);
    status = GetInput(chip, config);
  } else if (command == "set") {
    LinuxOutputConfig config(
        /* gpio_index= */ index,
        /* gpio_polarity= */ polarity,
        /* gpio_default_state= */ *set_value);
    status = SetOutput(chip, config);
  } else if (command == "watch") {
    LinuxInputConfig config(
        /* gpio_index= */ index,
        /* gpio_polarity= */ polarity);
    status = WatchInput(chip, config, trigger);
  }

  // Handle the return status accordingly.
  return status.ok() ? 0 : 2;
}
