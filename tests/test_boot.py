from pathlib import Path
import re
p=Path(__file__).resolve().parents[1]
java=(p/'app/src/main/java/org/musicworld/bl40native/MainActivity.java').read_text()
cpp=(p/'app/src/main/cpp/native_engine.cpp').read_text()
gradle=(p/'app/build.gradle').read_text()
assert 'private native boolean nativeInitAudio(AssetManager assets)' in java
assert 'Java_org_musicworld_bl40native_MainActivity_nativeInitAudio' in cpp
assert 'nativeStart();' in java
assert 'loaded=true;' in java
assert 'frame.requestRender()' in java
assert 'startSound();' in java
assert java.index('nativeStart();') < java.index('loaded=true;') < java.index('startSound();')
assert 'gBootAssetsRegistered.store((int)(i+1)' in cpp
assert 'gBootStage.store(6,std::memory_order_release)' in cpp
assert cpp.index('g.ready.store(true') < cpp.index('static bool initSoundAfterGraphics')
assert cpp.index('g.synth=synth;') < cpp.index('JNIEXPORT jboolean JNICALL Java_org_musicworld_bl40native_MainActivity_nativeInitAudio')
assert 'g.sf2PresetCount.load(std::memory_order_acquire)>0' in cpp
assert "versionCode 35" in gradle
assert 'org.musicworld.bl40native.juego' in gradle
assert 'if(stopping.get()||loaded||bootComplete)return' in java
print('OK: arranque visual antes del sonido, JNI conectado y diagnóstico de fases')
