// Music World LG BL40 - Android runtime for the reconstructed guest core.
// This is an ARM/Thumb HLE core, not complete Qualcomm firmware emulation.
// Guest time advances by exactly 10 ms only after bl40_tick(10) completes.
#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include "bl40_wasm2c_backend.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
// Reloj monotónico con pasos virtuales completos
#include "bl40_touch_policy.hpp"
#include "bl40_stability_policy.hpp"
#include "bl40_runtime_clock.hpp"
#include "bl40_dynarmic_bridge.hpp"
#include "bl40_perfhint.hpp"
#include <unordered_map>
#include <vector>

#define TSF_IMPLEMENTATION
#include "tsf.h"
#define TML_IMPLEMENTATION
#include "tml.h"

#define TAG "BL40Native"
#define INFO(...) __android_log_print(ANDROID_LOG_INFO,TAG,__VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR,TAG,__VA_ARGS__)
// Boot stages are atomic: the UI can inspect them while the native core owns its single
// execution thread. No JNI call from the renderer enters WAMR.
static std::atomic<int> gBootStage{0};
static std::atomic<int> gBootAssetsRegistered{0};
static constexpr int W=346,H=776;
static constexpr size_t FRAME_BYTES=(size_t)W*H*2;
using Clock=std::chrono::steady_clock;
using Ms=std::chrono::milliseconds;

struct Track {
 tml_message* first=nullptr;
 tml_message* next=nullptr;
 uint64_t guestStart=0;
 int channelOffset=0;
 bool music=false;
 uint32_t durationMs=0;
 std::string source="unknown";
 Track()=default;
 Track(tml_message* ptr,uint64_t start,bool bg,uint32_t dur,const char* filename):first(ptr),next(ptr),guestStart(start),music(bg),durationMs(dur),source(filename?filename:"unknown"){}
 Track(const Track&)=delete;
 Track& operator=(const Track&)=delete;
 Track(Track&& o) noexcept { *this=std::move(o); }
 Track& operator=(Track&& o) noexcept {
   if(this!=&o){if(first)tml_free(first);first=o.first;next=o.next;guestStart=o.guestStart;channelOffset=o.channelOffset;music=o.music;durationMs=o.durationMs;source=std::move(o.source);o.first=nullptr;o.next=nullptr;}
   return *this;
 }
 ~Track(){if(first)tml_free(first);}
};
// MIDI processing: extracting MIDI bytes remains on guest/core thread;
// parsing MIDI files happens on a separate worker. The guest core is never called in parallel.
// MIDI identity mapping from the asset catalogue.
// Duration-only guessing mislabels short ambient background loops as effects.
struct MidiIdentity {uint32_t bytes, fnvHash;bool music;const char* name;};
static constexpr MidiIdentity ORIGINAL_MIDI[] = {
 {24140u,0x12d3d067u,true,"a00.mid"},
 {29886u,0xdd17cd7bu,true,"a01.mid"},
 {28996u,0xfd5e6ba1u,true,"a02.mid"},
 {23782u,0x856efd16u,true,"a03.mid"},
 {31483u,0x2602e4f3u,true,"a04.mid"},
 {32137u,0x9f0c01b2u,true,"a05.mid"},
 {21361u,0xf85dc0b9u,true,"a06.mid"},
 {31273u,0x26c1a1f7u,true,"a07.mid"},
 {27235u,0x74212103u,true,"a08.mid"},
 {33958u,0x26cb8c82u,true,"a09.mid"},
 {744u,0xa7e5ea5eu,true,"amb00.mid"},
 {1004u,0xb784f721u,true,"amb01.mid"},
 {737u,0xbe377652u,true,"amb02.mid"},
 {96u,0x8cb02ed0u,true,"amb03.mid"},
 {658u,0x78cbdd2eu,true,"amb04.mid"},
 {760u,0x4495d9abu,true,"amb05.mid"},
 {516u,0x54490bd3u,true,"amb06.mid"},
 {278u,0x6dbbabddu,false,"b10.mid"},
 {833u,0x864b82abu,false,"b11.mid"},
 {141u,0x1567309bu,false,"b12.mid"},
 {606u,0x73a77e8eu,false,"b13.mid"},
 {278u,0x367454c0u,false,"b20.mid"},
 {1725u,0x3ad41ba2u,false,"b21.mid"},
 {240u,0x5f4a35a9u,false,"b22.mid"},
 {592u,0xf6d85791u,false,"b23.mid"},
 {762u,0xb4d71630u,false,"b30.mid"},
 {866u,0xcb4adb25u,false,"b31.mid"},
 {416u,0xda512d14u,false,"b32.mid"},
 {266u,0xc800cd74u,false,"b33.mid"},
 {213u,0x78dd6d30u,false,"b40.mid"},
 {654u,0x815deb05u,false,"b41.mid"},
 {386u,0x3fa713f3u,false,"b42.mid"},
 {2004u,0x70eb12e9u,false,"b43.mid"},
 {106u,0xfb460db7u,false,"be00.mid"},
 {212u,0xe2eb603bu,false,"be01.mid"},
 {323u,0x6ef340b7u,false,"be02.mid"},
 {143u,0x0b8b54acu,false,"be03.mid"},
 {1532u,0x62a9dcccu,false,"be04.mid"},
 {1178u,0xa2c3bb34u,false,"be05.mid"},
 {29691u,0x026a0d09u,true,"ch1_bg.mid"},
 {12864u,0xe7075235u,true,"ch2_bg.mid"},
 {19872u,0xd8680fc0u,true,"ch4_bg.mid"},
 {6820u,0xeb1787bau,false,"ch4_effect.mid"},
 {7893u,0xf6580adfu,false,"ch5_effect.mid"},
 {2452u,0x2418b2e2u,false,"ch6_effect.mid"},
 {1289u,0xb7ad823eu,false,"ch7_effect.mid"},
 {283u,0xe1d853dbu,false,"ch8_effect.mid"},
 {414u,0xcf8fc3bdu,false,"e00.mid"},
 {486u,0x156f3ca3u,false,"e01.mid"},
 {476u,0x144153a6u,false,"e02.mid"},
 {466u,0x5a174fb0u,false,"e03.mid"},
 {478u,0x3f69a996u,false,"e04.mid"},
 {586u,0x9b88fe8du,false,"e05.mid"},
 {499u,0x239b9325u,false,"e06.mid"},
 {1903u,0x996ff836u,false,"e07.mid"},
 {179u,0xd2241231u,false,"e08.mid"},
 {228u,0xbae38538u,false,"e09.mid"},
 {228u,0xff1a3bd4u,false,"e10.mid"},
 {228u,0x34f2f220u,false,"e11.mid"},
 {978u,0xef0b809fu,false,"e12.mid"},
 {552u,0x6399e2fbu,false,"ee00.mid"},
 {354u,0xb9881951u,false,"ee01.mid"},
 {208u,0xdfd8ae85u,false,"ee02.mid"},
 {2144u,0xf8f2687cu,false,"ef_mid_00.mid"},
 {632u,0xd651644cu,false,"ef_mid_01.mid"},
 {986u,0x10fd2fd9u,false,"ef_mid_02.mid"},
 {879u,0xd740cd9bu,false,"ef_mid_03.mid"},
 {6524u,0x41560e31u,false,"ef_mid_04.mid"},
 {733u,0xaec6a0fbu,false,"ef_wav_00.mid"},
 {200u,0x9a25bde0u,false,"ef_wav_01.mid"},
 {163u,0x53b24dafu,false,"ef_wav_02.mid"},
 {242u,0xb0e221a0u,false,"ef_wav_03.mid"},
 {185u,0xe3f8449au,false,"ef_wav_04.mid"},
 {824u,0x596a4f73u,false,"ef_wav_05.mid"},
 {168u,0xc40d11a5u,false,"ef_wav_06.mid"},
 {558u,0x4d482ddfu,false,"ef_wav_07.mid"},
 {149u,0x903b230du,false,"ef_wav_08.mid"},
 {193u,0x42dccde8u,false,"ef_wav_09.mid"},
};
// The guest requests e00.mid and a00.mid at exactly the same virtual instant
// during its intro->menu transition. Treat only that specific coincidence as
// a duplicated introductory cue; do not suppress ordinary menu/gameplay SFX.
// This is a narrow HLE compatibility rule pending original CMX service tracing.
static bool redundantIntroCue(const Track& effect,const Track& music){
 if(effect.source!="e00.mid" || music.source!="a00.mid" || !music.music || music.guestStart>15000)return false;
 const uint64_t gap=(effect.guestStart>music.guestStart)?
    effect.guestStart-music.guestStart:music.guestStart-effect.guestStart;
 return gap<=40;
}
static uint32_t midiHash(const std::vector<uint8_t>& data){
 uint32_t h=2166136261u;
 for(uint8_t b:data){h^=b;h*=16777619u;}
 return h;
}
static const MidiIdentity* identifyMidi(const std::vector<uint8_t>& data){
 const uint32_t hash=midiHash(data);
 for(const auto& e:ORIGINAL_MIDI)if(e.bytes==data.size()&&e.fnvHash==hash)return &e;
 return nullptr;
}

struct MidiJob {std::vector<uint8_t> bytes;uint64_t guestStart;};
struct State {
 bool nativeReady=false;
 // BL40 AOT+PERF V1.3: all WASM accesses, including counters, happen on the core thread.
 std::unordered_map<std::string, bool> fnCache;
 bool useAot=false;
 bool useNativeC=false;
 std::atomic<uint64_t> soundServiceCalls{0},guestFileOpens{0},guestMidiDropped{0},guestPendingMidi{0},guestFastPairs{0},unknownServiceCalls{0};
 std::atomic<uint64_t> synthNotesOn{0},wasmLookupMisses{0};
 std::vector<uint8_t> wasmBytes;
 std::vector<uint8_t> bankBytes;
 std::thread coreThread,midiThread;
 std::mutex midiMutex;
 std::condition_variable midiCv;
 std::deque<MidiJob> midiJobs;
 std::mutex frameMutex,audioMutex,touchMutex;
 // The Android PCM worker sleeps until the guest has actually completed
 // a new virtual-time tick. It never steps the guest from the audio thread.
 std::mutex audioClockMutex;
 std::condition_variable audioClockCv;
 std::condition_variable touchCv;
 std::vector<uint8_t> frame=std::vector<uint8_t>(FRAME_BYTES,0);
 std::vector<Track> tracks;
 std::deque<Track> incoming;
 // These candidates are ONLY accessed by the guest/core thread.
 bool sceneCandidateActive=false;
 uint64_t sceneCandidateAtMs=0,sceneCandidateFiles=0;
 std::array<uint16_t,450> sceneCandidatePixels{};
 // Guest thread publishes the scene; AudioTrack observes it using acquire.
 std::atomic<uint64_t> sceneEpoch{0},sceneTriggerMs{0},sceneChanges{0};
 // Deteccion pasiva de fin de partida (Game Over / resultados): solo hilo del nucleo.
 std::array<std::array<uint16_t,450>,5> passiveRing{};
 uint64_t passiveRingAt[5]={0,0,0,0,0};
 int passiveRingN=0;uint64_t passiveNextProbeMs=0,passiveCooldownUntilMs=0;
 std::atomic<uint64_t> passiveScenes{0};
 std::atomic<uint64_t> coreAffinityMask{0};
 uint64_t audioSeenSceneEpoch=0; // AudioTrack thread only (under audioMutex).
 std::atomic<uint64_t> sceneMusicStops{0},musicReplacements{0},finishedTrackStops{0};
 std::atomic<uint64_t> midiIdentified{0},midiUnknownClassifications{0},audioSceneFlushes{0};
 std::atomic<uint64_t> channelControllerResets{0},deferredMidiStarts{0},duplicateBackgroundIgnored{0},sceneStopsFlushed{0};
 std::atomic<uint64_t> introOverlapSuppressed{0},pcmZeroWriteAvoided{0};
 // An identical long introductory cue must not have three active copies.
 std::atomic<uint64_t> introLongJingleReplacements{0},introDuplicateJingleIgnored{0};
 std::mutex diagnosticMutex;
 std::vector<std::array<uint32_t,8>> firstSoundServiceRaw,firstAudioServiceRaw;
 uint32_t lastSoundServiceIndex=0,lastAudioServiceIndex=0;
 struct SlowTickInfo {uint64_t guestMs;uint32_t costUs,files,sounds;};
 std::vector<SlowTickInfo> firstSlowTicks;
 // AudioTrack observes this epoch for both scene changes and music replacements.
 std::atomic<uint64_t> audioOutputEpoch{0},sceneAllTrackStops{0},longJingleStops{0},musicFlushRequests{0},introAutoSceneChanges{0};
 struct AudioEvent {uint64_t guestMs;const char* action;std::string source;bool music;};
 std::deque<AudioEvent> audioHistory; // Under audioMutex, bounded to 48 entries.
 std::deque<AudioEvent> audioHistoryFirst; // First 24 events for intro diagnosis.
 // Conservative automatic intro->menu detection for scenes without user input.
 bool introAutoDone=false,introBaseSet=false,introPending=false;
 uint64_t introLastProbeMs=0,introPendingAt=0,introOpenCount=0;
 std::array<uint16_t,450> introBasePixels{};
 // Touch delivery can be quick while the guest itself presents a frame late.
 bool pendingTouchVisual=false;
 Clock::time_point pendingTouchAt{};
 std::atomic<uint64_t> touchVisualCount{0},touchVisualUs{0},touchVisualMaxUs{0};
 struct Touch {int kind,x,y;Clock::time_point hostAt;};std::deque<Touch> touches;
 std::atomic<uint64_t> touchEnqueued{0},touchDelivered{0},touchCoalesced{0},touchDelayTotalUs{0};
 std::atomic<uint64_t> touchDelayWorstUs{0},touchQueuePeak{0},touchWakeups{0},touchRejected{0};
 std::atomic<uint64_t> touchMoveDispatched{0},touchUpDispatched{0},touchMoveSamplesCollapsed{0};
 std::atomic<uint64_t> touchSpatialWaypointsPreserved{0};
 std::atomic<uint64_t> audioGuestWaits{0},audioGuestWakeTimeouts{0},audioGuestWaitUs{0};
 std::atomic<uint64_t> audioGuestFramesRendered{0},audioClockStarvedFrames{0},audioClockStarvedBlocks{0},audioLateMidiEvents{0};
 // V1.9: host audio stream must never synthesize many seconds of obsolete PCM.
 // These counters distinguish guest work from PCM cursor discontinuities.
 std::atomic<uint64_t> audioResyncs{0},audioDroppedStaleFrames{0},audioSkippedMidiEvents{0};
 std::atomic<uint64_t> audioSilentFastForwardFrames{0},audioResyncEventsReplayed{0};
 std::atomic<uint64_t> hostDeadlineResets{0},hostDeadlineLostUs{0};
 Clock::time_point lastAudioResync{}; // owned exclusively by AudioTrack thread.
 std::atomic<uint64_t> deadlineDebtClamps{0},deadlineCatchupTicks{0},deadlineDebtMaxUs{0};
 std::atomic<uint64_t> deadlineLateStartTicks{0};
 // Snapshot of the host clock engine; all counters are published by the guest thread.
 std::atomic<uint64_t> clockDebtUs{0},clockAccruedUs{0},clockRepaidUs{0},clockDiscardedUs{0},clockRepayments{0};
 std::atomic<uint64_t> clockPhaseResyncs{0},clockLateWakeUs{0};
 tsf* synth=nullptr;
 std::atomic<bool> ready{false},running{false},paused{false},stop{false};
 std::atomic<uint64_t> virtualMs{0},guestFrames{0},midiCount{0},slowTicks{0},tickCount{0},outputFrames{0};
 std::atomic<int> highestTickUs{0};
 std::atomic<uint64_t> tickCpuUs{0},midiPumpUs{0},frameReadUs{0},midiJobsQueued{0},midiParseErrors{0},guestMidiCaptured{0};
 std::atomic<double> lastRms{0},peakRms{0};
 std::atomic<uint64_t> audioAudibleBlocks{0},audioSilentBlocks{0},audioRenderUs{0};
 std::atomic<uint64_t> schedulerSleepUs{0},schedulerPauseUs{0},schedulerWakeLateUs{0};
 std::atomic<uint64_t> tickOver10Ms{0},tickOver20Ms{0},synthFailedNotes{0},peakActiveVoices{0};
 // Histograma de coste por bl40_tick (us): <0.1ms,<1,<2,<5,<10,<20,<40,<80,>=80 ms.
 std::atomic<uint64_t> tickHist[9]{},heavyTickUs{0},heavyTickCount{0};
 std::atomic<int> sf2PresetCount{0};
 uint32_t lastSerial=0;
 int sf2TrackSlot=0;
 std::string error;
 Clock::time_point wallStart;
 bool initializedRuntime=false;
} g;

static void setError(const std::string& error){ERR("%s",error.c_str());g.error=error;}
static std::vector<uint8_t> readAsset(AAssetManager* mgr,const char* name){
 std::vector<uint8_t> data; if(!mgr){setError("Android AssetManager nulo");return data;}
 AAsset* a=AAssetManager_open(mgr,name,AASSET_MODE_STREAMING);
 if(!a){setError(std::string("Falta archivo del juego: ")+name);return data;}
 auto size=AAsset_getLength64(a);
 if(size<1 || size>250000000){AAsset_close(a);setError("Tamaño de recurso no válido");return data;}
 data.resize((size_t)size);size_t used=0;
 while(used<data.size()) {int n=AAsset_read(a,data.data()+used, (size_t)std::min((size_t)1<<20,data.size()-used));if(n<=0)break;used+=(size_t)n;}
 AAsset_close(a);
 if(used!=data.size()){setError(std::string("Error leyendo: ")+name);data.clear();}
 return data;
}
static uint16_t u16(const uint8_t* p){return (uint16_t)(p[0]|(p[1]<<8));}
static uint32_t u32(const uint8_t* p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int32_t invoke(const char* fn,std::initializer_list<uint32_t> values={}){
 if(!g.nativeReady)return 0;
 uint32_t result=0;
 std::string error;
 if(!bl40_native_call(fn,values,result,error)){
  setError(error);return 0;
 }
 return (int32_t)result;
}
static uint32_t wasmStore(const uint8_t* ptr,uint32_t len,bool zeroEnd=false){
 if(len>50000000)return 0;
 const uint32_t p=(uint32_t)invoke("web_alloc",{len+(zeroEnd?1u:0u)});
 if(!p || !bl40_native_validate_address(p,len+(zeroEnd?1u:0u))){setError("Memoria del núcleo insuficiente para recursos");return 0;}
 auto* dst=(uint8_t*)bl40_native_pointer(p);
 if(!dst)return 0;memcpy(dst,ptr,len);if(zeroEnd)dst[len]=0;return p;
}
static bool loadPack(const std::vector<uint8_t>& pack){
 if(pack.size()<8)return false;size_t off=4;
 const uint32_t n=u32(pack.data());
 if(n!=145){setError("El paquete debe contener 145 recursos");return false;}
 for(uint32_t i=0;i<n;i++){
  if(off+2>pack.size())return false;uint16_t nl=u16(pack.data()+off);off+=2;
  if(nl<3||off+nl+4>pack.size())return false;
  const uint8_t* name=pack.data()+off;off+=nl;
  uint32_t len=u32(pack.data()+off);off+=4;
  if(len>pack.size()-off)return false;
  auto* data=pack.data()+off;off+=len;
  uint32_t np=wasmStore(name,nl,true),dp=wasmStore(data,len);
  if(!np||!dp||invoke("bl40_register_asset",{np,dp,len})!=1){setError("Error registrando entrada "+std::to_string(i));return false;}
  gBootAssetsRegistered.store((int)(i+1), std::memory_order_release);
  // Mirror the registered guest assets: do NOT free guest buffers after registration.
 }
 return off==pack.size();
}
static void grabFrame(bool force=false){
 const uint32_t seq=(uint32_t)invoke("bl40_presentation_count");
 if(!force && seq==g.lastSerial)return;
 const uint32_t p=(uint32_t)invoke("bl40_frame_ptr");
 if(!p||!bl40_native_validate_address(p,(uint32_t)FRAME_BYTES))return;
 void* src=bl40_native_pointer(p);if(!src)return;
 {std::lock_guard<std::mutex> guard(g.frameMutex);memcpy(g.frame.data(),src,FRAME_BYTES);}
 g.lastSerial=seq;g.guestFrames.fetch_add(1,std::memory_order_relaxed);
 if(g.pendingTouchVisual){
  const uint64_t us=(uint64_t)std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-g.pendingTouchAt).count());
  g.touchVisualCount++;g.touchVisualUs.fetch_add(us,std::memory_order_relaxed);
  auto old=g.touchVisualMaxUs.load(std::memory_order_relaxed);
  while(us>old&&!g.touchVisualMaxUs.compare_exchange_weak(old,us,std::memory_order_relaxed)){}
  g.pendingTouchVisual=false;
 }
}
// A touch can initiate a real menu transition even when the guest never
// requests a new song. Port the scene fingerprint (450 RGB565 samples).
// This is a guarded HLE heuristic, NOT a native BL40 IPC event.
static bool sceneSample(std::array<uint16_t,450>& samples){
 const uint32_t ptr=(uint32_t)invoke("bl40_frame_ptr");
 if(!ptr || !bl40_native_validate_address(ptr,(uint32_t)FRAME_BYTES))return false;
 auto* data=(const uint8_t*)bl40_native_pointer(ptr);
 if(!data)return false;
 size_t k=0;
 for(int y=0;y<25;y++)for(int x=0;x<18;x++){
  const size_t off=2*((size_t)(y*30+11)*W+(size_t)(x*18+7));
  samples[k++]=(uint16_t)(data[off]|(data[off+1]<<8));
 }
 return true;
}
static void markSceneCandidate(){
 g.sceneCandidateAtMs=g.virtualMs.load(std::memory_order_relaxed);
 g.sceneCandidateFiles=(uint32_t)invoke("bl40_pc_open_count");
 g.sceneCandidateActive=sceneSample(g.sceneCandidatePixels);
}
static void announceScene(uint64_t now,uint64_t triggeredAt,const char* why){
 g.sceneTriggerMs.store(triggeredAt,std::memory_order_relaxed);
 g.sceneEpoch.fetch_add(1,std::memory_order_release);
 // PCM flush is requested only after the audio thread actually removes
 // an old track. A visual scene change without a voice stop is not an
 // audio discontinuity and must not reset AudioTrack.
 g.sceneChanges++;
 INFO("Escena HLE: %s, guest=%llu inicio=%llu",why,
   (unsigned long long)now,(unsigned long long)triggeredAt);
}
static void checkSceneCandidate(){
 if(!g.sceneCandidateActive)return;
 const uint64_t now=g.virtualMs.load(std::memory_order_relaxed);
 if(now<g.sceneCandidateAtMs+80)return;
 if(now>g.sceneCandidateAtMs+1400){g.sceneCandidateActive=false;return;}
 std::array<uint16_t,450> current{};
 if(!sceneSample(current))return;
 double sum=0;int changed=0;
 for(size_t i=0;i<current.size();i++){
  const uint16_t a=g.sceneCandidatePixels[i],b=current[i];
  const double d=std::abs((int)(a>>11)-(int)(b>>11))/31.0
    +std::abs((int)((a>>5)&63)-(int)((b>>5)&63))/63.0
    +std::abs((int)(a&31)-(int)(b&31))/31.0;
  sum+=d;if(d>0.5)changed++;
 }
 const double fraction=(double)changed/current.size(),avg=sum/current.size();
 const bool resourceOpened=(uint32_t)invoke("bl40_pc_open_count")>g.sceneCandidateFiles;
 const bool gameplay=g.sceneCandidateFiles>=80;
 // A changed resource by itself is NOT proof of a scene transition: a
 // gameplay sound effect can also load an asset.
 // A visual-only change is not sufficient evidence of an LG scene switch.
 // Require a resource transition as well; don't flush audio on animations.
 const bool changedScene=resourceOpened&&fraction>=0.12&&avg>=0.12;
 if(!changedScene)return;
 const uint64_t trigger=g.sceneCandidateAtMs;
 g.sceneCandidateActive=false;
 g.introAutoDone=true;
 announceScene(now,trigger,resourceOpened?"touch-y-recurso":"touch-y-pantalla");
}

// The first splash->menu transition can occur WITHOUT a touch event.
// Only detect one high-confidence passive transition during the first 18 s.
// This is an HLE heuristic; no fabricated guest time or firmware claim.
static void checkIntroAutomaticScene(){
 const uint64_t now=g.virtualMs.load(std::memory_order_relaxed);
 if(g.introAutoDone || g.sceneCandidateActive || g.sceneEpoch.load(std::memory_order_relaxed)>0 || now>=18000)return;
 if(now<g.introLastProbeMs+120)return;
 g.introLastProbeMs=now;
 std::array<uint16_t,450> current{};
 if(!sceneSample(current))return;
 const uint64_t opened=(uint32_t)invoke("bl40_pc_open_count");
 if(!g.introBaseSet){
  g.introBasePixels=current;g.introOpenCount=opened;
  g.introBaseSet=true;return;
 }
 int big=0;double total=0;
 for(size_t i=0;i<current.size();i++){
  const int a=g.introBasePixels[i],b=current[i];
  const double d=std::abs((a>>11)-(b>>11))/31.0
       +std::abs(((a>>5)&63)-((b>>5)&63))/63.0
       +std::abs((a&31)-(b&31))/31.0;
  total+=d;if(d>0.5)++big;
 }
 const double fraction=(double)big/current.size(),avg=total/current.size();
 // Guard against normal gameplay animation and short fullscreen flashes.
 const bool newAssets=opened>g.introOpenCount;
 const bool substantial=(newAssets&&fraction>=0.28&&avg>=0.26)
    ||(opened<80&&fraction>=0.70&&avg>=0.63);
 if(!substantial){
  g.introPending=false;g.introBasePixels=current;g.introOpenCount=opened;
  return;
 }
 if(!g.introPending){g.introPending=true;g.introPendingAt=now;return;}
 if(now<g.introPendingAt+120)return;
 g.introAutoDone=true;
 g.introAutoSceneChanges++;
 // Keep freshly launched menu music, which can start before the first new frame.
 const uint64_t conservativeCutoff=g.introPendingAt>350?g.introPendingAt-350:0;
 announceScene(now,conservativeCutoff,"intro-automatica");
}

// El juego NO avisa al anfitrion cuando termina la musica (no hay evento de parada en la interfaz del
// huesped), y pantallas como "Game Over" aparecen sin que el jugador toque: el detector por toque no las
// ve y la musica del nivel seguia sonando. Heuristica estricta SOLO dentro de una partida (muchos recursos
// abiertos): si la pantalla cambia casi por completo respecto a hace ~400 ms, se anuncia un cambio de
// escena. Medido sobre una grabacion real: el juego normal no supera 0.35 de cambio; el Game Over da ~0.55.
static void checkPassiveGameplayScene(){
 const uint64_t now=g.virtualMs.load(std::memory_order_relaxed);
 if(now<g.passiveNextProbeMs)return;
 g.passiveNextProbeMs=now+100;
 std::array<uint16_t,450> cur{};
 if(!sceneSample(cur))return;
 // anillo de las ultimas 5 muestras (una cada 100 ms)
 if(g.passiveRingN<5){g.passiveRing[g.passiveRingN]=cur;g.passiveRingAt[g.passiveRingN]=now;g.passiveRingN++;}
 else{
  const std::array<uint16_t,450> ref=g.passiveRing[1]; // ~400 ms atras
  const uint64_t refAt=g.passiveRingAt[1];
  for(int i=0;i<4;i++){g.passiveRing[i]=g.passiveRing[i+1];g.passiveRingAt[i]=g.passiveRingAt[i+1];}
  g.passiveRing[4]=cur;g.passiveRingAt[4]=now;
  if((uint32_t)invoke("bl40_pc_open_count")<80)return;       // no estamos en una partida
  if(g.sceneCandidateActive||now<g.passiveCooldownUntilMs)return;
  double sum=0;int changed=0;
  for(size_t i=0;i<cur.size();i++){
   const uint16_t a=ref[i],b=cur[i];
   const double d=std::abs((int)(a>>11)-(int)(b>>11))/31.0
     +std::abs((int)((a>>5)&63)-(int)((b>>5)&63))/63.0
     +std::abs((int)(a&31)-(int)(b&31))/31.0;
   sum+=d;if(d>0.5)changed++;
  }
  const double fraction=(double)changed/cur.size(),avg=sum/cur.size();
  if(fraction>=0.40&&avg>=0.55){
   g.passiveCooldownUntilMs=now+4000;
   g.passiveScenes++;
   const uint64_t trigger=(now>350)?now-350:0;
   (void)refAt;
   announceScene(now,trigger,"pasiva-en-partida");
  }
 }
}

static void midiParserLoop(){
 while(true){
  MidiJob job;
  {
   std::unique_lock<std::mutex> lock(g.midiMutex);
   g.midiCv.wait(lock,[]{return g.stop.load()||!g.midiJobs.empty();});
   if(g.midiJobs.empty()&&g.stop.load())break;
   job=std::move(g.midiJobs.front());g.midiJobs.pop_front();
  }
  tml_message* midi=tml_load_memory(job.bytes.data(),(int)job.bytes.size());
  if(!midi){g.midiParseErrors++;ERR("MIDI invalido: %zu bytes",job.bytes.size());continue;}
  int chans=0,progs=0,notes=0;unsigned first=0,duration=0;
  tml_get_info(midi,&chans,&progs,&notes,&first,&duration);
  const MidiIdentity* known=identifyMidi(job.bytes);
  // Follow the source asset catalogue. For unknown MIDI use its duration only
  // as a fallback; don't infer that a short ambient loop is a sound effect.
  const bool bg=known?known->music:(duration>=4500);
  if(known)g.midiIdentified++;else g.midiUnknownClassifications++;
  Track track(midi,job.guestStart,bg,duration,known?known->name:"unknown");
  {std::lock_guard<std::mutex> lock(g.audioMutex);g.incoming.push_back(std::move(track));}
  g.midiCount++;
  INFO("MIDI=%s clase=%s bytes=%zu notas=%d durMs=%u",known?known->name:"desconocido",bg?"musica":"efecto",job.bytes.size(),notes,duration);
 }
}
static void flushGuestMidi(){
 // All WASM calls stay on the sole guest thread; never call core from parser/audio.
 int cap=0;
 while(invoke("bl40_pc_midi_pending")>0 && cap++<24){
  int need=invoke("bl40_pc_midi_pop",{0,0,0,0});
  if(need>=-16||need<-200000){ERR("Longitud MIDI no valida: %d",need);break;}
  const uint32_t len=(uint32_t)(-need);
  uint32_t buf=(uint32_t)invoke("web_alloc",{len});
  uint32_t lr=(uint32_t)invoke("web_alloc",{4});
  uint32_t cy=(uint32_t)invoke("web_alloc",{8});
  if(!buf||!lr||!cy){setError("No hay espacio para mensaje MIDI");break;}
  const int got=invoke("bl40_pc_midi_pop",{buf,len,lr,cy});
  if(got>0&&got<=(int)len&&bl40_native_validate_address(buf,(uint32_t)got)){
   auto* ptr=(const uint8_t*)bl40_native_pointer(buf);
   if(ptr){
    MidiJob job;job.bytes.assign(ptr,ptr+got);
    job.guestStart=g.virtualMs.load(std::memory_order_acquire);
    {std::lock_guard<std::mutex> lock(g.midiMutex);g.midiJobs.push_back(std::move(job));}
    g.midiJobsQueued++;g.midiCv.notify_one();
   }
  }else{ERR("Error extrayendo MIDI got=%d need=%u",got,len);g.midiParseErrors++;}
  invoke("web_free",{buf});invoke("web_free",{lr});invoke("web_free",{cy});
 }
}
// Access only from the single guest/core thread. Raw service fields are
// intentionally unlabeled until their meaning is verified against the PXO.
static void captureRawServices(){
 const uint32_t sc=(uint32_t)invoke("bl40_pc_sound_service_count");
 const uint32_t ac=(uint32_t)invoke("bl40_pc_audio_trace_count");
 std::lock_guard<std::mutex> guard(g.diagnosticMutex);
 if(sc<g.lastSoundServiceIndex)g.lastSoundServiceIndex=0;
 if(ac<g.lastAudioServiceIndex)g.lastAudioServiceIndex=0;
 uint32_t so=0,ao=0;
 while(g.lastSoundServiceIndex<sc && so++<8){
  const uint32_t index=g.lastSoundServiceIndex++;
  if(g.firstSoundServiceRaw.size()<48){
   std::array<uint32_t,8> row{};
   for(uint32_t field=0;field<row.size();field++)
    row[field]=(uint32_t)invoke("bl40_pc_sound_service_n",{index,field});
   g.firstSoundServiceRaw.push_back(row);
  }
 }
 while(g.lastAudioServiceIndex<ac && ao++<8){
  const uint32_t index=g.lastAudioServiceIndex++;
  if(g.firstAudioServiceRaw.size()<48){
   std::array<uint32_t,8> row{};
   for(uint32_t field=0;field<row.size();field++)
    row[field]=(uint32_t)invoke("bl40_pc_audio_trace_n",{index,field});
   g.firstAudioServiceRaw.push_back(row);
  }
 }
}
static void sampleGuestCounters(){
 // Read guest HLE service counters; never call core from AudioTrack/UI.
 g.guestMidiCaptured.store((uint64_t)(uint32_t)invoke("bl40_pc_midi_captured"),std::memory_order_relaxed);
 g.guestMidiDropped.store((uint64_t)(uint32_t)invoke("bl40_pc_midi_dropped"),std::memory_order_relaxed);
 g.guestPendingMidi.store((uint64_t)(uint32_t)invoke("bl40_pc_midi_pending"),std::memory_order_relaxed);
 g.soundServiceCalls.store((uint64_t)(uint32_t)invoke("bl40_pc_sound_service_count"),std::memory_order_relaxed);
 g.guestFileOpens.store((uint64_t)(uint32_t)invoke("bl40_pc_open_count"),std::memory_order_relaxed);
 g.unknownServiceCalls.store((uint64_t)(uint32_t)invoke("bl40_pc_unknown_count"),std::memory_order_relaxed);
 g.guestFastPairs.store((uint64_t)(uint32_t)invoke("bl40_pc_fast_pair_spans"),std::memory_order_relaxed);
 captureRawServices();
}
// V1.6: monotonic absolute deadline with NO discarded backlog.
// A slow guest tick is followed by the next real tick immediately; it never
// triggers a new artificial sleep. The guest still executes every 10ms tick.
// En telefonos big.LITTLE el planificador puede dejar el hilo del nucleo en un nucleo lento. Se limita a
// los nucleos de mayor frecuencia (>=75 % de la maxima). Todo es opcional: si no se puede leer la
// frecuencia o fallar la llamada, el hilo queda como estaba. Devuelve la mascara aplicada (0 = sin cambio).
static unsigned long pinCoreThreadToFastCores(id_t tid){
 long freq[32]={0};long maxf=0;int n=0;
 for(;n<32;n++){
  char path[96];snprintf(path,sizeof path,"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",n);
  FILE* f=fopen(path,"r");if(!f)break;
  long v=0;if(fscanf(f,"%ld",&v)!=1)v=0;fclose(f);freq[n]=v;if(v>maxf)maxf=v;
 }
 if(n<4||maxf<=0)return 0;
 unsigned long mask=0;int fast=0,slow=0;
 for(int i=0;i<n;i++){if(freq[i]*4>=maxf*3){mask|=1UL<<i;fast++;}else slow++;}
 if(slow==0||fast<2)return 0; // todos iguales o un solo nucleo rapido: no tocar
 if(syscall(SYS_sched_setaffinity,(pid_t)tid,sizeof(mask),&mask)!=0)return 0;
 return mask;
}
static void coreLoop(){
 bl40_native_enter_guest_thread();
 const auto tid=(id_t)syscall(SYS_gettid);
 if(setpriority(PRIO_PROCESS,tid,-4)!=0)
  INFO("Prioridad de hilo host: permiso denegado; guest sin cambios");
 {
  const unsigned long m=pinCoreThreadToFastCores(tid);
  g.coreAffinityMask.store((uint64_t)m,std::memory_order_relaxed);
  if(m)INFO("Hilo del nucleo fijado a los nucleos rapidos: mascara=0x%lx",m);
  else INFO("Hilo del nucleo sin fijar a nucleos (CPU homogenea o sin acceso)");
 }
 bl40perf::Hint perfHint;
 if(perfHint.start((int32_t)tid,10000000LL))INFO("ADPF activo en el hilo del nucleo (objetivo 10 ms)");
 else INFO("ADPF no disponible; el hilo del nucleo usa planificacion normal");
 g.wallStart=Clock::now();
 auto deadline=g.wallStart;
 auto lastFrame=g.wallStart;
 Clock::time_point pauseStart=g.wallStart;
 bool wasPaused=false;
 uint64_t lastInputDispatchTick=UINT64_MAX; // Preserve temporal order of swipe positions.
 bl40clock::SmoothClock syncEngine;
 while(!g.stop.load(std::memory_order_acquire)){
  if(g.paused.load(std::memory_order_acquire)){
   if(!wasPaused){wasPaused=true;pauseStart=Clock::now();}
   const auto ps=Clock::now();
   {std::unique_lock<std::mutex> lock(g.touchMutex);
    g.touchCv.wait_for(lock,Ms(15),[]{return g.stop.load(std::memory_order_acquire)||!g.paused.load(std::memory_order_acquire);});}
   const auto pe=Clock::now();
   g.schedulerPauseUs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(pe-ps).count(),std::memory_order_relaxed);
   continue;
  }
  if(wasPaused){
   // Paused time must not be recovered, but real delays during gameplay must be.
   deadline=Clock::now(); // Drop paused host time, never replay it.
   syncEngine.reanchor();
   wasPaused=false;
  }
  std::deque<State::Touch> events;
  const auto beforeWait=Clock::now();
  {
   std::unique_lock<std::mutex> lock(g.touchMutex);
   const auto canDispatch=[&](){
    return !g.touches.empty() &&
      g.tickCount.load(std::memory_order_relaxed)!=lastInputDispatchTick;
   };
   // An Android ACTION_MOVE batch may contain many positions. The original
   // implementation delivered ALL of them before ONE guest tick, making
   // scrolling invisible to a guest that samples touch once per tick.
   // A gesture sample can now be delivered at most once per 10 ms GUEST tick.
   // Never stall a fast guest because the queue contains more MOVE events.
   if(beforeWait<deadline && !canDispatch()){
    g.touchCv.wait_until(lock,deadline,[&](){
     return g.stop.load(std::memory_order_acquire)
        ||g.paused.load(std::memory_order_acquire)||canDispatch();
    });
    const auto afterWait=Clock::now();
    g.schedulerSleepUs.fetch_add((uint64_t)std::max<int64_t>(0,
      std::chrono::duration_cast<std::chrono::microseconds>(afterWait-beforeWait).count()),
      std::memory_order_relaxed);
    if(afterWait>deadline)
     g.schedulerWakeLateUs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(afterWait-deadline).count(),std::memory_order_relaxed);
    else if(canDispatch())g.touchWakeups.fetch_add(1,std::memory_order_relaxed);
   }
   if(canDispatch()){
    events.push_back(g.touches.front());g.touches.pop_front();
    if(events.front().kind==1){
     // Resample Android's high-rate MOVE stream to ~20 px per guest tick,
     // rather than collapsing an entire swipe to its last point. Preserve
     // DOWN/UP boundaries and keep one MOVE per completed 10 ms guest tick.
     // If the guest has fallen >140 ms behind the input, prefer the latest
     // coordinates to avoid replaying a stale half-second gesture.
     const auto now=Clock::now();
     const bool stale=now-events.front().hostAt>Ms(bl40touch::kStaleInputMs);
     while(!g.touches.empty()&&g.touches.front().kind==1){
      const auto& next=g.touches.front();
      if(!bl40touch::collapseMove(events.front().x,events.front().y,
                                   next.x,next.y,stale)){
       g.touchSpatialWaypointsPreserved++;break;
      }
      events.front()=next;g.touches.pop_front();
      g.touchCoalesced++;g.touchMoveSamplesCollapsed++;
     }
    }
    lastInputDispatchTick=g.tickCount.load(std::memory_order_relaxed);
   }
  }
  if(g.stop.load(std::memory_order_acquire))break;
  if(g.paused.load(std::memory_order_acquire))continue;
  bool touchEdge=false;
  // Input may arrive *between* ticks: deliver immediately on the sole guest
  // thread without advancing the virtual guest clock. Never invoke WAMR on UI.
  for(const auto& e:events){
   const auto now=Clock::now();
   const auto lag=std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::microseconds>(now-e.hostAt).count());
   g.touchDelayTotalUs.fetch_add((uint64_t)lag,std::memory_order_relaxed);
   uint64_t prev=g.touchDelayWorstUs.load(std::memory_order_relaxed);
   while((uint64_t)lag>prev&&!g.touchDelayWorstUs.compare_exchange_weak(prev,(uint64_t)lag,std::memory_order_relaxed)){}
   if(e.kind==0)markSceneCandidate();
   if(e.kind==1)g.touchMoveDispatched++;
   if(e.kind==2)g.touchUpDispatched++;
   const int touchOk=invoke("bl40_send_touch",{(uint32_t)e.kind,(uint32_t)e.x,(uint32_t)e.y});
   if(touchOk==1)g.touchDelivered.fetch_add(1,std::memory_order_relaxed);
   else g.touchRejected.fetch_add(1,std::memory_order_relaxed);
   if(e.kind!=1){
    touchEdge=true;
    if(!g.pendingTouchVisual){g.pendingTouchVisual=true;g.pendingTouchAt=now;}
   }
  }
  if(touchEdge){
   flushGuestMidi();
   // A tap can update the framebuffer even before the next guest tick.
   grabFrame();
  }
  if(Clock::now()<deadline)continue; // touch wake: do not create virtual time
  const auto started=Clock::now();
  // Measure late starts but never execute guest ticks in catch-up bursts.
  if(started>deadline+Ms(1)){
   g.deadlineLateStartTicks.fetch_add(1,std::memory_order_relaxed);
   syncEngine.noteLateWake(std::chrono::duration_cast<std::chrono::microseconds>(started-deadline).count());
  }
  const int ok=invoke("bl40_tick",{10});
  const auto ended=Clock::now();
  const int elapsed=(int)std::chrono::duration_cast<std::chrono::microseconds>(ended-started).count();
  perfHint.work((int64_t)elapsed*1000);
  int old=g.highestTickUs.load();while(elapsed>old&&!g.highestTickUs.compare_exchange_weak(old,elapsed)){}
  g.tickCpuUs.fetch_add((uint64_t)std::max(0,elapsed),std::memory_order_relaxed);
  {
   static const int kEdges[8]={100,1000,2000,5000,10000,20000,40000,80000};
   int bucket=0;while(bucket<8&&elapsed>=kEdges[bucket])bucket++;
   g.tickHist[bucket].fetch_add(1,std::memory_order_relaxed);
   if(elapsed>=1000){
    g.heavyTickUs.fetch_add((uint64_t)elapsed,std::memory_order_relaxed);
    g.heavyTickCount.fetch_add(1,std::memory_order_relaxed);
   }
  }
  if(elapsed>10000)g.tickOver10Ms.fetch_add(1,std::memory_order_relaxed);
  if(elapsed>20000)g.tickOver20Ms.fetch_add(1,std::memory_order_relaxed);
  if(elapsed>50000){
   g.slowTicks++;
   std::lock_guard<std::mutex> lock(g.diagnosticMutex);
   if(g.firstSlowTicks.size()<24){
    g.firstSlowTicks.push_back({g.virtualMs.load(std::memory_order_relaxed),
      (uint32_t)elapsed,(uint32_t)invoke("bl40_pc_open_count"),
      (uint32_t)invoke("bl40_pc_sound_service_count")});
   }
  }
  if(ok!=1){if(g.error.empty())setError("bl40_tick(10) retornó fallo");g.stop.store(true);break;}
  {
   // Coordinate predicate evaluation with the producer to avoid losing a
   // wakeup between AudioTrack's last check and wait_for().
   std::lock_guard<std::mutex> guard(g.audioClockMutex);
   g.virtualMs.fetch_add(10,std::memory_order_release);
   g.tickCount++;
  }
  g.audioClockCv.notify_one();
  if((g.tickCount.load(std::memory_order_relaxed)&3u)==0){
   checkSceneCandidate();
   checkPassiveGameplayScene();
   // V1.9: passive intro fingerprint disabled pending a verified guest IPC event;
   // it was capable of stopping audio on an animation rather than a menu.
  }
  const auto afterTick=Clock::now();
  flushGuestMidi();
  const auto afterMidi=Clock::now();
  g.midiPumpUs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(afterMidi-afterTick).count());
  // Presentation sampling may run faster than the guest's own frame rate;
  // it never creates guest frames or advances the virtual clock.
  // Limit host framebuffer sampling to about 24 fps. The guest still executes
  // every 10-ms step; sampling does not alter guest time or audio pitch.
  if(afterMidi-lastFrame>=Ms(42)){
   grabFrame();lastFrame=Clock::now();
   g.frameReadUs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(lastFrame-afterMidi).count());
  }
  if(g.tickCount.load(std::memory_order_relaxed)%100==0)sampleGuestCounters();
  // Absolute host phase; a delayed tick does not shift all later ticks.
  // Bounded 8-ms minimum spacing allows gradual real-time alignment while
  // the guest ALWAYS advances exactly 10 ms per completed bl40_tick(10).
  const auto pacingNow=Clock::now();
  const auto nominalNext=started+Ms(10);
  if(pacingNow>nominalNext){
   g.hostDeadlineResets.fetch_add(1,std::memory_order_relaxed);
   g.hostDeadlineLostUs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(pacingNow-nominalNext).count(),std::memory_order_relaxed);
  }
  deadline=syncEngine.next(started,pacingNow);
  g.clockDebtUs.store(syncEngine.debtUs(),std::memory_order_relaxed);
  g.clockAccruedUs.store(syncEngine.accruedUs(),std::memory_order_relaxed);
  g.clockRepaidUs.store(syncEngine.repaidUs(),std::memory_order_relaxed);
  g.clockDiscardedUs.store(syncEngine.discardedUs(),std::memory_order_relaxed);
  g.clockRepayments.store(syncEngine.repayments(),std::memory_order_relaxed);
  g.clockPhaseResyncs.store(syncEngine.resyncs(),std::memory_order_relaxed);
  g.clockLateWakeUs.store(syncEngine.lateWakeUs(),std::memory_order_relaxed);
  g.deadlineCatchupTicks.store(syncEngine.repayments(),std::memory_order_relaxed);
  g.deadlineDebtClamps.store(syncEngine.discardedUs()?1:0,std::memory_order_relaxed);
  uint64_t worst=g.deadlineDebtMaxUs.load(std::memory_order_relaxed);
  while(syncEngine.debtUs()>worst && !g.deadlineDebtMaxUs.compare_exchange_weak(worst,syncEngine.debtUs(),std::memory_order_relaxed)){}

 }
 perfHint.stop();
 g.running=false;
 bl40_native_leave_guest_thread();
 INFO("Hilo del nucleo termino, guestMs=%llu",(unsigned long long)g.virtualMs.load());
}
static BL40DynarmicReport gDynarmic;
static bool boot(AAssetManager* mgr){
 gBootAssetsRegistered.store(0,std::memory_order_release);
 gBootStage.store(1,std::memory_order_release);
 // El puente de comprobación de PXO no sustituye el motor de juego AOT.
 // Dynarmic/OKL4 no está integrado en la ruta jugable.
 auto image=readAsset(mgr,"pxo_guest_ro.bin");
 gDynarmic=bl40DynarmicCheck(image.empty()?nullptr:image.data(),image.size());
 INFO("Comprobacion PXO original (NO motor del juego): compiled=%d executed=%d pass=%u/%u: %s",
    gDynarmic.compiled,gDynarmic.executed,gDynarmic.casesPassed,
    gDynarmic.casesRequested,gDynarmic.reason.c_str());

 gBootStage.store(2,std::memory_order_release);
 g.error.clear();g.stop=false;g.paused=false;g.virtualMs=0;g.guestFrames=0;g.midiCount=0;g.tickCount=0;g.slowTicks=0;g.highestTickUs=0;g.lastSerial=0;
 g.schedulerSleepUs=0;g.schedulerPauseUs=0;g.schedulerWakeLateUs=0;
 g.tickOver10Ms=0;g.tickOver20Ms=0;
 for(auto& h:g.tickHist)h=0;g.heavyTickUs=0;g.heavyTickCount=0;
 g.audioAudibleBlocks=0;g.audioSilentBlocks=0;g.audioRenderUs=0;
 g.peakRms=0;g.lastRms=0;g.synthFailedNotes=0;g.peakActiveVoices=0;g.sf2PresetCount=0;
 g.touchEnqueued=0;g.touchDelivered=0;g.touchCoalesced=0;g.touchDelayTotalUs=0;
 g.touchDelayWorstUs=0;g.touchQueuePeak=0;g.touchWakeups=0;g.touchRejected=0;
 g.touchMoveDispatched=0;g.touchUpDispatched=0;g.touchMoveSamplesCollapsed=0;
 g.audioGuestFramesRendered=0;g.audioClockStarvedFrames=0;g.audioClockStarvedBlocks=0;g.audioLateMidiEvents=0;g.outputFrames=0;
 g.audioResyncs=0;g.audioDroppedStaleFrames=0;g.audioSkippedMidiEvents=0;g.lastAudioResync={};
 g.audioSilentFastForwardFrames=0;g.audioResyncEventsReplayed=0;
  g.audioGuestWaits=0;g.audioGuestWakeTimeouts=0;g.audioGuestWaitUs=0;
  g.touchSpatialWaypointsPreserved=0;
 g.hostDeadlineResets=0;g.hostDeadlineLostUs=0;
 g.deadlineDebtClamps=0;g.deadlineCatchupTicks=0;g.deadlineDebtMaxUs=0;g.deadlineLateStartTicks=0;
 g.clockDebtUs=0;g.clockAccruedUs=0;g.clockRepaidUs=0;g.clockDiscardedUs=0;g.clockRepayments=0;
 g.clockPhaseResyncs=0;g.clockLateWakeUs=0;
 g.tickCpuUs=0;g.midiPumpUs=0;g.frameReadUs=0;g.midiJobsQueued=0;
 g.midiParseErrors=0;g.guestMidiCaptured=0;
 g.sceneCandidateActive=false;g.sceneCandidateAtMs=0;g.sceneCandidateFiles=0;
 g.sceneEpoch=0;g.sceneTriggerMs=0;g.sceneChanges=0;g.audioSeenSceneEpoch=0;
 g.passiveRingN=0;g.passiveNextProbeMs=0;g.passiveCooldownUntilMs=0;g.passiveScenes=0;
 g.sceneMusicStops=0;g.musicReplacements=0;g.finishedTrackStops=0;
 g.midiIdentified=0;g.midiUnknownClassifications=0;g.audioSceneFlushes=0;
 g.channelControllerResets=0;g.deferredMidiStarts=0;g.duplicateBackgroundIgnored=0;g.sceneStopsFlushed=0;
 g.introOverlapSuppressed=0;g.pcmZeroWriteAvoided=0;
 {std::lock_guard<std::mutex> guard(g.diagnosticMutex);
  g.firstSoundServiceRaw.clear();g.firstAudioServiceRaw.clear();g.firstSlowTicks.clear();
  g.lastSoundServiceIndex=0;g.lastAudioServiceIndex=0;}
 g.audioOutputEpoch=0;g.sceneAllTrackStops=0;g.longJingleStops=0;g.musicFlushRequests=0;g.introAutoSceneChanges=0;
 g.introLongJingleReplacements=0;g.introDuplicateJingleIgnored=0;
 g.introAutoDone=false;g.introBaseSet=false;g.introPending=false;g.introLastProbeMs=0;g.introPendingAt=0;g.introOpenCount=0;
 g.pendingTouchVisual=false;g.touchVisualCount=0;g.touchVisualUs=0;g.touchVisualMaxUs=0;
 {std::lock_guard<std::mutex> guard(g.audioMutex);g.audioHistory.clear();g.audioHistoryFirst.clear();}
 g.fnCache.clear();g.useAot=false;g.useNativeC=false;
 gBootStage.store(3,std::memory_order_release);
 // Núcleo WebAssembly convertido a C y compilado para ARM64.
 // No carga ningun .aot ni usa WAMR. No confundir con PXO/OKL4 nativo.
 std::string initError;
 if(!bl40_native_initialize(initError)){
  setError(std::string("Motor C ARM64: ")+initError);return false;
 }
 g.nativeReady=true;
 g.useNativeC=true;
 INFO("Motor wasm2c C ARM64 instanciado; sin WAMR ni interprete");
 gBootStage.store(4,std::memory_order_release);
 auto pack=readAsset(mgr,"bl40_assets.pack");if(pack.empty())return false;
 if(!loadPack(pack)){if(g.error.empty())setError("Paquete de datos truncado");return false;}
 gBootStage.store(5,std::memory_order_release);
 if(invoke("bl40_boot")!=1){setError("Arranque PXO falló: "+g.error);return false;}
 grabFrame(true);
 // Graphics and input must not wait for the sound-bank decoder.
 // The SF2 is initialized by a separate JNI worker AFTER nativeStart().
 sampleGuestCounters();
 g.ready.store(true,std::memory_order_release);
 gBootStage.store(6,std::memory_order_release);
 INFO("Nucleo C ARM64 listo y primer fotograma capturado; audio diferido");
 return true;
}
// Publish synthesizer exactly once, before starting either PCM output worker.
// The native core thread does not access TSF; Guest calls remain exclusive there.
static bool initSoundAfterGraphics(AAssetManager* mgr){
 if(!g.ready.load(std::memory_order_acquire)||!mgr)return false;
 AAsset* bank=AAssetManager_open(mgr,"timgm6mb_native.sf2",AASSET_MODE_STREAMING);
 if(!bank){INFO("SF2 no encontrado; imagen y touch siguen activos");return false;}
 auto length=AAsset_getLength64(bank);
 if(length<1||length>32000000){AAsset_close(bank);ERR("SF2 tiene tamaño no válido");return false;}
 g.bankBytes.resize((size_t)length);
 size_t used=0;
 while(used<g.bankBytes.size()){
  int n=AAsset_read(bank,g.bankBytes.data()+used,(size_t)std::min((size_t)1<<20,g.bankBytes.size()-used));
  if(n<=0)break;
  used+=(size_t)n;
 }
 AAsset_close(bank);
 if(used!=g.bankBytes.size()){ERR("Lectura SF2 incompleta");return false;}
 tsf* synth=tsf_load_memory(g.bankBytes.data(),(int)g.bankBytes.size());
 if(!synth){ERR("No se pudo decodificar el banco SF2");return false;}
 tsf_set_output(synth,TSF_STEREO_INTERLEAVED,48000,0);
 if(!tsf_set_max_voices(synth,128))ERR("Sin memoria para 128 voces SF2");
 tsf_set_volume(synth,1.0f);
 for(int c=0;c<16;c++)tsf_channel_set_presetnumber(synth,c,0,c==9);
 g.sf2PresetCount.store(tsf_get_presetcount(synth),std::memory_order_relaxed);
 g.synth=synth; // published before Android starts AAudio/AudioTrack.
 INFO("SF2 diferido listo: %d presets; el audio no bloqueó la imagen",g.sf2PresetCount.load());
 return true;
}
static void applyEvent(tsf* synth,int ch,tml_message* msg){
 if(ch<0||ch>=16*32)return;
 switch(msg->type){
  case TML_NOTE_ON:
   if(msg->velocity){
    if(tsf_channel_note_on(synth,ch,msg->key,(float)msg->velocity/127.f))g.synthNotesOn++;
    else g.synthFailedNotes++;
   }
   else tsf_channel_note_off(synth,ch,msg->key);break;
  case TML_NOTE_OFF: tsf_channel_note_off(synth,ch,msg->key);break;
  case TML_PROGRAM_CHANGE:tsf_channel_set_presetnumber(synth,ch,msg->program,ch%16==9);break;
  case TML_CONTROL_CHANGE:tsf_channel_midi_control(synth,ch,msg->control,msg->control_value);break;
  case TML_PITCH_BEND:tsf_channel_set_pitchwheel(synth,ch,msg->pitch_bend);break;
  default:break;
 }
}
// SoundFont voices advance ONLY for guest frames already executed.
// On overload we fill unavailable host samples with silence (pitch is preserved;
// the real fix is to make guest CPU reach 1x). 48 frames = 1 guest millisecond.
static void stopTrackVoices(const Track& track){
 for(int ch=0;ch<16;ch++)tsf_channel_sounds_off_all(g.synth,track.channelOffset+ch);
}
// TinySoundFont persists controller state across reuse of a MIDI channel.
// Release old voices, reset sustain/expression/pan/bend and then set defaults.
// This does NOT alter guest time, pitch, tempo or note positions.
static void resetTrackChannels(const Track& track){
 for(int ch=0;ch<16;ch++){
  const int id=track.channelOffset+ch;
  tsf_channel_sounds_off_all(g.synth,id);
  tsf_channel_midi_control(g.synth,id,121,0); // all controllers off
  tsf_channel_midi_control(g.synth,id,64,0);  // sustain pedal off
  tsf_channel_set_pitchwheel(g.synth,id,8192);
  tsf_channel_set_presetnumber(g.synth,id,0,ch==9);
 }
 g.channelControllerResets.fetch_add(16,std::memory_order_relaxed);
}
// Called only under audioMutex, never from the main/UI thread.
static void recordAudioEventLocked(const char* reason,const Track& track,uint64_t guestMs){
 if(g.audioHistoryFirst.size()<24)g.audioHistoryFirst.push_back({guestMs,reason,track.source,track.music});
 if(g.audioHistory.size()>=48)g.audioHistory.pop_front();
 g.audioHistory.push_back({guestMs,reason,track.source,track.music});
}
static void applySceneAudioLocked(){
 const uint64_t epoch=g.sceneEpoch.load(std::memory_order_acquire);
 if(epoch<=g.audioSeenSceneEpoch)return;
 const uint64_t cutoff=g.sceneTriggerMs.load(std::memory_order_relaxed);
 bool removedActiveTrack=false;
 for(auto it=g.tracks.begin();it!=g.tracks.end();){
  // Stop ALL old-scene voices, including jingles labelled as effects.
  // Keep tracks started at the touch/transition, which may belong to the new menu.
  if(it->guestStart+20<cutoff){
   if(it->music)g.sceneMusicStops++;
   recordAudioEventLocked("scene-stop",*it,cutoff);
   stopTrackVoices(*it);it=g.tracks.erase(it);g.sceneAllTrackStops++;removedActiveTrack=true;
  }else ++it;
 }
 // Old asynchronous parser jobs may complete after the transition.
 for(auto it=g.incoming.begin();it!=g.incoming.end();){
  if(it->guestStart+20<cutoff){
   if(it->music)g.sceneMusicStops++;
   recordAudioEventLocked("scene-discard-pending",*it,cutoff);
   it=g.incoming.erase(it);g.sceneAllTrackStops++;
  }else ++it;
 }
 if(removedActiveTrack){
  // Only flush the old PCM if real voices were stopped by this transition.
  g.audioOutputEpoch.fetch_add(1,std::memory_order_release);
  g.sceneStopsFlushed++;
 }
 g.audioSeenSceneEpoch=epoch;
}

// V2.6: advance the *original MIDI event timeline* and the synthesizer while
// discarding obsolete PCM. The V2.5 resync skipped NOTE_ON/NOTE_OFF entirely,
// losing the musical state after an overloaded scene. Offline rendering is
// bounded to 512 frames per operation and must stay on the audio producer.
static void fastForwardSynthLocked(uint64_t fromFrame,uint64_t toFrame){
 if(toFrame<=fromFrame||!g.synth)return;
 std::array<float,1024> discarded{};
 uint64_t cursor=fromFrame;
 while(cursor<toFrame){
  uint64_t nextEvent=toFrame;
  for(auto& t:g.tracks){
   while(t.next){
    const uint64_t when=(t.guestStart+(uint64_t)t.next->time)*48u;
    if(when>cursor){nextEvent=std::min(nextEvent,when);break;}
    applyEvent(g.synth,t.channelOffset+(int)t.next->channel,t.next);
    g.audioResyncEventsReplayed++;
    t.next=t.next->next;
   }
  }
  for(auto it=g.tracks.begin();it!=g.tracks.end();){
   const uint64_t endMs=it->guestStart+(uint64_t)it->durationMs+75;
   if(!it->next&&cursor>=endMs*48u){
    recordAudioEventLocked("fast-forward-end",*it,endMs);
    stopTrackVoices(*it);it=g.tracks.erase(it);g.finishedTrackStops++;
   }else ++it;
  }
  const uint64_t span=std::max<uint64_t>(1,std::min<uint64_t>(
    512u,std::min<uint64_t>(toFrame-cursor,nextEvent-cursor)));
  tsf_render_float(g.synth,discarded.data(),(int)span,0);
  cursor+=span;
 }
 g.audioSilentFastForwardFrames.fetch_add(toFrame-fromFrame,std::memory_order_relaxed);
}

// AudioThread ONLY: bound the age of PCM in the host stream. We never speed up
// virtual time or resample its pitch. When Android's audio consumer falls behind
// by >500ms, discard OBSOLETE PCM and old active voices, keeping future MIDI
// messages at their original guest timestamps. This can create an audible gap,
// but avoids playing 19 seconds of old menu music as observed in V1.8.
static uint64_t prepareAudioEpoch(){
 if(!g.ready.load(std::memory_order_acquire)||!g.synth||g.paused.load(std::memory_order_acquire))
  return g.audioOutputEpoch.load(std::memory_order_acquire);
 constexpr uint64_t RATE=48, MAX_BACKLOG=500*RATE, TARGET_BACKLOG=80*RATE;
 const uint64_t end=g.virtualMs.load(std::memory_order_acquire)*RATE;
 const uint64_t position=g.audioGuestFramesRendered.load(std::memory_order_relaxed);
 if(end<=position+MAX_BACKLOG)return g.audioOutputEpoch.load(std::memory_order_acquire);
 const auto now=Clock::now();
 // Throttle any necessary stream resync so it cannot thrash AudioTrack.
 if(g.lastAudioResync.time_since_epoch().count()!=0 && now-g.lastAudioResync<Ms(1500))
  return g.audioOutputEpoch.load(std::memory_order_acquire);
 std::lock_guard<std::mutex> guard(g.audioMutex);
 const uint64_t newPosition=end>TARGET_BACKLOG?end-TARGET_BACKLOG:0;
 if(newPosition<=position)return g.audioOutputEpoch.load(std::memory_order_acquire);
 applySceneAudioLocked();
 // Do not skip note-on and note-off during resync. Run the missing MIDI
 // timeline without submitting stale PCM to the Android output stream.
 // This preserves voice/controller state; it does not invent guest time.
 fastForwardSynthLocked(position,newPosition);
 g.audioGuestFramesRendered.store(newPosition,std::memory_order_release);
 g.audioDroppedStaleFrames.fetch_add(newPosition-position,std::memory_order_relaxed);
 g.audioResyncs.fetch_add(1,std::memory_order_relaxed);
 g.lastAudioResync=now;
 INFO("Audio stream rebase: descartados=%llu guest frames, tiempo LG intacto",
      (unsigned long long)(newPosition-position));
 return g.audioOutputEpoch.fetch_add(1,std::memory_order_acq_rel)+1;
}
static int renderAudio(short* dst,int frames){
 if(frames<=0||frames>4096)return 0;
 memset(dst,0,(size_t)frames*2*sizeof(short));
 if(!g.ready||g.paused.load(std::memory_order_acquire)||!g.synth)return 0;
 constexpr uint64_t FRAMES_PER_MS=48;
 const uint64_t limit=g.virtualMs.load(std::memory_order_acquire)*FRAMES_PER_MS;
 const uint64_t cursor=g.audioGuestFramesRendered.load(std::memory_order_relaxed);
 const uint64_t available=limit>cursor?limit-cursor:0;
 const int playable=(int)std::min<uint64_t>((uint64_t)frames,available);
 if(playable<frames){g.audioClockStarvedFrames.fetch_add((uint64_t)(frames-playable),std::memory_order_relaxed);g.audioClockStarvedBlocks++;}
 if(playable<=0){
  // Apply transitions even if the guest has not supplied new audio frames.
  {std::lock_guard<std::mutex> guard(g.audioMutex);applySceneAudioLocked();}
  g.lastRms.store(0,std::memory_order_relaxed);
  g.pcmZeroWriteAvoided.fetch_add(1,std::memory_order_relaxed);
  return 0;
 }
 thread_local std::vector<float> audio;
 audio.resize((size_t)playable*2);
 const auto audioStarted=Clock::now();
 {
  // Only the Java AudioTrack worker owns TSF. MIDI parser only queues parsed
  // tracks under this mutex. The core never uses TSF or MIDI structures.
  std::lock_guard<std::mutex> guard(g.audioMutex);
  applySceneAudioLocked();
  while(!g.incoming.empty()){
   // Do not activate a new track before its guest-time onset: the audio
   // stream may lag the guest by a few blocks. Early activation previously
   // stopped old background music before the new track was due to sound.
   if(g.incoming.front().guestStart*FRAMES_PER_MS>cursor+(uint64_t)playable){
    g.deferredMidiStarts++;
    break;
   }
   Track track=std::move(g.incoming.front());g.incoming.pop_front();
   const uint64_t sceneCutoff=g.sceneTriggerMs.load(std::memory_order_relaxed);
   if(g.audioSeenSceneEpoch>0 && track.guestStart+20<sceneCutoff){
    if(track.music)g.sceneMusicStops++;
    recordAudioEventLocked("scene-discard-late-midi",track,sceneCutoff);
    g.sceneAllTrackStops++;continue;
   }
   // If a short introduction cue was queued alongside a00.mid at exactly
   // the same guest timestamp, do not mix two introductions together.
   // All other effects remain unchanged.
   if(!track.music){
    bool redundant=false;
    for(const auto& bg:g.tracks)
     if(redundantIntroCue(track,bg)){redundant=true;break;}
    if(redundant){
     recordAudioEventLocked("intro-e00-suppressed",track,track.guestStart);
     g.introOverlapSuppressed++;
     continue;
    }
   }
   // ORIGINAL MIDI OBSERVATION (DZ V10B): ef_mid_00.mid lasts ~7111 ms,
   // yet the HLE guest can request it every ~2840 ms during the introduction.
   // It was classified as an SFX, so several instances played together.
   // Enforce one *instance of this same cue* without silencing other sounds.
   // This is a limited HLE compatibility rule, not proven original CMX policy.
   if(!track.music && track.source=="ef_mid_00.mid"){
    bool duplicate=false, replaced=false;
    for(auto it=g.tracks.begin();it!=g.tracks.end();){
     if(it->source!="ef_mid_00.mid" || it->music){++it;continue;}
     if(track.guestStart>=it->guestStart && track.guestStart-it->guestStart<=60){
      duplicate=true;break;
     }
     recordAudioEventLocked("intro-long-jingle-replaced",*it,track.guestStart);
     stopTrackVoices(*it);
     it=g.tracks.erase(it);
     g.introLongJingleReplacements++;
     replaced=true;
    }
    if(duplicate){
     recordAudioEventLocked("intro-long-jingle-duplicate-ignored",track,track.guestStart);
     g.introDuplicateJingleIgnored++;
     continue;
    }
    if(replaced){
     // The audio writer must not keep old PCM for the stopped introduction.
     g.audioOutputEpoch.fetch_add(1,std::memory_order_release);
     g.musicFlushRequests++;
    }
   }
   if(track.music){
    // Only suppress the same background song restarted almost immediately;
    // actual new songs retain their original guest timestamps.
    bool duplicate=false;
    for(const auto& old:g.tracks){
     if(old.music && old.source==track.source &&
        track.guestStart>=old.guestStart &&
        track.guestStart-old.guestStart<150){duplicate=true;break;}
    }
    if(duplicate){
     recordAudioEventLocked("duplicate-bg-ignored",track,track.guestStart);
     g.duplicateBackgroundIgnored++;
     continue;
    }
    bool stoppedSomething=false;
    for(auto it=g.tracks.begin();it!=g.tracks.end();){
     // New background music has exclusive ownership of the soundtrack.
     // Very old long jingles cannot continue into the next menu.
     const bool staleLongJingle=!it->music && it->durationMs>=1500
       && it->guestStart+500<track.guestStart;
     const bool overlappingIntroCue=redundantIntroCue(*it,track);
     if(it->music||staleLongJingle||overlappingIntroCue){
      recordAudioEventLocked(overlappingIntroCue?"intro-e00-suppressed":
        (staleLongJingle?"new-music-cut-jingle":"music-replaced"),*it,track.guestStart);
      if(overlappingIntroCue)g.introOverlapSuppressed++;
      stopTrackVoices(*it);it=g.tracks.erase(it);
      if(staleLongJingle)g.longJingleStops++;
      else if(!overlappingIntroCue)g.musicReplacements++;
      stoppedSomething=true;
     }else ++it;
    }
    if(stoppedSomething){
     // Ask Java to flush buffered PCM from the replaced background track.
     g.audioOutputEpoch.fetch_add(1,std::memory_order_release);
     g.musicFlushRequests++;
    }
   }
   int slot=0;bool free=false;
   for(;slot<31;slot++){
    free=true;for(const auto& t:g.tracks)if(t.channelOffset==slot*16){free=false;break;}
    if(free)break;
   }
   if(!free)continue;
   track.channelOffset=slot*16;
   resetTrackChannels(track);
   // Async MIDI parsing may complete after an audio resync. Fast-forward
   // stale events (never burst old note-ons at the current PCM cursor).
   const uint64_t lateFloor=cursor>48*60?cursor-48*60:0;
   while(track.next && (track.guestStart+(uint64_t)track.next->time)*FRAMES_PER_MS<lateFloor){
    const auto type=track.next->type;
    if(type==TML_PROGRAM_CHANGE || type==TML_CONTROL_CHANGE || type==TML_PITCH_BEND)
     applyEvent(g.synth,track.channelOffset+(int)track.next->channel,track.next);
    else g.audioSkippedMidiEvents.fetch_add(1,std::memory_order_relaxed);
    track.next=track.next->next;
   }
   recordAudioEventLocked("start",track,track.guestStart);
   g.tracks.push_back(std::move(track));
  }
  int done=0;
  while(done<playable){
   const uint64_t guestFrame=cursor+(uint64_t)done;
   uint64_t nextEventFrame=cursor+(uint64_t)playable;
   // Apply each MIDI event at its scheduled guest-time position rather than
   // activating a whole 10ms batch at the start of a 512-frame PCM block.
   for(auto& t:g.tracks){
    while(t.next){
     const uint64_t when=(t.guestStart+(uint64_t)t.next->time)*FRAMES_PER_MS;
     if(when>guestFrame){nextEventFrame=std::min(nextEventFrame,when);break;}
     if(when<guestFrame)g.audioLateMidiEvents.fetch_add(1,std::memory_order_relaxed);
     applyEvent(g.synth,t.channelOffset+(int)t.next->channel,t.next);
     t.next=t.next->next;
    }
   }
   for(auto it=g.tracks.begin();it!=g.tracks.end();){
    // TML events may end while a TSF voice still sustains (missing note-off).
    // End-of-track + 75ms follows the finite-tail policy.
    const uint64_t endMs=it->guestStart+(uint64_t)it->durationMs+75;
    if(!it->next && guestFrame >= endMs*FRAMES_PER_MS){
     recordAudioEventLocked("end",*it,endMs);
     stopTrackVoices(*it);it=g.tracks.erase(it);g.finishedTrackStops++;
    }else ++it;
   }
   const int part=(int)std::max<uint64_t>(1,std::min<uint64_t>((uint64_t)(playable-done),nextEventFrame-guestFrame));
   tsf_render_float(g.synth,audio.data()+2*done,part,0);
   done+=part;
  }
 }
 g.audioGuestFramesRendered.fetch_add((uint64_t)playable,std::memory_order_release);
 const auto audioEnded=Clock::now();
 g.audioRenderUs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(audioEnded-audioStarted).count(),std::memory_order_relaxed);
 const uint64_t voices=(uint64_t)std::max(0,tsf_active_voice_count(g.synth));
 uint64_t oldVoices=g.peakActiveVoices.load(std::memory_order_relaxed);
 while(voices>oldVoices&&!g.peakActiveVoices.compare_exchange_weak(oldVoices,voices,std::memory_order_relaxed)){}
 double energy=0;
 for(int i=0;i<playable*2;i++){
  const float mixed=std::tanh(audio[(size_t)i]*0.85f);
  energy+=mixed*mixed;
  dst[i]=(short)std::lround(std::max(-1.f,std::min(1.f,mixed))*32767.f);
 }
 const double rms=std::sqrt(energy/((size_t)frames*2));
 g.lastRms.store(rms,std::memory_order_relaxed);
 double oldRms=g.peakRms.load(std::memory_order_relaxed);
 while(rms>oldRms&&!g.peakRms.compare_exchange_weak(oldRms,rms,std::memory_order_relaxed)){}
 if(rms>0.00001)g.audioAudibleBlocks++;else g.audioSilentBlocks++;
 g.outputFrames+=(uint64_t)playable;
 return playable;
}
static std::string diagnostic(){
 std::ostringstream o;
 const auto wall=(g.running?Clock::now():Clock::now());
 const uint64_t ms=g.ready?std::chrono::duration_cast<Ms>(wall-g.wallStart).count():0;
 const uint64_t ticks=g.tickCount.load();
 const uint64_t virt=g.virtualMs.load();
 const double rate=ms?((double)virt/ms):0;
 const auto pausedUs=g.schedulerPauseUs.load(std::memory_order_relaxed);
 const double activeMs=std::max(0.0,(double)ms-(double)pausedUs/1000.0);
 const double activeRate=activeMs>0?((double)virt/activeMs):0;
 o<<"{\n  \"app\": \"MusicWorld BL40 / C ARM64\",\n  \"backend\": \"juego=wasm2c C ARM64 phase-locked; WAMR no enlazado; Dynarmic PXO/OKL4 no integrado; video=GLES2; audio=AAudio/TSF\",\n";
 o<<"  \"realFirmwareLLE\": false,\n  \"guestClockMultiplier\": 1.0,\n  \"scheduler\": \"phase-locked 10ms guest tick, 8ms minimum host start spacing, 400ms emergency reanchor\",\n";
 o<<"  \"virtualMs\": "<<virt<<",\n  \"wallMs\": "<<ms<<",\n  \"effectiveSpeed\": "<<rate<<",\n";
 o<<"  \"effectiveActiveSpeed\": "<<activeRate<<",\n";
 o<<"  \"activeWallMs\": "<<activeMs<<",\n";
 o<<"  \"pausedWallMs\": "<<(pausedUs/1000.0)<<",\n";
 o<<"  \"schedulerSleepMs\": "<<(g.schedulerSleepUs.load()/1000.0)<<",\n";
 o<<"  \"schedulerWakeLateMs\": "<<(g.schedulerWakeLateUs.load()/1000.0)<<",\n";
 o<<"  \"hostDeadlineResets\": "<<g.hostDeadlineResets.load()<<",\n";
 o<<"  \"hostDeadlineLostMs\": "<<(g.hostDeadlineLostUs.load()/1000.0)<<",\n";
 o<<"  \"clockDebtMs\": "<<(g.clockDebtUs.load()/1000.0)<<",\n";
 o<<"  \"clockAccruedMs\": "<<(g.clockAccruedUs.load()/1000.0)<<",\n";
 o<<"  \"clockRepaidMs\": "<<(g.clockRepaidUs.load()/1000.0)<<",\n";
 o<<"  \"clockDiscardedMs\": "<<(g.clockDiscardedUs.load()/1000.0)<<",\n";
 o<<"  \"clockRepayments\": "<<g.clockRepayments.load()<<",\n";
 o<<"  \"clockPhaseResyncs\": "<<g.clockPhaseResyncs.load()<<",\n";
 o<<"  \"clockLateWakeMs\": "<<(g.clockLateWakeUs.load()/1000.0)<<",\n";
 o<<"  \"deadlineCatchupTicks\": "<<g.deadlineCatchupTicks.load()<<",\n";
 o<<"  \"deadlineLateStartTicks\": "<<g.deadlineLateStartTicks.load()<<",\n";
 o<<"  \"touchWakeups\": "<<g.touchWakeups.load()<<",\n";
 o<<"  \"touchRejected\": "<<g.touchRejected.load()<<",\n";
 o<<"  \"touchMoveDispatched\": "<<g.touchMoveDispatched.load()<<",\n";
 o<<"  \"touchUpDispatched\": "<<g.touchUpDispatched.load()<<",\n";
 o<<"  \"touchMoveSamplesCollapsed\": "<<g.touchMoveSamplesCollapsed.load()<<",\n";
 o<<"  \"audioGuestFramesRendered\": "<<g.audioGuestFramesRendered.load()<<",\n";
 o<<"  \"audioClockStarvedFrames\": "<<g.audioClockStarvedFrames.load()<<",\n";
 o<<"  \"audioGuestWaits\": "<<g.audioGuestWaits.load()<<",\n";
 o<<"  \"audioGuestWakeTimeouts\": "<<g.audioGuestWakeTimeouts.load()<<",\n";
 o<<"  \"audioGuestWaitMs\": "<<(g.audioGuestWaitUs.load()/1000.0)<<",\n";
 o<<"  \"audioClockStarvedBlocks\": "<<g.audioClockStarvedBlocks.load()<<",\n";
 o<<"  \"audioResyncs\": "<<g.audioResyncs.load()<<",\n";
 o<<"  \"audioDroppedStaleFrames\": "<<g.audioDroppedStaleFrames.load()<<",\n";
 o<<"  \"audioSkippedMidiEvents\": "<<g.audioSkippedMidiEvents.load()<<",\n";
 o<<"  \"audioResyncEventsReplayed\": "<<g.audioResyncEventsReplayed.load()<<",\n";
 o<<"  \"audioSilentFastForwardFrames\": "<<g.audioSilentFastForwardFrames.load()<<",\n";
 o<<"  \"audioLateMidiEvents\": "<<g.audioLateMidiEvents.load()<<",\n";
 o<<"  \"audioGuestBacklogMs\": "<<(std::max<int64_t>(0,(int64_t)g.virtualMs.load()*48-(int64_t)g.audioGuestFramesRendered.load())/48.0)<<",\n";
 o<<"  \"deadlineDebtClamps\": "<<g.deadlineDebtClamps.load()<<",\n";
 o<<"  \"deadlineDebtMaxMs\": "<<(g.deadlineDebtMaxUs.load()/1000.0)<<",\n";
 o<<"  \"touchEnqueued\": "<<g.touchEnqueued.load()<<",\n";
 o<<"  \"touchDelivered\": "<<g.touchDelivered.load()<<",\n";
 o<<"  \"touchCoalesced\": "<<g.touchCoalesced.load()<<",\n";
 o<<"  \"touchQueuePeak\": "<<g.touchQueuePeak.load()<<",\n";
 o<<"  \"touchDelayAvgMs\": "<<((g.touchDelivered.load()+g.touchRejected.load())? (g.touchDelayTotalUs.load()/1000.0)/(g.touchDelivered.load()+g.touchRejected.load()):0.0)<<",\n";
 o<<"  \"touchDelayWorstMs\": "<<(g.touchDelayWorstUs.load()/1000.0)<<",\n";
 o<<"  \"touchSpatialWaypointsPreserved\": "<<g.touchSpatialWaypointsPreserved.load()<<",\n";
 o<<"  \"tickOver10Ms\": "<<g.tickOver10Ms.load()<<",\n";
 o<<"  \"tickOver20Ms\": "<<g.tickOver20Ms.load()<<",\n";
 {
  static const char* kNames[9]={"lt0_1ms","lt1ms","lt2ms","lt5ms","lt10ms","lt20ms","lt40ms","lt80ms","ge80ms"};
  o<<"  \"tickHistogram\": {";
  for(int i=0;i<9;i++)o<<(i?", ":"")<<"\""<<kNames[i]<<"\": "<<g.tickHist[i].load(std::memory_order_relaxed);
  o<<"},\n";
  const uint64_t hc=g.heavyTickCount.load(std::memory_order_relaxed);
  o<<"  \"heavyTickCount\": "<<hc<<",\n  \"heavyTickAvgMs\": "<<(hc?(double)g.heavyTickUs.load(std::memory_order_relaxed)/hc/1000.0:0.0)<<",\n";
  const double activeWall=std::max(1.0,activeMs);
  o<<"  \"coreCpuLoadPercent\": "<<((double)g.tickCpuUs.load(std::memory_order_relaxed)/1000.0/activeWall*100.0)<<",\n";
 }
 o<<"  \"tickCount\": "<<ticks<<",\n  \"worstTickMs\": "<<(g.highestTickUs.load()/1000.0)<<",\n  \"tickOver50Ms\": "<<g.slowTicks.load()<<",\n";
 o<<"  \"presentedFrames\": "<<g.guestFrames.load()<<",\n  \"approxGuestFramesPerSecond\": "<<(virt>0?g.guestFrames.load()*1000.0/virt:0)<<",\n";
 o<<"  \"coreTickCpuMs\": "<<(g.tickCpuUs.load()/1000.0)<<",\n";
 o<<"  \"midiPumpCpuMs\": "<<(g.midiPumpUs.load()/1000.0)<<",\n";
 o<<"  \"frameReadCpuMs\": "<<(g.frameReadUs.load()/1000.0)<<",\n";
 o<<"  \"guestMidiCaptured\": "<<g.guestMidiCaptured.load()<<",\n";
 o<<"  \"midiJobsQueued\": "<<g.midiJobsQueued.load()<<",\n";
 o<<"  \"midiParseErrors\": "<<g.midiParseErrors.load()<<",\n";
 o<<"  \"aotEnabled\": false,\n";
 o<<"  \"wasm2cNativeEnabled\": "<<(g.useNativeC?"true":"false")<<",\n";
 o<<"  \"soundServiceCalls\": "<<g.soundServiceCalls.load()<<",\n";
 o<<"  \"guestFileOpens\": "<<g.guestFileOpens.load()<<",\n";
 o<<"  \"guestMidiDropped\": "<<g.guestMidiDropped.load()<<",\n";
 o<<"  \"guestMidiPending\": "<<g.guestPendingMidi.load()<<",\n";
 o<<"  \"guestFastPairs\": "<<g.guestFastPairs.load()<<",\n";
 o<<"  \"unknownServiceCalls\": "<<g.unknownServiceCalls.load()<<",\n";
 o<<"  \"synthNotesOn\": "<<g.synthNotesOn.load()<<",\n";
 o<<"  \"wasmLookupMisses\": "<<g.wasmLookupMisses.load()<<",\n";
 o<<"  \"audioPeakRms\": "<<g.peakRms.load()<<",\n";
 o<<"  \"audioAudibleBlocks\": "<<g.audioAudibleBlocks.load()<<",\n";
 o<<"  \"audioSilentBlocks\": "<<g.audioSilentBlocks.load()<<",\n";
 o<<"  \"audioRenderWallMs\": "<<(g.audioRenderUs.load()/1000.0)<<",\n";
 o<<"  \"peakActiveVoices\": "<<g.peakActiveVoices.load()<<",\n";
 o<<"  \"synthFailedNotes\": "<<g.synthFailedNotes.load()<<",\n";
 o<<"  \"sf2PresetCount\": "<<g.sf2PresetCount.load()<<",\n";
 o<<"  \"midiReceived\": "<<g.midiCount.load()<<",\n  \"audioOutputFrames\": "<<g.outputFrames.load()<<",\n  \"audioRms\": "<<g.lastRms.load()<<",\n";
 o<<"  \"runtimePolicy\": \"phase-locked-wasm2c-native\",\n";
 o<<"  \"introOverlapSuppressed\": "<<g.introOverlapSuppressed.load()<<",\n";
 o<<"  \"introLongJingleReplacements\": "<<g.introLongJingleReplacements.load()<<",\n";
 o<<"  \"introDuplicateJingleIgnored\": "<<g.introDuplicateJingleIgnored.load()<<",\n";
 o<<"  \"pcmZeroWriteAvoided\": "<<g.pcmZeroWriteAvoided.load()<<",\n";
 {std::lock_guard<std::mutex> lock(g.diagnosticMutex);
  auto outputRows=[&](const char* label,const std::vector<std::array<uint32_t,8>>& rows){
   o<<"  \""<<label<<"\": [";
   for(size_t i=0;i<rows.size();i++){
    if(i)o<<",";o<<"[";
    for(size_t c=0;c<rows[i].size();c++){if(c)o<<",";o<<rows[i][c];}
    o<<"]";
   }
   o<<"],\n";
  };
  outputRows("soundServiceRawFirst",g.firstSoundServiceRaw);
  outputRows("audioTraceRawFirst",g.firstAudioServiceRaw);
  o<<"  \"slowTicksFirst\": [";
  for(size_t i=0;i<g.firstSlowTicks.size();i++){
   const auto& e=g.firstSlowTicks[i];if(i)o<<",";
   o<<"{\"guestMs\":"<<e.guestMs<<",\"cpuMs\":"<<(e.costUs/1000.0)
    <<",\"opens\":"<<e.files<<",\"soundCalls\":"<<e.sounds<<"}";
  }
  o<<"],\n";
 }
 o<<"  \"introAutoSceneChanges\": "<<g.introAutoSceneChanges.load()<<",\n";
 o<<"  \"sceneAllTrackStops\": "<<g.sceneAllTrackStops.load()<<",\n";
 o<<"  \"longJingleStops\": "<<g.longJingleStops.load()<<",\n";
 o<<"  \"musicFlushRequests\": "<<g.musicFlushRequests.load()<<",\n";
 o<<"  \"touchVisualSamples\": "<<g.touchVisualCount.load()<<",\n";
 o<<"  \"touchToFrameAvgMs\": "<<(g.touchVisualCount.load()?g.touchVisualUs.load()/1000.0/g.touchVisualCount.load():0)<<",\n";
 o<<"  \"touchToFrameWorstMs\": "<<g.touchVisualMaxUs.load()/1000.0<<",\n";
 o<<"  \"trackHistoryFirst\": [";
 {std::lock_guard<std::mutex> guard(g.audioMutex);
  for(size_t i=0;i<g.audioHistoryFirst.size();i++){
   const auto& ev=g.audioHistoryFirst[i];if(i)o<<",";
   o<<"{\"guestMs\":"<<ev.guestMs<<",\"event\":\""<<ev.action
     <<"\",\"midi\":\""<<ev.source<<"\",\"music\":"<<(ev.music?"true":"false")<<"}";
  }
 }
 o<<"],\n";
 o<<"  \"trackHistory\": [";
 {std::lock_guard<std::mutex> guard(g.audioMutex);
  bool comma=false;
  for(const auto& event:g.audioHistory){
   if(comma)o<<",";comma=true;
   o<<"{\"guestMs\":"<<event.guestMs<<",\"event\":\""<<event.action
    <<"\",\"midi\":\""<<event.source<<"\",\"music\":"<<(event.music?"true":"false")<<"}";
  }
 }
 o<<"],\n";
 o<<"  \"sceneEpoch\": "<<g.sceneEpoch.load()<<",\n";
 o<<"  \"passiveGameplayScenes\": "<<g.passiveScenes.load()<<",\n";
 o<<"  \"coreAffinityMask\": "<<g.coreAffinityMask.load()<<",\n";
 o<<"  \"sceneChanges\": "<<g.sceneChanges.load()<<",\n";
 o<<"  \"sceneMusicStops\": "<<g.sceneMusicStops.load()<<",\n";
 o<<"  \"musicReplacements\": "<<g.musicReplacements.load()<<",\n";
 o<<"  \"finishedTrackStops\": "<<g.finishedTrackStops.load()<<",\n";
 o<<"  \"channelControllerResets\": "<<g.channelControllerResets.load()<<",\n";
 o<<"  \"deferredMidiStarts\": "<<g.deferredMidiStarts.load()<<",\n";
 o<<"  \"duplicateBackgroundIgnored\": "<<g.duplicateBackgroundIgnored.load()<<",\n";
 o<<"  \"sceneStopsFlushed\": "<<g.sceneStopsFlushed.load()<<",\n";
 o<<"  \"midiIdentified\": "<<g.midiIdentified.load()<<",\n";
 o<<"  \"midiUnknownClassifications\": "<<g.midiUnknownClassifications.load()<<",\n";
 o<<"  \"audioSceneFlushes\": "<<g.audioSceneFlushes.load()<<",\n";
 o<<"  \"cpuGameBackend\": \"wasm2c C ARM64 (no PXO LLE)\",\n";
 o<<"  \"dynarmicA32Jit\": {\"compiled\": "<<(gDynarmic.compiled?"true":"false")
  <<", \"executed\": "<<(gDynarmic.executed?"true":"false")
  <<", \"passed\": "<<(gDynarmic.success?"true":"false")
  <<", \"cases\": "<<gDynarmic.casesPassed
  <<", \"expected\": "<<gDynarmic.casesRequested
  <<", \"guestInstructions\": "<<gDynarmic.guestInstructions
  <<", \"detail\": \"";
 for(char c:gDynarmic.reason){if(c=='"'||c=='\\')o<<'\\';if((unsigned char)c>=32)o<<c;}
 o<<"\"},\n";
 o<<"  \"guestPresentationSerial\": "<<g.lastSerial<<",\n";
 o<<"  \"sampleBank\": \"TimGM6mb -> SF2, GPL-2.0, NOT LG CMX\",\n  \"sampleBankLoaded\": "<<(g.sf2PresetCount.load(std::memory_order_acquire)>0?"true":"false")<<",\n";
 o<<"  \"ready\": "<<(g.ready?"true":"false")<<",\n  \"running\": "<<(g.running?"true":"false")<<",\n  \"error\": \"";
 for(char c:g.error){if(c=='"'||c=='\\')o<<'\\';if(c>=32&&c<127)o<<c;}
 o<<"\"\n}\n";return o.str();
}
extern "C" JNIEXPORT jboolean JNICALL Java_org_musicworld_bl40native_MainActivity_nativeInit(JNIEnv* env,jobject, jobject assetMgr){
 auto* mgr=AAssetManager_fromJava(env,assetMgr);
 if(!mgr)return JNI_FALSE;
 return boot(mgr)?JNI_TRUE:JNI_FALSE;
}
extern "C" JNIEXPORT jstring JNICALL Java_org_musicworld_bl40native_MainActivity_nativeBootStatus(JNIEnv* env,jobject){
 const int stage=gBootStage.load(std::memory_order_acquire);
 const int assets=gBootAssetsRegistered.load(std::memory_order_acquire);
 const char* name="preparando";
 switch(stage){
  case 1:name="verificando PXO";break;
  case 2:name="cargando núcleo WASM";break;
  case 3:name="inicializando codigo C nativo";break;
  case 4:name="registrando recursos";break;
  case 5:name="arrancando juego";break;
  case 6:name="imagen lista";break;
  default:break;
 }
 char buf[128];
 if(stage==4)std::snprintf(buf,sizeof(buf),"%s: %d/145",name,assets);
 else std::snprintf(buf,sizeof(buf),"%s",name);
 return env->NewStringUTF(buf);
}
extern "C" JNIEXPORT jboolean JNICALL Java_org_musicworld_bl40native_MainActivity_nativeInitAudio(JNIEnv* env,jobject,jobject assetMgr){
 auto* mgr=AAssetManager_fromJava(env,assetMgr);
 return initSoundAfterGraphics(mgr)?JNI_TRUE:JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL Java_org_musicworld_bl40native_MainActivity_nativeStart(JNIEnv*,jobject){
 if(!g.ready||g.running)return;
 g.stop=false;g.running=true;
 g.midiThread=std::thread(midiParserLoop);
 g.coreThread=std::thread(coreLoop);
}
extern "C" JNIEXPORT void JNICALL Java_org_musicworld_bl40native_MainActivity_nativePause(JNIEnv*,jobject,jboolean paused){
 g.paused.store(paused==JNI_TRUE,std::memory_order_release);g.touchCv.notify_one();g.audioClockCv.notify_all();
}
extern "C" JNIEXPORT void JNICALL Java_org_musicworld_bl40native_MainActivity_nativeTouch(JNIEnv*,jobject,jint kind,jint x,jint y){
 if(!g.ready)return;
 x=std::max(0,std::min(W-1,(int)x));y=std::max(0,std::min(H-1,(int)y));
 const auto now=Clock::now();
 std::lock_guard<std::mutex> lock(g.touchMutex);
 g.touchEnqueued.fetch_add(1,std::memory_order_relaxed);
 // Merge only redundant adjacent moves. DOWN and UP are never discarded.
 if(kind==1 && !g.touches.empty() && g.touches.back().kind==1){
  const auto& prev=g.touches.back();
  const int distance=std::abs(prev.x-x)+std::abs(prev.y-y);
  const auto age=std::chrono::duration_cast<std::chrono::milliseconds>(now-prev.hostAt).count();
  if(distance<=2 && age<4){g.touches.back()={1,(int)x,(int)y,now};g.touchCoalesced++;g.touchCv.notify_one();return;}
 }
 if(g.touches.size()>=128){
  auto oldMove=std::find_if(g.touches.begin(),g.touches.end(),[](const State::Touch&t){return t.kind==1;});
  if(oldMove!=g.touches.end()){g.touches.erase(oldMove);g.touchCoalesced++;}
 }
 g.touches.push_back({(int)kind,(int)x,(int)y,now});
 uint64_t peak=g.touchQueuePeak.load(std::memory_order_relaxed);
 while(g.touches.size()>peak && !g.touchQueuePeak.compare_exchange_weak(peak,(uint64_t)g.touches.size(),std::memory_order_relaxed)){}
 g.touchCv.notify_one();
}
// Render thread reads only an atomic counter, never guest state.
extern "C" JNIEXPORT jlong JNICALL Java_org_musicworld_bl40native_MainActivity_nativePrepareAudio(JNIEnv*,jobject){
 return (jlong)prepareAudioEpoch();
}
// AudioTrack worker only: wait for new guest PCM without busy-spinning.
// Returns immediately if at least 128 frames are available; otherwise waits
// until the next completed guest tick, pause, stop or an audio scene flush.
extern "C" JNIEXPORT void JNICALL Java_org_musicworld_bl40native_MainActivity_nativeWaitForGuestAudio(JNIEnv*,jobject,jint timeoutMs){
 const auto epoch=g.audioOutputEpoch.load(std::memory_order_acquire);
 const auto predicate=[&](){
  const uint64_t limit=g.virtualMs.load(std::memory_order_acquire)*48u;
  const uint64_t cursor=g.audioGuestFramesRendered.load(std::memory_order_acquire);
  return g.stop.load(std::memory_order_acquire)||g.paused.load(std::memory_order_acquire)||
         g.audioOutputEpoch.load(std::memory_order_acquire)!=epoch||
         limit>=cursor+128u;
 };
 const int ms=std::max(1,std::min(30,(int)timeoutMs));
 const auto started=Clock::now();
 std::unique_lock<std::mutex> guard(g.audioClockMutex);
 if(!g.audioClockCv.wait_for(guard,Ms(ms),predicate))g.audioGuestWakeTimeouts++;
 g.audioGuestWaits++;
 const auto waited=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-started).count();
 if(waited>0)g.audioGuestWaitUs.fetch_add((uint64_t)waited,std::memory_order_relaxed);
}
extern "C" JNIEXPORT jlong JNICALL Java_org_musicworld_bl40native_MainActivity_nativeAudioEpoch(JNIEnv*,jobject){
 return (jlong)g.audioOutputEpoch.load(std::memory_order_acquire);
}
extern "C" JNIEXPORT void JNICALL Java_org_musicworld_bl40native_MainActivity_nativeSceneAudioFlushed(JNIEnv*,jobject){
 g.audioSceneFlushes.fetch_add(1,std::memory_order_relaxed);
}
extern "C" JNIEXPORT jlong JNICALL Java_org_musicworld_bl40native_MainActivity_nativeFrameCounter(JNIEnv*,jobject){
 return (jlong)g.guestFrames.load(std::memory_order_acquire);
}
extern "C" JNIEXPORT jboolean JNICALL Java_org_musicworld_bl40native_MainActivity_nativeCopyFrame(JNIEnv* env,jobject,jobject directBuf){
 void* p=env->GetDirectBufferAddress(directBuf);if(!p||env->GetDirectBufferCapacity(directBuf)<(jlong)FRAME_BYTES)return JNI_FALSE;
 std::lock_guard<std::mutex> guard(g.frameMutex);memcpy(p,g.frame.data(),FRAME_BYTES);return JNI_TRUE;
}
extern "C" JNIEXPORT jint JNICALL Java_org_musicworld_bl40native_MainActivity_nativeRenderPcm(JNIEnv* env,jobject,jshortArray out,jint frames){
 if(!out||frames<1||frames>4096||env->GetArrayLength(out)<frames*2)return 0;
 jshort* pcm=env->GetShortArrayElements(out,nullptr);if(!pcm)return 0;
 const int playable=renderAudio(pcm,(int)frames);
 env->ReleaseShortArrayElements(out,pcm,0);
 return (jint)playable;
}
extern "C" JNIEXPORT jstring JNICALL Java_org_musicworld_bl40native_MainActivity_nativeStats(JNIEnv* env,jobject){
 std::string str=diagnostic();return env->NewStringUTF(str.c_str());
}
extern "C" JNIEXPORT jboolean JNICALL Java_org_musicworld_bl40native_MainActivity_nativeReady(JNIEnv*,jobject){return g.ready?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT void JNICALL Java_org_musicworld_bl40native_MainActivity_nativeStop(JNIEnv*,jobject){
 g.stop=true;g.touchCv.notify_all();g.midiCv.notify_all();g.audioClockCv.notify_all();
 if(g.coreThread.joinable())g.coreThread.join();
 g.midiCv.notify_all();
 if(g.midiThread.joinable())g.midiThread.join();
 g.ready=false;
 bl40_native_release();g.nativeReady=false;g.useNativeC=false;
 // El sintetizador TSF se libera por el ciclo de vida Android; mientras
 // Android audio output threads may still render could race with its memory.
}

// Shared guest-timestamped sound mixer, isolated from the real-time AAudio callback.
extern "C" int bl40AudioRender(int16_t* out,int frames){return renderAudio(out,frames);}
extern "C" uint64_t bl40AudioPrepare(){return prepareAudioEpoch();}
extern "C" uint64_t bl40AudioEpoch(){return g.audioOutputEpoch.load(std::memory_order_acquire);}
extern "C" void bl40AudioWait(int timeoutMs){
  const auto epoch=g.audioOutputEpoch.load(std::memory_order_acquire);
  const auto predicate=[&](){
    const uint64_t limit=g.virtualMs.load(std::memory_order_acquire)*48u;
    const uint64_t cursor=g.audioGuestFramesRendered.load(std::memory_order_acquire);
    return g.stop.load(std::memory_order_acquire)||g.paused.load(std::memory_order_acquire)||
      g.audioOutputEpoch.load(std::memory_order_acquire)!=epoch||limit>=cursor+128u;
  };
  std::unique_lock<std::mutex> guard(g.audioClockMutex);
  g.audioClockCv.wait_for(guard,Ms(std::max(1,std::min(30,timeoutMs))),predicate);
}
extern "C" void bl40AudioFlushed(){g.audioSceneFlushes.fetch_add(1,std::memory_order_relaxed);}
