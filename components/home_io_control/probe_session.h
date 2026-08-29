#pragma once

/// @file probe_session.h
/// @brief Ordered plan of diagnostic probes across every registered device, advanced one step at a
///        time from the hub loop.
/// @ingroup hioc_diagnostics
///
/// `probe_device` and `probe_sweep` (management_actions.h) answer one question about one device per
/// gesture. Characterising a whole installation means repeating that by hand for every probe, every
/// documented starting index and every paired device — dozens of Home Assistant action calls whose
/// replies then have to be stitched back together from a log that was never captured as one run.
///
/// This is that walk, expressed once: a fixed plan of (device, probe, index) steps with a cursor,
/// so one button press produces one delimited log session covering the whole installation.
///
/// Two things it deliberately is not:
///
/// - **Not a wider sweep.** The plan uses exactly the "Start with" values documented per probe in
///   docs/diagnostic-probes.md, which are the safest available for each — field-observed on real
///   hubs talking to real motors, or (for `private_fn`) known from production software. Widening
///   past them is described there as a separate, deliberate decision, and automating it across
///   every paired device at once is the opposite of deliberate. There is likewise no `unknown4a`
///   step, because no such probe exists to schedule (ADR 0024).
/// - **Not a blocking run.** `probe_sweep` accepts blocking the loop for ~a minute because it is
///   bounded to one device and one probe. This plan is that many times over — a session across a
///   real installation runs for many minutes, which as one synchronous call would mean a dead API,
///   a dead OTA and a watchdog. So the class holds only the plan and the cursor; the hub executes
///   one step per loop pass, leaving every other component serviced in between.
///
/// The class itself is pure bookkeeping — no radio, no hub, no clock — so the plan's shape and its
/// skip/advance rules are testable without hardware.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace home_io_control {

/// @brief One probe frame in a session plan.
/// @ingroup hioc_diagnostics
struct ProbeStep {
  std::string device_id;  ///< Target device, as registered.
  std::string probe;      ///< Probe name, one of ManagementActions::probe_device()'s names.
  std::string index;      ///< Index argument in probe_device()'s string form; empty for
                          ///< `general_info3`, which takes none.
};

/// @brief A fixed multi-device diagnostic probe plan and a cursor into it.
/// @ingroup hioc_diagnostics
class ProbeSession {
 public:
  /// @brief Gap between steps, matching `probe_sweep`'s own index spacing.
  ///
  /// Same reasoning, and it applies more strongly here: this transmits on a shared ISM band to
  /// what are often battery devices, so a tight loop is antisocial and would also keep a device
  /// awake long enough to skew the very replies the session exists to collect.
  static constexpr uint32_t STEP_DELAY_MS = 1000;

  /// @brief How one executed step turned out, as far as the plan is concerned.
  enum class StepOutcome : uint8_t {
    ANSWERED,       ///< Device replied; the reply is in the log.
    SILENT,         ///< No usable reply (nothing received, or an error-coded one).
    REFUSED_DEVICE  ///< The refusal will recur for every remaining step on this device
                    ///< (ManagementActionResult::terminal_refusal), so skip the rest of it.
  };

  /// @brief The steps this session runs for a single device, in execution order.
  ///
  /// Ordered least-speculative first: `general_info3` sends no payload at all, then the three
  /// probes whose indices were observed on the wire from real hubs, and `private_fn` last as the
  /// only one whose starting values come from production software elsewhere rather than from a
  /// capture of our own. A session that upsets a device is then most likely to have got the
  /// better-evidenced answers out first.
  /// @param device_id Device the steps target.
  /// @return This device's steps.
  [[nodiscard]] static std::vector<ProbeStep> plan_for_device(const std::string &device_id);

  /// @brief Number of steps plan_for_device() produces, for sizing and log summaries.
  [[nodiscard]] static std::size_t steps_per_device();

  /// @brief Build the plan for @p device_ids and arm the cursor.
  /// @param device_ids Devices to probe, in the order they should be visited.
  /// @return false (and stays idle) if @p device_ids is empty or a session is already running.
  bool start(const std::vector<std::string> &device_ids);

  /// @return true while steps remain; false once the plan is exhausted or abort() was called.
  [[nodiscard]] bool is_running() const { return this->cursor_ < this->steps_.size(); }

  /// @brief The step to execute now.
  /// @pre is_running().
  [[nodiscard]] const ProbeStep &current() const { return this->steps_[this->cursor_]; }

  /// @brief Fold an executed step's outcome into the totals and advance the cursor.
  ///
  /// On REFUSED_DEVICE the cursor skips every remaining step for the same device rather than just
  /// the current one, and those are counted as skipped: a terminal refusal means the same answer
  /// is waiting for each of them, and re-asking would fill the log with identical refusals for no
  /// information. The session continues with the next device — the realistic cause is one device
  /// being mid-movement, which says nothing about the others.
  /// @pre is_running().
  /// @param outcome How the step at current() turned out.
  void record(StepOutcome outcome);

  /// @brief End the session immediately, leaving the totals readable for a final summary.
  void abort() { this->cursor_ = this->steps_.size(); }

  /// @return 1-based position of current() within the plan; total_steps() + 1 once finished.
  [[nodiscard]] std::size_t step_number() const { return this->cursor_ + 1; }
  /// @return Total number of steps in the plan.
  [[nodiscard]] std::size_t total_steps() const { return this->steps_.size(); }
  /// @return Number of devices the plan covers.
  [[nodiscard]] std::size_t device_count() const { return this->device_count_; }
  /// @return Steps that produced a reply.
  [[nodiscard]] std::size_t answered() const { return this->answered_; }
  /// @return Steps that produced no usable reply.
  [[nodiscard]] std::size_t silent() const { return this->silent_; }
  /// @return Steps never sent, because their device refused terminally.
  [[nodiscard]] std::size_t skipped() const { return this->skipped_; }

 private:
  std::vector<ProbeStep> steps_;
  std::size_t cursor_{0};
  std::size_t device_count_{0};
  std::size_t answered_{0};
  std::size_t silent_{0};
  std::size_t skipped_{0};
};

}  // namespace home_io_control
}  // namespace esphome
