/// @file hub_probe_session_test.cpp
/// @brief Tests for hub_probe_session.cpp: starting a session, and stepping it from the loop.

#include "hub_core.h"

#include "stubs/radio_test_common.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// Records what the session asked for and hands back a scripted reply, so these tests exercise the
/// driver's scheduling and bookkeeping without going near the radio. probe_device() is virtual on
/// the hub precisely so this substitution is possible; hub_management_test.cpp covers the real
/// implementation behind it.
class TestableProbeSessionComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::advance_probe_session_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::probe_session_;
  using IOHomeControlComponent::radio_;

  struct ProbeCall {
    std::string device_id;
    std::string probe;
    std::string index;
  };

  ManagementActionResult probe_device(const std::string &device_id, const std::string &probe,
                                      const std::string &index) override {
    this->calls.push_back({device_id, probe, index});
    ManagementActionResult result;
    result.action = "probe_device";
    result.device_id = device_id;
    result.probe_name = probe;
    result.probe_index = index;
    if (this->refuse_device == device_id) {
      result.terminal_refusal = true;
      result.message = "device is moving; refusing to probe mid-transaction";
      return result;
    }
    result.success = this->answer;
    result.message = this->answer ? "answered" : "no response";
    if (this->answer) {
      result.has_response_cmd = true;
      result.response_cmd = 0x04;
      result.response_hex = "00112233";
    }
    return result;
  }

  std::vector<ProbeCall> calls;
  bool answer{true};
  std::string refuse_device;
};

/// Step the session the way loop() does, bounded so a scheduling bug fails the test instead of
/// hanging it. The cap is generous because ProbeSession::STEP_DELAY_MS pacing costs many no-op
/// passes per executed step under the host clock, which advances one millisecond per millis() call.
void pump(TestableProbeSessionComponent &component, int max_passes = 200000) {
  for (int i = 0; i < max_passes && component.probe_session_.is_running(); i++)
    component.advance_probe_session_();
}

void configure(TestableProbeSessionComponent &component, MockRadio &radio, const std::vector<std::string> &devices) {
  component.radio_ = &radio;
  component.set_diagnostic_probes_enabled(true);
  for (const auto &device_id : devices)
    component.add_device(device_id);
}

}  // namespace

// --- Starting -----------------------------------------------------------------

TEST(HubProbeSessionTest, RefusesWhenDiagnosticProbesAreNotEnabled) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {"AAA111"});
  component.set_diagnostic_probes_enabled(false);

  // The button entity only exists for a build that opted in, but the hub re-checks anyway: the
  // gate on sending undecoded opcodes must not rest on codegen alone.
  EXPECT_FALSE(component.start_probe_session());
  EXPECT_FALSE(component.probe_session_.is_running());
}

TEST(HubProbeSessionTest, RefusesWithNoRegisteredDevices) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {});

  EXPECT_FALSE(component.start_probe_session());
  EXPECT_FALSE(component.probe_session_.is_running());
}

TEST(HubProbeSessionTest, StartsAPlanCoveringEveryRegisteredDevice) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {"AAA111", "BBB222"});

  ASSERT_TRUE(component.start_probe_session());
  EXPECT_TRUE(component.probe_session_.is_running());
  EXPECT_EQ(component.probe_session_.device_count(), 2u);
  EXPECT_EQ(component.probe_session_.total_steps(), 2 * ProbeSession::steps_per_device());
  // Nothing transmitted yet -- start_probe_session() only arms the plan; loop() sends the frames.
  EXPECT_TRUE(component.calls.empty());
}

TEST(HubProbeSessionTest, SecondStartWhileRunningIsRefused) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {"AAA111"});
  ASSERT_TRUE(component.start_probe_session());
  component.advance_probe_session_();

  EXPECT_FALSE(component.start_probe_session());
  EXPECT_TRUE(component.probe_session_.is_running());
}

// --- Stepping -----------------------------------------------------------------

TEST(HubProbeSessionTest, RunsEveryPlannedProbeExactlyOnce) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {"AAA111", "BBB222"});
  ASSERT_TRUE(component.start_probe_session());

  pump(component);

  ASSERT_FALSE(component.probe_session_.is_running());
  EXPECT_EQ(component.calls.size(), 2 * ProbeSession::steps_per_device());
  EXPECT_EQ(component.probe_session_.answered(), component.calls.size());
  // Devices are walked contiguously, not interleaved.
  EXPECT_EQ(component.calls.front().device_id, "AAA111");
  EXPECT_EQ(component.calls.back().device_id, "BBB222");
}

TEST(HubProbeSessionTest, SendsAtMostOneProbePerPass) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {"AAA111"});
  ASSERT_TRUE(component.start_probe_session());

  // The whole point of the design: a session covering a real installation runs for minutes, and a
  // pass that fired more than one blocking exchange would be that many seconds of frozen loop.
  for (int i = 0; i < 50000 && component.calls.size() < 3; i++) {
    const std::size_t before = component.calls.size();
    component.advance_probe_session_();
    ASSERT_LE(component.calls.size() - before, 1u);
  }
  EXPECT_GE(component.calls.size(), 3u);
}

TEST(HubProbeSessionTest, YieldsWhileTheOperationQueueIsBusy) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {"AAA111"});
  ASSERT_TRUE(component.start_probe_session());
  component.op_queue_.enqueue_request_status("AAA111");

  // A cover press or a due status poll must never wait minutes behind a diagnostic run.
  for (int i = 0; i < 5000; i++)
    component.advance_probe_session_();
  EXPECT_TRUE(component.calls.empty());

  // ...and the session resumes once the queue drains, rather than being dropped.
  (void) component.op_queue_.pop();
  pump(component);
  EXPECT_EQ(component.calls.size(), ProbeSession::steps_per_device());
}

TEST(HubProbeSessionTest, SilentDeviceStillRunsEveryStep) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {"AAA111"});
  component.answer = false;
  ASSERT_TRUE(component.start_probe_session());

  pump(component);

  // "Nothing answered" is itself a result worth having for every probe, so a silent device is not
  // abandoned the way a terminally-refusing one is.
  EXPECT_EQ(component.calls.size(), ProbeSession::steps_per_device());
  EXPECT_EQ(component.probe_session_.silent(), ProbeSession::steps_per_device());
}

TEST(HubProbeSessionTest, TerminalRefusalSkipsThatDeviceAndContinuesWithTheNext) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {"AAA111", "BBB222"});
  component.refuse_device = "AAA111";
  ASSERT_TRUE(component.start_probe_session());

  pump(component);

  // One attempt against the refusing device, a full plan against the other -- a device that is
  // moving says nothing about the rest of the installation.
  EXPECT_EQ(component.calls.size(), 1 + ProbeSession::steps_per_device());
  EXPECT_EQ(component.calls.front().device_id, "AAA111");
  // All nine, including the one that drew the refusal: probe_device() refuses before it builds a
  // frame, so nothing was transmitted to AAA111 at all.
  EXPECT_EQ(component.probe_session_.skipped(), ProbeSession::steps_per_device());
  EXPECT_EQ(component.probe_session_.answered(), ProbeSession::steps_per_device());
}

TEST(HubProbeSessionTest, AbortStopsSendingFurtherProbes) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {"AAA111", "BBB222"});
  ASSERT_TRUE(component.start_probe_session());
  for (int i = 0; i < 50000 && component.calls.empty(); i++)
    component.advance_probe_session_();
  ASSERT_EQ(component.calls.size(), 1u);

  component.abort_probe_session();

  EXPECT_FALSE(component.probe_session_.is_running());
  for (int i = 0; i < 5000; i++)
    component.advance_probe_session_();
  EXPECT_EQ(component.calls.size(), 1u);
}

// --- Status reporting ---------------------------------------------------------

TEST(HubProbeSessionTest, PublishesProgressAndAFinalTally) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {"AAA111"});
  std::vector<std::string> statuses;
  component.set_probe_session_status_callback([&statuses](const std::string &s) { statuses.push_back(s); });

  ASSERT_TRUE(component.start_probe_session());
  pump(component);

  // A run lasts minutes and its real output is the log the user is busy capturing, so "is it still
  // going, and did it finish" has to be answerable from the entity alone.
  ASSERT_FALSE(statuses.empty());
  EXPECT_EQ(statuses.front(), "running 0/" + std::to_string(ProbeSession::steps_per_device()));
  EXPECT_EQ(statuses.back(),
            "done " + std::to_string(ProbeSession::steps_per_device()) + " answered, 0 silent, 0 skipped");
}

TEST(HubProbeSessionTest, PublishesNothingWithoutASession) {
  MockRadio radio;
  TestableProbeSessionComponent component;
  configure(component, radio, {"AAA111"});
  std::vector<std::string> statuses;
  component.set_probe_session_status_callback([&statuses](const std::string &s) { statuses.push_back(s); });

  component.abort_probe_session();  // no session running
  for (int i = 0; i < 100; i++)
    component.advance_probe_session_();

  EXPECT_TRUE(statuses.empty());
}
