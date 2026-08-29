/// @file probe_session_test.cpp
/// @brief Tests for ProbeSession's plan construction and cursor advance.

#include "probe_session.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// Run every remaining step with the same outcome, so a test can say "this whole device answered"
/// without spelling out nine identical calls.
void run_all(ProbeSession &session, ProbeSession::StepOutcome outcome) {
  while (session.is_running())
    session.record(outcome);
}

}  // namespace

// --- Plan shape --------------------------------------------------------------

TEST(ProbeSessionTest, PlanCoversEveryDocumentedStartingValue) {
  const auto steps = ProbeSession::plan_for_device("ABC123");
  ASSERT_EQ(steps.size(), ProbeSession::steps_per_device());

  // The exact set from docs/radio_diagnostics.md's "Start with" column. Asserted as a set of
  // probe+index pairs rather than by index into the table, so reordering the plan (which the
  // ordering test below governs) does not have to touch this one.
  const std::set<std::pair<std::string, std::string>> expected = {
      {"general_info3", ""},      {"status_ext", "0x00"}, {"status_ext", "0x01"},
      {"private2", "0x00"},       {"private2", "0x03"},   {"private2_short", "0x00"},
      {"private2_short", "0x03"}, {"private_fn", "0x06"}, {"private_fn", "0x09"},
  };
  std::set<std::pair<std::string, std::string>> actual;
  for (const auto &step : steps)
    actual.insert({step.probe, step.index});
  EXPECT_EQ(actual, expected);
}

TEST(ProbeSessionTest, PlanTargetsTheRequestedDevice) {
  for (const auto &step : ProbeSession::plan_for_device("FEEB1E"))
    EXPECT_EQ(step.device_id, "FEEB1E");
}

TEST(ProbeSessionTest, PlanRunsLeastSpeculativeFirstAndPrivateFnLast) {
  const auto steps = ProbeSession::plan_for_device("ABC123");
  ASSERT_FALSE(steps.empty());
  // general_info3 sends no payload at all, so it leads; private_fn's indices are the only ones in
  // the table never observed on our own wire, so they trail. See plan_for_device()'s doxygen.
  EXPECT_EQ(steps.front().probe, "general_info3");
  EXPECT_EQ(steps.back().probe, "private_fn");
}

TEST(ProbeSessionTest, PlanNeverSchedulesUnknown4a) {
  // ADR 0024: there is no such probe to schedule, and a session walking every device is exactly
  // where an accidental one would do the most damage.
  for (const auto &step : ProbeSession::plan_for_device("ABC123"))
    EXPECT_NE(step.probe, "unknown4a");
}

// --- Starting -----------------------------------------------------------------

TEST(ProbeSessionTest, StartBuildsOnePlanPerDeviceInOrder) {
  ProbeSession session;
  ASSERT_TRUE(session.start({"AAA111", "BBB222", "CCC333"}));

  EXPECT_TRUE(session.is_running());
  EXPECT_EQ(session.device_count(), 3u);
  EXPECT_EQ(session.total_steps(), 3 * ProbeSession::steps_per_device());
  EXPECT_EQ(session.step_number(), 1u);
  // Devices are visited one at a time rather than interleaved: a probe run is easier to read, and
  // a terminal refusal easier to skip past, when one device's steps are contiguous.
  EXPECT_EQ(session.current().device_id, "AAA111");
}

TEST(ProbeSessionTest, StartRejectsAnEmptyDeviceList) {
  ProbeSession session;
  EXPECT_FALSE(session.start({}));
  EXPECT_FALSE(session.is_running());
  EXPECT_EQ(session.total_steps(), 0u);
}

TEST(ProbeSessionTest, StartRefusesWhileRunningRatherThanRestarting) {
  ProbeSession session;
  ASSERT_TRUE(session.start({"AAA111"}));
  session.record(ProbeSession::StepOutcome::ANSWERED);

  EXPECT_FALSE(session.start({"BBB222", "CCC333"}));
  // The in-flight run is untouched -- a second press must not discard a half-captured session.
  EXPECT_EQ(session.device_count(), 1u);
  EXPECT_EQ(session.step_number(), 2u);
  EXPECT_EQ(session.current().device_id, "AAA111");
}

TEST(ProbeSessionTest, StartAfterCompletionResetsTheTotals) {
  ProbeSession session;
  ASSERT_TRUE(session.start({"AAA111"}));
  run_all(session, ProbeSession::StepOutcome::SILENT);
  ASSERT_EQ(session.silent(), ProbeSession::steps_per_device());

  ASSERT_TRUE(session.start({"BBB222"}));
  EXPECT_EQ(session.answered(), 0u);
  EXPECT_EQ(session.silent(), 0u);
  EXPECT_EQ(session.skipped(), 0u);
  EXPECT_EQ(session.step_number(), 1u);
}

// --- Advancing ----------------------------------------------------------------

TEST(ProbeSessionTest, RecordAdvancesOneStepAndTallies) {
  ProbeSession session;
  ASSERT_TRUE(session.start({"AAA111"}));

  session.record(ProbeSession::StepOutcome::ANSWERED);
  session.record(ProbeSession::StepOutcome::SILENT);

  EXPECT_EQ(session.step_number(), 3u);
  EXPECT_EQ(session.answered(), 1u);
  EXPECT_EQ(session.silent(), 1u);
  EXPECT_EQ(session.skipped(), 0u);
}

TEST(ProbeSessionTest, SessionEndsWhenThePlanIsExhausted) {
  ProbeSession session;
  ASSERT_TRUE(session.start({"AAA111", "BBB222"}));
  run_all(session, ProbeSession::StepOutcome::ANSWERED);

  EXPECT_FALSE(session.is_running());
  EXPECT_EQ(session.answered(), 2 * ProbeSession::steps_per_device());
}

TEST(ProbeSessionTest, TerminalRefusalSkipsTheRestOfThatDeviceOnly) {
  ProbeSession session;
  ASSERT_TRUE(session.start({"AAA111", "BBB222"}));

  session.record(ProbeSession::StepOutcome::ANSWERED);
  session.record(ProbeSession::StepOutcome::REFUSED_DEVICE);

  // Straight to the next device: the refusal (typically "device is moving") recurs identically for
  // every remaining step on AAA111, but says nothing at all about BBB222.
  ASSERT_TRUE(session.is_running());
  EXPECT_EQ(session.current().device_id, "BBB222");
  EXPECT_EQ(session.current().probe, "general_info3");
  EXPECT_EQ(session.skipped(), ProbeSession::steps_per_device() - 1);
  EXPECT_EQ(session.answered(), 1u);
}

TEST(ProbeSessionTest, TerminalRefusalOnTheLastDeviceEndsTheSession) {
  ProbeSession session;
  ASSERT_TRUE(session.start({"AAA111"}));
  session.record(ProbeSession::StepOutcome::REFUSED_DEVICE);

  EXPECT_FALSE(session.is_running());
  EXPECT_EQ(session.skipped(), ProbeSession::steps_per_device());
}

TEST(ProbeSessionTest, EveryStepIsAccountedForExactlyOnce) {
  ProbeSession session;
  ASSERT_TRUE(session.start({"AAA111", "BBB222", "CCC333"}));

  session.record(ProbeSession::StepOutcome::ANSWERED);
  session.record(ProbeSession::StepOutcome::REFUSED_DEVICE);  // rest of AAA111
  session.record(ProbeSession::StepOutcome::SILENT);
  run_all(session, ProbeSession::StepOutcome::ANSWERED);

  // The end-of-session summary is only meaningful if the three tallies partition the plan.
  EXPECT_EQ(session.answered() + session.silent() + session.skipped(), session.total_steps());
}

TEST(ProbeSessionTest, AbortStopsTheSessionButKeepsTheTotalsReadable) {
  ProbeSession session;
  ASSERT_TRUE(session.start({"AAA111", "BBB222"}));
  session.record(ProbeSession::StepOutcome::ANSWERED);
  session.abort();

  EXPECT_FALSE(session.is_running());
  // Readable afterwards, because abort()'s caller still logs an end-of-session summary.
  EXPECT_EQ(session.answered(), 1u);
  EXPECT_EQ(session.total_steps(), 2 * ProbeSession::steps_per_device());
}

TEST(ProbeSessionTest, RecordAfterTheEndIsANoOp) {
  ProbeSession session;
  ASSERT_TRUE(session.start({"AAA111"}));
  run_all(session, ProbeSession::StepOutcome::ANSWERED);
  const std::size_t answered = session.answered();

  session.record(ProbeSession::StepOutcome::ANSWERED);

  EXPECT_EQ(session.answered(), answered);
  EXPECT_FALSE(session.is_running());
}
