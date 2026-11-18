#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>

// Monotonic deadlines for completed 10 ms guest steps.
// Virtual time is advanced exclusively by bl40_tick(10) in native_engine.cpp.
// Re-anchoring is restricted to pauses or pathological >400 ms host stalls.
// In particular, a slow tick does NOT permanently move the next 10 ms phase.
namespace bl40clock {
class SmoothClock {
 public:
  static constexpr int64_t kGuestPeriodUs=10000;
  // Catch-up is bounded: guest executions never START closer than 8 ms
  // when they are shorter than 8 ms. This is phase recovery, not a clock
  // multiplier: every guest call still executes one and only one 10 ms step.
  static constexpr int64_t kMinimumStartSpacingUs=8000;
  static constexpr int64_t kDebtLimitUs=400000;

  // Full reset at boot. On pause call reanchor() to retain lifetime statistics.
  void reset() {
    reanchor();
    accruedUs_=repaidUs_=discardedUs_=lateWakeUs_=0;
    repayments_=resyncs_=0;
  }
  void reanchor() { anchored_=false; debtUs_=0; }
  // Track missed wakeups, but never count them TWICE as both a late wake and
  // a long guest tick. The two delays measure different conditions.
  void noteLateWake(int64_t lateUs) {
    if(lateUs>0)lateWakeUs_+=(uint64_t)lateUs;
  }

  template <class TimePoint>
  TimePoint next(TimePoint began,TimePoint finished) {
    using Us=std::chrono::microseconds;
    if(!anchored_) {
      idealDue_=began+Us(kGuestPeriodUs);
      anchored_=true;
    } else {
      idealDue_+=Us(kGuestPeriodUs);
    }
    const int64_t processing=std::max<int64_t>(0,
      std::chrono::duration_cast<Us>(finished-began).count());
    if(processing>kGuestPeriodUs)
      accruedUs_+=(uint64_t)(processing-kGuestPeriodUs);

    // A 5-second Android suspend or a severe workload cannot trigger
    // seconds of catch-up at 1.25x. Rebase only the HOST deadline, without
    // touching virtualMs or dropping any guest instruction/tick.
    const auto lateness=std::chrono::duration_cast<Us>(finished-idealDue_).count();
    if(lateness>kDebtLimitUs) {
      const auto shift=Us(lateness-kDebtLimitUs);
      idealDue_+=shift;
      discardedUs_+=(uint64_t)shift.count();
      ++resyncs_;
    }
    const auto minimum=began+Us(kMinimumStartSpacingUs);
    const auto due=std::max({idealDue_,minimum,finished});
    debtUs_=(uint64_t)std::max<int64_t>(0,
      std::chrono::duration_cast<Us>(due-idealDue_).count());
    // This measures actual phase recovery, not a fabricated millisecond
    // of progress just because an old, capped debt counter was nonzero.
    const int64_t gap=std::max<int64_t>(0,
      std::chrono::duration_cast<Us>(due-began).count());
    if(debtUs_>0 && gap<kGuestPeriodUs) {
      repaidUs_+=(uint64_t)(kGuestPeriodUs-gap);
      ++repayments_;
    }
    return due;
  }
  uint64_t debtUs()const{return debtUs_;}
  uint64_t accruedUs()const{return accruedUs_;}
  uint64_t repaidUs()const{return repaidUs_;}
  uint64_t discardedUs()const{return discardedUs_;}
  uint64_t repayments()const{return repayments_;}
  uint64_t lateWakeUs()const{return lateWakeUs_;}
  uint64_t resyncs()const{return resyncs_;}
 private:
  using TimePoint=std::chrono::steady_clock::time_point;
  TimePoint idealDue_{};
  bool anchored_=false;
  uint64_t debtUs_=0,accruedUs_=0,repaidUs_=0,discardedUs_=0;
  uint64_t repayments_=0,lateWakeUs_=0,resyncs_=0;
};
}
