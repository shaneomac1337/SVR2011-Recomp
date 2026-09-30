#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/ui/window.h>

#include "native/native_renderer.h"

class Svr2011App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Svr2011App>(new Svr2011App(ctx, "svr2011",
        PPCImageConfig));
  }

  void OnPostSetup() override {
    if (rex::cvar::GetFlagByName("svr_profile") == "true") {
      rex::perf::Profiler::Startup();
    }
    // v0.10.0 defines perf_log_csv and writes counters at guest swaps, but
    // does not open the requested file. Initialize it before guest threads run.
    const auto path = rex::cvar::GetFlagByName("perf_log_csv");
    if (!path.empty()) {
      rex::perf::SetCsvLogPath(path);
    }
    svr::native::Configure(runtime());
  }

  void OnShutdown() override { svr::native::Shutdown(); }

  void OnPreLaunchModule() override {
    // Hide the cursor over the game once the mouse has been still for a second.
    if (auto* game_window = window()) {
      game_window->SetCursorAutoHideDelayMs(1000);
      game_window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
    }
  }
};
