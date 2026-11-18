#include <cassert>
#include <cstdio>
#include "../app/src/main/cpp/bl40_spsc_pcm.hpp"
int main() {
 Bl40PcmRing ring;
 int16_t src[960]{};
 assert(ring.put(src,480)==480);
 assert(ring.queued()==480);
 ring.discardOld();
 assert(ring.queued()==0); // Scene PCM discarded, no playable frames left.
 int16_t dst[960]{};
 assert(ring.get(dst,480)==0); // Callback acknowledges consumer pointer.
 assert(ring.queued()==0);
 assert(ring.put(src,480)==480);
 assert(ring.get(dst,480)==480);
 assert(ring.queued()==0);
 std::puts("OK: discarded PCM is not reported as queued");
}
