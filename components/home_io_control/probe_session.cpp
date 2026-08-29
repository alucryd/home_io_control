/// @file probe_session.cpp
/// @brief ProbeSession plan construction and cursor advance.

#include "probe_session.h"

namespace esphome {
namespace home_io_control {

namespace {

/// One probe and one index to send it with. Mirrors the "Start with" column of
/// docs/diagnostic-probes.md's probe table; see ProbeSession::plan_for_device() for the ordering
/// rationale and probe_session.h for why the plan does not widen past these.
struct PlannedProbe {
  const char *probe;
  const char *index;  ///< Empty for a probe that takes none.
};

/// The per-device plan. Kept as a table rather than open-coded so "what does a session send" is
/// one readable list that lines up line-for-line with the documented table it comes from.
constexpr PlannedProbe PLAN[] = {
    // No payload at all — nothing to choose, nothing to get wrong.
    {"general_info3", ""},
    // Field-observed: real hubs send exactly these two block values to real motors.
    {"status_ext", "0x00"},
    {"status_ext", "0x01"},
    // Field-observed long/short wire forms; the modifiers are this component's own POS_FAVORITE
    // and POS_VENT_MODIFIER, which it already sends to these devices during normal operation.
    {"private2", "0x00"},
    {"private2", "0x03"},
    {"private2_short", "0x00"},
    {"private2_short", "0x03"},
    // Last: the only entries not observed on our own wire. Function IDs known from production
    // software elsewhere — the weakest evidence in the table, so the least to lose by running it
    // after everything else has already answered.
    {"private_fn", "0x06"},
    {"private_fn", "0x09"},
};

}  // namespace

std::size_t ProbeSession::steps_per_device() { return sizeof(PLAN) / sizeof(PLAN[0]); }

std::vector<ProbeStep> ProbeSession::plan_for_device(const std::string &device_id) {
  std::vector<ProbeStep> steps;
  steps.reserve(steps_per_device());
  for (const auto &planned : PLAN)
    steps.push_back(ProbeStep{device_id, planned.probe, planned.index});
  return steps;
}

bool ProbeSession::start(const std::vector<std::string> &device_ids) {
  if (device_ids.empty() || this->is_running())
    return false;

  this->steps_.clear();
  this->steps_.reserve(device_ids.size() * steps_per_device());
  for (const auto &device_id : device_ids) {
    auto device_steps = plan_for_device(device_id);
    this->steps_.insert(this->steps_.end(), device_steps.begin(), device_steps.end());
  }

  this->cursor_ = 0;
  this->device_count_ = device_ids.size();
  this->answered_ = 0;
  this->silent_ = 0;
  this->skipped_ = 0;
  return true;
}

void ProbeSession::record(StepOutcome outcome) {
  if (!this->is_running())
    return;

  if (outcome == StepOutcome::REFUSED_DEVICE) {
    // Walk to the first step of the next device, counting everything passed over — including the
    // refused step itself, which was sent but yielded nothing usable about this device's protocol.
    const std::string device_id = this->current().device_id;
    while (this->is_running() && this->current().device_id == device_id) {
      this->skipped_++;
      this->cursor_++;
    }
    return;
  }

  if (outcome == StepOutcome::ANSWERED) {
    this->answered_++;
  } else {
    this->silent_++;
  }
  this->cursor_++;
}

}  // namespace home_io_control
}  // namespace esphome
