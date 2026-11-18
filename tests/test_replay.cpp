#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
#include "../app/src/main/cpp/bl40_runtime_clock.hpp"
using namespace std::chrono;

// Baseline scheduling behavior, preserved for reproducible comparisons:
// each slow tick shifts the host deadline; recover no more than 1 ms per tick.
struct OldClock {
  int64_t debt=0;
  steady_clock::time_point next(steady_clock::time_point begin,
                                 steady_clock::time_point end){
    const auto elapsed=duration_cast<microseconds>(end-begin).count();
    debt=std::min<int64_t>(250000,debt+std::max<int64_t>(0,elapsed-10000));
    auto repay=std::min({std::max<int64_t>(0,10000-elapsed),int64_t{1000},debt});
    debt-=repay;
    return std::max(begin+microseconds(10000-repay),end);
  }
};
int main(){
 std::vector<int> cost;
 cost.insert(cost.end(),1,115710);             // synthetic maximum tick from a recorded load distribution
 cost.insert(cost.end(),138,60000);          // 139 ticks >50ms in JSON
 cost.insert(cost.end(),1405,28000);         // 1544 ticks >20ms
 cost.insert(cost.end(),401,12000);          // 1945 ticks >10ms
 cost.insert(cost.end(),13933,1200);         // remaining fast ticks
 assert(cost.size()==15878);
 std::shuffle(cost.begin(),cost.end(),std::mt19937(134));
 OldClock old;
 bl40clock::SmoothClock now;
 steady_clock::time_point a{},b{};
 for(int us:cost){
   a=old.next(a,a+microseconds(us));
   b=now.next(b,b+microseconds(us));
 }
 const double oldS=duration<double>(a.time_since_epoch()).count();
 const double newS=duration<double>(b.time_since_epoch()).count();
 const double virtualS=cost.size()*0.010;
 assert(newS<oldS);
 assert(newS>=virtualS);   // No faster than real time across whole trace.
 const double oldRate=virtualS/oldS,newRate=virtualS/newS;
 std::printf("Synthetic trace (recorded load distribution; NOT a device benchmark): "
             "guest %.2fs, baseline %.2fs (%.3fx), current %.2fs (%.3fx), diff %.2fs\n",
             virtualS,oldS,oldRate,newS,newRate,oldS-newS);
}
