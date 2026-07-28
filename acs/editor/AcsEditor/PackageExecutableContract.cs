// SPDX-License-Identifier: Apache-2.0
// Structural launchability preflight for Windows x64 package executables.

using System;
using System.Buffers.Binary;
using System.IO;

namespace AcsEditor.Packaging;

public sealed record PackageExecutableInspection(
    ushort Machine,
    ushort Subsystem,
    ushort SectionCount,
    uint EntryPointRva,
    uint ImageSize,
    uint HeaderSize);

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

        return new(
            machine,
            subsystem,
            sectionCount,
            entryPoint,
            imageSize,
            headerSize);
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

    private static void SkipExactly(Stream stream, int bytes)
    {
        Span<byte> scratch = stackalloc byte[4096];
        while (bytes > 0)
        {
            int chunk = Math.Min(bytes, scratch.Length);
            ReadExactly(stream, scratch[..chunk]);
            bytes -= chunk;
        }
    }
}
