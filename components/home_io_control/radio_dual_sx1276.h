#pragma once

/// @file radio_dual_sx1276.h
/// @brief Two-receiver composite driver for boards carrying a pair of SX1276 radios.
/// @ingroup hioc_radio
///
/// A single receiver can only ever be on one of the protocol's three channels, so it hears about a
/// third of the air. Devices answer within ~20 ms of a command and do not always answer on the
/// channel the command went out on, which is why a missed reply shows up as `saw_challenge=0` with
/// nothing received at all rather than as a late arrival — by the time a hop reaches the right
/// channel the reply is long gone.
///
/// Two radios covering two channels at once turn that third into two thirds. This class is the
/// whole feature: it presents one RadioDriver to the layers above, so ExchangeEngine, the hub loop
/// and the hop logic are untouched and keep working exactly as they do on a single-radio board.
///
/// Channel policy: the primary is **pinned to CH2** and does all transmitting; the secondary
/// alternates between CH1 and CH3. CH2 is where every command goes out and where most replies were
/// observed to land, so it is never given up; the other two are covered half the time each, which
/// is strictly more than the third a lone hopping receiver manages and never leaves the busiest
/// channel unwatched.
///
/// A consequence worth knowing: this driver deliberately **ignores the channel it is asked to tune
/// to**. The hub's hop walk reads get_current_freq() to pick a successor, so a pinned primary would
/// otherwise make it request the same channel forever and strand the secondary there. Each
/// change_frequency() call instead advances the secondary to the other scan channel, which turns
/// the existing hop into the thing that drives the sweep without the hub needing to know any of
/// this.

#include "radio_interface.h"

namespace esphome {
namespace home_io_control {

/// @brief Composite RadioDriver over a pair of SX1276 receivers.
/// @ingroup hioc_radio
class RadioDualSX1276 : public RadioDriver {
 public:
  /// Takes ownership of both receivers and deletes them, so the hub's existing single
  /// `delete radio_` stays correct and neither half can outlive the composite.
  ///
  /// Injected rather than constructed in place: the composite's job is fan-in and channel
  /// pairing, none of which is SX1276-specific, and taking drivers lets that logic be tested
  /// against doubles instead of against register programming another suite already covers.
  /// @param primary   Receiver that transmits and holds the exchange's channel. Must not be null.
  /// @param secondary Receive-only companion, parked one channel ahead. Must not be null.
  RadioDualSX1276(RadioDriver *primary, RadioDriver *secondary)
      : RadioDriver(nullptr), primary_(primary), secondary_(secondary) {}

  ~RadioDualSX1276() override {
    delete this->primary_;
    delete this->secondary_;
  }

  RadioDualSX1276(const RadioDualSX1276 &) = delete;
  RadioDualSX1276 &operator=(const RadioDualSX1276 &) = delete;

  /// Channel the primary never leaves: the one commands are transmitted on.
  static constexpr uint32_t PINNED_CHANNEL = FREQ_CH2;

  /// @brief The scan channel the secondary moves to next.
  ///
  /// Alternates between the two channels the pinned primary does not cover. Never returns
  /// PINNED_CHANNEL — a secondary that joined the primary would leave a third of the air unwatched
  /// while adding nothing.
  /// @param current Channel the secondary is on now.
  /// @return The other scan channel.
  [[nodiscard]] static uint32_t next_scan_channel(uint32_t current);

  /// @copydoc RadioDriver::init
  bool init() override;
  /// @copydoc RadioDriver::send_packet
  bool send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) override;
  /// @copydoc RadioDriver::wait_for_packet
  bool wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) override;
  /// @copydoc RadioDriver::check_for_packet
  bool check_for_packet(RadioRxPacket &packet) override;
  /// @copydoc RadioDriver::change_frequency
  void change_frequency(uint32_t freq_hz) override;
  /// @copydoc RadioDriver::set_mode_rx
  void set_mode_rx() override;
  /// @copydoc RadioDriver::set_mode_standby
  void set_mode_standby() override;
  /// @copydoc RadioDriver::read_rssi
  ///
  /// The primary's, because the only caller is listen-before-talk and the primary is the only
  /// radio that transmits — the channel the secondary happens to be watching says nothing about
  /// whether this hub may transmit.
  int16_t read_rssi() override { return this->primary_->read_rssi(); }
  /// @copydoc RadioDriver::is_sync_detected
  bool is_sync_detected() override {
    return this->primary_->is_sync_detected() || this->secondary_->is_sync_detected();
  }
  /// @copydoc RadioDriver::is_preamble_detected
  bool is_preamble_detected() override {
    return this->primary_->is_preamble_detected() || this->secondary_->is_preamble_detected();
  }
  /// @copydoc RadioDriver::apply_tuning
  void apply_tuning(const TuningConfig &tuning) override {
    this->primary_->apply_tuning(tuning);
    this->secondary_->apply_tuning(tuning);
  }
  /// @copydoc RadioDriver::hop_dwell_ms
  ///
  /// The primary's. This answers a chip question — how long an SX1276 must sit on a channel before
  /// it can hear anything — and both halves are the same part, so either would give the same
  /// number.
  [[nodiscard]] uint16_t hop_dwell_ms(const TuningConfig &tuning) const override {
    return this->primary_->hop_dwell_ms(tuning);
  }
  /// @copydoc RadioDriver::has_fast_tx_rx_turnaround
  [[nodiscard]] bool has_fast_tx_rx_turnaround() const override { return true; }
  /// @copydoc RadioDriver::response_preamble
  [[nodiscard]] uint16_t response_preamble() const override { return this->primary_->response_preamble(); }
  /// @brief Failed if either radio is unusable — a silent single-radio fallback would look like a
  /// working dual board while quietly halving its coverage.
  [[nodiscard]] bool is_failed() const override { return this->primary_->is_failed() || this->secondary_->is_failed(); }
  /// @copydoc RadioDriver::chip_name
  [[nodiscard]] const char *chip_name() const override { return "dual_sx1276"; }
  /// @copydoc RadioDriver::dump_debug
  void dump_debug() override;

 protected:
  /// Adopt a receiver's capture as this composite's own, so get_last_capture() describes the frame
  /// that was actually received rather than whichever radio happened to be polled last.
  void adopt_capture_(const RadioDriver &from) { this->last_capture_ = from.get_last_capture(); }

  RadioDriver *primary_;
  RadioDriver *secondary_;
};

}  // namespace home_io_control
}  // namespace esphome
