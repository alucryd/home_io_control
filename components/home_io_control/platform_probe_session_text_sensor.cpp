/// @file platform_probe_session_text_sensor.cpp
/// @brief Diagnostic text sensor tracking a running probe session's progress.
/// @ingroup hioc_platforms

#include "platform_probe_session_text_sensor.h"

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.probe_session";

void IOHomeProbeSessionTextSensor::setup() {
  if (this->parent_ == nullptr)
    return;

  this->parent_->set_probe_session_status_callback([this](const std::string &status) { this->publish_state(status); });
}

void IOHomeProbeSessionTextSensor::dump_config() { LOG_TEXT_SENSOR("", "IO-Homecontrol Probe Session", this); }

}  // namespace home_io_control
}  // namespace esphome
