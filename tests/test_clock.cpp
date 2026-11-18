#include <cassert>
#include <chrono>
#include <cstdio>
#include "../app/src/main/cpp/bl40_runtime_clock.hpp"
using namespace std::chrono;
int main() {
 using TP=steady_clock::time_point;
 using Us=microseconds;
 TP zero{};
 bl40clock::SmoothClock c;
 TP t=zero;
 // At normal 2 ms guest cost, every start is exactly 10 ms apart.
 for(int i=0;i<1000;i++) {
   auto d=c.next(t,t+Us(2000));
   assert(d-t==Us(10000)); t=d;
 }
 assert(c.debtUs()==0 && c.repayments()==0 && c.discardedUs()==0);
 // Slow 60ms tick: following starts are spaced 8 ms and phase catches up.
 auto d=c.next(t,t+Us(60000));
 assert(d==t+Us(60000)); t=d;
 assert(c.debtUs()>=50000);
 uint64_t countBefore=c.repayments();
 for(int i=0;i<30;i++) {
   d=c.next(t,t+Us(2500));
   assert(d-t>=Us(8000));
   assert(d>=t+Us(2500));
   t=d;
 }
 assert(c.debtUs()==0);
 assert(c.repayments()>countBefore);
 // A late wake is already represented by absolute monotonic phase;
 // it should not be accrued a second time as fake CPU debt.
 auto a=c.accruedUs();
 c.noteLateWake(120000);
 assert(c.accruedUs()==a);
 assert(c.lateWakeUs()==120000);
 // 4 seconds spent in a paused/suspended host does NOT create 4 seconds
 // of catch-up burst. Guest clock is never advanced by this clock.
 d=c.next(t,t+seconds(4));
 assert(d==t+seconds(4));
 assert(c.debtUs()<=bl40clock::SmoothClock::kDebtLimitUs);
 assert(c.discardedUs()>0);
 auto acc=c.accruedUs();
 c.reanchor();
 assert(c.accruedUs()==acc);
 d=c.next(t+seconds(10),t+seconds(10)+Us(2000));
 assert(d==t+seconds(10)+Us(10000));
 std::puts("OK: phase locked pacing, 10 ms steps, bounded recovery, pause-safe");
}
