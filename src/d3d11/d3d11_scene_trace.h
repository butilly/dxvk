#pragma once

#include <atomic>
#include "../util/util_env.h"

namespace dxvk::sceneTrace {
  inline bool enabled() {
    static const bool active = env::getExeName() == "sdhdship.exe";
    return active;
  }

  inline std::atomic<uint64_t> immediate{0}, deferred{0}, lists{0};
  inline std::atomic<uint64_t> missingVs{0}, colorDraws{0}, presents{0};
  inline std::atomic<uintptr_t> lastCaller{0}, lastMissingVsCaller{0};

  inline void draw(bool recorded, bool hasVs, bool hasColor, void* caller) {
    if (!enabled())
      return;
    (recorded ? deferred : immediate).fetch_add(1, std::memory_order_relaxed);
    lastCaller.store(uintptr_t(caller), std::memory_order_relaxed);
    if (!hasVs) {
      missingVs.fetch_add(1, std::memory_order_relaxed);
      lastMissingVsCaller.store(uintptr_t(caller), std::memory_order_relaxed);
    }
    if (hasColor)
      colorDraws.fetch_add(1, std::memory_order_relaxed);
  }
}
