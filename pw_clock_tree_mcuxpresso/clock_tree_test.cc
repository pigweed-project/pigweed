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

#include "pw_clock_tree_mcuxpresso/clock_tree.h"

#include "fsl_clock.h"
#include "fsl_power.h"
#include "mocks.h"
#include "pw_unit_test/framework.h"

namespace pw::clock_tree {
namespace {

class ClockMcuxpressoTest : public ::testing::Test {
 protected:
  void SetUp() override { sdk_state.Reset(); }
};

TEST_F(ClockMcuxpressoTest, Fro) {
  ClockMcuxpressoFroSource fro;
  ClockMcuxpressoFroDivider fro_div1(fro, kCLOCK_FroDiv1OutEn);
  fro_div1.Acquire();
  EXPECT_EQ(sdk_state.fro_enabled_mask, kCLOCK_FroDiv1OutEn);
  fro_div1.Release();
  EXPECT_EQ(sdk_state.fro_enabled_mask, 0u);
}

TEST_F(ClockMcuxpressoTest, LpOsc) {
  ClockMcuxpressoLpOsc lp_osc;
  lp_osc.Acquire();
  EXPECT_TRUE(sdk_state.lp_osc_pd_disabled);
  EXPECT_TRUE(sdk_state.lp_osc_clk_enabled);
  lp_osc.Release();
  EXPECT_FALSE(sdk_state.lp_osc_pd_disabled);
}

TEST_F(ClockMcuxpressoTest, Mclk) {
  class MockSource : public ClockSourceNonBlocking {
   public:
    Status DoEnable() override { return OkStatus(); }
    Status DoDisable() override { return OkStatus(); }
  } source;

  ClockMcuxpressoMclkNonBlocking mclk(source, 12345u);
  mclk.Acquire();
  EXPECT_EQ(sdk_state.mclk_freq, 12345u);
  mclk.Release();
  EXPECT_EQ(sdk_state.mclk_freq, 0u);
}

TEST_F(ClockMcuxpressoTest, ClkIn) {
  class MockSource : public ClockSourceNonBlocking {
   public:
    Status DoEnable() override { return OkStatus(); }
    Status DoDisable() override { return OkStatus(); }
  } source;

  ClockMcuxpressoClkInNonBlocking clk_in(source, 24000000u);
  clk_in.Acquire();
  EXPECT_EQ(sdk_state.clkin_freq, 24000000u);
  EXPECT_EQ(CLKCTL0->SYSOSCBYPASS, CLKCTL0_SYSOSCBYPASS_SEL(1));
  clk_in.Release();
  EXPECT_EQ(sdk_state.clkin_freq, 0u);
  EXPECT_EQ(CLKCTL0->SYSOSCBYPASS, CLKCTL0_SYSOSCBYPASS_SEL(7));
}

TEST_F(ClockMcuxpressoTest, Frg) {
  class MockSource : public ClockSourceNonBlocking {
   public:
    Status DoEnable() override { return OkStatus(); }
    Status DoDisable() override { return OkStatus(); }
  } source;

  clock_frg_clk_config_t config = {
      0, clock_frg_clk_config_t::kCLOCK_FrgMainClk, 1, 2};
  ClockMcuxpressoFrgNonBlocking frg(source, config);
  frg.Acquire();
  EXPECT_TRUE(sdk_state.frg_config_set);
  EXPECT_EQ(sdk_state.frg_config.sfg_clock_src,
            clock_frg_clk_config_t::kCLOCK_FrgMainClk);

  frg.Release();
  EXPECT_TRUE(sdk_state.frg_config_set);
  EXPECT_EQ(sdk_state.frg_config.sfg_clock_src,
            clock_frg_clk_config_t::kCLOCK_FrgNone);
}

TEST_F(ClockMcuxpressoTest, Selector) {
  class MockSource : public ClockSourceNonBlocking {
   public:
    Status DoEnable() override { return OkStatus(); }
    Status DoDisable() override { return OkStatus(); }
  } source;

  ClockMcuxpressoSelectorNonBlocking selector(source, 10, 20);
  selector.Acquire();
  EXPECT_EQ(sdk_state.last_attached_clk, 10u);
  selector.Release();
  EXPECT_EQ(sdk_state.last_attached_clk, 20u);
}

TEST_F(ClockMcuxpressoTest, Divider) {
  class MockSource : public ClockSourceNonBlocking {
   public:
    Status DoEnable() override { return OkStatus(); }
    Status DoDisable() override { return OkStatus(); }
  } source;

  ClockMcuxpressoDividerNonBlocking divider(source, 5, 2);
  divider.Acquire();
  EXPECT_EQ(sdk_state.last_div_name, 5u);
  EXPECT_EQ(sdk_state.last_div_val, 2u);
  EXPECT_EQ(source.ref_count(), 1u);

  divider.Release();
  EXPECT_EQ(source.ref_count(), 0u);
}

TEST_F(ClockMcuxpressoTest, AudioPll) {
  class MockSource : public ClockSourceNonBlocking {
   public:
    Status DoEnable() override { return OkStatus(); }
    Status DoDisable() override { return OkStatus(); }
  } source;

  clock_audio_pll_config_t config = {
      kCLOCK_AudioPllXtalIn, 0, 0, kCLOCK_AudioPllMult16};
  ClockMcuxpressoAudioPllNonBlocking audio_pll(source, config, 10);
  audio_pll.Acquire();
  EXPECT_TRUE(sdk_state.audio_pll_config_set);
  EXPECT_EQ(sdk_state.audio_pll_config.audio_pll_src, kCLOCK_AudioPllXtalIn);

  audio_pll.Release();
  EXPECT_FALSE(sdk_state.audio_pll_config_set);
}

TEST_F(ClockMcuxpressoTest, AudioPllBypass) {
  class MockSource : public ClockSourceNonBlocking {
   public:
    Status DoEnable() override { return OkStatus(); }
    Status DoDisable() override { return OkStatus(); }
  } source;

  ClockMcuxpressoAudioPllNonBlocking audio_pll(source,
                                               kCLOCK_AudioPllFroDiv8Clk);
  audio_pll.Acquire();
  EXPECT_EQ(CLKCTL1->AUDIOPLL0CLKSEL, (uint32_t)kCLOCK_AudioPllFroDiv8Clk);
  EXPECT_TRUE(CLKCTL1->AUDIOPLL0CTL0 & CLKCTL1_AUDIOPLL0CTL0_BYPASS_MASK);

  audio_pll.Release();
  EXPECT_FALSE(sdk_state.audio_pll_config_set);
}

TEST_F(ClockMcuxpressoTest, SysPll) {
  class MockSource : public ClockSourceNonBlocking {
   public:
    Status DoEnable() override { return OkStatus(); }
    Status DoDisable() override { return OkStatus(); }
  } source;

  clock_sys_pll_config_t config = {
      kCLOCK_SysPllXtalIn, 0, 0, kCLOCK_SysPllMult16};
  ClockMcuxpressoSysPllNonBlocking sys_pll(source, config, 1, 2, 3, 4);
  sys_pll.Acquire();
  EXPECT_TRUE(sdk_state.sys_pll_config_set);
  EXPECT_EQ(sdk_state.sys_pll_config.sys_pll_src, kCLOCK_SysPllXtalIn);

  sys_pll.Release();
  EXPECT_FALSE(sdk_state.sys_pll_config_set);
}

TEST_F(ClockMcuxpressoTest, Rtc) {
  class MockSource : public ClockSourceNonBlocking {
   public:
    Status DoEnable() override { return OkStatus(); }
    Status DoDisable() override { return OkStatus(); }
  } source;

  ClockMcuxpressoRtcNonBlocking rtc(source);
  rtc.Acquire();
  EXPECT_TRUE(sdk_state.osc32k_enabled);
  rtc.Release();
  EXPECT_FALSE(sdk_state.osc32k_enabled);
}

TEST_F(ClockMcuxpressoTest, ClockIp) {
  class MockSource : public ClockSourceNonBlocking {
   public:
    Status DoEnable() override { return OkStatus(); }
    Status DoDisable() override { return OkStatus(); }
  } source;

  ClockMcuxpressoClockIpNonBlocking clock_ip(source, 100);
  clock_ip.Acquire();
  EXPECT_EQ(sdk_state.last_enabled_clock_ip, 100u);
  clock_ip.Release();
  EXPECT_EQ(sdk_state.last_disabled_clock_ip, 100u);
}

}  // namespace
}  // namespace pw::clock_tree
