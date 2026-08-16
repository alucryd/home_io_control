/// @file radio_dual_sx1276.cpp
/// @brief Two-receiver composite driver implementation.
/// @ingroup hioc_radio

#include "radio_dual_sx1276.h"

#include "esphome/core/application.h"

#include <cinttypes>
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.dual_sx1276";

uint32_t RadioDualSX1276::next_scan_channel(uint32_t current) { return current == FREQ_CH1 ? FREQ_CH3 : FREQ_CH1; }

bool RadioDualSX1276::init() {
  if (!this->primary_->init()) {
    ESP_LOGE(TAG, "Primary SX1276 failed to initialize");
    return false;
  }
  if (!this->secondary_->init()) {
    // Deliberately fatal rather than degrading to one radio. A board configured for two receivers
    // that silently runs on one would present as healthy while missing a third of the air, and the
    // resulting intermittent "no reply" failures are exactly the kind that cost days to diagnose.
    ESP_LOGE(TAG, "Secondary SX1276 failed to initialize");
    return false;
  }
  // Park the pair: primary on the command channel for good, secondary on the first scan channel.
  this->primary_->change_frequency(PINNED_CHANNEL);
  this->secondary_->change_frequency(FREQ_CH1);
  this->current_freq_ = PINNED_CHANNEL;
  ESP_LOGI(TAG, "Dual SX1276 initialized (primary + secondary receiver)");
  return true;
}

bool RadioDualSX1276::send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) {
  // Only the primary transmits. The secondary stays in receive throughout, so a reply that lands
  // on its channel during or immediately after this transmission is still captured — the pair has
  // no transmit/receive turnaround between them the way a single radio does.
  const bool ok = this->primary_->send_packet(data, len, tx_config);
  // A transmission retunes the radio it goes out on. Commands use CH2 so this is normally a no-op,
  // but anything transmitting elsewhere would otherwise leave the primary stranded off its pin and
  // silently cost the coverage this board exists to buy.
  if (this->primary_->get_current_freq() != PINNED_CHANNEL)
    this->primary_->change_frequency(PINNED_CHANNEL);
  this->current_freq_ = PINNED_CHANNEL;
  return ok;
}

bool RadioDualSX1276::wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) {
  this->clear_last_capture();
  packet = RadioRxPacket{};

  const uint32_t start = millis();
  while (true) {
    // Poll both every pass rather than waiting on one: a blocking wait on either radio would spend
    // the whole window deaf to the other, which is the entire thing this class exists to avoid.
    if (this->primary_->check_for_packet(packet)) {
      this->adopt_capture_(*this->primary_);
      return true;
    }
    if (this->secondary_->check_for_packet(packet)) {
      this->adopt_capture_(*this->secondary_);
      return true;
    }
    if (millis() - start > timeout_ms)
      return false;
    App.feed_wdt();
    delay(1);
  }
}

bool RadioDualSX1276::check_for_packet(RadioRxPacket &packet) {
  if (this->primary_->check_for_packet(packet)) {
    this->adopt_capture_(*this->primary_);
    return true;
  }
  if (this->secondary_->check_for_packet(packet)) {
    this->adopt_capture_(*this->secondary_);
    return true;
  }
  return false;
}

void RadioDualSX1276::change_frequency(uint32_t freq_hz) {
  // `freq_hz` is intentionally unused. Callers are the hub's hop walk and discovery, both of which
  // simply want "move on"; honouring the request would drag the primary off CH2, and because the
  // walk derives its next channel from get_current_freq() a pinned primary makes it ask for the
  // same one every time. Advancing the secondary here is what turns those calls into a real sweep.
  (void) freq_hz;
  this->secondary_->change_frequency(next_scan_channel(this->secondary_->get_current_freq()));
  if (this->primary_->get_current_freq() != PINNED_CHANNEL)
    this->primary_->change_frequency(PINNED_CHANNEL);
  this->current_freq_ = PINNED_CHANNEL;
}

void RadioDualSX1276::set_mode_rx() {
  this->primary_->set_mode_rx();
  this->secondary_->set_mode_rx();
}

void RadioDualSX1276::set_mode_standby() {
  this->primary_->set_mode_standby();
  this->secondary_->set_mode_standby();
}

void RadioDualSX1276::dump_debug() {
  ESP_LOGCONFIG(TAG, "  Dual SX1276 Diagnostic:");
  ESP_LOGCONFIG(TAG, "    Primary channel: %" PRIu32 " Hz", this->primary_->get_current_freq());
  this->primary_->dump_debug();
  ESP_LOGCONFIG(TAG, "    Secondary channel: %" PRIu32 " Hz", this->secondary_->get_current_freq());
  this->secondary_->dump_debug();
}

}  // namespace home_io_control
}  // namespace esphome
