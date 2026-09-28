// Player settings and the runtime arguments they produce.
// Keep in step with scripts/Launcher.Core.ps1; tests/test_portable.ps1 compares both.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;

namespace Svr2011Launcher
{
    public sealed class LauncherSettings
    {
        public int Version = 1;
        public string DisplayMode = "Borderless";
        public string WindowSize = "1280x720";
        public int Scale = 1;
        public string Controller = "Auto";
        public bool PerfCapture;
        public string Presentation = "Fifo";
        public string FramePacing = "Auto";

        static readonly string[] DisplayModes = { "Borderless", "Windowed" };
        static readonly string[] WindowSizes = { "1280x720", "1600x900", "1920x1080" };
        static readonly string[] Controllers = { "Auto", "Xbox" };
        static readonly string[] Presentations = { "Immediate", "Mailbox", "Fifo" };
        static readonly string[] FramePacings = { "Auto", "Game", "Even" };

        public void Validate()
        {
            if (Version != 1) throw new InvalidDataException("Unsupported launcher settings version.");
            if (Array.IndexOf(DisplayModes, DisplayMode) < 0) throw new InvalidDataException("Invalid display mode.");
            if (Array.IndexOf(WindowSizes, WindowSize) < 0) throw new InvalidDataException("Invalid window size.");
            if (Scale < 1 || Scale > 3) throw new InvalidDataException("Invalid resolution scale.");
            if (Array.IndexOf(Controllers, Controller) < 0) throw new InvalidDataException("Invalid controller mode.");
            if (Array.IndexOf(Presentations, Presentation) < 0) throw new InvalidDataException("Invalid display synchronization mode.");
            if (Array.IndexOf(FramePacings, FramePacing) < 0) throw new InvalidDataException("Invalid frame pacing mode.");
        }

        public static LauncherSettings Load(string path)
        {
            var settings = new LauncherSettings();
            if (!File.Exists(path)) return settings;
            var values = new JavaScriptSerializer().DeserializeObject(File.ReadAllText(path, Encoding.UTF8))
                as Dictionary<string, object>;
            if (values == null) throw new InvalidDataException("Settings are not a JSON object.");
            settings.Version = Require<int>(values, "version");
            settings.DisplayMode = Require<string>(values, "displayMode");
            settings.WindowSize = Require<string>(values, "windowSize");
            settings.Scale = Require<int>(values, "scale");
            settings.Controller = Require<string>(values, "controller");
            settings.PerfCapture = Require<bool>(values, "perfCapture");
            // Older version-one files predate display synchronization controls.
            settings.Presentation = values.ContainsKey("presentation") ? Require<string>(values, "presentation") : "Fifo";
            settings.FramePacing = values.ContainsKey("framePacing") ? Require<string>(values, "framePacing") : "Auto";
            // "Even" was a separate choice until Automatic became even pacing on every monitor.
            if (settings.FramePacing == "Even") settings.FramePacing = "Auto";
            settings.Validate();
            return settings;
        }

        static T Require<T>(Dictionary<string, object> values, string name)
        {
            object value;
            if (!values.TryGetValue(name, out value) || !(value is T)) throw new InvalidDataException("Invalid setting: " + name);
            return (T)value;
        }

        public void Save(string path)
        {
            Validate();
            var serializer = new JavaScriptSerializer();
            var json = new StringBuilder();
            json.AppendLine("{");
            json.AppendLine("  \"version\": " + Version + ",");
            json.AppendLine("  \"displayMode\": " + serializer.Serialize(DisplayMode) + ",");
            json.AppendLine("  \"windowSize\": " + serializer.Serialize(WindowSize) + ",");
            json.AppendLine("  \"scale\": " + Scale + ",");
            json.AppendLine("  \"controller\": " + serializer.Serialize(Controller) + ",");
            json.AppendLine("  \"perfCapture\": " + (PerfCapture ? "true" : "false") + ",");
            json.AppendLine("  \"presentation\": " + serializer.Serialize(Presentation) + ",");
            json.AppendLine("  \"framePacing\": " + serializer.Serialize(FramePacing));
            json.AppendLine("}");
            var directory = Path.GetDirectoryName(Path.GetFullPath(path));
            Directory.CreateDirectory(directory);
            var temp = Path.Combine(directory, Path.GetRandomFileName());
            try
            {
                File.WriteAllText(temp, json.ToString(), new UTF8Encoding(false));
                if (File.Exists(path)) File.Replace(temp, path, null);
                else File.Move(temp, path);
            }
            finally
            {
                if (File.Exists(temp)) File.Delete(temp);
            }
        }

        public List<string> DisplayArguments()
        {
            Validate();
            var size = WindowSize.Split('x');
            return new List<string>
            {
                "--fullscreen=" + Lower(DisplayMode == "Borderless"),
                "--window_width=" + size[0],
                "--window_height=" + size[1],
                "--draw_resolution_scale_x=" + Scale,
                "--draw_resolution_scale_y=" + Scale,
                // Specify the alias too so a previously persisted runtime value cannot override it.
                "--resolution_scale=" + Scale,
                "--input_backend=" + (Controller == "Xbox" ? "xinput" : "sdl"),
                "--vulkan_allow_present_mode_immediate=" + Lower(Presentation == "Immediate"),
                "--vulkan_allow_present_mode_mailbox=" + Lower(Presentation != "Fifo"),
                "--vulkan_allow_present_mode_fifo_relaxed=" + Lower(Presentation == "Immediate"),
                "--present_pace_to_guest_vblank=" + Lower(PacesToGuestVblank()),
            };
        }

        // The game advances a fixed step per frame, so each frame belongs on an even 60 Hz beat. The
        // runtime shows it 5 ms after its vblank, just after the game finishes it, on every display.
        public bool PacesToGuestVblank()
        {
            return FramePacing != "Game";
        }

        static string Lower(bool value) { return value ? "true" : "false"; }
    }
}
