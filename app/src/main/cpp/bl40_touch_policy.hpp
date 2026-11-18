#pragma once
#include <cstdlib>
namespace bl40touch {
// Manhattan distance of 20 guest pixels defines one spatial sample.
// It is an input resampling policy, NOT a replacement for firmware touch IPC.
constexpr int kSpatialWaypointPx=20;
constexpr int kStaleInputMs=140;
inline bool collapseMove(int x0,int y0,int x1,int y1,bool stale){
 return stale || (std::abs(x1-x0)+std::abs(y1-y0)<kSpatialWaypointPx);
}
}
