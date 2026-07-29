// SPDX-License-Identifier: Apache-2.0
// Structural launchability preflight for Windows x64 package executables.

using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Xml;
using System.Xml.Linq;

namespace AcsEditor.Packaging;

public sealed record PackageExecutableProductMetadata(
    string ProductName,
    string ProductVersion,
    string CompanyName,
    string FileDescription,
    string LegalCopyright,
    string SupportUrl,
    string FileVersion,
    string OriginalFilename);

public sealed record PackageApplicationManifestInspection(
    string AssemblyName,
    string AssemblyVersion,
    string ProcessorArchitecture,
    string RequestedExecutionLevel,
    bool UiAccess);

public sealed record PackageExecutableInspection(
    ushort Machine,
    ushort Subsystem,
    ushort SectionCount,
    uint EntryPointRva,
    uint ImageSize,
    uint HeaderSize,
    PackageExecutableProductMetadata? ProductMetadata = null,
    PackageApplicationManifestInspection? ApplicationManifest = null)
{
    internal IReadOnlyList<ushort> VersionResourceLanguages { get; init; } =
        Array.Empty<ushort>();

    internal byte[]? VersionResourceBytes { get; init; }

    internal IReadOnlyList<ushort> ApplicationManifestResourceLanguages
        { get; init; } = Array.Empty<ushort>();
}

public sealed class PackageApplicationManifestException : IOException
{
    public PackageApplicationManifestException(string message)
        : base(message)
    {
    }

    public PackageApplicationManifestException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
}

/// <summary>
/// Performs a bounded, side-effect-free PE header inspection. Package validation
/// must not launch a project-controlled executable merely to decide whether it
/// is safe to publish, but it must also not accept arbitrary bytes named
/// <c>.exe</c>. This contract establishes the Windows x64 loader-facing baseline
/// before archive publication.
/// </summary>
public static class PackageExecutableContract
{
    private const int DosHeaderSize = 64;
    private const int CoffHeaderSize = 20;
    private const int MinimumPe32PlusOptionalHeaderSize = 112;
    private const int MaximumPeHeaderOffset = 1024 * 1024;
    private const ushort Amd64Machine = 0x8664;
    private const ushort Pe32PlusMagic = 0x020b;
    private const ushort ExecutableImage = 0x0002;
    private const ushort DynamicLibrary = 0x2000;
    private const ushort WindowsGuiSubsystem = 2;
    private const ushort WindowsConsoleSubsystem = 3;
    private const int PeSectionHeaderSize = 40;
    private const uint SectionContainsCode = 0x00000020;
    private const uint SectionMemoryExecute = 0x20000000;
    private const int Pe32PlusNumberOfRvaAndSizesOffset = 108;
    private const int Pe32PlusDataDirectoryOffset = 112;
    private const int ResourceDataDirectoryIndex = 2;
    private const int ResourceDataDirectorySize = 8;
    private const int MaximumResourceDirectoryBytes = 64 * 1024 * 1024;
    private const int MaximumMetadataResourceBytes = 4 * 1024 * 1024;
    private const int MaximumResourceDirectoryVisits = 4096;
    private const int MaximumResourceDirectoryEntries = 16 * 1024;
    private const ushort VersionResourceType = 16;
    private const ushort ManifestResourceType = 24;
    private const ushort PrimaryResourceName = 1;

    private readonly record struct PeSection(
        uint VirtualAddress,
        uint VirtualSize,
        uint RawOffset,
        uint RawSize);

    private sealed record ResourceRecord(
        ushort Type,
        ushort Name,
        ushort Language,
        byte[] Bytes);

    private sealed record ResourceInspection(
        IReadOnlyList<ResourceRecord> Versions,
        IReadOnlyList<ResourceRecord> ApplicationManifests);

    private readonly record struct ResourceDirectoryEntry(
        bool HasNumericName,
        uint NumericName,
        bool IsDirectory,
        int TargetOffset);

    private sealed class ResourceTraversalBudget
    {
        private readonly HashSet<int> _visitedDirectories = [];
        private int _remainingEntries = MaximumResourceDirectoryEntries;

        public void EnterDirectory(int offset, int entryCount)
        {
            if (_visitedDirectories.Count >= MaximumResourceDirectoryVisits)
            {
                throw new InvalidDataException(
                    "Package executable PE resource directory exceeds the global visit limit.");
            }
            if (!_visitedDirectories.Add(offset))
            {
                throw new InvalidDataException(
                    "Package executable PE resource directory reuses or cycles through a directory.");
            }
            if (entryCount > _remainingEntries)
            {
                throw new InvalidDataException(
                    "Package executable PE resource directory exceeds the global entry limit.");
            }
            _remainingEntries -= entryCount;
        }
    }

    public static PackageExecutableInspection InspectFile(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        using FileStream stream = new(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read);
        return Inspect(stream, stream.Length);
    }

    /// <summary>
    /// Inspects one complete image stream. Seekable streams must begin at
    /// position zero and <paramref name="declaredLength"/> must equal their
    /// total length. Non-seekable archive-entry streams must likewise be newly
    /// opened at their first byte.
    /// </summary>
    public static PackageExecutableInspection Inspect(
        Stream stream,
        long declaredLength)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (!stream.CanRead)
            throw new InvalidDataException("Package executable stream is not readable.");
        if (stream.CanSeek &&
            (stream.Position != 0 || stream.Length != declaredLength))
        {
            throw new InvalidDataException(
                "Package executable inspection requires a complete stream positioned at byte zero.");
        }
        if (declaredLength < DosHeaderSize + 4 + CoffHeaderSize)
            throw new InvalidDataException("Package executable is shorter than a PE image header.");

        Span<byte> dos = stackalloc byte[DosHeaderSize];
        ReadExactly(stream, dos);
        if (dos[0] != (byte)'M' || dos[1] != (byte)'Z')
            throw new InvalidDataException("Package executable is missing the DOS MZ signature.");

        int peOffset = BinaryPrimitives.ReadInt32LittleEndian(dos[0x3c..]);
        if (peOffset < DosHeaderSize ||
            peOffset > MaximumPeHeaderOffset ||
            peOffset > declaredLength - 4 - CoffHeaderSize)
        {
            throw new InvalidDataException(
                "Package executable has an invalid or unbounded PE header offset.");
        }

        SkipExactly(stream, peOffset - DosHeaderSize);
        Span<byte> signature = stackalloc byte[4];
        ReadExactly(stream, signature);
        if (signature[0] != (byte)'P' ||
            signature[1] != (byte)'E' ||
            signature[2] != 0 ||
            signature[3] != 0)
        {
            throw new InvalidDataException("Package executable is missing the PE signature.");
        }

        Span<byte> coff = stackalloc byte[CoffHeaderSize];
        ReadExactly(stream, coff);
        ushort machine = BinaryPrimitives.ReadUInt16LittleEndian(coff);
        ushort sectionCount = BinaryPrimitives.ReadUInt16LittleEndian(coff[2..]);
        ushort optionalHeaderSize =
            BinaryPrimitives.ReadUInt16LittleEndian(coff[16..]);
        ushort characteristics =
            BinaryPrimitives.ReadUInt16LittleEndian(coff[18..]);

        if (machine != Amd64Machine)
            throw new InvalidDataException(
                $"Package executable machine 0x{machine:x4} is not Windows x64 (AMD64).");
        if (sectionCount is 0 or > 96)
            throw new InvalidDataException(
                "Package executable has an invalid PE section count.");
        if ((characteristics & ExecutableImage) == 0 ||
            (characteristics & DynamicLibrary) != 0)
        {
            throw new InvalidDataException(
                "Package executable PE characteristics do not describe an executable image.");
        }
        if (optionalHeaderSize < MinimumPe32PlusOptionalHeaderSize ||
            peOffset + 4L + CoffHeaderSize + optionalHeaderSize > declaredLength)
        {
            throw new InvalidDataException(
                "Package executable has a truncated PE32+ optional header.");
        }

        byte[] optional = new byte[optionalHeaderSize];
        ReadExactly(stream, optional);
        ushort magic = BinaryPrimitives.ReadUInt16LittleEndian(optional);
        uint entryPoint = BinaryPrimitives.ReadUInt32LittleEndian(optional.AsSpan(16));
        uint imageSize = BinaryPrimitives.ReadUInt32LittleEndian(optional.AsSpan(56));
        uint headerSize = BinaryPrimitives.ReadUInt32LittleEndian(optional.AsSpan(60));
        ushort subsystem = BinaryPrimitives.ReadUInt16LittleEndian(optional.AsSpan(68));
        long sectionTableEnd =
            peOffset + 4L + CoffHeaderSize + optionalHeaderSize +
            sectionCount * PeSectionHeaderSize;

        if (magic != Pe32PlusMagic)
            throw new InvalidDataException(
                "Package executable is not a PE32+ image.");
        if (entryPoint == 0 || entryPoint >= imageSize)
            throw new InvalidDataException(
                "Package executable does not declare an in-image entry point.");
        if (subsystem is not (WindowsGuiSubsystem or WindowsConsoleSubsystem))
            throw new InvalidDataException(
                $"Package executable subsystem {subsystem} is not Windows GUI or console.");
        if (headerSize == 0 ||
            headerSize > declaredLength ||
            imageSize < headerSize ||
            sectionTableEnd > declaredLength ||
            headerSize < sectionTableEnd)
        {
            throw new InvalidDataException(
                "Package executable image/header sizes are inconsistent.");
        }

        byte[] sectionTable =
            new byte[checked(sectionCount * PeSectionHeaderSize)];
        ReadExactly(stream, sectionTable);
        bool entryPointIsExecutableCode = false;
        var sections = new List<PeSection>(sectionCount);
        for (int index = 0; index < sectionCount; index++)
        {
            ReadOnlySpan<byte> section = sectionTable.AsSpan(
                index * PeSectionHeaderSize,
                PeSectionHeaderSize);
            uint virtualSize =
                BinaryPrimitives.ReadUInt32LittleEndian(section[8..]);
            uint virtualAddress =
                BinaryPrimitives.ReadUInt32LittleEndian(section[12..]);
            uint rawSize =
                BinaryPrimitives.ReadUInt32LittleEndian(section[16..]);
            uint rawOffset =
                BinaryPrimitives.ReadUInt32LittleEndian(section[20..]);
            uint sectionCharacteristics =
                BinaryPrimitives.ReadUInt32LittleEndian(section[36..]);
            sections.Add(new(
                virtualAddress,
                virtualSize,
                rawOffset,
                rawSize));

            ulong mappedSize = Math.Max((ulong)virtualSize, rawSize);
            ulong virtualEnd = (ulong)virtualAddress + mappedSize;
            if (mappedSize == 0 ||
                virtualAddress < headerSize ||
                virtualAddress >= imageSize ||
                virtualEnd > imageSize)
            {
                throw new InvalidDataException(
                    $"Package executable section {index} has an invalid virtual image range.");
            }

            if (rawSize > 0)
            {
                ulong rawEnd = (ulong)rawOffset + rawSize;
                if (rawOffset < headerSize || rawEnd > (ulong)declaredLength)
                {
                    throw new InvalidDataException(
                        $"Package executable section {index} has an invalid raw file range.");
                }
            }

            bool containsEntryPoint =
                entryPoint >= virtualAddress &&
                (ulong)entryPoint < virtualEnd;
            if (containsEntryPoint &&
                rawSize > 0 &&
                (ulong)entryPoint - virtualAddress < rawSize &&
                (sectionCharacteristics & SectionContainsCode) != 0 &&
                (sectionCharacteristics & SectionMemoryExecute) != 0)
            {
                entryPointIsExecutableCode = true;
            }
        }
        if (!entryPointIsExecutableCode)
        {
            throw new InvalidDataException(
                "Package executable entry point is not inside an executable code section.");
        }

        uint resourceRva = 0;
        uint resourceSize = 0;
        uint numberOfRvaAndSizes =
            BinaryPrimitives.ReadUInt32LittleEndian(
                optional.AsSpan(Pe32PlusNumberOfRvaAndSizesOffset));
        uint availableDataDirectories = checked(
            (uint)((optional.Length - Pe32PlusDataDirectoryOffset) /
                   ResourceDataDirectorySize));
        if (numberOfRvaAndSizes > availableDataDirectories)
        {
            throw new InvalidDataException(
                "Package executable declares more PE data directories than its optional header contains.");
        }
        int resourceDirectoryOffset =
            Pe32PlusDataDirectoryOffset +
            ResourceDataDirectoryIndex * ResourceDataDirectorySize;
        if (numberOfRvaAndSizes > ResourceDataDirectoryIndex)
        {
            resourceRva = BinaryPrimitives.ReadUInt32LittleEndian(
                optional.AsSpan(resourceDirectoryOffset));
            resourceSize = BinaryPrimitives.ReadUInt32LittleEndian(
                optional.AsSpan(resourceDirectoryOffset + 4));
        }
        ResourceInspection resources = ReadResourceInspection(
            stream,
            declaredLength,
            sectionTableEnd,
            resourceRva,
            resourceSize,
            sections);

        PackageExecutableProductMetadata? productMetadata = null;
        byte[]? versionBytes = null;
        if (resources.Versions.Count > 0)
        {
            PackageExecutableProductMetadata[] versions =
                resources.Versions
                    .Select(record => ParseVersionInfo(record.Bytes))
                    .ToArray();
            productMetadata = versions[0];
            if (versions.Any(candidate => candidate != productMetadata))
            {
                throw new InvalidDataException(
                    "Package executable contains conflicting VERSIONINFO languages.");
            }
            versionBytes = resources.Versions[0].Bytes;
        }

        PackageApplicationManifestInspection? applicationManifest = null;
        if (resources.ApplicationManifests.Count > 1)
        {
            throw new PackageApplicationManifestException(
                "Package executable contains multiple application-manifest languages.");
        }
        if (resources.ApplicationManifests.Count == 1)
        {
            applicationManifest = ParseApplicationManifest(
                resources.ApplicationManifests[0].Bytes);
        }

        return new(
            machine,
            subsystem,
            sectionCount,
            entryPoint,
            imageSize,
            headerSize,
            productMetadata,
            applicationManifest)
        {
            VersionResourceLanguages =
                resources.Versions.Select(record => record.Language).ToArray(),
            VersionResourceBytes = versionBytes,
            ApplicationManifestResourceLanguages =
                resources.ApplicationManifests
                    .Select(record => record.Language)
                    .ToArray(),
        };
    }

    private static ResourceInspection ReadResourceInspection(
        Stream stream,
        long declaredLength,
        long currentOffset,
        uint resourceRva,
        uint resourceSize,
        IReadOnlyList<PeSection> sections)
    {
        if (resourceRva == 0 && resourceSize == 0)
        {
            return new(
                Array.Empty<ResourceRecord>(),
                Array.Empty<ResourceRecord>());
        }
        if (resourceRva == 0 ||
            resourceSize == 0 ||
            resourceSize > MaximumResourceDirectoryBytes)
        {
            throw new InvalidDataException(
                "Package executable has an invalid or unbounded PE resource directory.");
        }

        PeSection? containingSection = null;
        foreach (PeSection section in sections)
        {
            ulong mappedSize = Math.Max(
                (ulong)section.VirtualSize,
                section.RawSize);
            if (resourceRva >= section.VirtualAddress &&
                (ulong)resourceRva < (ulong)section.VirtualAddress + mappedSize)
            {
                if (containingSection is not null)
                {
                    throw new InvalidDataException(
                        "Package executable PE resource directory maps to overlapping sections.");
                }
                containingSection = section;
            }
        }
        if (containingSection is not { } resourceSection)
        {
            throw new InvalidDataException(
                "Package executable PE resource directory is outside its image sections.");
        }

        ulong sectionDelta = resourceRva - resourceSection.VirtualAddress;
        ulong rawStart = (ulong)resourceSection.RawOffset + sectionDelta;
        ulong rawEnd = rawStart + resourceSize;
        ulong sectionRawEnd =
            (ulong)resourceSection.RawOffset + resourceSection.RawSize;
        if (sectionDelta > resourceSection.RawSize ||
            rawEnd > sectionRawEnd ||
            rawEnd > (ulong)declaredLength ||
            rawStart < (ulong)currentOffset)
        {
            throw new InvalidDataException(
                "Package executable PE resource directory has an invalid raw file range.");
        }

        SkipExactly(stream, checked((long)rawStart - currentOffset));
        byte[] bytes = new byte[checked((int)resourceSize)];
        ReadExactly(stream, bytes);
        return ParseResourceDirectory(bytes, resourceRva);
    }

    private static ResourceInspection ParseResourceDirectory(
        byte[] directory,
        uint resourceRva)
    {
        var traversal = new ResourceTraversalBudget();
        var versions = new List<ResourceRecord>();
        var manifests = new List<ResourceRecord>();
        var identities = new HashSet<(ushort Type, ushort Name, ushort Language)>();
        foreach (ResourceDirectoryEntry typeEntry in
                 ReadResourceDirectoryEntries(directory, 0, traversal))
        {
            if (!typeEntry.HasNumericName ||
                typeEntry.NumericName is not (
                    VersionResourceType or ManifestResourceType))
            {
                continue;
            }
            ushort type = checked((ushort)typeEntry.NumericName);
            if (!typeEntry.IsDirectory)
            {
                throw new InvalidDataException(
                    $"Package executable resource type {type} does not contain a name directory.");
            }

            foreach (ResourceDirectoryEntry nameEntry in
                     ReadResourceDirectoryEntries(
                         directory,
                         typeEntry.TargetOffset,
                         traversal))
            {
                if (type == ManifestResourceType &&
                    (!nameEntry.HasNumericName ||
                     nameEntry.NumericName != PrimaryResourceName))
                {
                    // Resource IDs other than CREATEPROCESS_MANIFEST_RESOURCE_ID
                    // do not control process activation and are left untouched.
                    continue;
                }
                if (!nameEntry.HasNumericName ||
                    nameEntry.NumericName != PrimaryResourceName)
                {
                    throw new InvalidDataException(
                        "Package executable VERSIONINFO must use resource ID 1.");
                }
                if (!nameEntry.IsDirectory)
                {
                    throw new InvalidDataException(
                        $"Package executable resource type {type} does not contain a language directory.");
                }

                foreach (ResourceDirectoryEntry languageEntry in
                         ReadResourceDirectoryEntries(
                             directory,
                             nameEntry.TargetOffset,
                             traversal))
                {
                    if (!languageEntry.HasNumericName ||
                        languageEntry.NumericName > ushort.MaxValue ||
                        languageEntry.IsDirectory)
                    {
                        throw new InvalidDataException(
                            $"Package executable resource type {type} has an invalid language entry.");
                    }
                    ushort language = checked((ushort)languageEntry.NumericName);
                    if (!identities.Add((type, PrimaryResourceName, language)))
                    {
                        throw new InvalidDataException(
                            $"Package executable resource type {type} duplicates language {language}.");
                    }
                    byte[] payload = ReadResourcePayload(
                        directory,
                        languageEntry.TargetOffset,
                        resourceRva);
                    var record = new ResourceRecord(
                        type,
                        PrimaryResourceName,
                        language,
                        payload);
                    if (type == VersionResourceType)
                        versions.Add(record);
                    else
                        manifests.Add(record);
                }
            }
        }
        return new(versions, manifests);
    }

    internal static void ValidateResourceDirectoryForSelfTest(
        byte[] directory,
        uint resourceRva = 0x1000)
    {
        ArgumentNullException.ThrowIfNull(directory);
        _ = ParseResourceDirectory(directory, resourceRva);
    }

    private static IReadOnlyList<ResourceDirectoryEntry>
        ReadResourceDirectoryEntries(
            byte[] directory,
            int offset,
            ResourceTraversalBudget traversal)
    {
        EnsureRange(directory, offset, 16, "resource directory header");
        ushort namedCount = BinaryPrimitives.ReadUInt16LittleEndian(
            directory.AsSpan(offset + 12));
        ushort idCount = BinaryPrimitives.ReadUInt16LittleEndian(
            directory.AsSpan(offset + 14));
        int count = checked(namedCount + idCount);
        if (count > 4096)
        {
            throw new InvalidDataException(
                "Package executable PE resource directory has too many entries.");
        }
        traversal.EnterDirectory(offset, count);
        EnsureRange(
            directory,
            offset + 16,
            checked(count * 8),
            "resource directory entries");

        var entries = new List<ResourceDirectoryEntry>(count);
        for (int index = 0; index < count; index++)
        {
            int entryOffset = offset + 16 + index * 8;
            uint name = BinaryPrimitives.ReadUInt32LittleEndian(
                directory.AsSpan(entryOffset));
            uint target = BinaryPrimitives.ReadUInt32LittleEndian(
                directory.AsSpan(entryOffset + 4));
            bool hasNumericName = (name & 0x80000000u) == 0;
            if (!hasNumericName)
            {
                uint nameOffset = name & 0x7fffffffu;
                if (nameOffset > int.MaxValue)
                {
                    throw new InvalidDataException(
                        "Package executable resource name offset exceeds supported bounds.");
                }
                ValidateResourceString(directory, (int)nameOffset);
            }
            uint rawTargetOffset = target & 0x7fffffffu;
            if (rawTargetOffset > int.MaxValue)
            {
                throw new InvalidDataException(
                    "Package executable resource target offset exceeds supported bounds.");
            }
            int targetOffset = (int)rawTargetOffset;
            EnsureRange(
                directory,
                targetOffset,
                (target & 0x80000000u) != 0 ? 16 : 16,
                "resource directory target");
            entries.Add(new(
                hasNumericName,
                name & 0x7fffffffu,
                (target & 0x80000000u) != 0,
                targetOffset));
        }
        return entries;
    }

    private static void ValidateResourceString(byte[] directory, int offset)
    {
        EnsureRange(directory, offset, 2, "resource name");
        ushort characters = BinaryPrimitives.ReadUInt16LittleEndian(
            directory.AsSpan(offset));
        if (characters > 1024)
        {
            throw new InvalidDataException(
                "Package executable resource name exceeds the supported bound.");
        }
        EnsureRange(
            directory,
            offset + 2,
            checked(characters * 2),
            "resource name");
    }

    private static byte[] ReadResourcePayload(
        byte[] directory,
        int dataEntryOffset,
        uint resourceRva)
    {
        EnsureRange(directory, dataEntryOffset, 16, "resource data entry");
        uint dataRva = BinaryPrimitives.ReadUInt32LittleEndian(
            directory.AsSpan(dataEntryOffset));
        uint size = BinaryPrimitives.ReadUInt32LittleEndian(
            directory.AsSpan(dataEntryOffset + 4));
        if (dataRva < resourceRva ||
            size == 0 ||
            size > MaximumMetadataResourceBytes)
        {
            throw new InvalidDataException(
                "Package executable metadata resource has an invalid size or RVA.");
        }
        uint relative = dataRva - resourceRva;
        if (relative > int.MaxValue || size > int.MaxValue)
        {
            throw new InvalidDataException(
                "Package executable metadata resource exceeds supported offsets.");
        }
        EnsureRange(
            directory,
            checked((int)relative),
            checked((int)size),
            "resource payload");
        return directory.AsSpan(
            checked((int)relative),
            checked((int)size)).ToArray();
    }

    private static PackageExecutableProductMetadata ParseVersionInfo(byte[] bytes)
    {
        VersionBlock root = ReadVersionBlock(bytes, 0, bytes.Length);
        if (root.Length != bytes.Length ||
            root.Key != "VS_VERSION_INFO" ||
            root.Type != 0 ||
            root.ValueLength != 52)
        {
            throw new InvalidDataException(
                "Package executable VERSIONINFO root is invalid.");
        }
        ReadOnlySpan<byte> fixedInfo = bytes.AsSpan(root.ValueOffset, 52);
        if (BinaryPrimitives.ReadUInt32LittleEndian(fixedInfo) != 0xfeef04bdu ||
            BinaryPrimitives.ReadUInt32LittleEndian(fixedInfo[4..]) != 0x00010000u)
        {
            throw new InvalidDataException(
                "Package executable VERSIONINFO fixed header is invalid.");
        }
        string fixedFileVersion = FormatFixedVersion(
            BinaryPrimitives.ReadUInt32LittleEndian(fixedInfo[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(fixedInfo[12..]));

        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (VersionBlock child in EnumerateVersionChildren(bytes, root))
        {
            if (child.Key != "StringFileInfo")
                continue;
            foreach (VersionBlock table in EnumerateVersionChildren(bytes, child))
            {
                foreach (VersionBlock value in EnumerateVersionChildren(bytes, table))
                {
                    if (value.Type != 1 || value.ValueLength == 0)
                    {
                        throw new InvalidDataException(
                            "Package executable VERSIONINFO string value is invalid.");
                    }
                    int valueBytes = checked(value.ValueLength * 2);
                    ReadOnlySpan<byte> encoded =
                        bytes.AsSpan(value.ValueOffset, valueBytes);
                    if (encoded[^2] != 0 || encoded[^1] != 0)
                    {
                        throw new InvalidDataException(
                            "Package executable VERSIONINFO string is not terminated.");
                    }
                    string text = Encoding.Unicode.GetString(encoded[..^2]);
                    if (text.Contains('\0'))
                    {
                        throw new InvalidDataException(
                            "Package executable VERSIONINFO string contains an embedded NUL.");
                    }
                    if (values.TryGetValue(value.Key, out string? prior) &&
                        !string.Equals(prior, text, StringComparison.Ordinal))
                    {
                        throw new InvalidDataException(
                            $"Package executable VERSIONINFO conflicts for '{value.Key}'.");
                    }
                    values[value.Key] = text;
                }
            }
        }

        if (!values.TryGetValue("ProductName", out string? productName) ||
            !values.TryGetValue("ProductVersion", out string? productVersion))
        {
            throw new InvalidDataException(
                "Package executable VERSIONINFO is missing product identity strings.");
        }
        return new(
            productName,
            productVersion,
            ValueOrEmpty(values, "CompanyName"),
            ValueOrEmpty(values, "FileDescription"),
            ValueOrEmpty(values, "LegalCopyright"),
            ValueOrEmpty(values, "SupportUrl"),
            values.TryGetValue("FileVersion", out string? fileVersion)
                ? fileVersion
                : fixedFileVersion,
            ValueOrEmpty(values, "OriginalFilename"));
    }

    private sealed record VersionBlock(
        int Offset,
        int Length,
        ushort ValueLength,
        ushort Type,
        string Key,
        int ValueOffset,
        int ChildrenOffset);

    private static VersionBlock ReadVersionBlock(
        byte[] bytes,
        int offset,
        int containingEnd)
    {
        EnsureRange(bytes, offset, 6, "VERSIONINFO block header");
        ushort length = BinaryPrimitives.ReadUInt16LittleEndian(
            bytes.AsSpan(offset));
        ushort valueLength = BinaryPrimitives.ReadUInt16LittleEndian(
            bytes.AsSpan(offset + 2));
        ushort type = BinaryPrimitives.ReadUInt16LittleEndian(
            bytes.AsSpan(offset + 4));
        if (length < 8 || offset + length > containingEnd)
        {
            throw new InvalidDataException(
                "Package executable VERSIONINFO block length is invalid.");
        }

        int cursor = offset + 6;
        var key = new StringBuilder();
        bool terminated = false;
        while (cursor + 2 <= offset + length)
        {
            ushort character = BinaryPrimitives.ReadUInt16LittleEndian(
                bytes.AsSpan(cursor));
            cursor += 2;
            if (character == 0)
            {
                terminated = true;
                break;
            }
            key.Append((char)character);
            if (key.Length > 1024)
            {
                throw new InvalidDataException(
                    "Package executable VERSIONINFO key exceeds the supported bound.");
            }
        }
        if (!terminated)
        {
            throw new InvalidDataException(
                "Package executable VERSIONINFO key is not terminated.");
        }

        int valueOffset = Align4(cursor);
        int valueBytes = type == 1
            ? checked(valueLength * 2)
            : valueLength;
        if (valueOffset > offset + length ||
            valueBytes > offset + length - valueOffset)
        {
            throw new InvalidDataException(
                "Package executable VERSIONINFO value exceeds its block.");
        }
        int childrenOffset = Align4(valueOffset + valueBytes);
        if (childrenOffset > offset + length)
            childrenOffset = offset + length;
        return new(
            offset,
            length,
            valueLength,
            type,
            key.ToString(),
            valueOffset,
            childrenOffset);
    }

    private static IEnumerable<VersionBlock> EnumerateVersionChildren(
        byte[] bytes,
        VersionBlock parent)
    {
        int cursor = parent.ChildrenOffset;
        int end = parent.Offset + parent.Length;
        while (cursor < end)
        {
            if (bytes.AsSpan(cursor, end - cursor).IndexOfAnyExcept((byte)0) < 0)
                yield break;
            VersionBlock child = ReadVersionBlock(bytes, cursor, end);
            yield return child;
            int childEnd = cursor + child.Length;
            if (childEnd == end)
                yield break;
            cursor = Align4(childEnd);
            if (cursor > end)
            {
                throw new InvalidDataException(
                    "Package executable VERSIONINFO child alignment exceeds its parent.");
            }
        }
    }

    private static PackageApplicationManifestInspection ParseApplicationManifest(
        byte[] bytes)
    {
        try
        {
            using var stream = new MemoryStream(bytes, writable: false);
            using XmlReader reader = XmlReader.Create(
                stream,
                new XmlReaderSettings
                {
                    DtdProcessing = DtdProcessing.Prohibit,
                    IgnoreComments = false,
                    IgnoreWhitespace = false,
                    MaxCharactersInDocument = MaximumMetadataResourceBytes,
                    XmlResolver = null,
                });
            XDocument document = XDocument.Load(
                reader,
                LoadOptions.PreserveWhitespace | LoadOptions.SetLineInfo);
            XNamespace asm1 = "urn:schemas-microsoft-com:asm.v1";
            XElement root = document.Root ??
                throw new PackageApplicationManifestException(
                    "Package executable application manifest is empty.");
            if (root.Name != asm1 + "assembly" ||
                (string?)root.Attribute("manifestVersion") != "1.0")
            {
                throw new PackageApplicationManifestException(
                    "Package executable application manifest root is invalid.");
            }

            XElement[] identities =
                root.Elements(asm1 + "assemblyIdentity").ToArray();
            if (identities.Length > 1)
            {
                throw new PackageApplicationManifestException(
                    "Package executable application manifest must contain at most one assemblyIdentity.");
            }
            string name = "";
            string version = "";
            string architecture = "*";
            if (identities.Length == 1)
            {
                XElement identity = identities[0];
                name = (string?)identity.Attribute("name") ?? "";
                version = (string?)identity.Attribute("version") ?? "";
                architecture =
                    (string?)identity.Attribute("processorArchitecture") ?? "*";
                string type = (string?)identity.Attribute("type") ?? "win32";
                if (string.IsNullOrWhiteSpace(name) ||
                    !IsManifestVersion(version) ||
                    !string.Equals(type, "win32", StringComparison.Ordinal) ||
                    architecture is not ("amd64" or "*"))
                {
                    throw new PackageApplicationManifestException(
                        "Package executable application manifest identity is not Windows x64 compatible.");
                }
            }

            XElement[] executionLevels = root
                .Descendants()
                .Where(element =>
                    element.Name.LocalName == "requestedExecutionLevel")
                .ToArray();
            if (executionLevels.Length > 1)
            {
                throw new PackageApplicationManifestException(
                    "Package executable application manifest declares multiple execution levels.");
            }
            string level = "asInvoker";
            bool uiAccess = false;
            if (executionLevels.Length == 1)
            {
                XElement executionLevel = executionLevels[0];
                level = (string?)executionLevel.Attribute("level") ?? "";
                string uiAccessValue =
                    (string?)executionLevel.Attribute("uiAccess") ?? "false";
                if (uiAccessValue is not ("true" or "false"))
                {
                    throw new PackageApplicationManifestException(
                        "Package executable application manifest uiAccess value is invalid.");
                }
                uiAccess = uiAccessValue == "true";
            }
            if (!string.Equals(level, "asInvoker", StringComparison.Ordinal) ||
                uiAccess)
            {
                throw new PackageApplicationManifestException(
                    "Package executable application manifest must use asInvoker with uiAccess=false.");
            }
            return new(name, version, architecture, level, uiAccess);
        }
        catch (PackageApplicationManifestException)
        {
            throw;
        }
        catch (Exception error) when (
            error is XmlException or InvalidOperationException or
                ArgumentException or DecoderFallbackException)
        {
            throw new PackageApplicationManifestException(
                "Package executable application manifest is malformed.",
                error);
        }
    }

    private static bool IsManifestVersion(string value)
    {
        string[] parts = value.Split('.');
        return parts.Length == 4 &&
               parts.All(part =>
                   ushort.TryParse(
                       part,
                       System.Globalization.NumberStyles.None,
                       System.Globalization.CultureInfo.InvariantCulture,
                       out _));
    }

    private static string FormatFixedVersion(uint mostSignificant, uint leastSignificant) =>
        string.Create(
            System.Globalization.CultureInfo.InvariantCulture,
            $"{mostSignificant >> 16}.{mostSignificant & 0xffff}." +
            $"{leastSignificant >> 16}.{leastSignificant & 0xffff}");

    private static string ValueOrEmpty(
        IReadOnlyDictionary<string, string> values,
        string key) =>
        values.TryGetValue(key, out string? value) ? value : "";

    private static int Align4(int value) => checked((value + 3) & ~3);

    private static void EnsureRange(
        byte[] bytes,
        int offset,
        int length,
        string description)
    {
        if (offset < 0 ||
            length < 0 ||
            offset > bytes.Length ||
            length > bytes.Length - offset)
        {
            throw new InvalidDataException(
                $"Package executable {description} exceeds its PE resource bounds.");
        }
    }

    private static void ReadExactly(Stream stream, Span<byte> destination)
    {
        while (!destination.IsEmpty)
        {
            int read = stream.Read(destination);
            if (read <= 0)
                throw new EndOfStreamException(
                    "Package executable ended inside its PE headers.");
            destination = destination[read..];
        }
    }

    private static void ReadExactly(Stream stream, byte[] destination) =>
        ReadExactly(stream, destination.AsSpan());

    private static void SkipExactly(Stream stream, long bytes)
    {
        Span<byte> scratch = stackalloc byte[4096];
        while (bytes > 0)
        {
            int chunk = (int)Math.Min(bytes, scratch.Length);
            ReadExactly(stream, scratch[..chunk]);
            bytes -= chunk;
        }
    }
}
