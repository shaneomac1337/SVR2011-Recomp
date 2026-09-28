// Reads the dashboard artwork from the player's own disc (the nxeart package), so the
// launcher shows official art without this project ever distributing it.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace Svr2011Launcher
{
    public static class KeyArt
    {
        const int BlockSize = 0x1000;
        const int FirstBlock = 0xA000;
        const int FileTableEntrySize = 0x40;

        // nxeart is a read-only STFS package. Hash blocks sit between data blocks; each one starts
        // with the SHA-1 of a data block, which identifies it without decoding the package layout.
        public static byte[] Read(string packagePath, string fileName)
        {
            var package = File.ReadAllBytes(packagePath);
            if (package.Length < FirstBlock + 2 * BlockSize || Encoding.ASCII.GetString(package, 0, 4) != "PIRS")
                throw new InvalidDataException("Not a dashboard art package.");
            var offsets = new List<int>();
            for (var offset = FirstBlock; offset + BlockSize <= package.Length; offset += BlockSize) offsets.Add(offset);
            var digests = new HashSet<string>();
            using (var sha1 = SHA1.Create())
                foreach (var offset in offsets) digests.Add(Convert.ToBase64String(sha1.ComputeHash(package, offset, BlockSize)));
            var data = offsets.Where(offset => !digests.Contains(Convert.ToBase64String(package, offset, 20))).ToList();

            // The file table is the data block that names the files; entries are 64 bytes each.
            foreach (var tableIndex in Enumerable.Range(0, data.Count))
            {
                for (var entry = data[tableIndex]; entry < data[tableIndex] + BlockSize; entry += FileTableEntrySize)
                {
                    var nameLength = package[entry + 0x28] & 0x3F;
                    if (nameLength == 0) break;
                    if (Encoding.ASCII.GetString(package, entry, nameLength) != fileName) continue;
                    var blocks = package[entry + 0x29] | package[entry + 0x2A] << 8 | package[entry + 0x2B] << 16;
                    var start = package[entry + 0x2F] | package[entry + 0x30] << 8 | package[entry + 0x31] << 16;
                    var size = package[entry + 0x34] << 24 | package[entry + 0x35] << 16 | package[entry + 0x36] << 8 | package[entry + 0x37];
                    // Block numbers count from the file table block.
                    if (size <= 0 || tableIndex + start + blocks > data.Count || size > blocks * BlockSize)
                        throw new InvalidDataException("Damaged dashboard art package.");
                    var result = new byte[size];
                    for (var block = 0; block < blocks; block++)
                    {
                        var length = Math.Min(BlockSize, size - block * BlockSize);
                        Buffer.BlockCopy(package, data[tableIndex + start + block], result, block * BlockSize, length);
                    }
                    return result;
                }
            }
            throw new FileNotFoundException(fileName + " is not in the dashboard art package.");
        }
    }
}
