// svr2011 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <rex/perf/counter.h>

class Svr2011App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Svr2011App>(new Svr2011App(ctx, "svr2011",
        PPCImageConfig));
  }

  void OnPostSetup() override {
    // v0.10.0 defines perf_log_csv and writes counters at guest swaps, but
    // does not open the requested file. Initialize it before guest threads run.
    const auto path = rex::cvar::GetFlagByName("perf_log_csv");
    if (!path.empty()) {
      rex::perf::SetCsvLogPath(path);
    }
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
