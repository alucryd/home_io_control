/// @file hub_probe_session.cpp
/// @brief Hub-side driver for ProbeSession: starting a run, and stepping it from loop().
/// @ingroup hioc_hub
///
/// ProbeSession holds the plan; this file is what turns it into frames on the air. The whole
/// arrangement exists so that a run covering a real installation — dozens of probes across every
/// paired device, minutes of radio time — does not become one blocking call. Each loop() pass
/// executes at most one probe, which blocks for the usual single-exchange duration (ADR 0013) and
/// no longer, so Home Assistant, OTA and every other component stay serviced for the whole run.
///
/// The output is the log. Every line a session emits carries the `probe_session` marker so a
/// capture can be sliced out of a busy log, bracketed by START/END lines carrying the totals.

#include "hub_internal.h"

namespace esphome {
namespace home_io_control {

namespace {

/// Marker on every session line, so one run greps out of a log that is also carrying normal
/// traffic — which it will be, since the hub keeps operating throughout.
constexpr const char *SESSION_MARKER = "probe_session";

/// Classify a completed probe for the plan's bookkeeping. A terminal refusal is the only outcome
/// that says anything about the device's *remaining* steps, which is what ProbeSession needs in
/// order to decide whether to skip them; everything else is per-step.
ProbeSession::StepOutcome outcome_of(const IOHomeControlComponent::ManagementActionResult &result) {
  if (result.terminal_refusal)
    return ProbeSession::StepOutcome::REFUSED_DEVICE;
  return result.success ? ProbeSession::StepOutcome::ANSWERED : ProbeSession::StepOutcome::SILENT;
}

}  // namespace

bool IOHomeControlComponent::start_probe_session() {
  if (!this->diagnostic_probes_enabled_) {
    ESP_LOGW(detail::TAG, "%s refused: diagnostic probes are not enabled; set diagnostic_probes: true in YAML",
             SESSION_MARKER);
    return false;
  }
  if (this->probe_session_.is_running()) {
    // Deliberately not a restart: the run in progress is already partly captured, and throwing it
    // away on a stray second press is the one failure mode a long diagnostic cannot recover from.
    ESP_LOGW(detail::TAG, "%s already running: step %zu/%zu", SESSION_MARKER, this->probe_session_.step_number(),
             this->probe_session_.total_steps());
    return false;
  }

  std::vector<std::string> device_ids;
  device_ids.reserve(this->registry_.size());
  for (const auto &entry : this->registry_)
    device_ids.push_back(entry.first);

  if (!this->probe_session_.start(device_ids)) {
    ESP_LOGW(detail::TAG, "%s refused: no registered devices to probe", SESSION_MARKER);
    return false;
  }

  this->probe_session_started_ms_ = millis();
  // Leave the first step a full delay away rather than firing it inside this call: start_probe_session()
  // runs from a button press on the API task's turn in the loop, and the run should begin from the
  // same paced cadence every later step keeps.
  this->probe_session_last_step_ms_ = this->probe_session_started_ms_;

  ESP_LOGI(detail::TAG, "===== %s START ===== radio=%s devices=%zu steps=%zu (%zu per device)", SESSION_MARKER,
           this->radio_ != nullptr ? this->radio_->chip_name() : "none", this->probe_session_.device_count(),
           this->probe_session_.total_steps(), ProbeSession::steps_per_device());
  this->publish_probe_session_status_("running 0/" + std::to_string(this->probe_session_.total_steps()));
  return true;
}

void IOHomeControlComponent::abort_probe_session() {
  if (!this->probe_session_.is_running())
    return;
  ESP_LOGW(detail::TAG, "%s aborted at step %zu/%zu", SESSION_MARKER, this->probe_session_.step_number(),
           this->probe_session_.total_steps());
  this->probe_session_.abort();
  this->finish_probe_session_();
}

void IOHomeControlComponent::advance_probe_session_() {
  if (!this->probe_session_.is_running())
    return;
  // Anything a user actually asked for goes first. A session runs for minutes, and it must never
  // be the reason a cover press or a due status poll waits behind it.
  if (!this->op_queue_.empty())
    return;
  const uint32_t now = millis();
  if (now - this->probe_session_last_step_ms_ < ProbeSession::STEP_DELAY_MS)
    return;

  const ProbeStep step = this->probe_session_.current();
  const std::size_t number = this->probe_session_.step_number();
  const std::size_t total = this->probe_session_.total_steps();

  // Through the virtual hub entry point rather than management_actions_ directly, so this driver is
  // exercisable against a mock hub like every other queue_*/set_* path on this component.
  const ManagementActionResult result = this->probe_device(step.device_id, step.probe, step.index);

  if (result.has_response_cmd) {
    ESP_LOGI(detail::TAG, "%s [%zu/%zu] device=%s probe=%s index=%s -> cmd=0x%02X hex=%s", SESSION_MARKER, number,
             total, step.device_id.c_str(), step.probe.c_str(), step.index.empty() ? "-" : step.index.c_str(),
             result.response_cmd, result.response_hex.c_str());
  } else {
    ESP_LOGI(detail::TAG, "%s [%zu/%zu] device=%s probe=%s index=%s -> %s", SESSION_MARKER, number, total,
             step.device_id.c_str(), step.probe.c_str(), step.index.empty() ? "-" : step.index.c_str(),
             result.message.c_str());
  }

  const ProbeSession::StepOutcome outcome = outcome_of(result);
  if (outcome == ProbeSession::StepOutcome::REFUSED_DEVICE) {
    ESP_LOGW(detail::TAG, "%s skipping remaining steps for device=%s: %s", SESSION_MARKER, step.device_id.c_str(),
             result.message.c_str());
  }
  this->probe_session_.record(outcome);

  // Measured from the end of the exchange, not its start: the delay is there to leave the device
  // and the band alone between probes, and an exchange that took three retries has already had it.
  this->probe_session_last_step_ms_ = millis();

  if (this->probe_session_.is_running()) {
    this->publish_probe_session_status_("running " + std::to_string(this->probe_session_.step_number() - 1) + "/" +
                                        std::to_string(total));
  } else {
    this->finish_probe_session_();
  }
}

void IOHomeControlComponent::finish_probe_session_() {
  const uint32_t elapsed_s = (millis() - this->probe_session_started_ms_) / 1000;
  ESP_LOGI(detail::TAG, "===== %s END ===== answered=%zu silent=%zu skipped=%zu of %zu steps in %us", SESSION_MARKER,
           this->probe_session_.answered(), this->probe_session_.silent(), this->probe_session_.skipped(),
           this->probe_session_.total_steps(), elapsed_s);
  this->publish_probe_session_status_("done " + std::to_string(this->probe_session_.answered()) + " answered, " +
                                      std::to_string(this->probe_session_.silent()) + " silent, " +
                                      std::to_string(this->probe_session_.skipped()) + " skipped");
}

void IOHomeControlComponent::publish_probe_session_status_(const std::string &status) {
  if (this->probe_session_status_callback_)
    this->probe_session_status_callback_(status);
}

}  // namespace home_io_control
}  // namespace esphome
