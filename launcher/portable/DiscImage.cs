// Reads an Xbox 360 XDVDFS image without mounting it and extracts verified game files.
// Mirrors scripts/inspect_disc.py, including its defenses against malformed images.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace Svr2011Launcher
{
    public sealed class DiscEntry
    {
        public string Path;
        public long Offset;
        public long Size;
    }

    public sealed class ManifestEntry
    {
        public string Path;
        public long Size;
        public string Sha256;
    }

    public sealed class DiscImage : IDisposable
    {
        static readonly byte[] Magic = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");
        static readonly long[] Bases = { 0, 0xFD90000, 0x2080000, 0x18300000 };
        const int Sector = 2048;
        const int BlockSize = 1 << 20;
        readonly FileStream stream;
        readonly long size;
        readonly long partition;
        readonly uint rootSector;
        readonly uint rootSize;

        public DiscImage(string path)
        {
            stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, BlockSize);
            try
            {
                size = stream.Length;
                foreach (var candidate in Bases)
                {
                    if (candidate + 0x10800 > size) continue;
                    var header = Read(candidate + 0x10000, Sector);
                    if (StartsWith(header, 0) && StartsWith(header, Sector - Magic.Length))
                    {
                        partition = candidate;
                        rootSector = BitConverter.ToUInt32(header, 20);
                        rootSize = BitConverter.ToUInt32(header, 24);
                        return;
                    }
                }
                throw new InvalidDataException("This file is not an Xbox 360 disc image.");
            }
            catch
            {
                stream.Dispose();
                throw;
            }
        }

        public void Dispose() { stream.Dispose(); }

        bool StartsWith(byte[] data, int offset)
        {
            for (var i = 0; i < Magic.Length; i++) if (data[offset + i] != Magic[i]) return false;
            return true;
        }

        public byte[] Read(long offset, int count)
        {
            if (offset < 0 || count < 0 || offset + count > size) throw new InvalidDataException("Read outside the disc image.");
            stream.Position = offset;
            var data = new byte[count];
            var done = 0;
            while (done < count)
            {
                var read = stream.Read(data, done, count - done);
                if (read == 0) throw new InvalidDataException("The disc image is truncated.");
                done += read;
            }
            return data;
        }

        public List<DiscEntry> Files()
        {
            var result = new List<DiscEntry>();
            ReadDirectory(rootSector, rootSize, "", 0, new HashSet<uint>(), result);
            var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in result)
                if (!names.Add(entry.Path)) throw new InvalidDataException("Duplicate file names in disc.");
            result.Sort((a, b) => string.Compare(a.Path, b.Path, StringComparison.OrdinalIgnoreCase));
            return result;
        }

        void ReadDirectory(uint sector, uint length, string parent, int depth, HashSet<uint> seen, List<DiscEntry> result)
        {
            if (depth > 64 || seen.Contains(sector)) throw new InvalidDataException("Cyclic or excessively deep directory structure.");
            if (length == 0) return;
            seen.Add(sector);
            if (length > 16 * 1024 * 1024) throw new InvalidDataException("Unreasonably large directory table.");
            var data = Read(partition + (long)sector * Sector, (int)length);
            var pending = new Stack<int>();
            var visited = new HashSet<int>();
            pending.Push(0);
            while (pending.Count > 0)
            {
                var offset = pending.Pop();
                if (!visited.Add(offset) || offset + 14 > length) throw new InvalidDataException("Invalid directory entry pointer.");
                var left = BitConverter.ToUInt16(data, offset);
                var right = BitConverter.ToUInt16(data, offset + 2);
                var entrySector = BitConverter.ToUInt32(data, offset + 4);
                var entryLength = BitConverter.ToUInt32(data, offset + 8);
                var attributes = data[offset + 12];
                var nameLength = data[offset + 13];
                if (nameLength == 0 || offset + 14 + nameLength > length) throw new InvalidDataException("Invalid entry name length.");
                var name = Encoding.ASCII.GetString(data, offset + 14, nameLength);
                if (name == "." || name == ".." || name.IndexOfAny(new[] { '/', '\\', ':', '\0', '<', '>', '"', '|', '?', '*' }) >= 0)
                    throw new InvalidDataException("Unsafe entry name in disc.");
                var relative = parent.Length > 0 ? parent + "/" + name : name;
                var absolute = partition + (long)entrySector * Sector;
                if (absolute + entryLength > size) throw new InvalidDataException("Entry outside the disc image: " + relative);
                if ((attributes & 0x10) != 0) ReadDirectory(entrySector, entryLength, relative, depth + 1, seen, result);
                else result.Add(new DiscEntry { Path = relative, Offset = absolute, Size = entryLength });
                if (right != 0) pending.Push(right * 4);
                if (left != 0) pending.Push(left * 4);
            }
        }

        public static List<ManifestEntry> LoadManifest(string path)
        {
            var result = new List<ManifestEntry>();
            foreach (var line in File.ReadAllLines(path))
            {
                if (line.Length == 0 || line[0] == '#') continue;
                var parts = line.Split(new[] { '\t' }, 3);
                if (parts.Length != 3) throw new InvalidDataException("Malformed disc manifest.");
                result.Add(new ManifestEntry { Sha256 = parts[0], Size = long.Parse(parts[1]), Path = parts[2] });
            }
            return result;
        }

        // Fast identity check: same file list and sizes, and the executable hash matches.
        public List<DiscEntry> MatchManifest(List<ManifestEntry> manifest, string executablePath)
        {
            var files = Files();
            var expected = manifest.ToDictionary(m => m.Path, StringComparer.OrdinalIgnoreCase);
            if (files.Count != expected.Count) throw new DiscMismatchException();
            foreach (var entry in files)
            {
                ManifestEntry match;
                if (!expected.TryGetValue(entry.Path, out match) || match.Size != entry.Size) throw new DiscMismatchException();
            }
            var executable = files.First(f => string.Equals(f.Path, executablePath, StringComparison.OrdinalIgnoreCase));
            using (var hash = new SHA256Cng())
            {
                var digest = Hex(hash.ComputeHash(Read(executable.Offset, (int)executable.Size)));
                if (digest != expected[executable.Path].Sha256) throw new DiscMismatchException();
            }
            return files;
        }

        // Writes one file into an empty staging tree and verifies its hash while copying.
        public void Extract(DiscEntry entry, string root, string expectedSha256, Action<long> progress, CancellationToken cancel)
        {
            var fullRoot = System.IO.Path.GetFullPath(root).TrimEnd('\\') + "\\";
            var target = System.IO.Path.GetFullPath(System.IO.Path.Combine(fullRoot, entry.Path.Replace('/', '\\')));
            if (!target.StartsWith(fullRoot, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Extraction destination escapes the game folder.");
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(target));
            var buffer = new byte[BlockSize];
            using (var hash = new SHA256Cng())
            using (var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None, BlockSize))
            {
                stream.Position = entry.Offset;
                var remaining = entry.Size;
                while (remaining > 0)
                {
                    cancel.ThrowIfCancellationRequested();
                    var read = stream.Read(buffer, 0, (int)Math.Min(buffer.Length, remaining));
                    if (read == 0) throw new InvalidDataException("The disc image is truncated.");
                    hash.TransformBlock(buffer, 0, read, null, 0);
                    output.Write(buffer, 0, read);
                    remaining -= read;
                    progress(read);
                }
                hash.TransformFinalBlock(buffer, 0, 0);
                if (Hex(hash.Hash) != expectedSha256)
                    throw new InvalidDataException("A game file failed verification: " + entry.Path + ". The disc image may be damaged.");
            }
        }

        public static string Hex(byte[] data)
        {
            var text = new StringBuilder(data.Length * 2);
            foreach (var b in data) text.Append(b.ToString("x2"));
            return text.ToString();
        }
    }

    public sealed class DiscMismatchException : Exception
    {
        public DiscMismatchException()
            : base("This disc image is not the supported version of WWE SmackDown vs. Raw 2011 (Xbox 360, USA/Europe).") { }
    }
}
