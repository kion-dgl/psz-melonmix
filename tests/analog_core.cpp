// Synthetic guest program: no game ROM or firmware files required.
#include "NDS.h"
#include "Args.h"
#include "NDSCart/CartCommon.h"
#include "PSZPlugin.h"
#include "PSZAnalog.h"
#include <QCoreApplication>
#include <cstdlib>
#include <cstdio>
#include <cstring>
using namespace melonDS;
constexpr u32 Player=0x021A20B0, Manager=0x0224B880, Camera=0x022512C0;
constexpr u32 Caller=0x02010000, Hook=PSZMix::Analog::HookPC;
static void Put(NDS& nds,u32 address,u32 value,unsigned bytes=4) {
    for(unsigned i=0;i<bytes;++i) nds.MainRAM[address-0x02000000+i]=u8(value>>(8*i));
}
static void Check(bool jit,bool branches,unsigned blockSize) {
    NDSArgs args;
    if(jit) args.JIT=JITArgs{blockSize,true,branches,true};
    else args.JIT=std::nullopt;
    auto nds=std::make_unique<NDS>(std::move(args));
    auto rom=std::make_unique<u8[]>(0x1000);
    std::memcpy(rom.get()+0xC,"C24E",4);
    nds->SetNDSCart(std::make_unique<NDSCart::CartCommon>(std::move(rom),0x1000,0,false,ROMListEntry{},NDSCart::Retail,nullptr));
    nds->Reset();
    NDS::Current=nds.get();
    Put(*nds,0x02108D04,Player); Put(*nds,Player+0x240,3);
    Put(*nds,0x0211B330,0x40,2); Put(*nds,0x02108C84,Manager);
    Put(*nds,Manager+0x40,Camera); Put(*nds,Camera,0x020F99A8);
    Put(*nds,Camera+0x82,20030,2);
    // Every loop decodes digital input again, calls the real hook entry, then
    // records the result in r3. Thus a cached block skipping the hook fails.
    Put(*nds,Caller,0x4620,2); Put(*nds,Caller+2,0x2100,2);
    Put(*nds,Caller+4,0x8011,2);
    const int delta=int(Hook)-int(Caller+6+4);
    Put(*nds,Caller+6,0xF000|((delta>>12)&0x7FF),2);
    Put(*nds,Caller+8,0xF800|((delta>>1)&0x7FF),2);
    Put(*nds,Caller+10,0x8813,2); Put(*nds,Caller+12,0xE7F8,2);
    Put(*nds,Hook,0xB0AAB570); Put(*nds,Hook+4,0x48C51C05);
    Put(*nds,Hook+8,0xBD70B02A); // restore stack and return
    auto& cpu=nds->ARM9;
    cpu.CPSR=0xFF; cpu.R[13]=0x023FF000; cpu.R[4]=Player; cpu.R[2]=Player+0xC2;
    cpu.JumpTo(Caller|1);
    auto run=[&](float x,float y,u32 low,u32 high) {
        PSZMix::SetMoveStick(x,y);
        nds->ARM9Target=nds->ARM9Timestamp+20000;
        if(jit) cpu.Execute<CPUExecuteMode::JIT>();
        else cpu.Execute<CPUExecuteMode::Interpreter>();
        if(cpu.R[3]<low || cpu.R[3]>high) {
            std::fprintf(stderr,"jit=%d branches=%d block=%u got=%u expected=%u..%u pc=%08x\n",jit,branches,blockSize,cpu.R[3],low,high,cpu.R[15]);
            std::abort();
        }
    };
    run(1,0,36414,36414); // cold compilation
    run(0,-1,52798,52798); // cached blocks, changed input
    run(.6f,-.8f,46086,46088);
    run(0,0,0,0);
    Put(*nds,Player+0x240,9); run(1,0,0,0);
    Put(*nds,Player+0x240,3); run(1,0,36414,36414);
    Put(*nds,0x0211B330,0,2); run(1,0,0,0);
    Put(*nds,0x0211B330,0x40,2);
    auto otherRom=std::make_unique<u8[]>(0x1000);
    std::memcpy(otherRom.get()+0xC,"C24J",4);
    nds->SetNDSCart(std::make_unique<NDSCart::CartCommon>(std::move(otherRom),0x1000,0,false,ROMListEntry{},NDSCart::Retail,nullptr));
    run(1,0,0,0);
    std::printf("analog core: %s branches=%d block=%u passed\n",jit?"JIT":"interpreter",branches,blockSize);
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    Check(false,false,32);
    for(bool branches : {false,true}) for(unsigned size : {8u,32u,128u}) Check(true,branches,size);
}
