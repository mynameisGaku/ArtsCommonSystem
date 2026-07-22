// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor.Packaging;

public sealed record RuntimeDependencyResolution(
    IReadOnlyList<string> Dependencies,
    IReadOnlyList<string> Unresolved);

/// <summary>
/// Uses CMake's PE dependency scanner after a successful Release build. Windows
/// system DLLs are intentionally excluded; only redistributable, non-system
/// dependencies are returned to the staging layer.
/// </summary>
public static class RuntimeDependencyResolver
{
    private static readonly UTF8Encoding Utf8NoBom = new(false);

    public static async Task<RuntimeDependencyResolution> ResolveAsync(
        string executablePath,
        IEnumerable<string> searchDirectories,
        Action<string>? log = null,
        CancellationToken cancellationToken = default)
    {
        string executable = Path.GetFullPath(executablePath);
        if (!File.Exists(executable))
            throw new FileNotFoundException("依存関係を調べる実行ファイルが見つかりません。", executable);

        string workDirectory = Path.Combine(
            Path.GetTempPath(),
            "acs-runtime-deps-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(workDirectory);
        string script = Path.Combine(workDirectory, "resolve.cmake");
        string resolvedFile = Path.Combine(workDirectory, "resolved.txt");
        string unresolvedFile = Path.Combine(workDirectory, "unresolved.txt");

        try
        {
            var directories = searchDirectories
                .Where(Directory.Exists)
                .Select(Path.GetFullPath)
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .OrderBy(path => path, StringComparer.Ordinal)
                .ToArray();

            var source = new StringBuilder();
            source.AppendLine("cmake_minimum_required(VERSION 3.21)");
            source.AppendLine("if(POLICY CMP0207)");
            source.AppendLine("  cmake_policy(SET CMP0207 NEW)");
            source.AppendLine("endif()");
            source.AppendLine("file(GET_RUNTIME_DEPENDENCIES");
            source.Append("  EXECUTABLES ").AppendLine(CMakeLiteral(executable));
            source.AppendLine("  RESOLVED_DEPENDENCIES_VAR ACS_RESOLVED");
            source.AppendLine("  UNRESOLVED_DEPENDENCIES_VAR ACS_UNRESOLVED");
            if (directories.Length > 0)
            {
                source.AppendLine("  DIRECTORIES");
                foreach (string directory in directories)
                    source.Append("    ").AppendLine(CMakeLiteral(directory));
            }
            source.AppendLine("  PRE_EXCLUDE_REGEXES \"^(api-ms-|ext-ms-)\"");
            // MSVC runtime is not an OS component. Include it before the broad
            // Windows-system exclusion, then replace the resolved System32 copy
            // with the matching file from Visual Studio's licensed Redist tree.
            source.AppendLine("  POST_INCLUDE_REGEXES");
            source.AppendLine("    \".*[/][Mm][Ss][Vv][Cc][Pp][0-9_]*\\\\.[Dd][Ll][Ll]$\"");
            source.AppendLine("    \".*[/][Vv][Cc][Rr][Uu][Nn][Tt][Ii][Mm][Ee][0-9_]*\\\\.[Dd][Ll][Ll]$\"");
            source.AppendLine("    \".*[/][Cc][Oo][Nn][Cc][Rr][Tt][0-9_]*\\\\.[Dd][Ll][Ll]$\"");
            source.AppendLine("  POST_EXCLUDE_REGEXES");
            source.AppendLine("    \".*[Ss][Yy][Ss][Tt][Ee][Mm]32[\\\\\\\\/].*\"");
            source.AppendLine("    \".*[Ss][Yy][Ss][Ww][Oo][Ww]64[\\\\\\\\/].*\"");
            source.AppendLine("    \".*[Ww][Ii][Nn][Ss][Xx][Ss][\\\\\\\\/].*\"");
            source.AppendLine(")");
            source.AppendLine("list(REMOVE_DUPLICATES ACS_RESOLVED)");
            source.AppendLine("list(SORT ACS_RESOLVED)");
            source.Append("file(WRITE ").Append(CMakeLiteral(resolvedFile)).AppendLine(" \"\")");
            source.AppendLine("foreach(ACS_DEP IN LISTS ACS_RESOLVED)");
            source.Append("  file(APPEND ").Append(CMakeLiteral(resolvedFile)).AppendLine(" \"${ACS_DEP}\\n\")");
            source.AppendLine("endforeach()");
            source.AppendLine("list(REMOVE_DUPLICATES ACS_UNRESOLVED)");
            source.AppendLine("list(SORT ACS_UNRESOLVED)");
            source.Append("file(WRITE ").Append(CMakeLiteral(unresolvedFile)).AppendLine(" \"\")");
            source.AppendLine("foreach(ACS_DEP IN LISTS ACS_UNRESOLVED)");
            source.Append("  file(APPEND ").Append(CMakeLiteral(unresolvedFile)).AppendLine(" \"${ACS_DEP}\\n\")");
            source.AppendLine("endforeach()");
            await File.WriteAllTextAsync(script, source.ToString(), Utf8NoBom, cancellationToken);

            var start = new ProcessStartInfo
            {
                FileName = "cmake",
                WorkingDirectory = workDirectory,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            start.ArgumentList.Add("-P");
            start.ArgumentList.Add(script);

            using var process = new Process { StartInfo = start };
            process.OutputDataReceived += (_, eventArgs) =>
            {
                if (!string.IsNullOrEmpty(eventArgs.Data))
                    log?.Invoke(eventArgs.Data);
            };
            process.ErrorDataReceived += (_, eventArgs) =>
            {
                if (!string.IsNullOrEmpty(eventArgs.Data))
                    log?.Invoke(eventArgs.Data);
            };

            if (!process.Start())
                throw new InvalidOperationException("CMake dependency scanner を起動できませんでした。");
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();

            try
            {
                await process.WaitForExitAsync(cancellationToken);
            }
            catch (OperationCanceledException)
            {
                try
                {
                    if (!process.HasExited)
                        process.Kill(entireProcessTree: true);
                }
                catch { }
                throw;
            }

            if (process.ExitCode != 0)
                throw new InvalidOperationException($"CMake dependency scanner が失敗しました (exit {process.ExitCode})。");

            var unresolvedList = ReadLines(unresolvedFile)
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToList();
            var resolvedList = new List<string>();
            foreach (string dependency in ReadLines(resolvedFile)
                         .Where(path => path.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
                         .Where(path => !Path.GetFileName(path).EndsWith(
                             "_reflect.dll",
                             StringComparison.OrdinalIgnoreCase)))
            {
                string selected = dependency;
                string name = Path.GetFileName(dependency);
                if (IsMsvcRuntime(name) && IsWindowsSystemPath(dependency))
                {
                    string? redistributable = FindMsvcRedistributable(name);
                    if (redistributable == null)
                    {
                        unresolvedList.Add(
                            $"{name} (Visual Studio x64 Redist source not found)");
                        continue;
                    }
                    selected = redistributable;
                }

                if (!IsWindowsSystemPath(selected))
                    resolvedList.Add(Path.GetFullPath(selected));
            }

            string[] resolved = resolvedList
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .OrderBy(path => Path.GetFileName(path), StringComparer.Ordinal)
                .ThenBy(path => path, StringComparer.Ordinal)
                .ToArray();
            string[] unresolved = unresolvedList
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .OrderBy(name => name, StringComparer.Ordinal)
                .ToArray();

            return new(resolved, unresolved);
        }
        finally
        {
            TryDelete(script);
            TryDelete(resolvedFile);
            TryDelete(unresolvedFile);
            try { Directory.Delete(workDirectory, recursive: false); } catch { }
        }
    }

    private static IEnumerable<string> ReadLines(string path) =>
        File.Exists(path)
            ? File.ReadLines(path, Encoding.UTF8).Where(line => !string.IsNullOrWhiteSpace(line))
            : Array.Empty<string>();

    private static string CMakeLiteral(string value)
    {
        string normalized = value.Replace('\\', '/');
        int equalsCount = 1;
        while (normalized.Contains("]" + new string('=', equalsCount) + "]", StringComparison.Ordinal))
            equalsCount++;
        string equals = new('=', equalsCount);
        return $"[{equals}[{normalized}]{equals}]";
    }

    private static bool IsMsvcRuntime(string fileName) =>
        fileName.StartsWith("msvcp", StringComparison.OrdinalIgnoreCase) ||
        fileName.StartsWith("vcruntime", StringComparison.OrdinalIgnoreCase) ||
        fileName.StartsWith("concrt", StringComparison.OrdinalIgnoreCase);

    private static bool IsWindowsSystemPath(string path)
    {
        string windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        if (string.IsNullOrEmpty(windows))
            return false;
        string normalizedWindows = Path.GetFullPath(windows)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) +
            Path.DirectorySeparatorChar;
        return Path.GetFullPath(path)
            .StartsWith(normalizedWindows, StringComparison.OrdinalIgnoreCase);
    }

    private static string? FindMsvcRedistributable(string fileName)
    {
        string? environmentRoot = Environment.GetEnvironmentVariable("VCToolsRedistDir");
        if (!string.IsNullOrWhiteSpace(environmentRoot) && Directory.Exists(environmentRoot))
        {
            string? fromEnvironment = Directory.EnumerateFiles(
                    environmentRoot,
                    fileName,
                    SearchOption.AllDirectories)
                .Where(path => path.Contains(
                    $"{Path.DirectorySeparatorChar}x64{Path.DirectorySeparatorChar}",
                    StringComparison.OrdinalIgnoreCase))
                .OrderByDescending(path => path, StringComparer.Ordinal)
                .FirstOrDefault();
            if (fromEnvironment != null)
                return fromEnvironment;
        }

        var roots = new[]
        {
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
        }.Where(path => !string.IsNullOrEmpty(path))
         .Select(path => Path.Combine(path, "Microsoft Visual Studio"))
         .Where(Directory.Exists)
         .Distinct(StringComparer.OrdinalIgnoreCase);

        var candidates = new List<string>();
        foreach (string visualStudioRoot in roots)
        {
            try
            {
                foreach (string generation in Directory.EnumerateDirectories(visualStudioRoot))
                foreach (string edition in Directory.EnumerateDirectories(generation))
                {
                    string redistRoot = Path.Combine(edition, "VC", "Redist", "MSVC");
                    if (!Directory.Exists(redistRoot))
                        continue;
                    foreach (string version in Directory.EnumerateDirectories(redistRoot)
                                 .OrderByDescending(path => Path.GetFileName(path), StringComparer.Ordinal))
                    {
                        string x64 = Path.Combine(version, "x64");
                        if (!Directory.Exists(x64))
                            continue;
                        candidates.AddRange(Directory.EnumerateFiles(
                            x64,
                            fileName,
                            SearchOption.AllDirectories));
                    }
                }
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        return candidates
            .OrderByDescending(path => path, StringComparer.Ordinal)
            .FirstOrDefault();
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path) &&
                (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0)
                File.Delete(path);
        }
        catch { }
    }
}
