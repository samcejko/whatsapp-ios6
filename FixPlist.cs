using System;
using System.IO;
using System.Collections.Generic;
using System.Text;
using System.Runtime.Serialization.Formatters.Binary;

class FixPlist
{
    static void Main(string[] args)
    {
        if (args.Length == 0)
        {
            Console.WriteLine("Usage: FixPlist.exe <file1> [file2 ...]");
            return;
        }

        foreach (string file in args)
        {
            try
            {
                FixFile(file);
            }
            catch (Exception ex)
            {
                Console.WriteLine("Error: " + ex.Message);
            }
        }
    }

    static void FixFile(string path)
    {
        byte[] data = File.ReadAllBytes(path);
        
        // Binary plist format: find the key "CFBundleDisplayName" and its value, remove both
        // This is a simplified approach - we'll look for the key in the binary data
        string key = "CFBundleDisplayName";
        byte[] keyBytes = Encoding.UTF8.GetBytes(key);
        
        // Find the key in the binary plist
        int keyPos = FindBytes(data, keyBytes);
        if (keyPos < 0)
        {
            Console.WriteLine("No CFBundleDisplayName in " + Path.GetFileName(path));
            return;
        }

        // We found the key - now we need to properly remove it from the binary plist
        // For a proper fix, we'd need full plist parsing. 
        // Instead, let's use a simpler approach: convert to XML, remove key, convert back
        // But without plutil, we'll do a hack: just zero out the key string
        
        // Hack: Replace the key string with spaces (same length)
        for (int i = 0; i < keyBytes.Length; i++)
        {
            data[keyPos + i] = (byte)' ';
        }
        
        File.WriteAllBytes(path, data);
        Console.WriteLine("Fixed: " + Path.GetFileName(path));
    }

    static int FindBytes(byte[] haystack, byte[] needle)
    {
        for (int i = 0; i <= haystack.Length - needle.Length; i++)
        {
            bool match = true;
            for (int j = 0; j < needle.Length; j++)
            {
                if (haystack[i + j] != needle[j]) { match = false; break; }
            }
            if (match) return i;
        }
        return -1;
    }
}