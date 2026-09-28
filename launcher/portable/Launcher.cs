// Portable player launcher: first-run disc setup, settings, play and session logs.
// Everything lives next to this executable so the folder can be moved or zipped.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace Svr2011Launcher
{
    public sealed class Layout
    {
        public readonly string Root;
        public Layout(string root) { Root = Path.GetFullPath(root).TrimEnd('\\'); }
        public string GameDir { get { return Path.Combine(Root, "game"); } }
        public string GameExe { get { return Path.Combine(GameDir, "svr2011.exe"); } }
        public string Manifest { get { return Path.Combine(GameDir, "disc-manifest.tsv"); } }
        public string VersionFile { get { return Path.Combine(GameDir, "version.txt"); } }
        public string DataDir { get { return Path.Combine(Root, "gamedata"); } }
        public string StagingDir { get { return Path.Combine(Root, "gamedata.partial"); } }
        public string DataMarker { get { return Path.Combine(DataDir, ".svr2011-verified"); } }
        public string UserDir { get { return Path.Combine(Root, "userdata"); } }
        public string SaveDir { get { return Path.Combine(UserDir, "game"); } }
        public string SettingsPath { get { return Path.Combine(UserDir, "launcher-settings.json"); } }
        public string CacheDir { get { return Path.Combine(Root, "cache"); } }
        public string LogsDir { get { return Path.Combine(Root, "logs"); } }

        public bool GameDataReady { get { return File.Exists(DataMarker) && File.Exists(Path.Combine(DataDir, "default.xex")); } }

        public List<string> GameArguments(LauncherSettings settings, string runDir)
        {
            var arguments = new List<string>
            {
                "--gpu_plugin=xenos", "--vulkan_device=-1",
                "--game_data_root=" + DataDir, "--user_data_root=" + SaveDir, "--cache_root=" + CacheDir,
                "--log_file=" + Path.Combine(runDir, "runtime.log"), "--log_level=info", "--log_flush_interval=1",
            };
            arguments.AddRange(settings.DisplayArguments());
            if (settings.PerfCapture) arguments.Add("--perf_log_csv=" + Path.Combine(runDir, "perf.csv"));
            // FSI keeps EDRAM contents across frames the guest does not redraw (character-select flicker).
            arguments.Add("--render_target_path_vulkan=fsi");
            // The validated texture compatibility setting; see docs/performance.md.
            arguments.Add("--gpu_allow_invalid_fetch_constants=true");
            // Unseen shaders hold one frame while they build instead of stalling per pipeline.
            arguments.Add("--vulkan_async_skip_placeholder_pipelines=true");
            // Created attires are baked on the GPU and read by the game on the CPU.
            arguments.Add("--readback_resolve=burst");
            return arguments;
        }

        // Windows command-line quoting as parsed by CommandLineToArgvW.
        public static string Quote(string argument)
        {
            if (argument.Length > 0 && argument.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return argument;
            var text = new StringBuilder("\"");
            var slashes = 0;
            foreach (var c in argument)
            {
                if (c == '\\') { slashes++; continue; }
                if (c == '"') text.Append('\\', slashes * 2 + 1);
                else text.Append('\\', slashes);
                slashes = 0;
                text.Append(c);
            }
            text.Append('\\', slashes * 2);
            return text.Append('"').ToString();
        }
    }

    public static class Program
    {
        [STAThread]
        public static int Main(string[] args)
        {
            var layout = new Layout(AppDomain.CurrentDomain.BaseDirectory);
            if (args.Length > 0) return RunCheck(layout, args);
            var app = new Application();
            app.DispatcherUnhandledException += (sender, e) =>
            {
                try
                {
                    Directory.CreateDirectory(layout.LogsDir);
                    File.WriteAllText(Path.Combine(layout.LogsDir, "launcher-error.txt"), e.Exception.ToString());
                }
                catch (IOException) { }
                MessageBox.Show("The launcher hit an unexpected error:\n\n" + e.Exception.Message +
                    "\n\nDetails were saved to logs\\launcher-error.txt.", "SVR 2011", MessageBoxButton.OK, MessageBoxImage.Error);
                e.Handled = true;
            };
            return app.Run(new LauncherWindow(layout).Window);
        }

        // Non-interactive checks used by tests/test_portable.ps1. Results go to a file
        // because this is a windowed executable without a console.
        static int RunCheck(Layout layout, string[] args)
        {
            var output = args[args.Length - 1];
            try
            {
                string result;
                if (args[0] == "--print-arguments" && args.Length == 3)
                {
                    var settings = LauncherSettings.Load(args[1]);
                    result = string.Join("\n", layout.GameArguments(settings, Path.Combine(layout.LogsDir, "session")).ToArray());
                }
                else if (args[0] == "--quote" && args.Length == 3)
                {
                    result = Layout.Quote(args[1]);
                }
                else if (args[0] == "--verify-disc" && args.Length == 4)
                {
                    using (var disc = new DiscImage(args[1])) disc.MatchManifest(DiscImage.LoadManifest(args[2]), "default.xex");
                    result = "ok";
                }
                else if (args[0] == "--key-art" && args.Length == 5)
                {
                    File.WriteAllBytes(args[3], KeyArt.Read(args[1], args[2]));
                    result = "ok";
                }
                else if (args[0] == "--extract" && args.Length == 5)
                {
                    var manifest = DiscImage.LoadManifest(args[2]).ToDictionary(m => m.Path, StringComparer.OrdinalIgnoreCase);
                    using (var disc = new DiscImage(args[1]))
                        foreach (var entry in disc.MatchManifest(manifest.Values.ToList(), "default.xex"))
                            disc.Extract(entry, args[3], manifest[entry.Path].Sha256, n => { }, CancellationToken.None);
                    result = "ok";
                }
                else throw new ArgumentException("Unknown check.");
                File.WriteAllText(output, result, new UTF8Encoding(false));
                return 0;
            }
            catch (Exception error)
            {
                File.WriteAllText(output, "error: " + error.Message, new UTF8Encoding(false));
                return 1;
            }
        }
    }

    public sealed class LauncherWindow
    {
        public readonly Window Window;
        readonly Layout layout;
        readonly Dictionary<string, FrameworkElement> ui = new Dictionary<string, FrameworkElement>();
        LauncherSettings settings;
        bool loading;
        Process game;
        string runPath;
        CancellationTokenSource setupCancel;

        static readonly Brush Normal = new SolidColorBrush(Color.FromRgb(0xF2, 0xF4, 0xF8));
        static readonly Brush Muted = new SolidColorBrush(Color.FromRgb(0xA9, 0xB2, 0xC3));
        static readonly Brush Error = new SolidColorBrush(Color.FromRgb(0xFF, 0x8B, 0x8B));

        [DllImport("dwmapi.dll")]
        static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);

        public LauncherWindow(Layout layout)
        {
            this.layout = layout;
            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("Svr2011Launcher.Portable.xaml"))
                Window = (Window)XamlReader.Load(stream);
            foreach (var name in new[] { "BuildLabel", "SetupView", "IsoPath", "Browse", "SetupProgress", "SetupDetail", "SetupStatus",
                "Install", "PlayView", "SettingsPanel", "DisplayMode", "WindowSize", "Scale", "ScaleHelp", "Presentation",
                "PresentationHelp", "Controller", "PerfCapture", "Save", "Reset", "Status", "Play", "Logs",
                "Art", "SettingsDrawer", "SettingsToggle", "CloseSettings", "FramePacing", "FramePacingHelp" })
                ui[name] = (FrameworkElement)Window.FindName(name);

            string warning = null;
            try { settings = LauncherSettings.Load(layout.SettingsPath); }
            catch (Exception)
            {
                settings = new LauncherSettings();
                warning = "Saved settings could not be read. Defaults loaded; use Save to replace them.";
            }
            if (File.Exists(layout.VersionFile)) Text("BuildLabel").Text = File.ReadAllText(layout.VersionFile).Trim();

            ShowSettings(settings);
            foreach (var name in new[] { "DisplayMode", "WindowSize", "Scale", "Controller", "Presentation", "FramePacing" })
                ((ComboBox)ui[name]).SelectionChanged += (s, e) => SetDirty();
            Button("PerfCapture").Click += (s, e) => SetDirty();
            Button("Save").Click += (s, e) => SaveClicked();
            Button("Reset").Click += (s, e) =>
            {
                ShowSettings(new LauncherSettings());
                SetStatus("Defaults restored. Choose Save or Play to keep them.", false);
            };
            Button("Play").Click += (s, e) => PlayClicked();
            Button("Logs").Click += (s, e) => LogsClicked();
            Button("Browse").Click += (s, e) => BrowseClicked();
            Button("Install").Click += (s, e) => InstallClicked();
            Button("SettingsToggle").Click += (s, e) => ShowSettingsDrawer(ui["SettingsDrawer"].Visibility != Visibility.Visible);
            Button("CloseSettings").Click += (s, e) => ShowSettingsDrawer(false);
            // An open dropdown handles Escape first, so this only closes the drawer itself.
            Window.KeyDown += (s, e) =>
            {
                if (e.Key == Key.Escape && ui["SettingsDrawer"].Visibility == Visibility.Visible) { ShowSettingsDrawer(false); e.Handled = true; }
            };
            // Match the title bar to the dark window on Windows 10 20H1 and later.
            Window.SourceInitialized += (s, e) =>
            {
                var dark = 1;
                DwmSetWindowAttribute(new WindowInteropHelper(Window).Handle, 20, ref dark, sizeof(int));
            };
            ((TextBox)ui["IsoPath"]).TextChanged += (s, e) =>
                Button("Install").IsEnabled = ((TextBox)ui["IsoPath"]).Text.Trim().Length > 0;
            Window.Closing += (s, e) =>
            {
                if (setupCancel == null) return;
                var answer = MessageBox.Show(Window, "Game files are still being set up. Stop and close? You can start again later.",
                    "SVR 2011", MessageBoxButton.YesNo, MessageBoxImage.Warning);
                if (answer == MessageBoxResult.Yes) setupCancel.Cancel();
                else e.Cancel = true;
            };
            // Dispose only our handle. Never terminate the game.
            Window.Closed += (s, e) => { if (game != null) game.Dispose(); };

            var problem = PackageProblem();
            if (problem != null)
            {
                ShowPlay(layout.GameDataReady);
                SetStatus(problem, true);
                SetSetupStatus(problem, true);
                Button("Play").IsEnabled = false;
                Button("Browse").IsEnabled = false;
                ui["IsoPath"].IsEnabled = false;
            }
            else if (!layout.GameDataReady) ShowPlay(false);
            else
            {
                ShowPlay(true);
                if (warning != null) SetStatus(warning, true);
                else SetStatus("Ready. Saves are kept in the userdata folder.", false);
            }
        }

        TextBlock Text(string name) { return (TextBlock)ui[name]; }
        ButtonBase Button(string name) { return (ButtonBase)ui[name]; }
        ComboBox Combo(string name) { return (ComboBox)ui[name]; }

        string PackageProblem()
        {
            if (!File.Exists(layout.GameExe) || !File.Exists(layout.Manifest))
                return "The game folder is incomplete. Extract the whole zip again.";
            var temp = Path.GetTempPath().TrimEnd('\\');
            if (layout.Root.StartsWith(temp, StringComparison.OrdinalIgnoreCase))
                return "Extract the zip to a normal folder (for example C:\\Games\\SVR2011) and start the launcher from there.";
            if (layout.Root.Any(c => c > 127))
                return "Move this folder to a path that uses only English letters and digits, for example C:\\Games\\SVR2011.";
            try
            {
                var probe = Path.Combine(layout.Root, ".write-test-" + Path.GetRandomFileName());
                File.WriteAllText(probe, "");
                File.Delete(probe);
            }
            catch (Exception)
            {
                return "This folder is read-only. Move it somewhere you can write to, such as C:\\Games\\SVR2011 (not Program Files).";
            }
            if (!File.Exists(Path.Combine(Environment.SystemDirectory, "vulkan-1.dll")))
                return "Vulkan is not installed. Update your graphics driver (NVIDIA, AMD or Intel) and try again.";
            return null;
        }

        void ShowPlay(bool play)
        {
            ui["PlayView"].Visibility = play ? Visibility.Visible : Visibility.Collapsed;
            ui["SetupView"].Visibility = play ? Visibility.Collapsed : Visibility.Visible;
            if (play) LoadArt();
        }

        // The art is decoration: without it the window keeps its plain background.
        void LoadArt()
        {
            try
            {
                var image = new BitmapImage();
                image.BeginInit();
                image.CacheOption = BitmapCacheOption.OnLoad;
                image.StreamSource = new MemoryStream(KeyArt.Read(Path.Combine(layout.DataDir, "nxeart"), "nxebg.jpg"));
                image.EndInit();
                image.Freeze();
                ((Image)ui["Art"]).Source = image;
            }
            catch (Exception) { }
        }

        void ShowSettingsDrawer(bool open)
        {
            ui["SettingsDrawer"].Visibility = open ? Visibility.Visible : Visibility.Collapsed;
            if (open) Combo("DisplayMode").Focus();
            else Button("SettingsToggle").Focus();
        }

        void SetStatus(string message, bool error)
        {
            Text("Status").Text = message;
            Text("Status").Foreground = error ? Error : Normal;
        }

        void SetSetupStatus(string message, bool error)
        {
            Text("SetupStatus").Text = message;
            Text("SetupStatus").Foreground = error ? Error : Muted;
        }

        static void Select(ComboBox control, object value)
        {
            foreach (ComboBoxItem item in control.Items)
                if (Convert.ToString(item.Tag) == Convert.ToString(value)) { control.SelectedItem = item; break; }
        }

        static string Tag(ComboBox control) { return Convert.ToString(((ComboBoxItem)control.SelectedItem).Tag); }

        void ShowSettings(LauncherSettings value)
        {
            loading = true;
            Select(Combo("DisplayMode"), value.DisplayMode);
            Select(Combo("WindowSize"), value.WindowSize);
            Select(Combo("Scale"), value.Scale);
            Select(Combo("Controller"), value.Controller);
            Select(Combo("Presentation"), value.Presentation);
            Select(Combo("FramePacing"), value.FramePacing);
            ((CheckBox)ui["PerfCapture"]).IsChecked = value.PerfCapture;
            UpdateHelp();
            loading = false;
        }

        LauncherSettings ReadControls()
        {
            return new LauncherSettings
            {
                DisplayMode = Tag(Combo("DisplayMode")),
                WindowSize = Tag(Combo("WindowSize")),
                Scale = int.Parse(Tag(Combo("Scale"))),
                Controller = Tag(Combo("Controller")),
                Presentation = Tag(Combo("Presentation")),
                FramePacing = Tag(Combo("FramePacing")),
                PerfCapture = ((CheckBox)ui["PerfCapture"]).IsChecked == true,
            };
        }

        void UpdateHelp()
        {
            Combo("WindowSize").IsEnabled = Tag(Combo("DisplayMode")) == "Windowed";
            Text("ScaleHelp").Text = int.Parse(Tag(Combo("Scale"))) > 1
                ? "Higher detail uses more GPU resources. Check entrances and finishers; return to native if rendering breaks."
                : "Native rendering is the tested setting. Window size does not change rendering detail.";
            switch (Tag(Combo("Presentation")))
            {
                case "Mailbox": Text("PresentationHelp").Text = "Syncs display output without a fixed FPS cap. Falls back to monitor VSync if unavailable."; break;
                case "Immediate": Text("PresentationHelp").Text = "Shows each frame as soon as it is ready. Slightly lower latency, but the image can tear."; break;
                default: Text("PresentationHelp").Text = "Each frame lands on a display refresh, without tearing. No FPS limiter needed."; break;
            }
            Text("FramePacingHelp").Text = Tag(Combo("FramePacing")) == "Game"
                ? "Shows each frame the moment the game finishes it. Frames arrive 13-21 ms apart, so motion is less even."
                : "Shows every frame on an even 60 Hz beat, a few milliseconds after the game finishes it. Smoothest on every monitor.";
        }

        void SetDirty()
        {
            if (loading) return;
            UpdateHelp();
            SetStatus("Unsaved changes. Play also saves your settings.", false);
        }

        void SaveClicked()
        {
            try
            {
                settings = ReadControls();
                settings.Save(layout.SettingsPath);
                SetStatus("Settings saved. They apply the next time you play.", false);
            }
            catch (Exception error) { SetStatus("Could not save settings: " + error.Message, true); }
        }

        void PlayClicked()
        {
            try
            {
                if (game != null || Process.GetProcessesByName("svr2011").Length > 0)
                {
                    SetStatus("The game is already running. Close it before starting another session.", true);
                    return;
                }
                if (!layout.GameDataReady) throw new InvalidOperationException("Game files are missing. Restart the launcher to set them up.");
                settings = ReadControls();
                settings.Save(layout.SettingsPath);
                PruneSessions();
                runPath = Path.Combine(layout.LogsDir, "session-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
                Directory.CreateDirectory(runPath);
                settings.Save(Path.Combine(runPath, "settings.json"));
                Directory.CreateDirectory(layout.SaveDir);
                Directory.CreateDirectory(layout.CacheDir);
                var arguments = layout.GameArguments(settings, runPath);
                File.WriteAllLines(Path.Combine(runPath, "arguments.txt"), arguments);
                var info = new ProcessStartInfo(layout.GameExe, string.Join(" ", arguments.Select(Layout.Quote).ToArray()))
                {
                    WorkingDirectory = layout.GameDir,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                };
                game = Process.Start(info);
                game.EnableRaisingEvents = true;
                var session = runPath;
                var started = DateTime.Now;
                game.Exited += (s, e) => Window.Dispatcher.BeginInvoke(new Action(() => GameExited(session, started)));
                ui["SettingsPanel"].IsEnabled = false;
                ShowSettingsDrawer(false);
                Button("Play").IsEnabled = false;
                ((ContentControl)ui["Play"]).Content = "Game running";
                SetStatus("Game starting. You can close this launcher; the game keeps running.", false);
            }
            catch (Exception error) { SetStatus("Could not start: " + error.Message, true); }
        }

        void GameExited(string session, DateTime started)
        {
            var code = game.ExitCode;
            game.Dispose();
            game = null;
            var fatal = FatalTargets(Path.Combine(session, "runtime.log"));
            var result = new Dictionary<string, object>
            {
                { "exit_code", code }, { "outcome", code == 0 ? "exited" : "crashed" },
                { "elapsed_seconds", Math.Round((DateTime.Now - started).TotalSeconds, 1) },
                { "fatal_guest_targets", fatal },
            };
            try { File.WriteAllText(Path.Combine(session, "result.json"), new JavaScriptSerializer().Serialize(result)); }
            catch (IOException) { }
            ui["SettingsPanel"].IsEnabled = true;
            Button("Play").IsEnabled = true;
            ((ContentControl)ui["Play"]).Content = "_Play";
            if (code == 0) SetStatus("Game closed normally. Ready for another match.", false);
            else SetStatus("The game stopped unexpectedly. Choose Logs; the newest session folder holds the log for an issue on the project's GitHub page.", true);
        }

        static List<string> FatalTargets(string log)
        {
            var targets = new List<string>();
            if (!File.Exists(log)) return targets;
            var pattern = new Regex(@"\[FATAL\].*guest address (0x[0-9A-Fa-f]+)");
            try
            {
                using (var reader = new StreamReader(new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)))
                {
                    string line;
                    while ((line = reader.ReadLine()) != null)
                    {
                        var match = pattern.Match(line);
                        if (match.Success) targets.Add(match.Groups[1].Value);
                    }
                }
            }
            catch (IOException) { }
            return targets;
        }

        // Runtime logs can reach ~100 MB per session; keep the most recent sessions only.
        void PruneSessions()
        {
            if (!Directory.Exists(layout.LogsDir)) return;
            var old = new DirectoryInfo(layout.LogsDir).GetDirectories("session-*")
                .OrderByDescending(d => d.Name, StringComparer.Ordinal).Skip(9);
            foreach (var directory in old)
            {
                try { directory.Delete(true); }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
        }

        // Open the logs folder with the newest session selected, ready to zip for an issue.
        void LogsClicked()
        {
            var newest = Directory.Exists(layout.LogsDir)
                ? new DirectoryInfo(layout.LogsDir).GetDirectories("session-*").OrderByDescending(d => d.Name, StringComparer.Ordinal).FirstOrDefault()
                : null;
            if (newest == null) OpenFolder(layout.LogsDir, false);
            else OpenFolder(newest.FullName, true);
        }

        void OpenFolder(string path, bool select)
        {
            try
            {
                if (!select) Directory.CreateDirectory(path);
                var arguments = select ? "/select," + Layout.Quote(path) : Layout.Quote(path);
                Process.Start(new ProcessStartInfo("explorer.exe", arguments) { UseShellExecute = true });
            }
            catch (Exception error) { SetStatus("Could not open the folder: " + error.Message, true); }
        }

        void BrowseClicked()
        {
            var dialog = new Microsoft.Win32.OpenFileDialog
            {
                Title = "Choose your WWE SmackDown vs. Raw 2011 disc image",
                Filter = "Xbox 360 disc image (*.iso)|*.iso|All files (*.*)|*.*",
            };
            if (dialog.ShowDialog(Window) != true) return;
            ((TextBox)ui["IsoPath"]).Text = dialog.FileName;
            SetSetupStatus("Choose Set up game files to check and copy the game.", false);
        }

        async void InstallClicked()
        {
            var iso = ((TextBox)ui["IsoPath"]).Text.Trim().Trim('"');
            if (!File.Exists(iso))
            {
                SetSetupStatus("That disc image could not be found. Choose Browse to pick the .iso file.", true);
                return;
            }
            if (Directory.Exists(layout.DataDir))
            {
                SetSetupStatus("A gamedata folder already exists but is incomplete. Rename or delete it, then try again.", true);
                return;
            }
            Button("Install").IsEnabled = false;
            Button("Browse").IsEnabled = false;
            ui["IsoPath"].IsEnabled = false;
            ui["SetupProgress"].Visibility = Visibility.Visible;
            setupCancel = new CancellationTokenSource();
            var cancel = setupCancel.Token;
            try
            {
                SetSetupStatus("Checking the disc image…", false);
                var manifest = DiscImage.LoadManifest(layout.Manifest);
                var total = manifest.Sum(m => m.Size);
                var drive = new DriveInfo(Path.GetPathRoot(layout.Root));
                if (drive.AvailableFreeSpace < total + (512L << 20))
                    throw new IOException(string.Format("Not enough disk space. Setup needs {0:0.0} GB free on drive {1}.", (total + (512L << 20)) / 1e9, drive.Name));
                var progress = (ProgressBar)ui["SetupProgress"];
                long done = 0;
                var clock = Stopwatch.StartNew();
                long lastUpdate = -1000;
                string current = "";
                Action<long> report = n =>
                {
                    done += n;
                    if (clock.ElapsedMilliseconds - lastUpdate < 200) return;
                    lastUpdate = clock.ElapsedMilliseconds;
                    var snapshot = done;
                    var file = current;
                    Window.Dispatcher.BeginInvoke(new Action(() =>
                    {
                        progress.Value = (double)snapshot / total;
                        Text("SetupDetail").Text = string.Format("{0:0.0} of {1:0.0} GB · {2}", snapshot / 1e9, total / 1e9, file);
                    }));
                };
                await Task.Run(() =>
                {
                    var expected = manifest.ToDictionary(m => m.Path, StringComparer.OrdinalIgnoreCase);
                    using (var disc = new DiscImage(iso))
                    {
                        var files = disc.MatchManifest(manifest, "default.xex");
                        Window.Dispatcher.BeginInvoke(new Action(() => SetSetupStatus("Copying and verifying game files. This can take several minutes…", false)));
                        // The staging folder belongs to this launcher; a previous run may have been interrupted.
                        if (Directory.Exists(layout.StagingDir)) Directory.Delete(layout.StagingDir, true);
                        Directory.CreateDirectory(layout.StagingDir);
                        foreach (var entry in files)
                        {
                            current = entry.Path;
                            disc.Extract(entry, layout.StagingDir, expected[entry.Path].Sha256, report, cancel);
                        }
                    }
                    File.WriteAllText(Path.Combine(layout.StagingDir, ".svr2011-verified"), DateTime.UtcNow.ToString("o"));
                    Directory.Move(layout.StagingDir, layout.DataDir);
                }, cancel);
                progress.Value = 1;
                Text("SetupDetail").Text = "";
                ShowPlay(true);
                SetStatus("Game files are ready. Connect your controller and choose Play.", false);
            }
            catch (OperationCanceledException) { }
            catch (Exception error)
            {
                SetSetupStatus(error.Message, true);
                Button("Install").IsEnabled = true;
                Button("Browse").IsEnabled = true;
                ui["IsoPath"].IsEnabled = true;
            }
            finally
            {
                setupCancel = null;
            }
        }
    }
}
