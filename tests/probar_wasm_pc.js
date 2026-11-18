// Smoke test uses Node's real WASM engine and the actual 145 registered LG resources.
// It does not claim to reproduce Android/WAMR behavior.
const fs=require('fs');
const path=require('path');
const assert=require('assert');
const dir=path.join(__dirname,'../app/src/main/assets');
const runtime=WebAssembly.instantiate(fs.readFileSync(path.join(dir,'bl40_core.wasm'))).then(({instance})=>{
 const w=instance.exports;
 const pack=fs.readFileSync(path.join(dir,'bl40_assets.pack'));
 const count=pack.readUInt32LE(0);
 assert.equal(count,145);
 let off=4;
 const put=(bytes,term)=>{
  const p=w.web_alloc(bytes.length+(term?1:0));
  assert(p>0,'guest alloc');
  new Uint8Array(w.memory.buffer,p,bytes.length).set(bytes);
  if(term)new Uint8Array(w.memory.buffer,p+bytes.length,1)[0]=0;
  return p;
 };
 for(let i=0;i<count;i++){
  const len=pack.readUInt16LE(off);off+=2;
  const name=pack.subarray(off,off+len);off+=len;
  const bytesLen=pack.readUInt32LE(off);off+=4;
  const bytes=pack.subarray(off,off+bytesLen);off+=bytesLen;
  assert.equal(w.bl40_register_asset(put(name,true),put(bytes,false),bytesLen),1);
 }
 assert.equal(off,pack.length);
 assert.equal(w.bl40_boot(),1);
 const first=w.bl40_presentation_count();
 for(let i=0;i<250;i++)assert.equal(w.bl40_tick(10),1);
 const last=w.bl40_presentation_count();
 assert(last>first,'no guest frames advanced');
 console.log(`OK: ${count} resources; boot=1; frames ${first} -> ${last}; native SF2 not required for video`);
});
runtime.catch(e=>{console.error(e);process.exitCode=1;});
