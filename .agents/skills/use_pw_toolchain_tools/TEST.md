# Testing Use Pigweed Toolchain Tools Skill

This document provides instructions on how to verify that the `Use Pigweed Toolchain Tools` skill is correctly triggering in agent sessions.

## Setup

1.  **Start a fresh agent session** (or clear context if possible) to ensure no prior knowledge bias.
2.  Ensure you are in the Pigweed repository root.

## Test Cases

Run the following prompts and verify the agent's behavior.

### 1. Disassembly Intent
**Prompt:**
> "I need to see the assembly for the `pw_status:status_test` binary built for rp2350. Can you disassemble it for me?"

**Expected Behavior:**
-   **Skill Trigger:** The agent should load `use_pw_toolchain_tools/SKILL.md`.
-   **Action:** The agent executes:
    ```bash
    bazelisk run --config=rp2350 //pw_toolchain/cc/current_toolchain:objdump -- -d ...
    ```
-   **Failure Mode:** If the agent tries to run `llvm-objdump` directly or says "I don't know where objdump is", the skill failed to trigger.

### 2. Binary Size Analysis
**Prompt:**
> "The binary seems too large. Can you check the size of `pw_status:status_test` (rp2350) and tell me the section sizes?"

**Expected Behavior:**
-   **Skill Trigger:** The agent should load `use_pw_toolchain_tools/SKILL.md`.
-   **Action:** The agent executes:
    ```bash
    bazelisk run --config=rp2350 //pw_toolchain/cc/current_toolchain:size -- ...
    ```

### 3. Symbol Inspection
**Prompt:**
> "I suspect there's a missing symbol in `pw_status:status_test`. Can you list all defined symbols for the rp2350 build?"

**Expected Behavior:**
-   **Skill Trigger:** The agent should load `use_pw_toolchain_tools/SKILL.md`.
-   **Action:** The agent executes `nm` via the toolchain wrapper:
    ```bash
    bazelisk run --config=rp2350 //pw_toolchain/cc/current_toolchain:nm -- ...
    ```

### 4. Coverage Analysis
**Prompt:**
> "I've run my tests and generated some profiles. Can you show me the coverage report for `pw_status` using the toolchain's coverage tools?"

**Expected Behavior:**
-   **Skill Trigger:** The agent should load `use_pw_toolchain_tools/SKILL.md`.
-   **Action:** The agent identifies `:cov` or `:gcov` as the appropriate tool:
    ```bash
    bazelisk run --config=host //pw_toolchain/cc/current_toolchain:cov -- ...
    ```

## Troubleshooting

If the skill does not trigger:
1.  **Check `SKILL.md` description**: Ensure keywords like "disassemble", "size", "symbols", "coverage" are present.
2.  **Check explicit intent**: Did you ask for "objdump" or "nm" specifically? Sometimes identifying the tool by name helps if the intent is vague.
3.  **Check negative signaling**: The description now says "MUST be used instead of system tools". This should increase priority in the agent's planning.
