# Testing Saleae Analyzer Skill

This document provides the evaluation plan, test prompts, and verification
criteria for testing the `saleae-analyzer` agent skill defined in
[`SKILL.md`](SKILL.md).

---

## 1. Automated Verification (Pre-Flight Checks)

Before running interactive agent prompts, verify that the skill definitions and
MCP tool schemas are intact:

```bash
# 1. Verify skill file exists and contains valid YAML frontmatter
head -n 10 .agents/skills/saleae/SKILL.md

# 2. Verify Saleae MCP tool schemas exist and are valid JSON
python3 -c '
import json, glob
schemas = glob.glob("/Users/liambucci/.gemini/jetski/mcp/saleae/*.json")
assert len(schemas) >= 10, f"Expected >= 10 schemas, found {len(schemas)}"
for s in schemas:
    with open(s) as f:
        json.load(f)
print(f"Validated {len(schemas)} Saleae MCP schemas successfully.")
'
```

---

## 2. Interactive Agent Test Scenarios

Run these prompts in fresh agent sessions to verify skill discovery, tool
selection, parameter calculation, and adherence to safety and configuration rules.

| # | Scenario | User Prompt | Expected MCP Calls / Actions | Prohibited Anti-Patterns |
| :--- | :--- | :--- | :--- | :--- |
| **1** | **MCP Server Not Enabled Diagnosis** | *(With Saleae MCP server disabled/unavailable)*<br>"Can you list the connected Saleae devices and start a capture on channel 0?" | Detects unavailable `saleae` MCP server; explains required setup steps (enable MCP in Settings, connect device, configure in MCP manager). | Running shell commands (`saleae-cli`, `Logic`, raw terminal scripts), failing silently, or hallucinating connected devices. |
| **2** | **Low-Speed Signal Capture (100 kHz)** | "I have a 100 kHz square wave on channel 0. Configure and start a capture for it." | Calls `start_capture` with `digitalSampleRate >= 1000000` (>= 1 MSps, 10x signal frequency) and channel 0 enabled. Prefers trigger capture or timer <= 30s. | Sampling below 1 MSps (< 10x frequency), configuring looping capture, or setting excessive timer durations. |
| **3** | **High-Speed Signal Capture (2 MHz)** | "I need to capture a 2 MHz SPI clock on channel 1. Configure a 5-second capture." | Calls `start_capture` with `digitalSampleRate >= 20000000` (>= 20 MSps, 10x), `timedCaptureMode` duration <= 30s (duration = 5s), and evaluates/applies glitch filtering. | Sample rate < 20 MSps, timer duration > 30s at > 10 MSps, or failing to address signal integrity/glitch filtering for > 1 MHz signals. |
| **4** | **UART TX/RX Capture & Analyzer** | "Capture a 115200 baud UART transmission. TX is on channel 2 and RX is on channel 3. Decode the data." | Prompts or sets sample rate >= 1.152 MSps (>= 10x baud rate), starts capture on channels 2 & 3, calls `add_analyzer` with `analyzerName: "Async Serial"` mapping channels and 115200 baud. | Hallucinating channels without user specification, using sample rate < 1.152 MSps, omitting the `Async Serial` analyzer. |
| **5** | **I2C Bus Capture & Analyzer** | "Record the I2C bus transaction between my MCU and sensor. SCL is on channel 4, SDA is on channel 5, bus speed is standard 100 kHz." | Calculates sample rate >= 1 MSps (10x 100 kHz), enables digital channels 4 & 5, configures trigger (or timer), calls `add_analyzer` with `analyzerName: "I2C"` mapping SCL to ch 4 and SDA to ch 5. | Sample rate < 1 MSps, failing to map SCL/SDA channels, attempting Looping capture mode. |
| **6** | **Unspecified Channel Clarification** | "Please capture an I2C transaction at 400 kHz." | Asks the user which channels are connected to SCL and SDA before initiating capture. | Guessing channel assignments (e.g. assuming ch 0 is SCL and ch 1 is SDA) without asking. |
| **7** | **Glitch Filter & Data Export** | "The 10 MHz SPI capture on channel 0 has bus glitches. Apply a glitch filter and export the decoded table to /tmp/spi.csv." | Calculates glitch filter pulse width at 1/20th bit time (10 MHz -> 100 ns bit time -> 5 ns filter) in `logicDeviceConfiguration.glitchFilters`, captures, and invokes `export_data_table_csv`. | Hardcoding arbitrary filter values without calculating bit time ratio (1/20th to 1/5th), or using shell scripts to parse raw captures. |

---

## 3. Scenario Details and Verification Criteria

### Scenario 1: MCP Server Not Enabled Diagnosis

- **Prompt:**
  > "Can you check my Saleae analyzer to see what devices are connected and start a capture on channel 0?"
- **Preconditions:**
  Saleae MCP server is disabled or not running.
- **Expected Behavior:**
  1. The agent attempts to check devices via the `saleae` toolset (e.g. `get_devices`).
  2. Upon discovering the tool is unavailable or connection fails, the agent diagnoses that the Saleae MCP server is not enabled.
  3. The agent clearly instructs the user on the required prerequisite steps:
     - Connect the Saleae analyzer to the computer and wire the channels to the board.
     - Enable the MCP server in Saleae Logic "Settings".
     - Add/verify the Saleae MCP server in the MCP manager.
- **Negative Tests:**
  - Agent MUST NOT attempt to run local command-line binaries like `saleae`, `Logic`, or write custom python socket scripts to communicate directly with the Saleae.

### Scenario 2: Low-Speed Signal Capture (100 kHz)

- **Prompt:**
  > "I have a 100 kHz clock/PWM signal on channel 0. Please configure and start a capture for 2 seconds."
- **Expected Behavior:**
  1. The agent applies the 10x sample rate rule: $f_{\text{sample}} \ge 10 \times 100\text{ kHz} = 1\text{ MSps}$ (`digitalSampleRate: 1000000` or higher).
  2. Enables digital channel 0 in `logicChannels.digitalChannels`.
  3. Uses `timedCaptureMode` with `durationSeconds: 2` (or a digital trigger if specified).
  4. Calls `start_capture` with `ServerName: "saleae"`.
- **Negative Tests:**
  - `digitalSampleRate` < 1,000,000.
  - Setting looping capture mode (`manualCaptureMode` without duration or looping).

### Scenario 3: High-Speed Signal Capture (2 MHz)

- **Prompt:**
  > "I have a 2 MHz signal on channel 1 that will start when I press a button. Configure a capture for it."
- **Expected Behavior:**
  1. Calculates minimum digital sample rate: $f_{\text{sample}} \ge 10 \times 2\text{ MHz} = 20\text{ MSps}$ (`digitalSampleRate: 20000000` or higher).
  2. Prefers a `digitalCaptureMode` trigger (e.g., edge trigger on channel 1) since the event starts on a button press, with `afterTriggerSeconds` <= 30 seconds.
  3. Notes that signals above 1 MHz can encounter signal integrity issues and considers or configures the glitch filter (bit time = 500 ns; 1/20th bit time = 25 ns).
  4. If a timer capture is used instead, verifies `durationSeconds` <= 30 seconds (as required for sample rates above 10 MSps).
- **Negative Tests:**
  - Using a sample rate lower than 20 MSps.
  - Using timer capture duration > 30 seconds at sample rates > 10 MSps.
  - Ignoring signal integrity / glitch filter considerations for > 1 MHz signals.

### Scenario 4: UART TX/RX Capture & Analyzer

- **Prompt:**
  > "I want to capture and decode a UART communication running at 115200 baud. The MCU TX is on channel 2 and RX is on channel 3."
- **Expected Behavior:**
  1. Identifies bit rate: 115,200 bps. Minimum sample rate: $\ge 10 \times 115,200 \approx 1.152\text{ MSps}$ (e.g. 2 MSps or higher).
  2. Enables channels 2 and 3 in `logicChannels.digitalChannels`.
  3. Starts capture before device test/communication occurs.
  4. Calls `add_analyzer` with `analyzerName: "Async Serial"` (one analyzer per channel or configured for the specified channel) and passes analyzer settings:
     - Bit rate: 115200
     - Relevant channel index (channel 2 for TX, channel 3 for RX).
- **Negative Tests:**
  - Inverting TX/RX channels or failing to configure baud rate.
  - Sample rate below 10x baud rate (< 1.152 MSps).

### Scenario 5: I2C Bus Capture & Analyzer

- **Prompt:**
  > "Record and decode an I2C transaction between my MCU and a temp sensor. SCL is on channel 4, SDA is on channel 5, and the bus runs at standard 100 kHz."
- **Expected Behavior:**
  1. Calculates sample rate: $10 \times 100\text{ kHz} = 1\text{ MSps}$ minimum (`digitalSampleRate >= 1000000`).
  2. Enables digital channels `[4, 5]`.
  3. Configures trigger on SDA falling edge (or SCL falling edge), or timer capture <= 30 seconds.
  4. Invokes `add_analyzer` with:
     - `analyzerName: "I2C"`
     - SCL channel set to 4, SDA channel set to 5.
- **Negative Tests:**
  - Swapping SDA and SCL channels.
  - Sample rate < 1 MSps.
  - Using Looping capture mode.

---

## 4. Grading Checklist

A test session **PASSES** if and only if:
- [ ] **MCP server diagnostics:** When the Saleae MCP server is unavailable, the agent cleanly informs the user of the required setup steps in Logic Settings and MCP manager without attempting unsupported CLI commands.
- [ ] **Sample rate >= 10x frequency rule:** The agent always sets `digitalSampleRate` to at least 10 times the maximum signal/bus frequency (e.g., >= 1 MSps for 100 kHz, >= 20 MSps for 2 MHz, >= 1.152 MSps for 115200 baud).
- [ ] **High sample rate duration limit:** At sample rates above 10 MSps, timer captures never exceed 30 seconds.
- [ ] **Glitch filter calculation:** For high-frequency signals (> 1 MHz) or noisy captures, glitch filters are set to 1/20th (up to 1/5th) of one bit time.
- [ ] **Trigger vs timer preference:** Trigger captures are preferred for discrete transactions, and captures are initiated before starting on-device tests.
- [ ] **User channel specification:** The agent never assumes or guesses pin/channel mappings; if channels are unspecified by the user, the agent asks for clarification.
- [ ] **Protocol analyzer setup:** Appropriate analyzers (`"Async Serial"`, `"I2C"`, `"SPI"`, etc.) are attached with the correct channel assignments and protocol parameters.
- [ ] **No unauthorized looping captures:** The agent does not configure looping captures unless explicitly requested by the user.
