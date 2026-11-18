#pragma once
// Reusable sparse 32-bit guest-address-space bus for preliminary native A32 work.
// Not a Qualcomm MMU. Unmapped accesses are marked as faults, not silently added.
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
using u8=std::uint8_t; using u16=std::uint16_t; using u32=std::uint32_t; using u64=std::uint64_t;
struct GuestBus {
 static constexpr u32 PAGE = 4096;
 std::unordered_map<u32,std::array<u8,PAGE>> pages;
 bool fault=false;
 u32 fault_addr=0;
 void mapZero(u32 start,std::size_t count) {
  if(count==0)return;
  const u64 end=static_cast<u64>(start)+count;
  if(end>0x100000000ULL)throw std::runtime_error("VA overflow");
  for(u64 a=(start&~(PAGE-1u));a<end;a+=PAGE)pages.try_emplace(static_cast<u32>(a>>12));
 }
 bool has(u32 a) const{return pages.find(a>>12)!=pages.end();}
 void mapBytes(u32 addr,const std::vector<u8>& bytes){
  mapZero(addr,bytes.size());
  for(std::size_t i=0;i<bytes.size();++i)write8(static_cast<u32>(addr+i),bytes[i]);
 }
 u8 read8(u32 a) {auto it=pages.find(a>>12);if(it==pages.end()){fault=true;fault_addr=a;return 0;}return it->second[a&4095u];}
 u16 read16(u32 a) {return static_cast<u16>(read8(a)|u16(read8(a+1))<<8);}
 u32 read32(u32 a) {return u32(read16(a))|(u32(read16(a+2))<<16);}
 u64 read64(u32 a) {return u64(read32(a))|(u64(read32(a+4))<<32);}
 void write8(u32 a,u8 x) {auto it=pages.find(a>>12);if(it==pages.end()){fault=true;fault_addr=a;return;}it->second[a&4095u]=x;}
 void write16(u32 a,u16 x){write8(a,u8(x));write8(a+1,u8(x>>8));}
 void write32(u32 a,u32 x){write16(a,u16(x));write16(a+2,u16(x>>16));}
 void write64(u32 a,u64 x){write32(a,u32(x));write32(a+4,u32(x>>32));}
 std::optional<u32> readCode32(u32 a){
  for(unsigned i=0;i<4;i++)if(!has(a+i))return std::nullopt;
  return read32(a);
 }
 void clearFault(){fault=false;fault_addr=0;}
};
inline std::vector<u8> readBinary(const std::string& p){
 std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("no existe "+p);
 auto n=f.tellg();if(n<=0||n>5*1024*1024)throw std::runtime_error("tamaño inesperado");
 std::vector<u8> data(static_cast<std::size_t>(n));f.seekg(0);f.read((char*)data.data(),n);
 if(!f)throw std::runtime_error("error lectura");
 return data;
}
struct Scenario {u32 x,y,z;bool nullR0=false;};
struct Outcome {std::array<u32,16> r{};u32 pc=0;bool z=false;unsigned steps=0;};
static constexpr u32 ENTRY=0x0001FD2C;
static constexpr u32 INIT_DST=0x010C5F2C;
static constexpr u32 ARGS=0x02000000;
static constexpr u32 RET=0x0FF00000;
inline GuestBus setup(const std::vector<u8>& code,Scenario s){
 GuestBus bus;
 bus.mapBytes(0,code);
 bus.mapZero(INIT_DST&~4095u,4096);
 bus.mapZero(ARGS,4096);
 bus.write32(ARGS,s.x);bus.write32(ARGS+4,s.y);bus.write32(ARGS+8,s.z);
 for(unsigned i=0;i<3;i++)bus.write32(INIT_DST+4*i,0xDEADBEEF);
 bus.clearFault();return bus;
}
inline void assertPxoInitBytes(const std::vector<u8>& rom){
 const std::array<u8,20> expect{{0x00,0x28,0x06,0xD0,0x03,0x49,0x02,0x68,0x0A,0x60,0x42,0x68,0x4A,0x60,0x80,0x68,0x88,0x60,0x70,0x47}};
 if(rom.size()!=403120||ENTRY+expect.size()>rom.size())throw std::runtime_error("longitud RO distinta a PXO V10B");
 for(std::size_t i=0;i<expect.size();++i)
  if(rom[ENTRY+i]!=expect[i])throw std::runtime_error("instruccion PXO modificada "+std::to_string(i));
 if((u32(rom[0x1FD40])|(u32(rom[0x1FD41])<<8)|(u32(rom[0x1FD42])<<16)|(u32(rom[0x1FD43])<<24)) != INIT_DST)
  throw std::runtime_error("literal no coincide");
}
// Independent small Thumb-1 reference, only for the *actual 10 opcodes* of game init.
// It is NOT a full ARM interpreter or playable emulator.
inline Outcome runReference(GuestBus& bus, Scenario s){
 Outcome o;o.r.fill(0);o.r[0]=s.nullR0?0:ARGS;o.r[14]=RET|1u;o.pc=ENTRY;
 for(unsigned t=0;t<32;t++){
  if(o.pc==RET)return o;
  const u32 pc=o.pc;const u16 op=bus.read16(pc);o.steps++;
  if(op==0x2800){o.z=o.r[0]==0;o.pc+=2;}
  else if((op&0xFF00)==0xD000){const auto im=static_cast<std::int8_t>(op&0xff);o.pc= o.z ? (pc+4+static_cast<std::int32_t>(im)*2) : pc+2;}
  else if((op&0xF800)==0x4800){const u32 dest=(op>>8)&7u;const u32 va=((pc+4)&~3u)+((op&255u)<<2);o.r[dest]=bus.read32(va);o.pc+=2;}
  else if((op&0xF800)==0x6800){const u32 n=(op>>3)&7u,d=op&7u,imm=((op>>6)&31u)*4u;
     o.r[d]=bus.read32(o.r[n]+imm);o.pc+=2;}
  else if((op&0xF800)==0x6000){const u32 n=(op>>3)&7u,d=op&7u,imm=((op>>6)&31u)*4u;
     bus.write32(o.r[n]+imm,o.r[d]);o.pc+=2;}
  else if(op==0x4770){o.pc=o.r[14]&~1u;}
  else throw std::runtime_error("opcode no soportado en ensayo: "+std::to_string(op));
  if(bus.fault)throw std::runtime_error("referencia accedió VA sin mapear");
 }
 throw std::runtime_error("referencia no regresó en 32 instrucciones");
}
inline void checkScenario(GuestBus& bus, const Scenario& s,const Outcome& out){
 const auto wantR0=s.nullR0?0:s.z;
 if(out.pc!=RET||out.r[0]!=wantR0)throw std::runtime_error("retorno Thumb incorrecto");
 if(s.nullR0){
  if(out.steps!=3)throw std::runtime_error("camino null no hizo 3 instrucciones");
  for(int i=0;i<3;i++)if(bus.read32(INIT_DST+4*i)!=0xDEADBEEF)throw std::runtime_error("nullR0 escribió memoria");
 }else{
  if(out.steps!=10||out.r[1]!=INIT_DST||out.r[2]!=s.y)throw std::runtime_error("registros distintos de ABI PXO");
  if(bus.read32(INIT_DST)!=s.x||bus.read32(INIT_DST+4)!=s.y||bus.read32(INIT_DST+8)!=s.z)
   throw std::runtime_error("resultado ABI PXO incorrecto");
 }
 if(bus.fault)throw std::runtime_error("acceso fuera de memoria: "+std::to_string(bus.fault_addr));
}
