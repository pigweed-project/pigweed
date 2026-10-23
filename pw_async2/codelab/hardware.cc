// Copyright 2025 The Pigweed Authors
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

#include "hardware.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <thread>

#include "pw_assert/check.h"
#include "pw_async2/dispatcher.h"
#include "pw_log/log.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_stream/socket_stream.h"
#include "pw_stream/stream.h"
#include "pw_stream/sys_io_stream.h"
#include "pw_string/string.h"
#include "pw_string/util.h"
#include "webui/webui_server.h"

namespace codelab {
namespace {

using ::pw::Status;

// Events from the vending machine hardware (actually, the Python server).
enum : char {
  kCoinReceived = 'c',
  kKeypress1 = '1',
  kKeypress2 = '2',
  kKeypress3 = '3',
  kKeypress4 = '4',
  kQuit = 'q',
  kDebugDispatcher = 'd',
  kItemDropSensor = 'i',
};

// For use with kDebugDispatcher
pw::async2::Dispatcher* current_dispatcher = nullptr;

void DebugDispatcher() {
  if (current_dispatcher != nullptr) {
    current_dispatcher->LogRegisteredTasks();
  }
}

Status HandleSingleCommand(char command) {
  switch (command) {
    case kCoinReceived:
      coin_inserted_isr();
      break;
    case kItemDropSensor:
      item_drop_sensor_isr();
      break;
    case kKeypress1:
    case kKeypress2:
    case kKeypress3:
    case kKeypress4:
      key_press_isr(static_cast<char>(command) - '0');
      break;
    case kQuit:
      std::_Exit(0);
    case kDebugDispatcher:
      DebugDispatcher();
      break;
    default:
      PW_LOG_WARN("Received unexpected command: %c (0x%02x)",
                  static_cast<char>(command),
                  static_cast<int>(command));
      return Status::InvalidArgument();
  }

  return {};
}

Status StreamHardwareLoop(pw::stream::Reader& reader) {
  char command = '\0';

  while (true) {
    PW_TRY(reader.Read(&command, sizeof(command)));
    if (std::isspace(command)) {
      continue;  // ignore space characters
    }

    if (const auto status = HandleSingleCommand(command); !status.ok()) {
      return status;
    }
  }
}

Status CommandLineHardwareLoop() {
  pw::stream::SysIoReader sys_io_reader;
  return StreamHardwareLoop(sys_io_reader);
}

constexpr bool kUseWebUi = PW_ASYNC2_CODELAB_WEBUI;

void HardwareLoop() {
  // Registering an atexit handler allows us to terminate this thread when
  // there is a return from main() even when this thread is otherwise blocked
  // on a read from a stream.
  //
  // One consequence is that we don't know what main() returned, so we just
  // assume it returned zero.
  //
  // We also must use something other than std::exit() to terminate.
  // `std::quick_exit` would have been nice, but isn't available everywhere.
  std::atexit([]() { std::_Exit(0); });

  Status status = CommandLineHardwareLoop();
  std::_Exit(status.ok() ? 0 : 1);
}

}  // namespace

void SetDisplay(std::string_view text) {
  if (kUseWebUi) {
    webui::SetDisplay(text);
  } else {
    pw::InlineString<kDisplayCharacters> contents;
    pw::string::Append(contents, text).IgnoreError();
    PW_LOG_INFO("[ %-10s ]", contents.c_str());
  }
}

void SetDispenserMotorState(int item, MotorState state) {
  PW_LOG_INFO("[Motor for item %d set to %s]",
              item,
              state == MotorState::kOff ? "Off" : "On");

  if (kUseWebUi) {
    webui::SetDispenserMotorState(state == MotorState::kOff ? -item : item);
  }
}

void HardwareInit(pw::async2::Dispatcher* dispatcher) {
  current_dispatcher = dispatcher;

  if (kUseWebUi) {
    webui::StartWebUIServer([](std::string_view text) -> pw::Status {
      if (text.size() != 1) {
        pw::InlineString<kDisplayCharacters> command;
        pw::string::Append(command, text).IgnoreError();
        PW_LOG_ERROR("Unexpected command '%s'", command.c_str());
        return pw::Status::InvalidArgument();
      }

      return HandleSingleCommand(text[0]);
    });
  }

  PW_LOG_INFO("==========================================");
  PW_LOG_INFO("Command line HW simulation notes:");
  PW_LOG_INFO("  Type 'q' (then enter) to quit.");
  PW_LOG_INFO("  Type 'd' to show the dispatcher state.");
  PW_LOG_INFO("  Type 'c' to insert a coin.");
  PW_LOG_INFO("  Type 'i' to signal an item dropped.");
  PW_LOG_INFO("  Type '1'..'4' to press a keypad key.");
  PW_LOG_INFO("==========================================");

  std::thread hardware_thread(HardwareLoop);
  hardware_thread.detach();
}

}  // namespace codelab
