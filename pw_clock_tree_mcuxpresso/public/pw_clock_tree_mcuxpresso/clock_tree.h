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

#include "fsl_clock.h"
#include "fsl_power.h"
#include "pw_clock_tree/clock_tree.h"

namespace pw::clock_tree {

/// @module{pw_clock_tree_mcuxpresso}

/// Class implementing an FRO clock source.
class [[deprecated("Use ClockMcuxpressoFroSource instead")]]
ClockMcuxpressoFro final : public ClockSource<ElementNonBlockingCannotFail> {
 public:
  /// Constructor specifying the FRO divider output to manage.
  constexpr ClockMcuxpressoFro(clock_fro_output_en_t fro_output)
      : fro_output_(fro_output) {}

 private:
  /// Enable this FRO divider.
  Status DoEnable() final {
    CLOCK_EnableFroClk(CLKCTL0->FRODIVOEN | fro_output_);
    return OkStatus();
  }

  /// Disable this FRO divider.
  Status DoDisable() final {
    CLOCK_EnableFroClk(CLKCTL0->FRODIVOEN & ~fro_output_);
    return OkStatus();
  }

  /// FRO divider.
  const uint32_t fro_output_;
};

/// Class implementing the FRO clock source.
class ClockMcuxpressoFroSource final
    : public ClockSource<ElementNonBlockingCannotFail> {
 public:
  constexpr ClockMcuxpressoFroSource() = default;

 private:
  Status DoEnable() final {
    POWER_DisablePD(kPDRUNCFG_PD_FFRO);
    return OkStatus();
  }

  Status DoDisable() final {
    POWER_EnablePD(kPDRUNCFG_PD_FFRO);
    return OkStatus();
  }
};

/// FRO divider elements.
class ClockMcuxpressoFroDivider final
    : public DependentElement<ElementNonBlockingCannotFail> {
 public:
  constexpr ClockMcuxpressoFroDivider(ClockMcuxpressoFroSource& source,
                                      clock_fro_output_en_t fro_output)
      : DependentElement<ElementNonBlockingCannotFail>(source),
        fro_output_(fro_output) {}

 private:
  Status DoEnable() final {
    CLOCK_EnableFroClk(CLKCTL0->FRODIVOEN | fro_output_);
    return OkStatus();
  }

  Status DoDisable() final {
    CLOCK_EnableFroClk(CLKCTL0->FRODIVOEN & ~fro_output_);
    return OkStatus();
  }

  const uint32_t fro_output_;
};

/// Class implementing the low power oscillator clock source.
class ClockMcuxpressoLpOsc final
    : public ClockSource<ElementNonBlockingCannotFail> {
 private:
  /// Enable low power oscillator.
  Status DoEnable() final {
    // Power up the 1MHz low power oscillator power domain.
    POWER_DisablePD(kPDRUNCFG_PD_LPOSC);
    // POWER_ApplyPD() is not necessary for LPOSC_PD.

    // Wait for the low power oscillator to stabilize.
    CLOCK_EnableLpOscClk();
    return OkStatus();
  }

  /// Disable low power oscillator.
  Status DoDisable() final {
    // Power down the 1MHz low power oscillator power domain.
    POWER_EnablePD(kPDRUNCFG_PD_LPOSC);
    // POWER_ApplyPD() is not necessary for LPOSC_PD.
    return OkStatus();
  }
};

/// Class template implementing the MCLK IN clock source.
///
/// Template argument `ElementType` can be of class `ElementBlocking` or
/// `ElementNonBlockingCannotFail`.
template <typename ElementType>
class ClockMcuxpressoMclk final : public DependentElement<ElementType> {
 public:
  /// Constructor specifying the MCLK IN clock frequency in Hz and
  /// the dependent clock tree element to enable the MCLK clock source.
  template <typename SourceType>
  constexpr ClockMcuxpressoMclk(SourceType& source, uint32_t frequency)
      : DependentElement<ElementType>(source), frequency_(frequency) {}

 private:
  /// Set MCLK IN clock frequency.
  Status DoEnable() final {
    // Set global that stores external MCLK IN clock frequency.
    CLOCK_SetMclkFreq(frequency_);
    return OkStatus();
  }

  /// Set MCLK IN clock frequency to 0 Hz.
  Status DoDisable() final {
    // Set global that stores external MCLK IN clock frequency to zero.
    CLOCK_SetMclkFreq(0);
    return OkStatus();
  }

  /// MCLK IN frequency in Hz.
  uint32_t frequency_;
};

/// Alias for a blocking MCLK IN clock tree element.
/// This class should be used if the MCLK IN clock source depends on
/// another blocking clock tree element to enable the MCLK IN clock source.
using ClockMcuxpressoMclkBlocking = ClockMcuxpressoMclk<ElementBlocking>;

/// Alias for a non-blocking MCLK IN clock tree element where updates cannot
/// fail.
using ClockMcuxpressoMclkNonBlocking =
    ClockMcuxpressoMclk<ElementNonBlockingCannotFail>;

/// Class template implementing the CLK IN pin clock source and selecting
/// it as an input source for OSC Clock source.
///
/// Template argument `ElementType` can be of class `ElementBlocking` or
/// `ElementNonBlockingCannotFail`.
template <typename ElementType>
class ClockMcuxpressoClkIn final : public DependentElement<ElementType> {
 public:
  /// Constructor specifying the CLK IN pin clock frequency in Hz and
  /// the dependent clock tree element to enable the CLK IN pin clock source.
  template <typename SourceType>
  constexpr ClockMcuxpressoClkIn(SourceType& source, uint32_t frequency)
      : DependentElement<ElementType>(source), frequency_(frequency) {}

 private:
  /// Set CLK IN clock frequency.
  Status DoEnable() final {
    // Set global that stores external CLK IN pin clock frequency.
    CLOCK_SetClkinFreq(frequency_);

    // OSC clock source selector ClkIn.
    const uint8_t kCLOCK_OscClkIn = CLKCTL0_SYSOSCBYPASS_SEL(1);
    CLKCTL0->SYSOSCBYPASS = kCLOCK_OscClkIn;
    return OkStatus();
  }

  /// Set CLK IN clock frequency to 0 Hz.
  Status DoDisable() final {
    // Set global that stores external CLK IN pin clock frequency to zero.
    CLOCK_SetClkinFreq(0);

    // Set OSC clock source selector None, which gates output to reduce power.
    const uint8_t kCLOCK_OscNone = CLKCTL0_SYSOSCBYPASS_SEL(7);
    CLKCTL0->SYSOSCBYPASS = kCLOCK_OscNone;
    return OkStatus();
  }

  /// CLK IN frequency in Hz.
  uint32_t frequency_;
};

/// Alias for a blocking CLK IN pin clock tree element.
/// This class should be used if the CLK IN pin clock source depends on
/// another blocking clock tree element to enable the CLK IN pin clock source.
using ClockMcuxpressoClkInBlocking = ClockMcuxpressoClkIn<ElementBlocking>;

/// Alias for a non-blocking CLK IN pin clock tree element where updates cannot
/// fail.
using ClockMcuxpressoClkInNonBlocking =
    ClockMcuxpressoClkIn<ElementNonBlockingCannotFail>;

/// Class template implementing the FRG clock tree element.
///
/// Template argument `ElementType` can be of class `ElementBlocking` or
/// `ElementNonBlockingCannotFail`.
template <typename ElementType>
class ClockMcuxpressoFrg final : public DependentElement<ElementType> {
 public:
  /// Constructor specifying the source clock and FRG configuration.
  template <typename SourceType>
  constexpr ClockMcuxpressoFrg(SourceType& source,
                               const clock_frg_clk_config_t& config)
      : DependentElement<ElementType>(source), config_(config) {}

 private:
  // FRG clock source selector None, which gates output to reduce power.
  // The None source selector is not defined in the SDK.
  const uint8_t kCLOCK_FrgNone = 7;

  /// Enable FRG configuration.
  Status DoEnable() final {
    CLOCK_SetFRGClock(&config_);
    return OkStatus();
  }

  /// Disable FRG configuration.
  Status DoDisable() final {
    clock_frg_clk_config_t disable_config = config_;
    disable_config.sfg_clock_src =
        static_cast<decltype(disable_config.sfg_clock_src)>(kCLOCK_FrgNone);
    CLOCK_SetFRGClock(&disable_config);
    return OkStatus();
  }

  /// FRG clock configuration to enable FRG component.
  const clock_frg_clk_config_t config_;
};

/// Alias for a blocking FRG clock tree element.
using ClockMcuxpressoFrgBlocking = ClockMcuxpressoFrg<ElementBlocking>;

/// Alias for a non-blocking FRG clock tree element where updates cannot fail.
using ClockMcuxpressoFrgNonBlocking =
    ClockMcuxpressoFrg<ElementNonBlockingCannotFail>;

/// Class template implementing the clock selector element.
///
/// Template argument `ElementType` can be of class `ElementBlocking` or
/// `ElementNonBlockingCannotFail`.
template <typename ElementType>
class ClockMcuxpressoSelector : public DependentElement<ElementType> {
 public:
  /// Constructor specifying the source clock and the selector value
  /// when the selector should get enabled, and the selector value when
  /// the selector should get disabled to save power.
  template <typename SourceType>
  constexpr ClockMcuxpressoSelector(SourceType& source,
                                    clock_attach_id_t selector_enable,
                                    clock_attach_id_t selector_disable)
      : DependentElement<ElementType>(source),
        selector_enable_(selector_enable),
        selector_disable_(selector_disable) {}

 private:
  /// Enable selector.
  Status DoEnable() final {
    CLOCK_AttachClk(selector_enable_);
    return OkStatus();
  }

  /// Disable selector.
  Status DoDisable() final {
    CLOCK_AttachClk(selector_disable_);
    return OkStatus();
  }

  /// Enable selector value.
  clock_attach_id_t selector_enable_;
  /// Disable selector value.
  clock_attach_id_t selector_disable_;
};

/// Alias for a blocking clock selector clock tree element.
using ClockMcuxpressoSelectorBlocking =
    ClockMcuxpressoSelector<ElementBlocking>;

/// Alias for a non-blocking clock selector clock tree element where updates
/// cannot fail.
using ClockMcuxpressoSelectorNonBlocking =
    ClockMcuxpressoSelector<ElementNonBlockingCannotFail>;

/// Class template implementing the clock divider element.
///
/// Template argument `ElementType` can be of class `ElementBlocking` or
/// `ElementNonBlockingCannotFail`.
template <typename ElementType>
class ClockMcuxpressoDivider final : public ClockDividerElement<ElementType> {
 public:
  /// Constructor specifying the source clock, the name of the divder and
  /// the divider setting.
  template <typename SourceType>
  constexpr ClockMcuxpressoDivider(SourceType& source,
                                   clock_div_name_t divider_name,
                                   uint32_t divider)
      : ClockDividerElement<ElementType>(source, divider),
        divider_name_(divider_name) {}

 private:
  /// Set the divider configuration.
  Status DoEnable() final {
    CLOCK_SetClkDiv(divider_name_, this->divider());
    return OkStatus();
  }

  /// Name of divider.
  clock_div_name_t divider_name_;
};

/// Alias for a blocking clock divider clock tree element.
using ClockMcuxpressoDividerBlocking = ClockMcuxpressoDivider<ElementBlocking>;

/// Alias for a non-blocking clock divider clock tree element where updates
/// cannot fail.
using ClockMcuxpressoDividerNonBlocking =
    ClockMcuxpressoDivider<ElementNonBlockingCannotFail>;

/// Class template implementing the audio pll clock element.
///
/// The Audio PLL can either operate in the enabled mode where the PLL
/// and the phase fractional divider are enabled, or it can operate in
/// bypass mode, where both PLL and phase fractional divider are
/// clock gated.
/// When the Audio PLL clock tree gets disabled, both PLL and phase fractional
/// divider will be clock gated.
///
/// Template argument `ElementType` can be of class `ElementBlocking` or
/// `ElementNonBlockingCannotFail`.
template <typename ElementType>
class ClockMcuxpressoAudioPll : public DependentElement<ElementType> {
 public:
  /// Constructor specifying the configuration for the enabled Audio PLL.
  template <typename SourceType>
  constexpr ClockMcuxpressoAudioPll(SourceType& source,
                                    const clock_audio_pll_config_t& config,
                                    uint8_t audio_pfd_divider)
      : DependentElement<ElementType>(source),
        config_(&config),
        audio_pfd_divider_(audio_pfd_divider) {}

  /// Constructor to place the Audio PLL into bypass mode.
  template <typename SourceType>
  constexpr ClockMcuxpressoAudioPll(SourceType& source,
                                    audio_pll_src_t bypass_source)
      : DependentElement<ElementType>(source), bypass_source_(bypass_source) {}

 private:
  /// Configures and enables the audio PLL if `config_` is set, otherwise places
  /// the audio PLL in bypass mode.
  Status DoEnable() override {
    // If `config_` is specified, the PLL should be enabled and the phase
    // fractional divider PFD0 needs to get configured, otherwise the PLL
    // operates in bypass mode.
    if (config_ != nullptr) {
      // Configure Audio PLL clock source.
      CLOCK_InitAudioPll(config_);
      CLOCK_InitAudioPfd(kCLOCK_Pfd0, audio_pfd_divider_);
    } else {
      // PLL operates in bypass mode.
      CLKCTL1->AUDIOPLL0CLKSEL = bypass_source_;
      CLKCTL1->AUDIOPLL0CTL0 |= CLKCTL1_AUDIOPLL0CTL0_BYPASS_MASK;
    }
    return OkStatus();
  }

  /// Disables the audio PLL logic.
  Status DoDisable() override {
    if (config_ != nullptr) {
      // Clock gate the phase fractional divider PFD0.
      CLOCK_DeinitAudioPfd(kCLOCK_Pfd0);
    }

    // Power down Audio PLL
    CLOCK_DeinitAudioPll();
    return OkStatus();
  }

  /// Optional audio PLL configuration.
  const clock_audio_pll_config_t* config_ = nullptr;

  /// Optional audio kCLOCK_Pfd0 clock divider value.
  const uint8_t audio_pfd_divider_ = 0;

  /// Optional audio PLL bypass clock source.
  const audio_pll_src_t bypass_source_ = kCLOCK_AudioPllNone;
};

/// Class template implementing the sys pll clock element.
///
/// The Sys PLL can either operate in the enabled mode where the PLL
/// and the phase fractional divider are enabled, or it can operate in
/// bypass mode, where both PLL and phase fractional divider are
/// clock gated.
/// When the Sys PLL clock tree gets disabled, both PLL and phase fractional
/// divider will be clock gated.
///
/// Template argument `ElementType` can be of class `ElementBlocking` or
/// `ElementNonBlockingCannotFail`.
template <typename ElementType>
class ClockMcuxpressoSysPll : public DependentElement<ElementType> {
 public:
  /// Constructor specifying the configuration for the enabled Sys PLL.
  template <typename SourceType>
  constexpr ClockMcuxpressoSysPll(SourceType& source,
                                  const clock_sys_pll_config_t& config,
                                  uint8_t sys_pfd0_divider,
                                  uint8_t sys_pfd1_divider,
                                  uint8_t sys_pfd2_divider,
                                  uint8_t sys_pfd3_divider)
      : DependentElement<ElementType>(source),
        config_(&config),
        sys_pfd0_divider_(sys_pfd0_divider),
        sys_pfd1_divider_(sys_pfd1_divider),
        sys_pfd2_divider_(sys_pfd2_divider),
        sys_pfd3_divider_(sys_pfd3_divider) {}

  /// Constructor to place the Sys PLL into bypass mode.
  template <typename SourceType>
  constexpr ClockMcuxpressoSysPll(SourceType& source,
                                  sys_pll_src_t bypass_source)
      : DependentElement<ElementType>(source), bypass_source_(bypass_source) {}

 private:
  /// Configures and enables the audio PLL if `config_` is set, otherwise places
  /// the sys PLL in bypass mode.
  Status DoEnable() override {
    // If `config_` is specified, the PLL should be enabled and the phase
    // fractional divider PFD0 needs to get configured, otherwise the PLL
    // operates in bypass mode.
    if (config_ != nullptr) {
      // Configure Sys PLL clock source.
      CLOCK_InitSysPll(config_);

      if (sys_pfd0_divider_ != 0) {
        CLOCK_InitSysPfd(kCLOCK_Pfd0, sys_pfd0_divider_);
      }
      if (sys_pfd1_divider_ != 0) {
        CLOCK_InitSysPfd(kCLOCK_Pfd1, sys_pfd1_divider_);
      }
      if (sys_pfd2_divider_ != 0) {
        CLOCK_InitSysPfd(kCLOCK_Pfd2, sys_pfd2_divider_);
      }
      if (sys_pfd3_divider_ != 0) {
        CLOCK_InitSysPfd(kCLOCK_Pfd3, sys_pfd3_divider_);
      }
    } else {
      // PLL operates in bypass mode.
      CLKCTL0->SYSPLL0CLKSEL = bypass_source_;
      CLKCTL0->SYSPLL0CTL0 |= CLKCTL0_SYSPLL0CTL0_BYPASS_MASK;
    }
    return OkStatus();
  }

  /// Disables the Sys PLL logic.
  Status DoDisable() override {
    if (config_ != nullptr) {
      // Clock gate all the phase fractional divider PFD.
      CLOCK_DeinitSysPfd(kCLOCK_Pfd0);
      CLOCK_DeinitSysPfd(kCLOCK_Pfd1);
      CLOCK_DeinitSysPfd(kCLOCK_Pfd2);
      CLOCK_DeinitSysPfd(kCLOCK_Pfd3);
    }

    // Power down Sys PLL
    CLOCK_DeinitSysPll();
    return OkStatus();
  }

  /// Optional Sys PLL configuration.
  const clock_sys_pll_config_t* config_ = nullptr;

  /// Optional Sys kCLOCK_Pfd clock divider value.
  const uint8_t sys_pfd0_divider_ = 0;
  const uint8_t sys_pfd1_divider_ = 0;
  const uint8_t sys_pfd2_divider_ = 0;
  const uint8_t sys_pfd3_divider_ = 0;

  /// Optional Sys PLL bypass clock source.
  const sys_pll_src_t bypass_source_ = kCLOCK_SysPllNone;
};

/// Alias for a blocking audio PLL clock tree element.
using ClockMcuxpressoAudioPllBlocking =
    ClockMcuxpressoAudioPll<ElementBlocking>;

/// Alias for a non-blocking audio PLL clock tree element where updates
/// cannot fail.
using ClockMcuxpressoAudioPllNonBlocking =
    ClockMcuxpressoAudioPll<ElementNonBlockingCannotFail>;

/// Alias for a blocking sys PLL clock tree element.
using ClockMcuxpressoSysPllBlocking = ClockMcuxpressoSysPll<ElementBlocking>;

/// Alias for a non-blocking sys PLL clock tree element where updates
/// cannot fail.
using ClockMcuxpressoSysPllNonBlocking =
    ClockMcuxpressoSysPll<ElementNonBlockingCannotFail>;

/// Class template implementing the Rtc clock tree element.
///
/// Template argument `ElementType` can be of class `ElementBlocking` or
/// `ElementNonBlockingCannotFail`.
template <typename ElementType>
class ClockMcuxpressoRtc final : public DependentElement<ElementType> {
 public:
  /// Constructor specifying the dependent clock tree element to enable the
  /// Rtc clock source.
  template <typename SourceType>
  constexpr ClockMcuxpressoRtc(SourceType& source)
      : DependentElement<ElementType>(source) {}

 private:
  /// Enable 32 kHz RTC oscillator
  Status DoEnable() final {
    // Enable 32kHZ output of RTC oscillator.
    CLOCK_EnableOsc32K(true);
    return OkStatus();
  }

  /// Disable 32 kHz RTS oscillator.
  Status DoDisable() final {
    // Disable 32KHz output of RTC oscillator.
    CLOCK_EnableOsc32K(false);
    return OkStatus();
  }
};

/// Alias for a blocking Rtc clock tree element.
/// This class should be used if the Rtc clock source depends on
/// another blocking clock tree element to enable the Rtc clock source.
using ClockMcuxpressoRtcBlocking = ClockMcuxpressoRtc<ElementBlocking>;

/// Alias for a non-blocking Rtc clock tree element where updates
/// cannot fail.
using ClockMcuxpressoRtcNonBlocking =
    ClockMcuxpressoRtc<ElementNonBlockingCannotFail>;

/// Class template implementing the `clock_ip_name_t` clocks.
/// Managing `clock_ip_name_t` clocks with the clock tree allows to
/// save power when `FSL_SDK_DISABLE_DRIVER_CLOCK_CONTROL` is set.
///
/// Template argument `ElementType` can be of class `ElementBlocking` or
/// `ElementNonBlockingCannotFail`.
template <typename ElementType>
class ClockMcuxpressoClockIp final : public DependentElement<ElementType> {
 public:
  /// Constructor specifying the dependent clock tree element to enable the
  /// `clock_ip_name_t` clock source.
  template <typename SourceType>
  constexpr ClockMcuxpressoClockIp(SourceType& source, clock_ip_name_t clock)
      : DependentElement<ElementType>(source), clock_(clock) {}

 private:
  /// Enable the clock.
  Status DoEnable() final {
    CLOCK_EnableClock(clock_);
    return OkStatus();
  }

  /// Disable the clock.
  Status DoDisable() final {
    CLOCK_DisableClock(clock_);
    return OkStatus();
  }

  clock_ip_name_t clock_;
};

/// Alias for a blocking ClockIp clock tree element.
/// This class should be used if the ClockIp clock source depends on
/// another blocking clock tree element to enable the ClockIp clock source.
using ClockMcuxpressoClockIpBlocking = ClockMcuxpressoClockIp<ElementBlocking>;

/// Alias for a non-blocking ClockIp clock tree element where updates
/// cannot fail.
using ClockMcuxpressoClockIpNonBlocking =
    ClockMcuxpressoClockIp<ElementNonBlockingCannotFail>;
}  // namespace pw::clock_tree
