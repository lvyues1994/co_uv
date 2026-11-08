#pragma once
#include <cstddef>
#include <cstdint>
namespace uvco {
struct KcpSessionStats {
    std::uint64_t explicitFlushes=0,inputBatches=0,admissions=0,outputDrops=0;
    std::size_t maxExplicitFlushesPerDrive=0,queuedPackets=0,waitingSegments=0;
};
}
