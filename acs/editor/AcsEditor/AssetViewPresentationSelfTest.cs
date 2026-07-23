// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Text;

namespace AcsEditor;

internal static class AssetViewPresentationSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;

        void Check(bool condition, string label)
        {
            if (condition)
            {
                passed++;
                output.WriteLine("PASS: " + label);
            }
            else
            {
                failed++;
                output.WriteLine("FAIL: " + label);
            }
        }

        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-asset-view-presentation-" + Guid.NewGuid().ToString("N"));
        string assets = Path.Combine(root, "Assets");
        try
        {
            Directory.CreateDirectory(assets);
            var store = new AssetViewPresentationStore();
            Check(store.Load(assets) == AssetViewPresentationState.Default,
                "missing preferences use deterministic defaults");

            var requested = new AssetViewPresentationState(
                AssetViewMode.Details,
                ThumbnailSize: 500,
                ShowPreview: false,
                ShowFolders: false,
                ShowEmptyFolders: true);
            store.Save(assets, requested);
            AssetViewPresentationState loaded = store.Load(assets);
            Check(loaded.ViewMode == AssetViewMode.Details &&
                  loaded.ThumbnailSize ==
                      AssetViewPresentationState.MaximumThumbnailSize &&
                  !loaded.ShowPreview &&
                  !loaded.ShowFolders &&
                  !loaded.ShowEmptyFolders,
                "round trip normalizes mode, thumbnail size, and folder invariants");

            string path = AssetViewPresentationStore.GetStorePath(assets);
            Check(File.Exists(path) &&
                  !Directory.EnumerateFiles(
                          Path.GetDirectoryName(path)!,
                          "*.tmp-*",
                          SearchOption.TopDirectoryOnly)
                      .Any(),
                "atomic save leaves no temporary files");

            File.WriteAllText(path, "{not json", new UTF8Encoding(false));
            Check(store.Load(assets) == AssetViewPresentationState.Default,
                "corrupt preferences fail closed to defaults");

            File.WriteAllText(
                path,
                "{\"schemaVersion\":99,\"viewMode\":\"List\"," +
                "\"thumbnailSize\":48,\"showPreview\":true," +
                "\"showFolders\":true,\"showEmptyFolders\":true}",
                new UTF8Encoding(false));
            Check(store.Load(assets) == AssetViewPresentationState.Default,
                "unknown schema versions do not partially apply");

            File.WriteAllText(path, new string('x', 33 * 1024), new UTF8Encoding(false));
            Check(store.Load(assets) == AssetViewPresentationState.Default,
                "oversized preference files are bounded");

            var invalidMode = AssetViewPresentationState.Default with
            {
                ViewMode = (AssetViewMode)999,
                ThumbnailSize = 1,
            };
            AssetViewPresentationState normalized = invalidMode.Normalize();
            Check(normalized.ViewMode == AssetViewMode.Tiles &&
                  normalized.ThumbnailSize ==
                      AssetViewPresentationState.MinimumThumbnailSize,
                "invalid in-memory values normalize before persistence");
        }
        finally
        {
            try
            {
                if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException)
            {
            }
        }

        output.WriteLine(
            $"Asset View presentation self-test: {passed} PASS / {failed} failures");
        return failed;
    }
}
