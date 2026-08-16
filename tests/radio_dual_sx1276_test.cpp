#include "radio_dual_sx1276.h"

#include "exchange_engine.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <set>

using namespace esphome::home_io_control;

// ============================================================================
// RadioDualSX1276 test suite
// ============================================================================
// The composite's whole job is that neither receiver's state is lost: a frame arriving on either
// one must reach the caller, and the pair must cover two different channels at all times.

/// The composite owns its receivers, so hand it MockRadios and keep borrowed pointers to drive
/// them. Nothing here exercises SX1276 register programming — radio_sx1276_test.cpp covers that.
/// What is under test is fan-in and channel pairing, which is all this class actually does.
struct DualFixture {
  MockRadio *primary;
  MockRadio *secondary;
  RadioDualSX1276 radio;

  DualFixture() : primary(new MockRadio()), secondary(new MockRadio()), radio(primary, secondary) {}
};

static RadioRxPacket make_packet(uint8_t marker) {
  RadioRxPacket pkt{};
  pkt.len = 1;
  pkt.data[0] = marker;
  return pkt;
}

// --- Channel policy ------------------------------------------------------

TEST(RadioDualSX1276, ScanChannelAlternatesAndNeverTakesThePinnedOne) {
  // A secondary that wandered onto CH2 would duplicate the primary and leave a third of the air
  // unwatched while adding nothing.
  uint32_t ch = FREQ_CH1;
  for (int i = 0; i < 6; i++) {
    ch = RadioDualSX1276::next_scan_channel(ch);
    EXPECT_NE(ch, RadioDualSX1276::PINNED_CHANNEL) << "the scan must never land on the pinned channel";
  }
  EXPECT_EQ(RadioDualSX1276::next_scan_channel(FREQ_CH1), FREQ_CH3);
  EXPECT_EQ(RadioDualSX1276::next_scan_channel(FREQ_CH3), FREQ_CH1);
}

TEST(RadioDualSX1276, HoppingHoldsThePinnedChannelAndSweepsTheOtherTwo) {
  DualFixture f;
  f.primary->change_frequency(RadioDualSX1276::PINNED_CHANNEL);
  f.secondary->change_frequency(FREQ_CH1);

  std::set<uint32_t> scanned;
  for (int i = 0; i < 4; i++) {
    // The hub's hop passes whatever its walk computed; this driver ignores it by design.
    f.radio.change_frequency(FREQ_CH3);
    EXPECT_EQ(f.primary->get_current_freq(), RadioDualSX1276::PINNED_CHANNEL)
        << "the primary must never leave the channel commands go out on";
    scanned.insert(f.secondary->get_current_freq());
  }
  EXPECT_EQ(scanned, (std::set<uint32_t>{FREQ_CH1, FREQ_CH3}))
      << "repeated hops must cover both remaining channels, not stick on one";
}

TEST(RadioDualSX1276, TransmittingElsewhereStillLeavesThePrimaryPinned) {
  // A transmission retunes the radio it goes out on; without restoring the pin, one such frame
  // would silently cost the coverage this board exists to buy.
  DualFixture f;
  f.primary->change_frequency(RadioDualSX1276::PINNED_CHANNEL);

  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH3;
  cfg.preamble_len = SHORT_PREAMBLE;
  const uint8_t frame[] = {0xC8, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x31};
  f.radio.send_packet(frame, sizeof(frame), cfg);

  EXPECT_EQ(f.primary->get_current_freq(), RadioDualSX1276::PINNED_CHANNEL);
  EXPECT_EQ(f.radio.get_current_freq(), RadioDualSX1276::PINNED_CHANNEL);
}

// --- Receive fan-in --------------------------------------------------------

TEST(RadioDualSX1276, AFrameOnTheSecondaryReachesTheCaller) {
  // The failure this whole class exists to prevent: a reply arriving on the channel the primary is
  // not watching must not be lost.
  DualFixture f;
  f.secondary->queue_rx(make_packet(0xBB));

  RadioRxPacket got{};
  ASSERT_TRUE(f.radio.wait_for_packet(got, 500));
  EXPECT_EQ(got.data[0], 0xBB);
}

TEST(RadioDualSX1276, AFrameOnThePrimaryReachesTheCaller) {
  DualFixture f;
  f.primary->queue_rx(make_packet(0xAA));

  RadioRxPacket got{};
  ASSERT_TRUE(f.radio.wait_for_packet(got, 500));
  EXPECT_EQ(got.data[0], 0xAA);
}

TEST(RadioDualSX1276, WaitReturnsFalseWhenNeitherReceiverHearsAnything) {
  DualFixture f;
  RadioRxPacket got{};
  EXPECT_FALSE(f.radio.wait_for_packet(got, 5));
  EXPECT_EQ(got.len, 0u) << "a timed-out wait must not hand back a stale packet";
}

TEST(RadioDualSX1276, OnlyThePrimaryTransmits) {
  // A second transmitter would double this hub's air time for nothing; the second radio exists to
  // stop frames being missed, not to send more of them.
  DualFixture f;
  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = SHORT_PREAMBLE;
  const uint8_t frame[] = {0xC8, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x31};
  f.radio.send_packet(frame, sizeof(frame), cfg);

  EXPECT_EQ(f.primary->get_send_count(), 1);
  EXPECT_EQ(f.secondary->get_send_count(), 0) << "the secondary must stay in receive throughout";
}

// --- Composite identity ----------------------------------------------------

TEST(RadioDualSX1276, IdentifiesItselfAsThePair) {
  DualFixture f;
  EXPECT_FALSE(f.radio.is_failed()) << "a freshly constructed pair is not failed";
  EXPECT_STREQ(f.radio.chip_name(), "dual_sx1276");
  EXPECT_TRUE(f.radio.has_fast_tx_rx_turnaround()) << "both halves are SX1276s";
}

// ============================================================================
// Integration with the hub's own rotation.
//
// The composite reports a constant channel from get_current_freq(), and ExchangeEngine's
// hop_frequency() both derives its next channel from that value and loops until it lands on one
// that is not `skip_freq`. Those two facts have to be checked together: a pinned reading is exactly
// the input that could make that loop spin, and no test of either side alone would show it.
// ============================================================================

TEST(RadioDualSX1276, SurvivesEveryHopSkipValueTheEngineCanAskFor) {
  auto *primary = new MockRadio();
  auto *secondary = new MockRadio();
  RadioDualSX1276 dual(primary, secondary);
  RadioDriver *as_driver = &dual;

  TuningConfig tuning;
  ExchangeEngine engine(&as_driver, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // 0 is "skip nothing" (ROTATE_ALL_CHANNELS); the others are what ROTATE_SKIPPING_REQUEST passes
  // for each possible request channel.
  for (uint32_t skip :
       {0u, static_cast<uint32_t>(FREQ_CH1), static_cast<uint32_t>(FREQ_CH2), static_cast<uint32_t>(FREQ_CH3)}) {
    std::set<uint32_t> scanned;
    for (int i = 0; i < 4; i++) {
      engine.hop_frequency(skip);  // must terminate — a pinned reading could otherwise spin it
      EXPECT_EQ(primary->get_current_freq(), RadioDualSX1276::PINNED_CHANNEL)
          << "the primary stays on the command channel whatever the engine asks for";
      scanned.insert(secondary->get_current_freq());
    }
    EXPECT_EQ(scanned, (std::set<uint32_t>{FREQ_CH1, FREQ_CH3}))
        << "the secondary still sweeps both scan channels for skip=" << skip;
  }
}
