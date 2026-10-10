#include "PSZAnalog.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>

using namespace PSZMix::Analog;
constexpr uint32_t Player = 0x021A20B0, Manager = 0x0224B880, Camera = 0x022512C0;

static void Put(std::vector<uint8_t>& ram, uint32_t a, uint32_t value, unsigned n = 4)
{
    for (unsigned i = 0; i < n; ++i) ram.at(a - 0x02000000 + i) = uint8_t(value >> (8*i));
}
static uint16_t Get16(const std::vector<uint8_t>& ram, uint32_t a)
{
    return ram.at(a - 0x02000000) | (ram.at(a - 0x02000000 + 1) << 8);
}
static std::vector<uint8_t> Fixture()
{
    std::vector<uint8_t> ram(0x400000);
    Put(ram, HookPC, 0xB0AAB570);
    Put(ram, HookPC + 4, 0x48C51C05);
    Put(ram, 0x02108D04, Player);
    Put(ram, Player + 0x240, 3);
    Put(ram, 0x0211B330, 0x40, 2);
    Put(ram, 0x02108C84, Manager);
    Put(ram, Manager + 0x40, Camera);
    Put(ram, Camera, 0x020F99A8);
    Put(ram, Camera + 0x82, 20030, 2);
    Put(ram, Player + 0xC0, 9876, 2);
    Put(ram, Player + 0xC2, 52798, 2);
    Put(ram, Player + 0x100, 835);
    return ram;
}
int main()
{
    // Offsets measured from the original camera yaw for all eight directions.
    struct Direction { float x, y; uint16_t offset; };
    const Direction directions[] = {{0,-1,32768},{1,-1,24576},{1,0,16384},
        {1,1,8192},{0,1,0},{-1,1,57344},{-1,0,49152},{-1,-1,40960}};
    for (uint16_t camera : {uint16_t(0), uint16_t(20030), uint16_t(65530)})
        for (const auto& d : directions)
        {
            uint16_t yaw = 123;
            assert(Heading(Pack(d.x,d.y),camera,yaw));
            assert(yaw == uint16_t(camera+d.offset));
        }
    uint16_t yaw = 123;
    assert(!Heading(Pack(0,0),0,yaw) && yaw == 123);
    assert(!Heading(Pack(.2f,.2f),0,yaw));
    assert(!Heading(Pack(std::numeric_limits<float>::quiet_NaN(),0),0,yaw));
    assert(Pack(3,-3) == Pack(1,-1));
    assert(Heading(Pack(.6f,-.8f),0,yaw));
    assert(yaw >= 26056 && yaw <= 26058); // ~36.87 degrees clockwise from screen Up
    assert(yaw % 8192 != 0);

    auto ram = Fixture();
    const auto untouched = ram;
    assert(Apply(ram.data(),ram.size(),Player,Pack(.6f,-.8f)));
    assert(Get16(ram,Player+0xC2) >= 46086 && Get16(ram,Player+0xC2) <= 46088);
    // Facing, speed, positions and every other byte must remain original.
    Put(ram,Player+0xC2,Get16(untouched,Player+0xC2),2);
    assert(ram == untouched);
    // A new camera yaw must take effect immediately, without a fixed address cache.
    Put(ram,Camera+0x82,65530,2);
    assert(Apply(ram.data(),ram.size(),Player,Pack(1,0)));
    assert(Get16(ram,Player+0xC2) == 16378);

    auto reject = [](std::vector<uint8_t> r, uint32_t player = Player, uint32_t stick = Pack(1,0)) {
        const auto old = r;
        assert(!Apply(r.data(),r.size(),player,stick));
        assert(old == r);
    };
    for (unsigned state = 0; state < 32; ++state)
        if (state != 3) { auto r=Fixture(); Put(r,Player+0x240,state); reject(r); }
    for (const auto& edit : std::vector<std::pair<uint32_t,uint32_t>>{
        {0x02108D04,0}, {Player+0x250,1}, {Player+0x250,8},
        {Player+0xF8,1}, {Player+0xF8,0x400}, {Player+0x1FC,2},
        {0x0211B330,0}, {HookPC,0}, {HookPC+4,0},
        {0x02108C84,0}, {0x02108C84,0x023FFFFC},
        {Manager+0x64,4}, {Manager+0x40,0}, {Manager+0x40,0x023FFFFC}, {Camera,0}})
    { auto r=Fixture(); Put(r,edit.first,edit.second); reject(r); }
    reject(Fixture(),0x021A20B1);
    reject(Fixture(),0x023FFFFC);
    reject(Fixture(),Player,Pack(0,0));
    auto small = Fixture(); small.resize(1024); reject(small);
    assert(!Apply(nullptr,0x400000,Player,Pack(1,0)));
    std::puts("analog: eight-way oracle, arbitrary headings, camera wrap, guards and write isolation passed");
}
