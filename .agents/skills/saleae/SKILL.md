---
name: saleae-analyzer
description:
  Expert in using a Saleae logic analyzer to test or debug embedded systems
  by analyzing bus protocols like I2C, SPI, and UART.
---

# Saleae analyzer
A Saleae Logic Analyzer is a tool that can be used to capture and analyze
digital signals from an embedded system. These signals can either be bus
protocols like I2C, SPI, and UART, or they can be control signals like PWM
or digital signals from a sensor.

In order to use a Saleae analyzer, the user must first:
- Connect all channels to the appropriate signals on their board.
- Connect the Saleae to their computer.
- Enable the MCP server in "Settings".
- Add the Saleae MCP server in the MCP manager.

Use the MCP interface for interacting with the Saleae analyzer.

## Channels
A Saleae has either 8 or 16 total channels depending on the model. Channels can
record digital signals, analog signals, or both simultaneously on supported
hardware. Some bus protocols require multiple channels to decode; for example,
I2C requires both a clock and a data channel to decode a transaction.

The user must specify which channels are used to capture which signals, as well
as whether they should be captured as digital or analog channels.

### Digital channels
Digital channels capture the binary logic state (high or low) of a signal over
time. They are used for capturing digital logic waveforms, control lines (such
as GPIOs or PWM), and bus protocols decoded by analyzers (such as I2C, SPI, and
UART).

Digital signals can be configured to different voltage levels (e.g., 1.2V, 1.8V,
or 3.3V+) to match the logic level of the system under test. Unless the user
specifies a different voltage, select 3.3V+ by default.

### Analog channels
Analog channels sample and record continuous voltage waveforms over time,
similar to an oscilloscope. They are useful for measuring actual voltage levels,
inspecting analog sensor outputs, and debugging signal integrity issues such as
slow rise/fall times, voltage droop, ringing, or noise on digital lines.

### Sample rate
The sample rate is the rate at which the Saleae captures the state of a signal.

The sample rate for digital signals should be at least 10x the maximum
frequency of the signal you are capturing. For example, I2C at 100kHz should be
sampled at 1MHz (1MSps), while SPI at 10MHz should be sampled at 100MHz
(100MSps).

For analog signals, also use the 10x the maximum frequency, so a 10kHz sine wave
would be captured at 100kHz or greater. If the signal being measured is a square
wave (e.g. debugging signal integrity issues), start at 12.5MSps and only move
up to 50MSps if requested by the user.

If the signal to be measured is faster than 2MHz, signal integrity may start to
become an issue. If you are seeing unexpected or corrupted data, consider
enabling the "Glitch Filter".

## Capture triggers
Signal captures may be started in two main ways, Trigger and Timer. Do not use
Looping captures unless directly instructed to by the user.

Tips:
- Captures should be started before running the on-device test. This ensures
  the Saleae captures the entire transaction.
- Prefer Trigger captures over Timer captures where possible.

### Trigger
A Trigger Capture starts after a defined event is detected on a channel and
data is logged for a specified length of time.

Trigger captures should be used when:
- A well defined signal is expected on a specific channel and no other spurious
signals are expected.
- It is unknown when exactly the signal will occur and the possible window is
  greater than 30 seconds.

Tips:
- Set the capture duration after the trigger based on the length of time you
  expect the test to run for but no longer than 30 seconds.

### Timer
A Timer Capture starts immediately and data is logged for a specified length of
time.

Timer captures should be used in any of the following cases:
- Signals are periodic and occur more frequently than every 10 seconds.
- There is no clear trigger event that can be used to start the capture, for
  example the signal of interest is mixed in with other signals.
- It is unknown when exactly the signal will occur but the possible window is no
  longer than 30 seconds.

At sample rates above 10MSps, do not use a timer length longer than 30 seconds.

## Glitch filter
There is a glitch filter which can be enabled to remove high frequency glitches
from the captured signals. Use this if you see what looks like bus noise on the
signal or if the data being captured appears to be corrupted.

A good starting point for the glitch filter is 1/20th of one bit time of the
signal. For example, if measuring SPI at 10MHz the bit time is 100ns so the
glitch filter should be set to 5ns. If this does not fix the issue, you can try
increasing the glitch filter up to 1/5th of one bit time of the signal.
Alternatively, you can try changing the bit rate for asynchronous protocols,
such as UART.

## Analyzers
The Saleae software includes several analyzers that can be used to decode bus
protocols. Some common analyzers include:

- Async Serial (UART)
- CAN
- I2C
- I2S
- LIN
- SPI

Analyzers work on specified channels, where each channel corresponds to a
signal in the bus protocol. For example, for a standard I2C bus, you add an I2C
analyzer and specify during configuration which channels are used for the clock
and data lines.

## Exporting data
Once a signal has been captured and analyzed, Saleae can export the captured
and analyzed data as `.csv` format.
