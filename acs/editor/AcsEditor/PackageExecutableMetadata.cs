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

        NormalizeDeterministicPeHeader(fullPath);
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

    internal static int SetVolatilePeHeaderFieldsForSelfTest(
        string path,
        uint timeDateStamp,
        uint checksum,
        uint debugTimeDateStamp)
    {
        string fullPath = EnsureOrdinaryFile(path);
        return WriteVolatilePeHeaderFields(
            fullPath,
            timeDateStamp,
            checksum,
            debugTimeDateStamp);
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

    private static void NormalizeDeterministicPeHeader(string path) =>
        _ = WriteVolatilePeHeaderFields(
            path,
            timeDateStamp: 0,
            checksum: 0,
            debugTimeDateStamp: 0);

    private static int WriteVolatilePeHeaderFields(
        string path,
        uint timeDateStamp,
        uint checksum,
        uint debugTimeDateStamp)
    {
        using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.ReadWrite,
            FileShare.None,
            4096,
            FileOptions.WriteThrough);
        if (stream.Length < 0x40)
            throw new InvalidDataException("Packaged executable DOS header is truncated.");

        Span<byte> dosHeader = stackalloc byte[0x40];
        stream.ReadExactly(dosHeader);
        if (dosHeader[0] != (byte)'M' ||
            dosHeader[1] != (byte)'Z')
        {
            throw new InvalidDataException(
                "Packaged executable DOS signature is invalid.");
        }

        int peOffset = BinaryPrimitives.ReadInt32LittleEndian(
            dosHeader.Slice(0x3c, sizeof(int)));
        const int coffHeaderBytes = 20;
        const int checksumOffsetInOptionalHeader = 64;
        const int checksumFieldBytes = sizeof(uint);
        long minimumEnd =
            (long)peOffset +
            4 +
            coffHeaderBytes +
            checksumOffsetInOptionalHeader +
            checksumFieldBytes;
        if (peOffset < 0x40 ||
            minimumEnd > stream.Length)
        {
            throw new InvalidDataException(
                "Packaged executable PE header range is invalid.");
        }

        stream.Position = peOffset;
        Span<byte> peAndCoff = stackalloc byte[4 + coffHeaderBytes];
        stream.ReadExactly(peAndCoff);
        if (!peAndCoff[..4].SequenceEqual("PE\0\0"u8))
            throw new InvalidDataException("Packaged executable PE signature is invalid.");

        ushort optionalHeaderBytes =
            BinaryPrimitives.ReadUInt16LittleEndian(
                peAndCoff.Slice(4 + 16, sizeof(ushort)));
        if (optionalHeaderBytes <
                checksumOffsetInOptionalHeader + checksumFieldBytes ||
            (long)peOffset + 4 + coffHeaderBytes + optionalHeaderBytes >
                stream.Length)
        {
            throw new InvalidDataException(
                "Packaged executable optional header range is invalid.");
        }

        long optionalHeaderOffset = (long)peOffset + 4 + coffHeaderBytes;
        byte[] optionalHeader = new byte[optionalHeaderBytes];
        stream.Position = optionalHeaderOffset;
        stream.ReadExactly(optionalHeader);
        ushort magic =
            BinaryPrimitives.ReadUInt16LittleEndian(optionalHeader);
        if (magic is not (0x010b or 0x020b))
        {
            throw new InvalidDataException(
                "Packaged executable optional header format is unsupported.");
        }

        NormalizeResourceDirectoryTimestamps(
            stream,
            peAndCoff,
            optionalHeader,
            optionalHeaderOffset + optionalHeaderBytes,
            magic);
        int debugDirectoryEntries =
            WriteDebugDirectoryTimestamps(
                stream,
                peAndCoff,
                optionalHeader,
                optionalHeaderOffset + optionalHeaderBytes,
                magic,
                debugTimeDateStamp);

        Span<byte> value = stackalloc byte[sizeof(uint)];
        BinaryPrimitives.WriteUInt32LittleEndian(value, timeDateStamp);
        stream.Position = (long)peOffset + 4 + 4;
        stream.Write(value);
        BinaryPrimitives.WriteUInt32LittleEndian(value, checksum);
        stream.Position =
            optionalHeaderOffset + checksumOffsetInOptionalHeader;
        stream.Write(value);
        stream.Flush(flushToDisk: true);
        return debugDirectoryEntries;
    }

    private static void NormalizeResourceDirectoryTimestamps(
        FileStream stream,
        ReadOnlySpan<byte> peAndCoff,
        ReadOnlySpan<byte> optionalHeader,
        long sectionTableOffset,
        ushort optionalMagic)
    {
        const int sectionHeaderBytes = 40;
        const int resourceDirectoryIndex = 2;
        const int dataDirectoryEntryBytes = 8;
        const int maximumResourceDirectoryBytes = 64 * 1024 * 1024;
        const int maximumDirectoryVisits = 4096;
        const int maximumDirectoryEntries = 16 * 1024;

        ushort sectionCount =
            BinaryPrimitives.ReadUInt16LittleEndian(
                peAndCoff.Slice(4 + 2, sizeof(ushort)));
        if (sectionCount is 0 or > 96 ||
            sectionTableOffset +
                checked((long)sectionCount * sectionHeaderBytes) >
            stream.Length)
        {
            throw new InvalidDataException(
                "Packaged executable section table is invalid.");
        }

        int countOffset = optionalMagic == 0x020b ? 108 : 92;
        int directoryOffset = optionalMagic == 0x020b ? 112 : 96;
        if (optionalHeader.Length < directoryOffset)
        {
            throw new InvalidDataException(
                "Packaged executable data-directory header is truncated.");
        }
        uint directoryCount =
            BinaryPrimitives.ReadUInt32LittleEndian(
                optionalHeader.Slice(countOffset, sizeof(uint)));
        int availableDirectories =
            (optionalHeader.Length - directoryOffset) /
            dataDirectoryEntryBytes;
        if (directoryCount > availableDirectories)
        {
            throw new InvalidDataException(
                "Packaged executable data-directory count is invalid.");
        }
        if (directoryCount <= resourceDirectoryIndex)
            return;

        int resourceEntry =
            directoryOffset +
            resourceDirectoryIndex * dataDirectoryEntryBytes;
        uint resourceRva =
            BinaryPrimitives.ReadUInt32LittleEndian(
                optionalHeader.Slice(resourceEntry, sizeof(uint)));
        uint resourceSize =
            BinaryPrimitives.ReadUInt32LittleEndian(
                optionalHeader.Slice(
                    resourceEntry + sizeof(uint),
                    sizeof(uint)));
        if (resourceRva == 0 && resourceSize == 0)
            return;
        if (resourceRva == 0 ||
            resourceSize is 0 or > maximumResourceDirectoryBytes)
        {
            throw new InvalidDataException(
                "Packaged executable resource directory is invalid.");
        }

        byte[] sectionTable =
            new byte[checked(sectionCount * sectionHeaderBytes)];
        stream.Position = sectionTableOffset;
        stream.ReadExactly(sectionTable);
        long rawStart = -1;
        for (int index = 0; index < sectionCount; ++index)
        {
            ReadOnlySpan<byte> section = sectionTable.AsSpan(
                index * sectionHeaderBytes,
                sectionHeaderBytes);
            uint virtualSize =
                BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(8, 4));
            uint virtualAddress =
                BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(12, 4));
            uint rawSize =
                BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(16, 4));
            uint rawOffset =
                BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(20, 4));
            ulong mappedSize = Math.Max((ulong)virtualSize, rawSize);
            if (resourceRva < virtualAddress ||
                (ulong)resourceRva >=
                    (ulong)virtualAddress + mappedSize)
            {
                continue;
            }
            if (rawStart >= 0)
            {
                throw new InvalidDataException(
                    "Packaged executable resource directory maps ambiguously.");
            }
            ulong delta = resourceRva - virtualAddress;
            ulong rawEnd =
                (ulong)rawOffset + delta + resourceSize;
            if (delta > rawSize ||
                rawEnd > (ulong)rawOffset + rawSize ||
                rawEnd > (ulong)stream.Length)
            {
                throw new InvalidDataException(
                    "Packaged executable resource directory range is invalid.");
            }
            rawStart = checked((long)((ulong)rawOffset + delta));
        }
        if (rawStart < 0)
        {
            throw new InvalidDataException(
                "Packaged executable resource directory is unmapped.");
        }

        byte[] resource = new byte[checked((int)resourceSize)];
        stream.Position = rawStart;
        stream.ReadExactly(resource);
        var pending = new Stack<int>();
        var visited = new HashSet<int>();
        pending.Push(0);
        int totalEntries = 0;
        while (pending.Count > 0)
        {
            int offset = pending.Pop();
            if (!visited.Add(offset) ||
                visited.Count > maximumDirectoryVisits ||
                offset < 0 ||
                offset > resource.Length - 16)
            {
                throw new InvalidDataException(
                    "Packaged executable resource directory graph is invalid.");
            }

            resource.AsSpan(offset + 4, sizeof(uint)).Clear();
            int entryCount =
                BinaryPrimitives.ReadUInt16LittleEndian(
                    resource.AsSpan(offset + 12, sizeof(ushort))) +
                BinaryPrimitives.ReadUInt16LittleEndian(
                    resource.AsSpan(offset + 14, sizeof(ushort)));
            totalEntries = checked(totalEntries + entryCount);
            long entriesEnd =
                (long)offset + 16L + (long)entryCount * 8L;
            if (totalEntries > maximumDirectoryEntries ||
                entriesEnd > resource.Length)
            {
                throw new InvalidDataException(
                    "Packaged executable resource directory entries are invalid.");
            }
            for (int entry = 0; entry < entryCount; ++entry)
            {
                uint target =
                    BinaryPrimitives.ReadUInt32LittleEndian(
                        resource.AsSpan(
                            offset + 16 + entry * 8 + 4,
                            sizeof(uint)));
                if ((target & 0x80000000u) != 0)
                    pending.Push(checked((int)(target & 0x7fffffffu)));
            }
        }

        stream.Position = rawStart;
        stream.Write(resource);
    }

    private static int WriteDebugDirectoryTimestamps(
        FileStream stream,
        ReadOnlySpan<byte> peAndCoff,
        ReadOnlySpan<byte> optionalHeader,
        long sectionTableOffset,
        ushort optionalMagic,
        uint timeDateStamp)
    {
        const int sectionHeaderBytes = 40;
        const int debugDirectoryIndex = 6;
        const int dataDirectoryEntryBytes = 8;
        const int debugDirectoryEntryBytes = 28;
        const int timestampOffset = 4;
        const int maximumDebugDirectoryEntries = 64 * 1024;

        ushort sectionCount =
            BinaryPrimitives.ReadUInt16LittleEndian(
                peAndCoff.Slice(4 + 2, sizeof(ushort)));
        if (sectionCount is 0 or > 96 ||
            sectionTableOffset +
                checked((long)sectionCount * sectionHeaderBytes) >
            stream.Length)
        {
            throw new InvalidDataException(
                "Packaged executable section table is invalid.");
        }

        int countOffset = optionalMagic == 0x020b ? 108 : 92;
        int directoryOffset = optionalMagic == 0x020b ? 112 : 96;
        if (optionalHeader.Length < directoryOffset)
        {
            throw new InvalidDataException(
                "Packaged executable data-directory header is truncated.");
        }
        uint directoryCount =
            BinaryPrimitives.ReadUInt32LittleEndian(
                optionalHeader.Slice(countOffset, sizeof(uint)));
        int availableDirectories =
            (optionalHeader.Length - directoryOffset) /
            dataDirectoryEntryBytes;
        if (directoryCount > availableDirectories)
        {
            throw new InvalidDataException(
                "Packaged executable data-directory count is invalid.");
        }
        if (directoryCount <= debugDirectoryIndex)
            return 0;

        int debugEntry =
            directoryOffset +
            debugDirectoryIndex * dataDirectoryEntryBytes;
        uint debugRva =
            BinaryPrimitives.ReadUInt32LittleEndian(
                optionalHeader.Slice(debugEntry, sizeof(uint)));
        uint debugSize =
            BinaryPrimitives.ReadUInt32LittleEndian(
                optionalHeader.Slice(
                    debugEntry + sizeof(uint),
                    sizeof(uint)));
        if (debugRva == 0 && debugSize == 0)
            return 0;
        if (debugRva == 0 ||
            debugSize == 0 ||
            debugSize % debugDirectoryEntryBytes != 0)
        {
            throw new InvalidDataException(
                "Packaged executable debug directory is invalid.");
        }
        uint debugEntryCount = debugSize / debugDirectoryEntryBytes;
        if (debugEntryCount > maximumDebugDirectoryEntries)
        {
            throw new InvalidDataException(
                "Packaged executable debug directory exceeds supported bounds.");
        }

        byte[] sectionTable =
            new byte[checked(sectionCount * sectionHeaderBytes)];
        stream.Position = sectionTableOffset;
        stream.ReadExactly(sectionTable);
        long rawStart = -1;
        for (int index = 0; index < sectionCount; ++index)
        {
            ReadOnlySpan<byte> section = sectionTable.AsSpan(
                index * sectionHeaderBytes,
                sectionHeaderBytes);
            uint virtualSize =
                BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(8, 4));
            uint virtualAddress =
                BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(12, 4));
            uint rawSize =
                BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(16, 4));
            uint rawOffset =
                BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(20, 4));
            ulong mappedSize = Math.Max((ulong)virtualSize, rawSize);
            if (debugRva < virtualAddress ||
                (ulong)debugRva >=
                    (ulong)virtualAddress + mappedSize)
            {
                continue;
            }
            if (rawStart >= 0)
            {
                throw new InvalidDataException(
                    "Packaged executable debug directory maps ambiguously.");
            }
            ulong delta = debugRva - virtualAddress;
            ulong rawEnd = (ulong)rawOffset + delta + debugSize;
            if (delta > rawSize ||
                rawEnd > (ulong)rawOffset + rawSize ||
                rawEnd > (ulong)stream.Length)
            {
                throw new InvalidDataException(
                    "Packaged executable debug directory range is invalid.");
            }
            rawStart = checked((long)((ulong)rawOffset + delta));
        }
        if (rawStart < 0)
        {
            throw new InvalidDataException(
                "Packaged executable debug directory is unmapped.");
        }

        Span<byte> value = stackalloc byte[sizeof(uint)];
        BinaryPrimitives.WriteUInt32LittleEndian(value, timeDateStamp);
        for (uint index = 0; index < debugEntryCount; ++index)
        {
            stream.Position =
                rawStart +
                checked((long)index * debugDirectoryEntryBytes) +
                timestampOffset;
            stream.Write(value);
        }
        return checked((int)debugEntryCount);
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
