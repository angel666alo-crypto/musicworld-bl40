#include <cassert>
#include <chrono>
#include <cstdio>
#include "../app/src/main/cpp/bl40_stability_policy.hpp"
using namespace std::chrono;
using TP=steady_clock::time_point;
int main(){
  TP base{};
  const auto period=milliseconds(10);
  // Fast tick waits until 10ms after its start.
  assert(bl40stability::nextGuestDeadline(base,base+milliseconds(2),period)==base+period);
  // Slow tick (42ms) never runs four 10ms steps as a burst.
  auto late=bl40stability::nextGuestDeadline(base,base+milliseconds(42),period);
  assert(late==base+milliseconds(42));
  assert(bl40stability::nextGuestDeadline(late,late+milliseconds(2),period)==late+period);
  // Processing overhead (MIDI/GPU) also prevents catch-up.
  assert(bl40stability::nextGuestDeadline(base,base+milliseconds(75),period)==base+milliseconds(75));
  // 500 repeated fast steps cannot start at >1x speed.
  TP start=base;
  for(int i=0;i<500;i++){
    auto next=bl40stability::nextGuestDeadline(start,start+milliseconds(1),period);
    assert(next-start>=period);
    start=next;
  }
  assert(start==base+seconds(5));
  std::puts("OK: no catch-up bursts; >=10ms between guest tick starts");
}
