#pragma once

/// @file radio_secondary_spi.h
/// @brief SPI access for a second radio sharing the hub's bus on its own chip select.
/// @ingroup hioc_radio
///
/// The hub component is itself an `spi::SPIDevice` and implements `SpiAccess` by delegating to the
/// chip select ESPHome configured for it. That is exactly one chip select, so a board carrying two
/// radios needs a second device object: the clock, MOSI and MISO lines are shared, and only CS
/// (plus reset and DIO0, which are plain GPIO and belong to the driver) differ.
///
/// This is that object and nothing more — no radio knowledge, no protocol knowledge. It exists so
/// `RadioDualSX1276`'s second receiver can be handed a `SpiAccess` that asserts the right chip
/// select, while ESPHome's own SPI layer keeps arbitrating the shared bus between the two.

#include "esphome/components/spi/spi.h"
#include "esphome/core/component.h"
#include "radio_interface.h"

namespace esphome {
namespace home_io_control {

/// @brief A second chip select on the hub's SPI bus, presented as a SpiAccess.
/// @ingroup hioc_radio
///
/// Matches the hub's own bus parameters, because both radios are the same part on the same wires;
/// if a future board pairs unlike chips this is where the difference would live.
class RadioSecondarySpi : public Component,
                          public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                                spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_8MHZ>,
                          public SpiAccess {
 public:
  /// @brief Claim the chip select. Runs before the radio driver's init(), which needs the bus.
  void setup() override { this->spi_setup(); }

  /// @brief Get setup priority so the bus is ready before any driver touches it.
  /// @return setup_priority::BUS.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::BUS; }

  // --- SpiAccess implementation (delegates to SPIDevice, same as the hub does for its own) ---
  void spi_enable() override { this->enable(); }
  void spi_disable() override { this->disable(); }
  uint8_t spi_transfer(uint8_t data) override { return this->transfer_byte(data); }
  void spi_write(uint8_t data) override { this->write_byte(data); }
  uint8_t spi_read() override { return this->read_byte(); }
};

}  // namespace home_io_control
}  // namespace esphome
