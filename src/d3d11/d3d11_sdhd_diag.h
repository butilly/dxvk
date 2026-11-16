#pragma once

#include <atomic>
#include "d3d11_include.h"
#include "../util/util_env.h"
#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk::sdhd {

  // Temporary, executable-gated API diagnostics. Counts describe API activity,
  // not visible pixels or GPU completion. Deferred calls are recordings only.
  enum Event { Draw, Indexed, Indirect, Dispatch, VS, PS, Execute, Finish,
    MissingVS, ColorDraw, DepthDraw, OtherDraw, StateChange, NullVS, NullPS, Count };
  inline constexpr const char* names[] = {
    "draw", "indexed", "indirect", "dispatch", "VS", "PS", "execute", "finish",
    "missing-vs", "color", "depth-only", "other", "state-change", "null-VS", "null-PS"
  };
  inline std::atomic<uint64_t> events[2][Count] = {};
  inline std::atomic<uint64_t> noVS[2] = {}, noPS[2] = {};
  inline std::atomic<uint64_t> frames = {}, samples = {}, csReached = {};
  inline std::atomic<uint64_t> shaderRequests = {}, shaderSuccess = {}, shaderFailed = {};
  inline std::atomic<uint32_t> budgets[2][Count] = {};

  inline bool enabled() {
    static const bool value = [] {
      auto exe = env::getExeName();
      for (auto& c : exe)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      return exe == "sdhdship.exe";
    }();
    return value;
  }

  inline uint64_t observe(bool deferred, Event event) {
    if (!enabled()) return 0;
    events[deferred][event].fetch_add(1, std::memory_order_relaxed);
    if (frames.load(std::memory_order_relaxed) > 18000)
      return 0;
    // Four samples per event/context type per 300 presents, with a process cap.
    auto& budget = budgets[deferred][event];
    if (budget.load(std::memory_order_relaxed) >= 4 ||
        budget.fetch_add(1, std::memory_order_relaxed) >= 4)
      return 0;
    auto id = samples.fetch_add(1, std::memory_order_relaxed) + 1;
    return id <= 1024 ? id : 0;
  }

  inline void commandList(bool deferred, Event event, const void* list, bool restore) {
    auto id = observe(deferred, event);
    if (id)
      Logger::info(str::format("SDHD-DIAG list id=", id, " event=", names[event],
        " deferred=", deferred, " list=", list, " restore=", restore));
  }

  inline void present(const void* swapchain, const void* backBuffer, HRESULT result) {
    if (!enabled()) return;
    auto frame = frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame > 18000 || (frame > 4 && frame % 300)) return;
    Logger::info(str::format("SDHD-DIAG present=", frame, " swapchain=", swapchain,
      " backBuffer=", backBuffer,
      " hr=", result, " sampledCsReached=", csReached.load(),
      " shaderRequests=", shaderRequests.load(), " shaderSuccess=", shaderSuccess.load(),
      " shaderFailed=", shaderFailed.load(),
      " scope=process-cumulative API-not-GPU-completion"));
    for (uint32_t d = 0; d < 2; d++) {
      Logger::info(str::format("SDHD-DIAG totals deferred=", d,
        " draw=", events[d][Draw].load(), " indexed=", events[d][Indexed].load(),
        " indirect=", events[d][Indirect].load(), " dispatch=", events[d][Dispatch].load(),
        " VS=", events[d][VS].load(), " PS=", events[d][PS].load(),
        " execute=", events[d][Execute].load(), " finish=", events[d][Finish].load(),
        " drawNoVS=", noVS[d].load(), " drawNoPS=", noPS[d].load(),
        " colorWithVS=", events[d][ColorDraw].load(),
        " depthWithVS=", events[d][DepthDraw].load(),
        " otherWithVS=", events[d][OtherDraw].load(),
        " stateChanges=", events[d][StateChange].load(),
        " nullVSSet=", events[d][NullVS].load(),
        " nullPSSet=", events[d][NullPS].load()));
      if (!(frame % 300))
        for (auto& budget : budgets[d]) budget.store(0, std::memory_order_relaxed);
    }
  }

}
