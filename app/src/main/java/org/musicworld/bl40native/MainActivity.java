package org.musicworld.bl40native;

import android.app.Activity;
import android.os.Bundle;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.content.Intent;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Rect;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.net.Uri;
import android.content.res.AssetManager;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import android.opengl.GLSurfaceView;
import android.opengl.GLES20;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;
import java.util.concurrent.atomic.AtomicBoolean;

/** OpenGL ES 2 RGB565 renderer, native AAudio PCM and existing LG guest runtime. */
public class MainActivity extends Activity {
    static { System.loadLibrary("musicworld"); }
    private native boolean nativeInit(AssetManager assets);
    private native boolean nativeInitAudio(AssetManager assets);
    private native String nativeBootStatus();
    private native void nativeStart();
    private native void nativePause(boolean pause);
    private native void nativeStop();
    private native void nativeTouch(int kind,int x,int y);
    private native boolean nativeCopyFrame(ByteBuffer data);
    private native long nativeAudioEpoch();
    private native long nativePrepareAudio();
    private native void nativeSceneAudioFlushed();
    private native long nativeFrameCounter();
    private native int nativeRenderPcm(short[] pcm,int frames);
    private native void nativeWaitForGuestAudio(int timeoutMs);
    private native String nativeStats();
    private native boolean nativeReady();
    private native boolean nativeOpenAAudio();
    private native void nativeSetAAudioActive(boolean active);
    private native void nativeCloseAAudio();
    private native String nativeAAudioStats();

    private static final int W=346,H=776,WRITE_LOG=431;
    private final AtomicBoolean stopping=new AtomicBoolean(false);
    private FrameView frame;
    private AudioTrack audioTrack;
    private volatile boolean usingAAudio=false;
    private Thread audioThread,initThread,audioInitThread;
    private final Handler bootHandler=new Handler(Looper.getMainLooper());
    private long bootStartedAt;
    private TextView title;
    private volatile boolean loaded=false;
    private volatile boolean bootComplete=false;
    private volatile boolean audioActive=true;
    private final java.util.concurrent.atomic.AtomicLong audioWriteMs = new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicLong audioWriteMaxMs = new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicLong audioWrittenFrames = new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicLong audioWriteFailures = new java.util.concurrent.atomic.AtomicLong();

    private void immersive(){
      getWindow().getDecorView().setSystemUiVisibility(
        View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|
        View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|
        View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
      getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
    @Override public void onCreate(Bundle savedInstanceState){
      super.onCreate(savedInstanceState);immersive();
      FrameLayout layout=new FrameLayout(this);layout.setBackgroundColor(Color.BLACK);
      frame=new FrameView(this);layout.addView(frame,new FrameLayout.LayoutParams(-1,-1));
      title=new TextView(this);title.setText("Music World · iniciando...");title.setTextColor(0xffccdef7);
      title.setTextSize(13);title.setPadding(16,24,16,16);title.setGravity(Gravity.CENTER_HORIZONTAL);
      FrameLayout.LayoutParams titleParams=new FrameLayout.LayoutParams(-1,-2,Gravity.TOP);layout.addView(title,titleParams);
      // Sin botones de laboratorio: el juego abre directamente.
      // En caso de fallo se puede guardar el registro manteniendo pulsado el texto.
      title.setOnLongClickListener(v -> {saveLog();return true;});
      setContentView(layout);
      bootStartedAt=SystemClock.uptimeMillis();
      // Status is queried through atomics, never through the WASM VM.
      final Runnable[] progress={null};
      progress[0]=()->{
        if(stopping.get()||loaded||bootComplete)return;
        String phase;
        try{phase=nativeBootStatus();}catch(Throwable t){phase="motor nativo";}
        long seconds=(SystemClock.uptimeMillis()-bootStartedAt)/1000;
        title.setText("Music World · " + phase + " (" + seconds + " s)" +
           (seconds>=30?"\nSi tarda demasiado, mantén pulsado para exportar el registro.":""));
        bootHandler.postDelayed(progress[0],1000);
      };
      bootHandler.postDelayed(progress[0],1000);
      initThread=new Thread(()->{
        boolean ok=false;
        try {
          ok=nativeInit(getAssets());
          if(ok && !stopping.get()) {
            nativeStart();
            // FIRST show real framebuffer from native ARM64 C core. Never wait for AAudio or SF2.
            loaded=true;
            runOnUiThread(()->{title.setVisibility(View.GONE);frame.requestRender();});
            audioInitThread=new Thread(()->{
              try {
                if(nativeInitAudio(getAssets()) && !stopping.get())startSound();
              }catch(Throwable t){android.util.Log.e("BL40Native","Audio diferido falló; video activo",t);}
            },"BL40-Audio-Init");
            audioInitThread.start();
          }
        } catch(Throwable t) {
          android.util.Log.e("BL40Native","Error arrancando juego",t);
        }
        bootComplete=true;
        if(!loaded && !stopping.get())runOnUiThread(()->{
          title.setText("Error al iniciar Music World. Mantén pulsado para guardar el registro.");
          Toast.makeText(this,"Error de arranque; revisa LOG",Toast.LENGTH_LONG).show();
        });
      },"BL40-Init");
      initThread.start();
    }
    private int dp(float v){return (int)(v*getResources().getDisplayMetrics().density+.5f);}
    private void startSound(){
      // AAudio uses a high-priority callback that never touches guest core, JNI, or TSF.
      // Production/mixing happens on a separate native thread; AudioTrack is only fallback.
      if(Build.VERSION.SDK_INT>=26){
        try { if(nativeOpenAAudio()){
           usingAAudio=true;
           if(!audioActive)nativeSetAAudioActive(false);
           return;
         } }
        catch(Throwable e){android.util.Log.w("BL40Native","AAudio no disponible; AudioTrack fallback",e);}
      }
      int min=AudioTrack.getMinBufferSize(48000,AudioFormat.CHANNEL_OUT_STEREO,AudioFormat.ENCODING_PCM_16BIT);
      if(min<0)min=8192;
      AudioAttributes attrs=new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build();
      AudioFormat format=new AudioFormat.Builder().setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build();
      try{
       audioTrack=new AudioTrack.Builder()
          .setAudioAttributes(attrs).setAudioFormat(format)
          .setTransferMode(AudioTrack.MODE_STREAM)
          .setBufferSizeInBytes(Math.max(min,8192))
          .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
          .build();
       if(audioTrack.getState()!=AudioTrack.STATE_INITIALIZED)throw new IllegalStateException("AudioTrack sin inicializar");
       // Request a small effective buffer; Android chooses the safe supported size.
       final int requestedAudioFrames=2048;
       int selectedFrames=audioTrack.setBufferSizeInFrames(requestedAudioFrames);
       android.util.Log.i("BL40Native","AudioTrack requested="+requestedAudioFrames+" selected="+selectedFrames+
          " performance="+audioTrack.getPerformanceMode());
       audioTrack.setVolume(1.0f);
       audioTrack.play();
       audioThread=new Thread(()->{
        // BL40_AUDIO_PRIORITY_V1_3: kernel priority for PCM, not guest clock scaling.
        try{android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO);}
        catch(Exception ignored){}
        // Exactly one 10-ms guest tick at 48 kHz per call, not two JNI
        // renders for each tick. AudioTrack still receives small chunks.
        short[] samples=new short[480*2];
        long lastAudioEpoch=nativeAudioEpoch();
        int lastUnderruns=audioTrack.getUnderrunCount();
        int packets=0;
        boolean workerPaused=false;
        while(!stopping.get()){
         if(!audioActive){
          if(!workerPaused){
           try{audioTrack.pause();audioTrack.flush();}catch(Exception ex){android.util.Log.w("BL40Native","Pause audio worker",ex);}
           workerPaused=true;
          }
          try{Thread.sleep(8);}catch(InterruptedException ignored){}
          continue;
         }
         if(workerPaused){
          try{audioTrack.play();}catch(Exception ex){android.util.Log.w("BL40Native","Resume audio worker",ex);}
          workerPaused=false;
         }
         // Drop PCM already queued for the previous menu scene.
         // Only this dedicated audio thread controls flush/play during playback.
         long currentAudioEpoch=nativePrepareAudio();
         if(currentAudioEpoch!=lastAudioEpoch){
          try{
           audioTrack.pause();audioTrack.flush();audioTrack.play();
           nativeSceneAudioFlushed();
          }catch(Exception e){android.util.Log.w("BL40Native","Flush cambio de escena",e);}
          lastAudioEpoch=currentAudioEpoch;
         }
         // Do not inject artificial zero samples when the guest CPU has not
         // yet completed the corresponding virtual frames. The DAC may still
         // underrun during heavy guest ticks, but the PCM timeline stays true.
         int produced=nativeRenderPcm(samples,480);
         if(produced<=0){
          // Sleep until a real guest tick publishes PCM, not a 2-ms poll.
          nativeWaitForGuestAudio(15);
          continue;
         }
         // The PCM renderer can replace music mid-call. Flush obsolete
         // queued audio BEFORE enqueuing the freshly rendered samples.
         long afterRenderEpoch=nativeAudioEpoch();
         if(afterRenderEpoch!=lastAudioEpoch){
          try{
           audioTrack.pause();audioTrack.flush();audioTrack.play();
           nativeSceneAudioFlushed();
          }catch(Exception e){android.util.Log.w("BL40Native","Flush tras render MIDI",e);}
          lastAudioEpoch=afterRenderEpoch;
         }
         final int availableSamples=produced*2;
         int offset=0;
         while(offset<availableSamples&&!stopping.get()&&audioActive){
          long startNs=System.nanoTime();
          int count=audioTrack.write(samples,offset,availableSamples-offset,AudioTrack.WRITE_BLOCKING);
          long elapsedMs=Math.max(0L,(System.nanoTime()-startNs)/1000000L);
          audioWriteMs.addAndGet(elapsedMs);
          audioWriteMaxMs.accumulateAndGet(elapsedMs,Math::max);
          if(count<=0){audioWriteFailures.incrementAndGet();break;}
          audioWrittenFrames.addAndGet(count/2L);
          offset+=count;
         }
         if((++packets%96)==0){
          int underruns=audioTrack.getUnderrunCount();
          if(underruns>lastUnderruns){
           int cur=audioTrack.getBufferSizeInFrames();
           int adjusted=audioTrack.setBufferSizeInFrames(Math.min(8192,cur+512));
           android.util.Log.w("BL40Native","Audio underrun: buffer="+cur+" -> "+adjusted);
          }
          lastUnderruns=underruns;
         }
        }
       },"BL40-AudioTrack");audioThread.setPriority(Thread.MAX_PRIORITY);audioThread.start();
      }catch(Exception e){runOnUiThread(()->Toast.makeText(this,"Audio nativo: "+e.getMessage(),Toast.LENGTH_LONG).show());}
    }
    // Datos del telefono para interpretar las mediciones (modelo, SoC, hilos, estado termico).
    private String deviceInfoJson(){
      try{
        StringBuilder b=new StringBuilder("{");
        b.append("\"manufacturer\": \"").append(esc(Build.MANUFACTURER)).append("\"");
        b.append(", \"model\": \"").append(esc(Build.MODEL)).append("\"");
        b.append(", \"hardware\": \"").append(esc(Build.HARDWARE)).append("\"");
        if(Build.VERSION.SDK_INT>=31)b.append(", \"soc\": \"").append(esc(Build.SOC_MANUFACTURER+" "+Build.SOC_MODEL)).append("\"");
        b.append(", \"sdk\": ").append(Build.VERSION.SDK_INT);
        b.append(", \"abis\": \"").append(esc(java.util.Arrays.toString(Build.SUPPORTED_ABIS))).append("\"");
        b.append(", \"cpuThreads\": ").append(Runtime.getRuntime().availableProcessors());
        if(Build.VERSION.SDK_INT>=29){
          android.os.PowerManager pm=(android.os.PowerManager)getSystemService(Context.POWER_SERVICE);
          if(pm!=null)b.append(", \"thermalStatus\": ").append(pm.getCurrentThermalStatus());
        }
        return b.append("}").toString();
      }catch(Throwable t){return "{\"error\": \""+esc(String.valueOf(t))+"\"}";}
    }
    private static String esc(String v){return v==null?"":v.replace("\\","\\\\").replace("\"","'");}
    private void saveLog(){
      Intent i=new Intent(Intent.ACTION_CREATE_DOCUMENT);i.setType("application/json");i.addCategory(Intent.CATEGORY_OPENABLE);
      i.putExtra(Intent.EXTRA_TITLE,"MusicWorld_BL40_Android_Nativo_LOG.json");
      startActivityForResult(i,WRITE_LOG);
    }
    @Override protected void onActivityResult(int request,int result,Intent intent){
      super.onActivityResult(request,result,intent);
      if(request==WRITE_LOG&&result==RESULT_OK&&intent!=null&&intent.getData()!=null){
       Uri uri=intent.getData();
       try(OutputStream stream=getContentResolver().openOutputStream(uri)){
        if(stream==null)throw new IllegalStateException("No se pudo crear JSON");
        String stats=nativeStats();
        final int end=stats.lastIndexOf('}');
        if(end>=0){
         final int underruns=audioTrack!=null?audioTrack.getUnderrunCount():-1;
         final int bufferFrames=audioTrack!=null?audioTrack.getBufferSizeInFrames():-1;
         stats=stats.substring(0,end).trim()+",\n  \"androidAudioUnderruns\": "+underruns+
                 ",\n  \"androidAudioBufferFrames\": "+bufferFrames+
                 ",\n  \"androidAudioWriteWallMs\": "+audioWriteMs.get()+
                 ",\n  \"androidAudioWorstWriteMs\": "+audioWriteMaxMs.get()+
                 ",\n  \"androidAudioFramesWritten\": "+audioWrittenFrames.get()+
                 ",\n  \"androidAudioWriteFailures\": "+audioWriteFailures.get()+
                 ",\n  \"videoBackend\": \"OpenGL ES 2.0 RGB565\""+
                 ",\n  \"device\": "+deviceInfoJson()+
                 ",\n  \"nativeAAudio\": "+nativeAAudioStats()+"\n}";
        }
        stream.write(stats.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        Toast.makeText(this,"LOG guardado",Toast.LENGTH_SHORT).show();
       }catch(Exception e){Toast.makeText(this,"Error LOG: "+e.getMessage(),Toast.LENGTH_LONG).show();}
      }
    }
    @Override protected void onPause(){super.onPause();audioActive=false;if(usingAAudio)nativeSetAAudioActive(false);frame.onPause();if(loaded)nativePause(true);}
    @Override protected void onResume(){super.onResume();immersive();frame.onResume();if(loaded)nativePause(false);audioActive=true;if(usingAAudio)nativeSetAAudioActive(true);}
    @Override protected void onDestroy(){
      stopping.set(true);
      bootHandler.removeCallbacksAndMessages(null);
      frame.shutdown();
      if(usingAAudio){nativeCloseAAudio();usingAAudio=false;}
      if(audioTrack!=null){try{audioTrack.stop();}catch(Exception ignored){};}
      if(audioThread!=null)try{audioThread.join(1500);}catch(InterruptedException ignored){}
      if(loaded)nativeStop();
      if(audioTrack!=null)audioTrack.release();
      super.onDestroy();
    }
    // GPU textures are uploaded only when the guest presentation counter changes.
    // The request thread never calls GLES; all GL calls stay on GLSurfaceView's EGL thread.
    private class FrameView extends GLSurfaceView implements GLSurfaceView.Renderer {
      private final ByteBuffer rgb=ByteBuffer.allocateDirect(W*H*2).order(ByteOrder.nativeOrder());
      private final FloatBuffer quad=ByteBuffer.allocateDirect(4*4*Float.BYTES)
                  .order(ByteOrder.nativeOrder()).asFloatBuffer();
      private Thread watchThread;
      private final AtomicBoolean watching=new AtomicBoolean(true);
      private long previousFrame=-1,submittedFrame=-1;
      private int texture,program,vertexLocation,uvLocation,samplerLocation;
      private int surfaceW=1,surfaceH=1;
      private final float[] vertices={-1,1, 0,0,  -1,-1, 0,1,  1,1, 1,0,  1,-1, 1,1};
      FrameView(Context context){
        super(context);
        setEGLContextClientVersion(2);
        setPreserveEGLContextOnPause(true);
        setRenderer(this);
        setRenderMode(GLSurfaceView.RENDERMODE_WHEN_DIRTY);
        quad.put(vertices);quad.position(0);
        watchThread=new Thread(()->{
          while(watching.get()&&!stopping.get()){
            if(loaded){
              final long counter=nativeFrameCounter();
              if(counter!=submittedFrame){submittedFrame=counter;requestRender();}
            }
            try{Thread.sleep(9);}catch(InterruptedException ignored){}
          }
        },"BL40-GPU-Presenter");
        watchThread.start();
      }
      private int compileShader(int type,String shader){
        int id=GLES20.glCreateShader(type);GLES20.glShaderSource(id,shader);GLES20.glCompileShader(id);
        int[] result=new int[1];GLES20.glGetShaderiv(id,GLES20.GL_COMPILE_STATUS,result,0);
        if(result[0]==0){String error=GLES20.glGetShaderInfoLog(id);GLES20.glDeleteShader(id);throw new IllegalStateException("GL shader: "+error);}
        return id;
      }
      @Override public void onSurfaceCreated(GL10 gl,EGLConfig config){
        final String vs="attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV; void main(){ gl_Position=vec4(aPos,0.0,1.0); vUV=aUV; }";
        final String fs="precision mediump float; varying vec2 vUV; uniform sampler2D uFrame; void main(){ gl_FragColor=texture2D(uFrame,vUV); }";
        int v=compileShader(GLES20.GL_VERTEX_SHADER,vs),f=compileShader(GLES20.GL_FRAGMENT_SHADER,fs);
        program=GLES20.glCreateProgram();GLES20.glAttachShader(program,v);GLES20.glAttachShader(program,f);GLES20.glLinkProgram(program);
        int[] linked=new int[1];GLES20.glGetProgramiv(program,GLES20.GL_LINK_STATUS,linked,0);
        GLES20.glDeleteShader(v);GLES20.glDeleteShader(f);
        if(linked[0]==0)throw new IllegalStateException("GL link: "+GLES20.glGetProgramInfoLog(program));
        vertexLocation=GLES20.glGetAttribLocation(program,"aPos");
        uvLocation=GLES20.glGetAttribLocation(program,"aUV");
        samplerLocation=GLES20.glGetUniformLocation(program,"uFrame");
        int[] textures=new int[1];GLES20.glGenTextures(1,textures,0);texture=textures[0];
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,texture);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_MIN_FILTER,GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_MAG_FILTER,GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_WRAP_S,GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_WRAP_T,GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT,1);
        GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D,0,GLES20.GL_RGB,W,H,0,
                              GLES20.GL_RGB,GLES20.GL_UNSIGNED_SHORT_5_6_5,null);
        GLES20.glClearColor(0,0,0,1);
        previousFrame=-1;
      }
      @Override public void onSurfaceChanged(GL10 gl,int width,int height){surfaceW=Math.max(1,width);surfaceH=Math.max(1,height);}
      @Override public void onDrawFrame(GL10 gl){
        GLES20.glViewport(0,0,surfaceW,surfaceH);GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        if(loaded){
          long counter=nativeFrameCounter();
          if(counter!=previousFrame && nativeCopyFrame(rgb)){
            rgb.position(0);GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,texture);
            GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D,0,0,0,W,H,GLES20.GL_RGB,
                                   GLES20.GL_UNSIGNED_SHORT_5_6_5,rgb);
            previousFrame=counter;
          }
        }
        float scale=Math.min((float)surfaceW/W,(float)surfaceH/H);
        int rw=Math.max(1,Math.round(W*scale)),rh=Math.max(1,Math.round(H*scale));
        GLES20.glViewport((surfaceW-rw)/2,(surfaceH-rh)/2,rw,rh);
        GLES20.glUseProgram(program);GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,texture);
        GLES20.glUniform1i(samplerLocation,0);
        quad.position(0);GLES20.glEnableVertexAttribArray(vertexLocation);
        GLES20.glVertexAttribPointer(vertexLocation,2,GLES20.GL_FLOAT,false,16,quad);
        quad.position(2);GLES20.glEnableVertexAttribArray(uvLocation);
        GLES20.glVertexAttribPointer(uvLocation,2,GLES20.GL_FLOAT,false,16,quad);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP,0,4);
        GLES20.glDisableVertexAttribArray(vertexLocation);GLES20.glDisableVertexAttribArray(uvLocation);
      }
      void shutdown(){perfGestureHandler.removeCallbacks(perfGesture);watching.set(false);if(watchThread!=null)try{watchThread.join(500);}catch(InterruptedException ignored){} }
      private int activePointer=-1,lastTouchX=-1,lastTouchY=-1;
      // Mantén tres dedos 1.8 s para exportar JSON sin un menú debug permanente.
      private final Handler perfGestureHandler=new Handler(Looper.getMainLooper());
      private final Runnable perfGesture=()->{
        if(!stopping.get() && loaded){
          if(activePointer>=0)nativeTouch(2,Math.max(0,lastTouchX),Math.max(0,lastTouchY));
          activePointer=-1;
          saveLog();
        }
      };
      private Rect liveViewport(){
       final int cw=Math.max(1,getWidth()),ch=Math.max(1,getHeight());
       final float scale=Math.min((float)cw/W,(float)ch/H);
       final int rw=Math.max(1,Math.round(W*scale)),rh=Math.max(1,Math.round(H*scale));
       return new Rect((cw-rw)/2,(ch-rh)/2,(cw-rw)/2+rw,(ch-rh)/2+rh);
      }
      private int[] coords(float x,float y){
       final Rect r=liveViewport();
       final int w=Math.max(1,r.width()),h=Math.max(1,r.height());
       return new int[]{Math.min(W-1,Math.max(0,(int)Math.floor((x-r.left)*W/w))),
                        Math.min(H-1,Math.max(0,(int)Math.floor((y-r.top)*H/h)))};
      }
      private void moveTo(int x,int y){
       if(lastTouchX!=x || lastTouchY!=y){nativeTouch(1,x,y);lastTouchX=x;lastTouchY=y;}
      }
      @Override public boolean onTouchEvent(MotionEvent e){
       if(!loaded)return true;
       final int action=e.getActionMasked();
       if(action==MotionEvent.ACTION_POINTER_DOWN && e.getPointerCount()>=3){
         perfGestureHandler.removeCallbacks(perfGesture);
         perfGestureHandler.postDelayed(perfGesture,1800);
         return true;
       }
       if(action==MotionEvent.ACTION_POINTER_UP || action==MotionEvent.ACTION_UP ||
          action==MotionEvent.ACTION_CANCEL)perfGestureHandler.removeCallbacks(perfGesture);
       if(action==MotionEvent.ACTION_DOWN){
        activePointer=e.getPointerId(0);
        int[] p=coords(e.getX(0),e.getY(0));
        lastTouchX=p[0];lastTouchY=p[1];
        nativeTouch(0,p[0],p[1]);return true;
       }
       if(activePointer<0)return true;
       if(e.getPointerCount()==0){
        if(action==MotionEvent.ACTION_CANCEL){
         nativeTouch(1,Math.max(0,lastTouchX),Math.max(0,lastTouchY));
         nativeTouch(2,Math.max(0,lastTouchX),Math.max(0,lastTouchY));
         activePointer=-1;lastTouchX=-1;lastTouchY=-1;
        }
        return true;
       }
       int idx=e.findPointerIndex(activePointer);
       if(idx<0)idx=0;
       if(action==MotionEvent.ACTION_MOVE){
        // Only recent historical samples; avoid flooding JNI on 120 Hz screens.
        for(int h=0;h<e.getHistorySize();h++){
         int[] p=coords(e.getHistoricalX(idx,h),e.getHistoricalY(idx,h));
         moveTo(p[0],p[1]);
        }
        int[] p=coords(e.getX(idx),e.getY(idx));moveTo(p[0],p[1]);
        return true;
       }
       if(action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_CANCEL ||
          (action==MotionEvent.ACTION_POINTER_UP&&e.getPointerId(e.getActionIndex())==activePointer)){
        int[] p=coords(e.getX(idx),e.getY(idx));
        // Exactly one terminal MOVE: the previous implementation called
        // moveTo() AND dispatched the same MOVE again before UP.
        nativeTouch(1,p[0],p[1]); // terminal MOVE, required by HLE menu handlers
        nativeTouch(2,p[0],p[1]);
        activePointer=-1;lastTouchX=-1;lastTouchY=-1;
        return true;
       }
       return true;
      }
    }
}
