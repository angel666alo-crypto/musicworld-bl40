// BL40 native Android audio output: AAudio callback + lock-free stereo PCM queue.
// Audio producer invokes the existing guest-timestamped MIDI mixer on its own
// thread; the callback only copies ready samples. No host-time guest acceleration.
#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include "bl40_spsc_pcm.hpp"
#include "bl40_stability_policy.hpp"

extern "C" int bl40AudioRender(int16_t* out,int frames);
extern "C" uint64_t bl40AudioPrepare();
extern "C" uint64_t bl40AudioEpoch();
extern "C" void bl40AudioWait(int timeoutMs);
extern "C" void bl40AudioFlushed();

struct NativeAudio {
  Bl40PcmRing ring;
  AAudioStream* stream=nullptr;
  std::thread producer;
  std::atomic<bool> stop{false},active{true};
  std::atomic<uint64_t> callbacks{0},underrunFrames{0},totalFrames{0},writes{0},sceneDrops{0};
  static constexpr uint32_t kTargetPcmQueueFrames=bl40stability::kAudioQueueTargetFrames; // 80ms
  int burst=0,bufferFrames=0,rate=0;
  std::atomic<uint64_t> maxQueuedFrames{0};
  std::atomic<uint32_t> startupBufferedFrames{0};
  void loop(){
    std::array<int16_t,bl40stability::kNominalAudioBatchFrames*2> samples{};
    uint64_t epoch=bl40AudioEpoch();
    while(!stop.load(std::memory_order_acquire)){
      if(!active.load(std::memory_order_acquire)){
        std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;
      }
      // AAudio does not execute the guest: only the completed 10-ms guest
      // ticks make sample frames available to this producer.
      auto e=bl40AudioPrepare();
      if(e!=epoch){ring.discardOld();sceneDrops++;bl40AudioFlushed();epoch=e;}
      const uint64_t queued=ring.queued();
      uint64_t oldHigh=maxQueuedFrames.load(std::memory_order_relaxed);
      while(queued>oldHigh&&!maxQueuedFrames.compare_exchange_weak(oldHigh,queued,std::memory_order_relaxed)){}
      // Bound inherent sound latency; do not let music sit for 154ms in PCM.
      if(!bl40stability::canProduceMoreAudio(queued)){
        std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;
      }
      int frames=bl40AudioRender(samples.data(),bl40stability::kNominalAudioBatchFrames);
      e=bl40AudioEpoch();
      if(e!=epoch){ring.discardOld();sceneDrops++;bl40AudioFlushed();epoch=e;}
      if(frames>0){
        if(ring.put(samples.data(),(uint32_t)frames)==(uint32_t)frames)writes++;
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }else bl40AudioWait(15);
    }
  }
};
static std::mutex audioLifecycle;
static std::unique_ptr<NativeAudio> audio;

static aaudio_data_callback_result_t outputCallback(AAudioStream*,void* ctx,void* output,int32_t numFrames){
  auto* a=static_cast<NativeAudio*>(ctx);
  auto* pcm=static_cast<int16_t*>(output);
  if(numFrames<=0)return AAUDIO_CALLBACK_RESULT_CONTINUE;
  uint32_t filled=0;
  if(a->active.load(std::memory_order_acquire))filled=a->ring.get(pcm,(uint32_t)numFrames);
  if(filled<(uint32_t)numFrames){
    std::memset(pcm+filled*2,0,(numFrames-filled)*2*sizeof(int16_t));
    a->underrunFrames.fetch_add((uint32_t)numFrames-filled,std::memory_order_relaxed);
  }
  a->callbacks.fetch_add(1,std::memory_order_relaxed);
  a->totalFrames.fetch_add((uint32_t)numFrames,std::memory_order_relaxed);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_musicworld_bl40native_MainActivity_nativeOpenAAudio(JNIEnv*,jobject){
  std::lock_guard<std::mutex> guard(audioLifecycle);
  if(audio)return JNI_TRUE;
  auto a=std::make_unique<NativeAudio>();
  AAudioStreamBuilder* builder=nullptr;
  aaudio_result_t result=AAudio_createStreamBuilder(&builder);
  if(result!=AAUDIO_OK || !builder)return JNI_FALSE;
  AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setChannelCount(builder,2);
  AAudioStreamBuilder_setSampleRate(builder,48000);
  AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  // SHARED is safer on phones with other apps playing audio; no exclusive claim.
  AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setDataCallback(builder,outputCallback,a.get());
  result=AAudioStreamBuilder_openStream(builder,&a->stream);
  AAudioStreamBuilder_delete(builder);
  if(result!=AAUDIO_OK||!a->stream){
    __android_log_print(ANDROID_LOG_WARN,"BL40Native","AAudio open failed %d",result);
    return JNI_FALSE;
  }
  a->rate=AAudioStream_getSampleRate(a->stream);
  if(a->rate!=48000 || AAudioStream_getChannelCount(a->stream)!=2 ||
     AAudioStream_getFormat(a->stream)!=AAUDIO_FORMAT_PCM_I16){
    __android_log_print(ANDROID_LOG_WARN,"BL40Native","AAudio format not 48k stereo PCM16; using AudioTrack fallback");
    AAudioStream_close(a->stream);return JNI_FALSE;
  }
  a->burst=std::max<int>(1,AAudioStream_getFramesPerBurst(a->stream));
  const int request=std::min<int>(AAudioStream_getBufferCapacityInFrames(a->stream),std::max(4*a->burst,bl40stability::kMinimumHardwareBufferFrames));
  if(request>0)AAudioStream_setBufferSizeInFrames(a->stream,request);
  a->bufferFrames=AAudioStream_getBufferSizeInFrames(a->stream);
  a->producer=std::thread([ptr=a.get()](){ptr->loop();});
  // Prebuffer only BEFORE starting the DAC. Rendering and guest execution
  // remain on separate threads; no sound or clock is synthesized here.
  // This absorbs the first 40ms of scheduler/Android wakeup jitter.
  const auto prebufferUntil=std::chrono::steady_clock::now()+std::chrono::milliseconds(350);
  while(a->ring.queued()<1920 && std::chrono::steady_clock::now()<prebufferUntil){
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  a->startupBufferedFrames.store((uint32_t)a->ring.queued(),std::memory_order_relaxed);
  result=AAudioStream_requestStart(a->stream);
  if(result!=AAUDIO_OK){
    a->stop.store(true);a->producer.join();AAudioStream_close(a->stream);
    __android_log_print(ANDROID_LOG_WARN,"BL40Native","AAudio start failed %d",result);
    return JNI_FALSE;
  }
  __android_log_print(ANDROID_LOG_INFO,"BL40Native","AAudio opened rate=%d burst=%d buffer=%d",
                      a->rate,a->burst,a->bufferFrames);
  audio=std::move(a);
  return JNI_TRUE;
}
extern "C" JNIEXPORT void JNICALL Java_org_musicworld_bl40native_MainActivity_nativeSetAAudioActive(JNIEnv*,jobject,jboolean active){
  std::lock_guard<std::mutex> guard(audioLifecycle);
  if(!audio)return;
  bool value=(active==JNI_TRUE);
  if(value==audio->active.load())return;
  audio->active.store(value,std::memory_order_release);
  if(!value){audio->ring.discardOld();AAudioStream_requestPause(audio->stream);}
  else {audio->ring.discardOld();AAudioStream_requestStart(audio->stream);}
}
extern "C" JNIEXPORT void JNICALL Java_org_musicworld_bl40native_MainActivity_nativeCloseAAudio(JNIEnv*,jobject){
  std::lock_guard<std::mutex> guard(audioLifecycle);
  if(!audio)return;
  audio->stop.store(true,std::memory_order_release);
  if(audio->producer.joinable())audio->producer.join();
  AAudioStream_requestStop(audio->stream);
  AAudioStream_close(audio->stream);
  audio.reset();
}
extern "C" JNIEXPORT jstring JNICALL Java_org_musicworld_bl40native_MainActivity_nativeAAudioStats(JNIEnv* env,jobject){
  std::lock_guard<std::mutex> guard(audioLifecycle);
  if(!audio)return env->NewStringUTF("{\"enabled\":false}");
  char stats[384];
  std::snprintf(stats,sizeof(stats),
    "{\"enabled\":true,\"engine\":\"AAudio PCM16 callback\",\"rate\":%d,\"burstFrames\":%d,\"bufferFrames\":%d,"
    "\"callbacks\":%llu,\"underrunFrames\":%llu,\"writtenFrames\":%llu,\"queuedFrames\":%llu,\"sceneDrops\":%llu,\"targetQueueFrames\":%u,\"maxQueuedFrames\":%llu,\"startupBufferedFrames\":%u}",
    audio->rate,audio->burst,audio->bufferFrames,
    (unsigned long long)audio->callbacks.load(),
    (unsigned long long)audio->underrunFrames.load(),
    (unsigned long long)audio->totalFrames.load(),
    (unsigned long long)audio->ring.queued(),
    (unsigned long long)audio->sceneDrops.load(),
    NativeAudio::kTargetPcmQueueFrames,
    (unsigned long long)audio->maxQueuedFrames.load(),
    audio->startupBufferedFrames.load());
  return env->NewStringUTF(stats);
}
