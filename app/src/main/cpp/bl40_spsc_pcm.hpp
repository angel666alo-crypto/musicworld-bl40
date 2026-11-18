#pragma once
// SPSC PCM16 stereo queue. One writer thread; one AAudio realtime callback.
// The realtime reader never allocates, blocks, locks, or calls the guest CPU.
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>

class Bl40PcmRing {
public:
 static constexpr uint64_t kFrames=8192;
 static constexpr uint64_t kMask=kFrames-1;
 std::array<int16_t,kFrames*2> pcm{};
 std::atomic<uint64_t> head{0},tail{0},discardBefore{0};
 uint64_t queued() const {
   const auto h=head.load(std::memory_order_acquire);
   const auto t=tail.load(std::memory_order_acquire);
   const auto discarded=discardBefore.load(std::memory_order_acquire);
   // Do not report old-scene PCM as buffered audio after an epoch flush.
   // IMPORTANT: put() still uses the ACKNOWLEDGED consumer tail, not this
   // logical tail, to avoid overwriting a callback copying old samples.
   const auto playableTail=std::max(t,discarded);
   return h>=playableTail?h-playableTail:0;
 }
 // Only producer calls this. Next callback will discard old scene PCM.
 void discardOld(){
   const auto target=head.load(std::memory_order_acquire);
   auto old=discardBefore.load(std::memory_order_relaxed);
   while(old<target && !discardBefore.compare_exchange_weak(old,target,
                                   std::memory_order_release,std::memory_order_relaxed)){}
 }
 uint32_t put(const int16_t* src,uint32_t frames){
   if(frames==0||frames>kFrames)return 0;
   const uint64_t h=head.load(std::memory_order_relaxed);
   const uint64_t t=tail.load(std::memory_order_acquire);
   if(h<t||h-t>kFrames||frames>kFrames-(h-t))return 0;
   const size_t pos=(size_t)(h&kMask);
   const size_t first=std::min<size_t>(frames,kFrames-pos);
   std::memcpy(pcm.data()+pos*2,src,first*2*sizeof(int16_t));
   if(first<frames)std::memcpy(pcm.data(),src+first*2,(frames-first)*2*sizeof(int16_t));
   head.store(h+frames,std::memory_order_release);
   return frames;
 }
 uint32_t get(int16_t* dst,uint32_t frames){
   if(frames==0)return 0;
   uint64_t t=tail.load(std::memory_order_relaxed);
   const auto discard=discardBefore.load(std::memory_order_acquire);
   if(t<discard)t=discard;
   const auto h=head.load(std::memory_order_acquire);
   const uint32_t available=(uint32_t)std::min<uint64_t>(h-t,frames);
   const size_t pos=(size_t)(t&kMask);
   const size_t first=std::min<size_t>(available,kFrames-pos);
   if(first)std::memcpy(dst,pcm.data()+pos*2,first*2*sizeof(int16_t));
   if(first<available)std::memcpy(dst+first*2,pcm.data(),(available-first)*2*sizeof(int16_t));
   tail.store(t+available,std::memory_order_release);
   return available;
 }
};
