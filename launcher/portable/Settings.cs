// Player settings and the runtime arguments they produce.
// Keep in step with scripts/Launcher.Core.ps1; tests/test_portable.ps1 compares both.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
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
        public bool Sharpening = true;
        public string Renderer = "Classic";

        static readonly string[] DisplayModes = { "Borderless", "Windowed" };
        static readonly string[] WindowSizes = { "1280x720", "1600x900", "1920x1080" };
        static readonly string[] Controllers = { "Auto", "Xbox" };
        static readonly string[] Presentations = { "Immediate", "Mailbox", "Fifo" };
        static readonly string[] FramePacings = { "Auto", "Game", "Even" };
        static readonly string[] Renderers = { "Classic", "Native" };

        public void Validate()
        {
            if (Version != 1) throw new InvalidDataException("Unsupported launcher settings version.");
            if (Array.IndexOf(DisplayModes, DisplayMode) < 0) throw new InvalidDataException("Invalid display mode.");
            if (Array.IndexOf(WindowSizes, WindowSize) < 0) throw new InvalidDataException("Invalid window size.");
            if (Scale < 1 || Scale > 3) throw new InvalidDataException("Invalid resolution scale.");
            if (Array.IndexOf(Controllers, Controller) < 0) throw new InvalidDataException("Invalid controller mode.");
            if (Array.IndexOf(Presentations, Presentation) < 0) throw new InvalidDataException("Invalid display synchronization mode.");
            if (Array.IndexOf(FramePacings, FramePacing) < 0) throw new InvalidDataException("Invalid frame pacing mode.");
            if (Array.IndexOf(Renderers, Renderer) < 0) throw new InvalidDataException("Invalid renderer.");
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
            settings.Sharpening = values.ContainsKey("sharpening") ? Require<bool>(values, "sharpening") : true;
            settings.Renderer = values.ContainsKey("renderer") ? Require<string>(values, "renderer") : "Classic";
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
            json.AppendLine("  \"framePacing\": " + serializer.Serialize(FramePacing) + ",");
            json.AppendLine("  \"sharpening\": " + (Sharpening ? "true" : "false") + ",");
            json.AppendLine("  \"renderer\": " + serializer.Serialize(Renderer));
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

        // screenHeight: the screen's height in pixels for borderless fullscreen (0 = unknown).
        public List<string> DisplayArguments(int screenHeight)
        {
            Validate();
            var size = WindowSize.Split('x');
            var arguments = new List<string>
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
                // FSR 1 upscales when the game image is smaller than the screen; otherwise CAS sharpens or downsamples it.
                "--present_effect=" + (Sharpening ? "fsr" : "bilinear"),
                "--present_fsr_sharpness_reduction=0.5",
            };
            if (Renderer == "Native")
            {
                arguments.Add("--svr_native_renderer=true");
                arguments.Add("--svr_native_resolution_scale=" + Scale);
                arguments.Add("--svr_native_lod_bias=" + NativeLodBias(screenHeight));
            }
            return arguments;
        }

        // Sharper distant textures cost shimmer, unless each screen pixel averages several rendered ones.
        public string NativeLodBias(int screenHeight)
        {
            var outputHeight = DisplayMode == "Windowed" ? int.Parse(WindowSize.Split('x')[1]) : screenHeight;
            return outputHeight > 0 && 720 * Scale > outputHeight ? "-1" : "-0.5";
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct DevMode
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name;
            public short SpecVersion, DriverVersion, Size, DriverExtra;
            public int Fields, PositionX, PositionY, Orientation, FixedOutput;
            public short Color, Duplex, YResolution, TTOption, Collate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
            public short LogPixels;
            public int BitsPerPel, PelsWidth, PelsHeight, DisplayFlags, DisplayFrequency;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern bool EnumDisplaySettings(string device, int mode, ref DevMode devMode);

        // The primary screen's height in physical pixels, whatever the DPI scaling.
        public static int ScreenHeight()
        {
            var mode = new DevMode();
            mode.Size = (short)Marshal.SizeOf(typeof(DevMode));
            return EnumDisplaySettings(null, -1, ref mode) ? mode.PelsHeight : 0;
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
