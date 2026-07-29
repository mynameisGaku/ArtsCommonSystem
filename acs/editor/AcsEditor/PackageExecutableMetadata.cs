// SPDX-License-Identifier: Apache-2.0
// Deterministic Windows product metadata publication for private staged executables.

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml;

namespace AcsEditor.Packaging;

/// <summary>
/// Creates and verifies the product-facing resources on the private executable
/// copy in package staging. Source build outputs are inspected but never
/// modified. VERSIONINFO is replaced with one canonical language record; an
/// existing compatible process manifest is preserved, while a missing one is
/// supplied with a deterministic asInvoker manifest.
/// </summary>
public static class PackageExecutableMetadataContract
{
    private const ushort VersionResourceType = 16;
    private const ushort ManifestResourceType = 24;
    private const ushort PrimaryResourceName = 1;
    private const ushort EnglishUnitedStates = 0x0409;
    private const ushort UnicodeCodePage = 0x04b0;
    private const int MaximumProductNameLength = 256;
    private const int MaximumProductVersionLength = 256;
    private static readonly UTF8Encoding Utf8NoBom = new(false);
    private static readonly Regex ProductVersionPattern = new(
        @"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    public static PackageExecutableProductMetadata Create(
        string productName,
        string productVersion,
        PackageProductMetadata productMetadata,
        string originalFilename)
    {
        ArgumentNullException.ThrowIfNull(productMetadata);
        PackageProductMetadataContract.Validate(productMetadata);
        ValidateProductName(productName);
        string fileName = ValidateOriginalFilename(originalFilename);
        (ushort major, ushort minor, ushort patch) =
            ParseWindowsVersion(productVersion);
        string fileVersion = string.Create(
            CultureInfo.InvariantCulture,
            $"{major}.{minor}.{patch}.0");
        return new(
            productName,
            productVersion,
            productMetadata.Publisher,
            productMetadata.Description,
            productMetadata.Copyright,
            productMetadata.SupportUrl,
            fileVersion,
            fileName);
    }

    public static PackageExecutableInspection ApplyFile(
        string path,
        PackageExecutableProductMetadata expected)
    {
        ArgumentNullException.ThrowIfNull(expected);
        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException(
                "Windows executable product metadata requires the Windows resource updater.");
        }

        string fullPath = EnsureOrdinaryFile(path);
        ValidateExpected(expected);
        PackageExecutableInspection source =
            PackageExecutableContract.InspectFile(fullPath);
        byte[] versionInfo = BuildVersionInfo(expected);
        byte[]? generatedManifest = source.ApplicationManifest is null
            ? BuildApplicationManifest(expected)
            : null;

        IntPtr update = BeginUpdateResource(
            fullPath,
            deleteExistingResources: false);
        if (update == IntPtr.Zero)
            throw Win32Failure("begin staged executable resource update");

        bool updateOpen = true;
        try
        {
            foreach (ushort language in
                     source.VersionResourceLanguages.Distinct())
            {
                UpdateResourceOrThrow(
                    update,
                    VersionResourceType,
                    PrimaryResourceName,
                    language,
                    null,
                    "remove prior VERSIONINFO");
            }
            UpdateResourceOrThrow(
                update,
                VersionResourceType,
                PrimaryResourceName,
                EnglishUnitedStates,
                versionInfo,
                "write canonical VERSIONINFO");
            if (generatedManifest is not null)
            {
                UpdateResourceOrThrow(
                    update,
                    ManifestResourceType,
                    PrimaryResourceName,
                    EnglishUnitedStates,
                    generatedManifest,
                    "write canonical application manifest");
            }

            if (!EndUpdateResource(update, discard: false))
                throw Win32Failure("commit staged executable resource update");
            updateOpen = false;
        }
        finally
        {
            if (updateOpen)
                _ = EndUpdateResource(update, discard: true);
        }

        _ = EnsureOrdinaryFile(fullPath);
        PackageExecutableInspection published =
            PackageExecutableContract.InspectFile(fullPath);
        ValidateInspection(published, expected);
        if (generatedManifest is not null &&
            (published.ApplicationManifest?.AssemblyName !=
                 SanitizeManifestIdentity(expected.ProductName) ||
             published.ApplicationManifest.AssemblyVersion !=
                 expected.FileVersion ||
             published.ApplicationManifest.ProcessorArchitecture != "amd64"))
        {
            throw new PackageApplicationManifestException(
                "Generated executable application manifest does not match package identity.");
        }
        return published;
    }

    public static void ValidateInspection(
        PackageExecutableInspection inspection,
        PackageExecutableProductMetadata expected)
    {
        ArgumentNullException.ThrowIfNull(inspection);
        ArgumentNullException.ThrowIfNull(expected);
        ValidateExpected(expected);
        if (inspection.ProductMetadata != expected)
        {
            throw new InvalidDataException(
                "Packaged executable VERSIONINFO does not match package product metadata.");
        }
        if (inspection.VersionResourceLanguages.Count != 1 ||
            inspection.VersionResourceLanguages[0] != EnglishUnitedStates ||
            inspection.VersionResourceBytes is null ||
            !inspection.VersionResourceBytes.AsSpan().SequenceEqual(
                BuildVersionInfo(expected)))
        {
            throw new InvalidDataException(
                "Packaged executable VERSIONINFO is not the canonical deterministic resource.");
        }
        if (inspection.ApplicationManifest is not
            {
                RequestedExecutionLevel: "asInvoker",
                UiAccess: false,
                ProcessorArchitecture: "amd64" or "*",
            })
        {
            throw new PackageApplicationManifestException(
                "Packaged executable application manifest is missing or incompatible.");
        }
    }

    internal static byte[] BuildVersionInfoForSelfTest(
        PackageExecutableProductMetadata metadata) =>
        BuildVersionInfo(metadata);

    internal static void RemoveApplicationManifestForSelfTest(string path)
    {
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException();
        string fullPath = EnsureOrdinaryFile(path);
        PackageExecutableInspection inspection =
            PackageExecutableContract.InspectFile(fullPath);
        if (inspection.ApplicationManifestResourceLanguages.Count == 0)
            return;

        IntPtr update = BeginUpdateResource(
            fullPath,
            deleteExistingResources: false);
        if (update == IntPtr.Zero)
            throw Win32Failure("begin self-test manifest removal");
        bool updateOpen = true;
        try
        {
            foreach (ushort language in
                     inspection.ApplicationManifestResourceLanguages.Distinct())
            {
                UpdateResourceOrThrow(
                    update,
                    ManifestResourceType,
                    PrimaryResourceName,
                    language,
                    null,
                    "remove self-test application manifest");
            }
            if (!EndUpdateResource(update, discard: false))
                throw Win32Failure("commit self-test manifest removal");
            updateOpen = false;
        }
        finally
        {
            if (updateOpen)
                _ = EndUpdateResource(update, discard: true);
        }
        if (PackageExecutableContract
                .InspectFile(fullPath)
                .ApplicationManifest is not null)
        {
            throw new InvalidDataException(
                "Self-test could not remove the application manifest.");
        }
    }

    internal static void AddVersionInfoForSelfTest(
        string path,
        ushort name,
        ushort language,
        byte[] bytes)
    {
        ArgumentNullException.ThrowIfNull(bytes);
        UpdateSingleResourceForSelfTest(
            path,
            VersionResourceType,
            name,
            language,
            bytes);
    }

    internal static void ReplaceApplicationManifestForSelfTest(
        string path,
        byte[] bytes)
    {
        ArgumentNullException.ThrowIfNull(bytes);
        string fullPath = EnsureOrdinaryFile(path);
        RemoveApplicationManifestForSelfTest(fullPath);
        UpdateSingleResourceForSelfTest(
            fullPath,
            ManifestResourceType,
            PrimaryResourceName,
            EnglishUnitedStates,
            bytes);
    }

    private static void UpdateSingleResourceForSelfTest(
        string path,
        ushort type,
        ushort name,
        ushort language,
        byte[] bytes)
    {
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException();
        string fullPath = EnsureOrdinaryFile(path);
        IntPtr update = BeginUpdateResource(
            fullPath,
            deleteExistingResources: false);
        if (update == IntPtr.Zero)
            throw Win32Failure("begin self-test resource update");
        bool updateOpen = true;
        try
        {
            UpdateResourceOrThrow(
                update,
                type,
                name,
                language,
                bytes,
                "write self-test executable resource");
            if (!EndUpdateResource(update, discard: false))
                throw Win32Failure("commit self-test resource update");
            updateOpen = false;
        }
        finally
        {
            if (updateOpen)
                _ = EndUpdateResource(update, discard: true);
        }
    }

    private static byte[] BuildVersionInfo(
        PackageExecutableProductMetadata metadata)
    {
        ValidateExpected(metadata);
        (ushort major, ushort minor, ushort patch) =
            ParseWindowsVersion(metadata.ProductVersion);
        uint versionMostSignificant =
            ((uint)major << 16) | minor;
        uint versionLeastSignificant =
            ((uint)patch << 16);

        byte[] fixedFileInfo;
        using (var fixedStream = new MemoryStream())
        using (var writer = new BinaryWriter(
                   fixedStream,
                   Encoding.Unicode,
                   leaveOpen: true))
        {
            writer.Write(0xfeef04bdu);
            writer.Write(0x00010000u);
            writer.Write(versionMostSignificant);
            writer.Write(versionLeastSignificant);
            writer.Write(versionMostSignificant);
            writer.Write(versionLeastSignificant);
            writer.Write(0x0000003fu);
            writer.Write(0u);
            writer.Write(0x00040004u);
            writer.Write(1u);
            writer.Write(0u);
            writer.Write(0u);
            writer.Write(0u);
            writer.Flush();
            fixedFileInfo = fixedStream.ToArray();
        }

        var stringValues = new SortedDictionary<string, string>(
            StringComparer.Ordinal)
        {
            ["CompanyName"] = metadata.CompanyName,
            ["FileDescription"] = metadata.FileDescription,
            ["FileVersion"] = metadata.FileVersion,
            ["LegalCopyright"] = metadata.LegalCopyright,
            ["OriginalFilename"] = metadata.OriginalFilename,
            ["ProductName"] = metadata.ProductName,
            ["ProductVersion"] = metadata.ProductVersion,
            ["SupportUrl"] = metadata.SupportUrl,
        };
        byte[][] stringBlocks = stringValues
            .Select(pair => BuildVersionBlock(
                pair.Key,
                type: 1,
                value: Encoding.Unicode.GetBytes(pair.Value + "\0"),
                valueLength: checked((ushort)(pair.Value.Length + 1)),
                children: Array.Empty<byte[]>()))
            .ToArray();
        byte[] stringTable = BuildVersionBlock(
            "040904B0",
            type: 1,
            value: Array.Empty<byte>(),
            valueLength: 0,
            children: stringBlocks);
        byte[] stringFileInfo = BuildVersionBlock(
            "StringFileInfo",
            type: 1,
            value: Array.Empty<byte>(),
            valueLength: 0,
            children: [stringTable]);
        byte[] translation = new byte[4];
        BinaryPrimitives.WriteUInt16LittleEndian(
            translation,
            EnglishUnitedStates);
        BinaryPrimitives.WriteUInt16LittleEndian(
            translation.AsSpan(2),
            UnicodeCodePage);
        byte[] translationBlock = BuildVersionBlock(
            "Translation",
            type: 0,
            value: translation,
            valueLength: checked((ushort)translation.Length),
            children: Array.Empty<byte[]>());
        byte[] varFileInfo = BuildVersionBlock(
            "VarFileInfo",
            type: 1,
            value: Array.Empty<byte>(),
            valueLength: 0,
            children: [translationBlock]);
        return BuildVersionBlock(
            "VS_VERSION_INFO",
            type: 0,
            value: fixedFileInfo,
            valueLength: checked((ushort)fixedFileInfo.Length),
            children: [stringFileInfo, varFileInfo]);
    }

    private static byte[] BuildVersionBlock(
        string key,
        ushort type,
        byte[] value,
        ushort valueLength,
        IReadOnlyList<byte[]> children)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(
            stream,
            Encoding.Unicode,
            leaveOpen: true);
        writer.Write((ushort)0);
        writer.Write(valueLength);
        writer.Write(type);
        writer.Write(Encoding.Unicode.GetBytes(key + "\0"));
        WriteAlignment(writer);
        writer.Write(value);
        if (children.Count > 0)
        {
            WriteAlignment(writer);
            foreach (byte[] child in children)
            {
                writer.Write(child);
                WriteAlignment(writer);
            }
        }
        writer.Flush();
        byte[] block = stream.ToArray();
        if (block.Length > ushort.MaxValue)
        {
            throw new InvalidDataException(
                "Executable VERSIONINFO exceeds the Win32 resource length limit.");
        }
        BinaryPrimitives.WriteUInt16LittleEndian(
            block,
            checked((ushort)block.Length));
        return block;
    }

    private static void WriteAlignment(BinaryWriter writer)
    {
        while ((writer.BaseStream.Position & 3) != 0)
            writer.Write((byte)0);
    }

    private static byte[] BuildApplicationManifest(
        PackageExecutableProductMetadata metadata)
    {
        using var stream = new MemoryStream();
        using (XmlWriter writer = XmlWriter.Create(
                   stream,
                   new XmlWriterSettings
                   {
                       Encoding = Utf8NoBom,
                       Indent = true,
                       IndentChars = "  ",
                       NewLineChars = "\n",
                       NewLineHandling = NewLineHandling.Replace,
                       OmitXmlDeclaration = false,
                       CloseOutput = false,
                   }))
        {
            const string asm1 = "urn:schemas-microsoft-com:asm.v1";
            const string asm3 = "urn:schemas-microsoft-com:asm.v3";
            writer.WriteStartDocument(standalone: true);
            writer.WriteStartElement("assembly", asm1);
            writer.WriteAttributeString("manifestVersion", "1.0");
            writer.WriteStartElement("assemblyIdentity", asm1);
            writer.WriteAttributeString(
                "name",
                SanitizeManifestIdentity(metadata.ProductName));
            writer.WriteAttributeString("version", metadata.FileVersion);
            writer.WriteAttributeString("processorArchitecture", "amd64");
            writer.WriteAttributeString("type", "win32");
            writer.WriteEndElement();
            writer.WriteElementString(
                "description",
                asm1,
                metadata.FileDescription.Length > 0
                    ? metadata.FileDescription
                    : metadata.ProductName);
            writer.WriteStartElement("trustInfo", asm3);
            writer.WriteStartElement("security", asm3);
            writer.WriteStartElement("requestedPrivileges", asm3);
            writer.WriteStartElement("requestedExecutionLevel", asm3);
            writer.WriteAttributeString("level", "asInvoker");
            writer.WriteAttributeString("uiAccess", "false");
            writer.WriteEndElement();
            writer.WriteEndElement();
            writer.WriteEndElement();
            writer.WriteEndElement();
            writer.WriteEndElement();
            writer.WriteEndDocument();
        }
        return stream.ToArray();
    }

    private static string SanitizeManifestIdentity(string productName)
    {
        var result = new StringBuilder(productName.Length);
        foreach (char character in productName)
        {
            result.Append(
                char.IsLetterOrDigit(character) ||
                character is '_' or '-' or '.'
                    ? character
                    : '_');
        }
        if (result.Length == 0)
            result.Append('_');
        return result.ToString();
    }

    private static void ValidateExpected(
        PackageExecutableProductMetadata metadata)
    {
        ValidateProductName(metadata.ProductName);
        string fileName = ValidateOriginalFilename(metadata.OriginalFilename);
        if (!string.Equals(
                fileName,
                metadata.OriginalFilename,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Executable metadata original filename is not canonical.");
        }
        (ushort major, ushort minor, ushort patch) =
            ParseWindowsVersion(metadata.ProductVersion);
        string expectedFileVersion = string.Create(
            CultureInfo.InvariantCulture,
            $"{major}.{minor}.{patch}.0");
        if (!string.Equals(
                metadata.FileVersion,
                expectedFileVersion,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Executable metadata file version does not match its product version.");
        }
        ValidateResourceString(metadata.CompanyName, "publisher");
        ValidateResourceString(metadata.FileDescription, "description");
        ValidateResourceString(metadata.LegalCopyright, "copyright");
        ValidateResourceString(metadata.SupportUrl, "support URL");
        PackageProductMetadataContract.Validate(new(
            1,
            metadata.CompanyName,
            metadata.FileDescription,
            metadata.LegalCopyright,
            metadata.SupportUrl));
    }

    private static void ValidateProductName(string value)
    {
        if (string.IsNullOrWhiteSpace(value) ||
            value.Length > MaximumProductNameLength ||
            !string.Equals(value, value.Trim(), StringComparison.Ordinal) ||
            value.Any(character =>
                char.IsControl(character) ||
                char.GetUnicodeCategory(character) ==
                    UnicodeCategory.Format))
        {
            throw new InvalidDataException(
                "Executable product name is empty, unbounded, or contains unsafe text.");
        }
    }

    private static void ValidateResourceString(string? value, string field)
    {
        if (value is null || value.Contains('\0'))
        {
            throw new InvalidDataException(
                $"Executable metadata {field} contains an unsupported value.");
        }
    }

    private static string ValidateOriginalFilename(string value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value);
        string fileName = Path.GetFileName(value);
        if (!string.Equals(fileName, value, StringComparison.Ordinal) ||
            fileName.Length > 260 ||
            !fileName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ||
            fileName.Any(char.IsControl))
        {
            throw new InvalidDataException(
                "Executable metadata original filename must be one bounded .exe leaf name.");
        }
        return fileName;
    }

    private static (ushort Major, ushort Minor, ushort Patch)
        ParseWindowsVersion(string value)
    {
        if (string.IsNullOrWhiteSpace(value) ||
            value.Length > MaximumProductVersionLength ||
            !ProductVersionPattern.IsMatch(value))
        {
            throw new InvalidDataException(
                "Product version is not a supported semantic version.");
        }
        int suffix = value.Length;
        int prerelease = value.IndexOf('-');
        int build = value.IndexOf('+');
        if (prerelease >= 0)
            suffix = Math.Min(suffix, prerelease);
        if (build >= 0)
            suffix = Math.Min(suffix, build);
        string[] parts = value[..suffix].Split('.');
        if (parts.Length != 3 ||
            !ushort.TryParse(
                parts[0],
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out ushort major) ||
            !ushort.TryParse(
                parts[1],
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out ushort minor) ||
            !ushort.TryParse(
                parts[2],
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out ushort patch))
        {
            throw new InvalidDataException(
                "Product version numeric components must fit Windows VERSIONINFO (0-65535).");
        }
        return (major, minor, patch);
    }

    private static string EnsureOrdinaryFile(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        string fullPath = Path.GetFullPath(path);
        if (!File.Exists(fullPath) || Directory.Exists(fullPath))
        {
            throw new FileNotFoundException(
                "Staged executable resource target must be an existing ordinary file.",
                fullPath);
        }
        if ((File.GetAttributes(fullPath) & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException(
                "Staged executable resource target must not be a reparse point.");
        }
        string? current = Path.GetDirectoryName(fullPath);
        while (!string.IsNullOrEmpty(current))
        {
            if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
            {
                throw new InvalidDataException(
                    "Staged executable resource target must not traverse a reparse point.");
            }
            string? parent = Path.GetDirectoryName(current);
            if (string.IsNullOrEmpty(parent) ||
                string.Equals(parent, current, StringComparison.OrdinalIgnoreCase))
            {
                break;
            }
            current = parent;
        }
        return fullPath;
    }

    private static void UpdateResourceOrThrow(
        IntPtr update,
        ushort type,
        ushort name,
        ushort language,
        byte[]? data,
        string operation)
    {
        GCHandle pin = default;
        try
        {
            IntPtr pointer = IntPtr.Zero;
            uint length = 0;
            if (data is not null)
            {
                pin = GCHandle.Alloc(data, GCHandleType.Pinned);
                pointer = pin.AddrOfPinnedObject();
                length = checked((uint)data.Length);
            }
            if (!UpdateResource(
                    update,
                    new IntPtr(type),
                    new IntPtr(name),
                    language,
                    pointer,
                    length))
            {
                throw Win32Failure(operation);
            }
        }
        finally
        {
            if (pin.IsAllocated)
                pin.Free();
        }
    }

    private static Win32Exception Win32Failure(string operation) =>
        new(
            Marshal.GetLastWin32Error(),
            $"Could not {operation}.");

    [DllImport(
        "kernel32.dll",
        EntryPoint = "BeginUpdateResourceW",
        CharSet = CharSet.Unicode,
        SetLastError = true)]
    private static extern IntPtr BeginUpdateResource(
        string fileName,
        [MarshalAs(UnmanagedType.Bool)] bool deleteExistingResources);

    [DllImport(
        "kernel32.dll",
        EntryPoint = "UpdateResourceW",
        CharSet = CharSet.Unicode,
        SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UpdateResource(
        IntPtr update,
        IntPtr type,
        IntPtr name,
        ushort language,
        IntPtr data,
        uint dataLength);

    [DllImport(
        "kernel32.dll",
        EntryPoint = "EndUpdateResourceW",
        CharSet = CharSet.Unicode,
        SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EndUpdateResource(
        IntPtr update,
        [MarshalAs(UnmanagedType.Bool)] bool discard);
}
