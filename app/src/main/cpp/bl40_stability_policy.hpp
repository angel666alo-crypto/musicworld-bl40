#pragma once
#include <cstdint>
// Host-output and pacing policy, not a replacement for the LG ARM/Thumb CPU.
namespace bl40stability {
// Host-only pacing. A guest step always consumes precisely 10ms of virtual
// time, but slow hardware never causes catch-up bursts of faster-than-real-
// time guest execution. This deliberately trades speed for monotonic rhythm.
template <typename TimePoint, typename Duration>
constexpr TimePoint nextGuestDeadline(TimePoint started,TimePoint finished,Duration tickPeriod) {
  const TimePoint nominal=started+tickPeriod;
  return finished>nominal ? finished : nominal;
}

static constexpr int64_t kMaximumDeadlineDebtUs=40000;  // 40 ms, not seconds
static constexpr uint32_t kAudioQueueTargetFrames=4800; // 100ms @ 48kHz; transient-stall cushion
static constexpr int kMinimumHardwareBufferFrames=960;  // 20ms @ 48kHz
static constexpr int kNominalAudioBatchFrames=480;      // 10ms @ 48kHz
constexpr bool shouldRebaseHostDeadline(int64_t latenessUs) {
  return latenessUs>kMaximumDeadlineDebtUs;
}
constexpr bool canProduceMoreAudio(uint64_t queuedFrames) {
  return queuedFrames<kAudioQueueTargetFrames;
}
}
