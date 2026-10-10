#ifndef PSZ_ANALOG_H
#define PSZ_ANALOG_H

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace PSZMix::Analog
{
// USA C24E, ARM9 THUMB. Called after input decoding, before facing/translation.
// Keep this a block boundary in the JIT, even with branch optimizations enabled.
constexpr uint32_t HookPC = 0x0203CC8C;
constexpr float Deadzone = 0.35f;

// One atomic word carries a coherent pair across the UI/emulation threads.
inline uint32_t Pack(float x, float y)
{
    auto axis = [](float v) -> uint16_t {
        if (!std::isfinite(v)) return 0;
        v = v < -1.f ? -1.f : v > 1.f ? 1.f : v;
        return static_cast<uint16_t>(static_cast<int16_t>(v * 32767.f));
    };
    return uint32_t(axis(x)) | (uint32_t(axis(y)) << 16);
}

inline bool Heading(uint32_t stick, uint16_t cameraYaw, uint16_t& result)
{
    const float x = static_cast<int16_t>(stick & 0xFFFF) / 32767.f;
    const float y = static_cast<int16_t>(stick >> 16) / 32767.f;
    if (x * x + y * y < Deadzone * Deadzone) return false;
    // x right, y down. Original Up/Right/Down/Left offsets from the camera
    // are respectively 32768/16384/0/49152 (the atan2 term is SUBTRACTED).
    const int angle = static_cast<int>(std::atan2(x, -y) * (65536.0 / 6.283185307179586));
    result = static_cast<uint16_t>(int(cameraYaw) + 32768 - angle);
    return true;
}

// Bounds-checked raw RAM adapter, independent of either melonDS core's API.
// Caller checks the cartridge ID. Never use masked reads to validate pointers:
// a corrupt pointer must not alias another location in main RAM.
inline bool Apply(uint8_t* ram, size_t size, uint32_t player, uint32_t stick)
{
    if (!ram || size < 0x400000 || size > 0x1000000) return false;
    auto valid = [size](uint32_t address, size_t bytes) {
        return address >= 0x02000000 && bytes <= size &&
               size_t(address - 0x02000000) <= size - bytes;
    };
    auto read = [ram](uint32_t address, unsigned bytes) {
        uint32_t result = 0;
        for (unsigned i = 0; i < bytes; ++i)
            result |= uint32_t(ram[address - 0x02000000 + i]) << (i * 8);
        return result;
    };
    if (!valid(player, 0x254) || (player & 3) ||
        read(0x02108D04, 4) != player || read(player + 0x240, 4) != 3 ||
        (read(player + 0x250, 4) & 9) != 0 ||
        (read(player + 0xF8, 4) & 0x401) != 0 || read(player + 0x1FC, 1) == 2 ||
        !(read(0x0211B330, 2) & 0xF0)) return false;
    // Reject other code revisions, modified entry points and unloaded ARM9.
    if (read(HookPC, 4) != 0xB0AAB570 || read(HookPC + 4, 4) != 0x48C51C05)
        return false;
    const uint32_t manager = read(0x02108C84, 4);
    if (!valid(manager, 0x68) || (manager & 3)) return false;
    const uint32_t index = read(manager + 0x64, 4);
    if (index >= 4) return false;
    const uint32_t camera = read(manager + 0x40 + index * 4, 4);
    if (!valid(camera, 0x84) || (camera & 3) || read(camera, 4) != 0x020F99A8)
        return false;
    uint16_t target;
    if (!Heading(stick, static_cast<uint16_t>(read(camera + 0x82, 2)), target))
        return false;
    ram[player - 0x02000000 + 0xC2] = static_cast<uint8_t>(target);
    ram[player - 0x02000000 + 0xC3] = static_cast<uint8_t>(target >> 8);
    return true;
}
}
#endif
