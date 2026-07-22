// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AcsEditor;

internal sealed class EditorWorkspaceLayout
{
    public double HierarchyWidth { get; set; } = 260;
    public double InspectorWidth { get; set; } = 348;
    public double BottomDockHeight { get; set; } = 210;
    public bool HierarchyVisible { get; set; } = true;
    public bool InspectorVisible { get; set; } = true;
    public bool BottomDockVisible { get; set; } = true;
    public string BottomTab { get; set; } = "assets";

    public EditorWorkspaceLayout Clone() => new()
    {
        HierarchyWidth = HierarchyWidth,
        InspectorWidth = InspectorWidth,
        BottomDockHeight = BottomDockHeight,
        HierarchyVisible = HierarchyVisible,
        InspectorVisible = InspectorVisible,
        BottomDockVisible = BottomDockVisible,
        BottomTab = BottomTab,
    };
}

internal sealed class EditorWorkspaceProfile
{
    public string Name { get; init; } = "";
    public EditorWorkspaceLayout Layout { get; init; } = new();
    public bool IsBuiltIn { get; init; }

    [JsonIgnore]
    public string Kind => IsBuiltIn ? "BUILT-IN" : "USER";

    public EditorWorkspaceProfile Clone() => new()
    {
        Name = Name,
        Layout = Layout.Clone(),
        IsBuiltIn = IsBuiltIn,
    };
}

/// <summary>
/// Versioned, user-local storage for named editor workspaces.
/// The file path is fixed by the editor (or injected by the self-test), and all writes are
/// replace-atomic so a crash cannot leave a partially-written workspace catalogue.
/// </summary>
internal sealed class EditorWorkspaceStore
{
    internal const int SchemaVersion = 1;
    internal const int MaxUserProfiles = 32;
    private const long MaxFileBytes = 256 * 1024;
    internal const string DefaultWorkspaceName = "Level Editing";

    private sealed class StoreState
    {
        public int Version { get; set; } = SchemaVersion;
        public string LastActiveName { get; set; } = DefaultWorkspaceName;
        public List<StoredProfile> Profiles { get; set; } = new();
    }

    private sealed class StoredProfile
    {
        public string Name { get; set; } = "";
        public EditorWorkspaceLayout Layout { get; set; } = new();
    }

    private static readonly EditorWorkspaceProfile[] BuiltInProfiles =
    {
        new()
        {
            Name = DefaultWorkspaceName,
            IsBuiltIn = true,
            Layout = new EditorWorkspaceLayout
            {
                HierarchyWidth = 260,
                InspectorWidth = 348,
                BottomDockHeight = 230,
                HierarchyVisible = true,
                InspectorVisible = true,
                BottomDockVisible = true,
                BottomTab = "assets",
            },
        },
        new()
        {
            Name = "Focused Viewport",
            IsBuiltIn = true,
            Layout = new EditorWorkspaceLayout
            {
                HierarchyWidth = 260,
                InspectorWidth = 348,
                BottomDockHeight = 210,
                HierarchyVisible = false,
                InspectorVisible = false,
                BottomDockVisible = false,
                BottomTab = "console",
            },
        },
        new()
        {
            Name = "Debugging",
            IsBuiltIn = true,
            Layout = new EditorWorkspaceLayout
            {
                HierarchyWidth = 250,
                InspectorWidth = 330,
                BottomDockHeight = 360,
                HierarchyVisible = true,
                InspectorVisible = true,
                BottomDockVisible = true,
                BottomTab = "build",
            },
        },
    };

    private readonly string _path;
    private readonly List<EditorWorkspaceProfile> _userProfiles = new();

    internal EditorWorkspaceStore(string? path = null)
    {
        _path = path ?? DefaultPath;
        Reload();
    }

    internal static string DefaultPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "AcsEditor",
        "Workspaces",
        $"workspaces.v{SchemaVersion}.json");

    internal string LastActiveName { get; private set; } = DefaultWorkspaceName;
    internal string? LoadWarning { get; private set; }

    internal IReadOnlyList<EditorWorkspaceProfile> GetProfiles() =>
        BuiltInProfiles
            .Concat(_userProfiles.OrderBy(profile => profile.Name, StringComparer.OrdinalIgnoreCase))
            .Select(profile => profile.Clone())
            .ToArray();

    internal bool TryGetProfile(string? name, out EditorWorkspaceProfile profile)
    {
        profile = GetProfiles().FirstOrDefault(candidate =>
            string.Equals(candidate.Name, name, StringComparison.OrdinalIgnoreCase))!;
        return profile != null;
    }

    internal EditorWorkspaceProfile SaveUserProfile(
        string name,
        EditorWorkspaceLayout layout,
        bool overwrite,
        bool makeActive = true)
    {
        string normalizedName = ValidateName(name);
        if (IsBuiltInName(normalizedName))
            throw new InvalidOperationException("Built-in workspaces cannot be overwritten.");

        int existingIndex = _userProfiles.FindIndex(profile =>
            string.Equals(profile.Name, normalizedName, StringComparison.OrdinalIgnoreCase));
        if (existingIndex >= 0 && !overwrite)
            throw new InvalidOperationException($"Workspace '{normalizedName}' already exists.");
        if (existingIndex < 0 && _userProfiles.Count >= MaxUserProfiles)
            throw new InvalidOperationException(
                $"At most {MaxUserProfiles} user workspaces can be stored.");

        var profile = new EditorWorkspaceProfile
        {
            Name = normalizedName,
            Layout = NormalizeLayout(layout),
            IsBuiltIn = false,
        };
        if (existingIndex >= 0)
            _userProfiles[existingIndex] = profile;
        else
            _userProfiles.Add(profile);

        if (makeActive)
            LastActiveName = profile.Name;
        Persist();
        return profile.Clone();
    }

    internal EditorWorkspaceProfile DuplicateProfile(string sourceName, string destinationName)
    {
        if (!TryGetProfile(sourceName, out EditorWorkspaceProfile source))
            throw new InvalidOperationException($"Workspace '{sourceName}' does not exist.");
        return SaveUserProfile(
            destinationName,
            source.Layout,
            overwrite: false,
            makeActive: false);
    }

    internal EditorWorkspaceProfile RenameUserProfile(string oldName, string newName)
    {
        int index = FindUserProfile(oldName);
        string normalizedName = ValidateName(newName);
        if (IsBuiltInName(normalizedName))
            throw new InvalidOperationException("Built-in workspace names are reserved.");
        if (_userProfiles.Where((_, candidateIndex) => candidateIndex != index).Any(profile =>
                string.Equals(profile.Name, normalizedName, StringComparison.OrdinalIgnoreCase)))
            throw new InvalidOperationException($"Workspace '{normalizedName}' already exists.");

        string previousName = _userProfiles[index].Name;
        var renamed = new EditorWorkspaceProfile
        {
            Name = normalizedName,
            Layout = _userProfiles[index].Layout.Clone(),
            IsBuiltIn = false,
        };
        _userProfiles[index] = renamed;
        if (string.Equals(LastActiveName, previousName, StringComparison.OrdinalIgnoreCase))
            LastActiveName = normalizedName;
        Persist();
        return renamed.Clone();
    }

    internal void DeleteUserProfile(string name)
    {
        int index = FindUserProfile(name);
        string removedName = _userProfiles[index].Name;
        _userProfiles.RemoveAt(index);
        if (string.Equals(LastActiveName, removedName, StringComparison.OrdinalIgnoreCase))
            LastActiveName = DefaultWorkspaceName;
        Persist();
    }

    internal void MarkActive(string name)
    {
        if (!TryGetProfile(name, out EditorWorkspaceProfile profile))
            throw new InvalidOperationException($"Workspace '{name}' does not exist.");
        LastActiveName = profile.Name;
        Persist();
    }

    private void Reload()
    {
        _userProfiles.Clear();
        LastActiveName = DefaultWorkspaceName;
        LoadWarning = null;
        if (!File.Exists(_path))
            return;

        try
        {
            var info = new FileInfo(_path);
            if (info.Length <= 0 || info.Length > MaxFileBytes)
                throw new InvalidDataException("workspace catalogue has an invalid size");

            StoreState? state = JsonSerializer.Deserialize<StoreState>(
                File.ReadAllText(_path),
                JsonOptions);
            if (state == null || state.Version != SchemaVersion)
                throw new InvalidDataException("workspace catalogue version is unsupported");
            if (state.Profiles.Count > MaxUserProfiles)
                throw new InvalidDataException("workspace catalogue contains too many profiles");

            var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (StoredProfile stored in state.Profiles)
            {
                string name = ValidateName(stored.Name);
                if (IsBuiltInName(name) || !names.Add(name))
                    throw new InvalidDataException("workspace catalogue contains a reserved or duplicate name");
                _userProfiles.Add(new EditorWorkspaceProfile
                {
                    Name = name,
                    Layout = NormalizeLayout(stored.Layout),
                    IsBuiltIn = false,
                });
            }

            LastActiveName = GetProfiles().Any(profile =>
                string.Equals(profile.Name, state.LastActiveName, StringComparison.OrdinalIgnoreCase))
                ? GetProfiles().First(profile =>
                    string.Equals(
                        profile.Name,
                        state.LastActiveName,
                        StringComparison.OrdinalIgnoreCase)).Name
                : DefaultWorkspaceName;
        }
        catch (Exception ex)
        {
            _userProfiles.Clear();
            LastActiveName = DefaultWorkspaceName;
            LoadWarning = ex.Message;
        }
    }

    private void Persist()
    {
        string? directory = Path.GetDirectoryName(_path);
        if (string.IsNullOrWhiteSpace(directory))
            throw new InvalidOperationException("Workspace catalogue path has no parent directory.");
        Directory.CreateDirectory(directory);

        var state = new StoreState
        {
            LastActiveName = LastActiveName,
            Profiles = _userProfiles
                .OrderBy(profile => profile.Name, StringComparer.OrdinalIgnoreCase)
                .ThenBy(profile => profile.Name, StringComparer.Ordinal)
                .Select(profile => new StoredProfile
                {
                    Name = profile.Name,
                    Layout = profile.Layout.Clone(),
                })
                .ToList(),
        };

        byte[] json = JsonSerializer.SerializeToUtf8Bytes(state, JsonOptions);
        if (json.LongLength > MaxFileBytes)
            throw new InvalidDataException("workspace catalogue exceeds the size limit");

        string temp = _path + $".{Environment.ProcessId}.{Guid.NewGuid():N}.tmp";
        try
        {
            using (var stream = new FileStream(
                       temp,
                       FileMode.CreateNew,
                       FileAccess.Write,
                       FileShare.None,
                       16 * 1024,
                       FileOptions.WriteThrough))
            {
                stream.Write(json);
                stream.Flush(flushToDisk: true);
            }
            File.Move(temp, _path, overwrite: true);
        }
        finally
        {
            try
            {
                if (File.Exists(temp))
                    File.Delete(temp);
            }
            catch
            {
                // A stale temp file is harmless and never read as a catalogue.
            }
        }
    }

    private int FindUserProfile(string name)
    {
        int index = _userProfiles.FindIndex(profile =>
            string.Equals(profile.Name, name, StringComparison.OrdinalIgnoreCase));
        if (index < 0)
            throw new InvalidOperationException("Only user workspaces can be changed or deleted.");
        return index;
    }

    private static string ValidateName(string? name)
    {
        string normalized = (name ?? "").Trim();
        if (normalized.Length is < 1 or > 64)
            throw new ArgumentException("Workspace names must contain 1 to 64 characters.");
        if (normalized.Any(character => char.IsControl(character)))
            throw new ArgumentException("Workspace names cannot contain control characters.");
        return normalized;
    }

    private static bool IsBuiltInName(string name) => BuiltInProfiles.Any(profile =>
        string.Equals(profile.Name, name, StringComparison.OrdinalIgnoreCase));

    internal static EditorWorkspaceLayout NormalizeLayout(EditorWorkspaceLayout? layout)
    {
        layout ??= new EditorWorkspaceLayout();
        return new EditorWorkspaceLayout
        {
            HierarchyWidth = ClampFinite(layout.HierarchyWidth, 210, 960, 260),
            InspectorWidth = ClampFinite(layout.InspectorWidth, 280, 1200, 348),
            BottomDockHeight = ClampFinite(layout.BottomDockHeight, 140, 800, 210),
            HierarchyVisible = layout.HierarchyVisible,
            InspectorVisible = layout.InspectorVisible,
            BottomDockVisible = layout.BottomDockVisible,
            BottomTab = NormalizeBottomTab(layout.BottomTab),
        };
    }

    internal static string NormalizeBottomTab(string? tab) =>
        tab is "console" or "build" or "assets" or "profiler"
        ? tab
        : "console";

    private static double ClampFinite(
        double value,
        double minimum,
        double maximum,
        double fallback) =>
        double.IsFinite(value) ? Math.Clamp(value, minimum, maximum) : fallback;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = false,
        WriteIndented = true,
    };
}
