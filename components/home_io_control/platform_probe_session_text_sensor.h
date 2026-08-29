#pragma once

/// @file platform_probe_session_text_sensor.h
/// @brief Diagnostic text sensor tracking a running probe session's progress.
/// @ingroup hioc_platforms

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Diagnostic text sensor publishing probe-session progress and its final tally.
///
/// A session's real output is the log, but a run lasts minutes and the log is exactly what the
/// user is busy capturing — so "is it still going, and did it finish" needs to be answerable
/// without reading it. That is all this publishes: `running <done>/<total>` while stepping, then a
/// one-line tally. Nothing is published before the first session of this boot.
/// @ingroup hioc_platforms
class IOHomeProbeSessionTextSensor : public text_sensor::TextSensor, public Component {
 public:
  /// @brief Set the parent controller component.
  /// @param parent Pointer to the IOHomeControlComponent instance.
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }

  /// @brief Register the probe-session status callback.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  IOHomeControlComponent *parent_{nullptr};
};

}  // namespace home_io_control
}  // namespace esphome
