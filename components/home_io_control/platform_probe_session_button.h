#pragma once

/// @file platform_probe_session_button.h
/// @brief Button entity that starts a diagnostic probe session across every registered device.
/// @ingroup hioc_platforms
///
/// Generated only for a build configured with `diagnostic_probes: true`, like every other entity
/// belonging to an opt-in feature (see IOHomeLr1121FirmwareUpdateButton). The hub re-checks the
/// same flag when the button is pressed, so the gate does not depend on codegen alone.

#include "esphome/components/button/button.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Button that runs ProbeSession's whole plan when pressed in Home Assistant.
///
/// Press returns immediately: the session is armed here and stepped from the hub loop over the
/// following minutes. The result is the log — see IOHomeControlComponent::start_probe_session().
/// @ingroup hioc_platforms
class IOHomeProbeSessionButton : public button::Button, public Component {
 public:
  /// @brief Set the parent controller component.
  /// @param parent Pointer to the IOHomeControlComponent instance.
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }
  void dump_config() override {}

 protected:
  /// @brief Arm a probe session. Refusals are logged by the hub, not raised here.
  void press_action() override { this->parent_->start_probe_session(); }
  IOHomeControlComponent *parent_{nullptr};
};

/// @brief Button that stops a running probe session at the next step boundary.
///
/// A separate entity rather than a second press of the start button: a session runs for minutes,
/// so it needs a way out, but overloading one button would make a mistimed double-click discard a
/// run it had just started. No-op when nothing is running.
/// @ingroup hioc_platforms
class IOHomeProbeSessionStopButton : public button::Button, public Component {
 public:
  /// @brief Set the parent controller component.
  /// @param parent Pointer to the IOHomeControlComponent instance.
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }
  void dump_config() override {}

 protected:
  /// @brief Stop the running session, if any.
  void press_action() override { this->parent_->abort_probe_session(); }
  IOHomeControlComponent *parent_{nullptr};
};

}  // namespace home_io_control
}  // namespace esphome
