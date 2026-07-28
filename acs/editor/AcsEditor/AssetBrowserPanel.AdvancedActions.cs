// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;

namespace AcsEditor;

public partial class AssetBrowserPanel
{
    private int _trashMaintenanceGeneration = -1;

    private void OnOpenAssetActions(object sender, RoutedEventArgs e)
    {
        if (AssetActionsButton.ContextMenu is not ContextMenu menu) return;
        menu.PlacementTarget = AssetActionsButton;
        menu.Placement = PlacementMode.Bottom;
        menu.IsOpen = true;
    }

    private void OnAssetActionsOpened(object sender, RoutedEventArgs e)
    {
        bool ready = !_assetOperationInProgress &&
                     !_assetOperationsSuspended &&
                     _assetDatabase != null &&
                     _project != null;
        bool hasTrash = ready && HasTrashEntriesFast();
        ActionPackageReadiness.IsEnabled = ready;
        ActionFixRedirectors.IsEnabled = ready;
        ActionUndoDelete.IsEnabled = hasTrash;
        ActionEmptyTrash.IsEnabled = hasTrash;
    }

    private bool HasTrashEntriesFast()
    {
        Project? project = _project;
        if (project == null) return false;
        try
        {
            string entries = Path.Combine(
                project.AssetsDir,
                AssetDatabase.InternalDirectoryName,
                "trash",
                "entries");
            return Directory.Exists(entries) &&
                   Directory.EnumerateDirectories(entries).Any();
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    private void ScheduleTrashMaintenance(
        AssetDatabase database,
        int generation)
    {
        if (_trashMaintenanceGeneration == generation) return;
        _trashMaintenanceGeneration = generation;
        _ = RunTrashMaintenanceAsync(database, generation);
    }

    private async System.Threading.Tasks.Task RunTrashMaintenanceAsync(
        AssetDatabase database,
        int generation)
    {
        using IDisposable lifecycle = _assetOperationLifecycles.Enter();
        AssetBrowserSourcesStore? sourcesStore = _sourcesStore;
        try
        {
            TrashMaintenanceResult result = await RunAssetOperationAsync(
                () =>
                {
                    var workflow = new AssetTrashWorkflow(database);
                    bool sourcesChanged = false;
                    AssetTrashCleanupResult cleanup = workflow.ApplyRetention(
                        beforePermanentDelete: paths =>
                            sourcesChanged = PersistPurgedTrashSources(
                                database,
                                sourcesStore,
                                paths));
                    IReadOnlyList<string> deferred =
                        workflow.ListDeferredTransactionPaths();
                    return new TrashMaintenanceResult(
                        cleanup,
                        deferred,
                        sourcesChanged);
                },
                waitForTurn: true);
            if (!IsCurrentOperationContext(database, generation)) return;
            if (result.SourcesChanged)
                RefreshSavedSources();
            if (result.Cleanup.RemovedEntries != 0)
            {
                Log?.Invoke(
                    $"Trash retention removed {result.Cleanup.RemovedEntries} old entries " +
                    $"and reclaimed {FormatSize(result.Cleanup.ReclaimedBytes)}.");
            }
            foreach (string path in result.Cleanup.DeferredPaths)
                Log?.Invoke("Trash cleanup deferred: " + path);
            if (result.DeferredTransactions.Count != 0)
            {
                Log?.Invoke(
                    "Trash contains interrupted transactions that require recovery: " +
                    string.Join(", ", result.DeferredTransactions.Take(4)));
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception error)
        {
            if (IsCurrentOperationContext(database, generation))
                Log?.Invoke("Trash maintenance could not complete: " + error.Message);
        }
    }

    private static bool CanReplaceReferences(IReadOnlyList<AssetItem> selection)
    {
        if (selection.Count < 2 ||
            selection.Any(static item =>
                item.IsDirectory || string.IsNullOrWhiteSpace(item.AssetId)))
        {
            return false;
        }
        string kind = selection[0].Kind;
        return selection.All(item => string.Equals(
            item.Kind,
            kind,
            StringComparison.OrdinalIgnoreCase));
    }

    private IReadOnlyList<string> SelectedMigrationAssetIds()
    {
        List<AssetItem> selection = SelectedAssets();
        if (selection.Count == 0) return Array.Empty<string>();
        var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (AssetItem item in selection)
        {
            if (!item.IsDirectory)
            {
                if (!string.IsNullOrWhiteSpace(item.AssetId))
                    ids.Add(item.AssetId);
                continue;
            }
            foreach (AssetRecord record in _assetSnapshot)
            {
                if (IsUnder(record.FullPath, item.FullPath))
                    ids.Add(record.AssetId);
            }
        }
        return Array.AsReadOnly(ids
            .OrderBy(static id => id, StringComparer.Ordinal)
            .ToArray());
    }

    private async void OnReplaceReferences(object sender, RoutedEventArgs e)
    {
        using IDisposable lifecycle = _assetOperationLifecycles.Enter();
        List<AssetItem> selection = SelectedAssets();
        if (!CanReplaceReferences(selection) ||
            _assetDatabase == null ||
            _project == null ||
            !CanStartAssetOperation("Preview Replace References"))
        {
            return;
        }

        AssetItem target = Tiles.SelectedItem as AssetItem ?? selection[0];
        if (!selection.Contains(target) || target.IsDirectory ||
            string.IsNullOrWhiteSpace(target.AssetId))
        {
            target = selection[0];
        }
        string[] sourceIds = selection
            .Where(item => !ReferenceEquals(item, target))
            .Select(static item => item.AssetId)
            .ToArray();
        AssetDatabase database = _assetDatabase;
        Project project = _project;
        int generation = _projectRefreshGeneration;
        AssetReplaceReferencesPreview preview;
        try
        {
            preview = await RunAssetOperationAsync(() =>
            {
                database.Refresh(verifyContent: true);
                var workflow = new AssetManagementWorkflow(database);
                return workflow.PreviewReplaceReferences(sourceIds, target.AssetId);
            });
            if (!IsCurrentOperationContext(database, generation)) return;
        }
        catch (Exception error)
        {
            if (!IsCurrentOperationContext(database, generation)) return;
            ReportAssetOperationFailure(
                FormatAssetOperationError("Replace Referencesを確認できませんでした", error));
            return;
        }

        if (preview.Edits.Count == 0)
        {
            MessageBox.Show(
                Window.GetWindow(this),
                "置換対象の参照は見つかりませんでした。",
                "Replace References",
                MessageBoxButton.OK,
                MessageBoxImage.Information);
            return;
        }

        int replacements = preview.Edits.Sum(static edit => edit.ReplacementCount);
        string sources = string.Join(
            "\n",
            selection
                .Where(item => !ReferenceEquals(item, target))
                .Take(8)
                .Select(static item => "• " + item.Name));
        string summary =
            $"参照先: {target.Name}\n\n" +
            $"置換元:\n{sources}\n\n" +
            $"変更ファイル: {preview.Edits.Count}\n" +
            $"置換箇所: {replacements}\n\n" +
            "preview後に変更されたファイルがあればcommitは拒否されます。\n" +
            "置換元アセット自体は削除されません。続行しますか？";
        if (MessageBox.Show(
                Window.GetWindow(this),
                summary,
                "Replace References",
                MessageBoxButton.OKCancel,
                MessageBoxImage.Warning,
                MessageBoxResult.Cancel) != MessageBoxResult.OK)
        {
            return;
        }

        if (!CanStartAssetOperation("Commit Replace References")) return;
        string[] affectedPaths = preview.Edits
            .Select(edit => Path.GetFullPath(Path.Combine(
                project.AssetsDir,
                edit.RelativePath.Replace('/', Path.DirectorySeparatorChar))))
            .Where(path => IsUnderOrEqual(path, project.AssetsDir))
            .ToArray();
        AssetPathMutationStartingEventArgs? mutation = BeginAssetPathMutation(
            AssetPathMutationKind.ContentRewrite,
            affectedPaths);
        if (mutation == null) return;
        bool succeeded = false;
        try
        {
            AssetReplaceReferencesResult result = await RunAssetOperationAsync(() =>
            {
                database.Refresh(verifyContent: true);
                var workflow = new AssetManagementWorkflow(database);
                return workflow.CommitReplaceReferences(preview);
            });
            succeeded = true;
            if (!IsCurrentOperationContext(database, generation)) return;
            _assetSnapshot = database.Snapshot();
            RefreshView();
            Log?.Invoke(
                $"Replace References: {result.ReplacementCount} references across " +
                $"{result.ContentFileCount} content and {result.MetadataAssetCount} metadata files.");
        }
        catch (Exception error)
        {
            if (IsCurrentOperationContext(database, generation))
            {
                TryRefreshAfterOperationFailure();
                ReportAssetOperationFailure(
                    FormatAssetOperationError("Replace Referencesに失敗しました", error));
            }
        }
        finally
        {
            CompleteAssetPathMutation(mutation, succeeded);
        }
    }

    private async void OnMigrateAssets(object sender, RoutedEventArgs e)
    {
        using IDisposable lifecycle = _assetOperationLifecycles.Enter();
        if (_assetDatabase == null || _project == null) return;
        IReadOnlyList<string> assetIds = SelectedMigrationAssetIds();
        if (assetIds.Count == 0 ||
            !CanStartAssetOperation("Select Migrate destination"))
        {
            return;
        }

        var dialog = new Microsoft.Win32.OpenFolderDialog
        {
            Title = "移行先ACSプロジェクトのルートを選択",
            InitialDirectory = _project.RootDir,
        };
        Window? owner = Window.GetWindow(this);
        bool? accepted = owner != null
            ? dialog.ShowDialog(owner)
            : dialog.ShowDialog();
        if (accepted != true) return;
        string targetProjectRoot = dialog.FolderName;

        AssetDatabase database = _assetDatabase;
        int generation = _projectRefreshGeneration;
        AssetMigrationPreview preview;
        try
        {
            preview = await RunAssetOperationAsync(() =>
            {
                database.Refresh(verifyContent: true);
                var workflow = new AssetManagementWorkflow(database);
                return workflow.PreviewMigrate(assetIds, targetProjectRoot);
            });
            if (!IsCurrentOperationContext(database, generation)) return;
        }
        catch (Exception error)
        {
            if (IsCurrentOperationContext(database, generation))
            {
                ReportAssetOperationFailure(
                    FormatAssetOperationError("Migrate previewを作成できませんでした", error));
            }
            return;
        }

        string examples = string.Join(
            "\n",
            preview.Files.Take(8).Select(static file => "• Assets/" + file.RelativePath));
        if (preview.Files.Count > 8)
            examples += $"\n• …ほか {preview.Files.Count - 8} files";
        string summary =
            $"依存関係を含むアセット: {preview.AssetIds.Count}\n" +
            $"コピーするファイル: {preview.Files.Count}\n" +
            $"合計サイズ: {FormatSize(preview.TotalBytes)}\n\n" +
            examples + "\n\n" +
            "GUIDと依存関係を維持し、既存ファイルは上書きしません。続行しますか？";
        if (MessageBox.Show(
                owner,
                summary,
                "Migrate Assets",
                MessageBoxButton.OKCancel,
                MessageBoxImage.Information,
                MessageBoxResult.Cancel) != MessageBoxResult.OK)
        {
            return;
        }
        if (!CanStartAssetOperation("Commit Migrate")) return;

        try
        {
            AssetMigrationResult result = await RunAssetOperationAsync(() =>
            {
                database.Refresh(verifyContent: true);
                var workflow = new AssetManagementWorkflow(database);
                return workflow.CommitMigrate(preview);
            });
            if (!IsCurrentOperationContext(database, generation)) return;
            Log?.Invoke(
                $"Migrated {result.AssetCount} assets / {result.FileCount} files " +
                $"({FormatSize(result.TotalBytes)}) to {result.TargetAssetsRoot}.");
        }
        catch (Exception error)
        {
            if (IsCurrentOperationContext(database, generation))
            {
                ReportAssetOperationFailure(
                    FormatAssetOperationError("Migrateに失敗しました", error));
            }
        }
    }

    private async void OnFixUpRedirectors(object sender, RoutedEventArgs e)
    {
        using IDisposable lifecycle = _assetOperationLifecycles.Enter();
        if (_assetDatabase == null ||
            !CanStartAssetOperation("Preview Redirector Registry Cleanup"))
        {
            return;
        }
        AssetDatabase database = _assetDatabase;
        int generation = _projectRefreshGeneration;
        AssetRedirectorFixupPreview preview;
        try
        {
            preview = await RunAssetOperationAsync(() =>
            {
                database.Refresh(verifyContent: true);
                var workflow = new AssetManagementWorkflow(database);
                return workflow.PreviewFixUpRedirectors();
            });
            if (!IsCurrentOperationContext(database, generation)) return;
        }
        catch (Exception error)
        {
            if (IsCurrentOperationContext(database, generation))
            {
                ReportAssetOperationFailure(
                    FormatAssetOperationError("Redirectorを確認できませんでした", error));
            }
            return;
        }

        if (preview.Items.Count == 0)
        {
            MessageBox.Show(
                Window.GetWindow(this),
                "クリーンアップが必要なRedirector登録はありません。",
                "Clean Redirector Registry",
                MessageBoxButton.OK,
                MessageBoxImage.Information);
            return;
        }
        string details = string.Join(
            "\n",
            preview.Items.Take(10).Select(item =>
                $"• {item.Action}: {item.OriginalRelativePath}"));
        if (preview.Items.Count > 10)
            details += $"\n• …ほか {preview.Items.Count - 10} entries";
        if (MessageBox.Show(
                Window.GetWindow(this),
                $"{preview.Items.Count}件の無効・重複Redirector登録を整理します。\n" +
                "有効なRedirectorは保持され、パッケージや参照元アセットは再保存されません。" +
                $"\n\n{details}",
                "Clean Redirector Registry",
                MessageBoxButton.OKCancel,
                MessageBoxImage.Information,
                MessageBoxResult.Cancel) != MessageBoxResult.OK)
        {
            return;
        }
        if (!CanStartAssetOperation("Commit Redirector Registry Cleanup")) return;

        try
        {
            AssetRedirectorFixupResult result = await RunAssetOperationAsync(() =>
            {
                database.Refresh(verifyContent: true);
                var workflow = new AssetManagementWorkflow(database);
                return workflow.CommitFixUpRedirectors(preview);
            });
            if (!IsCurrentOperationContext(database, generation)) return;
            Log?.Invoke(
                $"Redirector registry cleanup: removed {result.RemovedCount}, updated " +
                $"{result.UpdatedCount}, remaining {result.RemainingCount}.");
        }
        catch (Exception error)
        {
            if (IsCurrentOperationContext(database, generation))
            {
                ReportAssetOperationFailure(
                    FormatAssetOperationError("Redirector修復に失敗しました", error));
            }
        }
    }

    private async void OnUndoLastDelete(object sender, RoutedEventArgs e)
    {
        using IDisposable lifecycle = _assetOperationLifecycles.Enter();
        if (_assetDatabase == null || _project == null ||
            !CanStartAssetOperation("Inspect Trash"))
        {
            return;
        }
        AssetDatabase database = _assetDatabase;
        Project project = _project;
        int generation = _projectRefreshGeneration;
        AssetTrashRestoreInspection inspection;
        try
        {
            inspection = await RunAssetOperationAsync(() =>
            {
                var workflow = new AssetTrashWorkflow(database);
                AssetTrashEntry? entry = workflow.ListEntries().FirstOrDefault();
                if (entry == null)
                    throw new InvalidOperationException("Trashは空です。");
                return workflow.InspectRestore(entry.EntryId);
            });
            if (!IsCurrentOperationContext(database, generation)) return;
        }
        catch (Exception error)
        {
            if (IsCurrentOperationContext(database, generation))
                ReportAssetOperationFailure("Trashを確認できませんでした: " + error.Message);
            return;
        }

        if (!inspection.CanRestore)
        {
            string collisions = string.Join(
                "\n",
                inspection.Collisions.Take(8).Select(static path => "• " + path));
            ReportAssetOperationFailure(
                "元の場所が使用中のため復元できません。\n\n" + collisions);
            return;
        }
        string names = string.Join(
            "\n",
            inspection.Entry.OriginalRelativePaths
                .Take(8)
                .Select(static path => "• Assets/" + path));
        if (MessageBox.Show(
                Window.GetWindow(this),
                $"最後の削除を元に戻しますか？\n\n{names}",
                "Undo Last Delete",
                MessageBoxButton.OKCancel,
                MessageBoxImage.Question,
                MessageBoxResult.Cancel) != MessageBoxResult.OK)
        {
            return;
        }
        if (!CanStartAssetOperation("Restore Trash entry")) return;
        string[] restoredRoots = inspection.Entry.OriginalRelativePaths
            .Select(relative => Path.GetFullPath(Path.Combine(
                project.AssetsDir,
                relative.Replace('/', Path.DirectorySeparatorChar))))
            .Where(path => IsUnderOrEqual(path, project.AssetsDir))
            .ToArray();
        AssetPathMutationStartingEventArgs? mutation = BeginAssetPathMutation(
            AssetPathMutationKind.ContentRewrite,
            restoredRoots);
        if (mutation == null) return;
        bool succeeded = false;

        try
        {
            AssetTrashRestoreResult restored = await RunAssetOperationAsync(() =>
            {
                var workflow = new AssetTrashWorkflow(database);
                return workflow.Restore(inspection.Entry.EntryId);
            });
            succeeded = true;
            if (!IsCurrentOperationContext(database, generation)) return;
            _assetSnapshot = database.Snapshot();
            RebuildSourcesTree();
            RefreshSavedSources();
            RefreshView();
            SelectAssets(restored.RestoredPaths);
            Log?.Invoke($"Restored {restored.RestoredPaths.Count} root item(s) from Trash.");
            if (restored.DeferredCleanupPath != null)
            {
                Log?.Invoke(
                    "Trash restore completed; deferred cleanup remains at " +
                    restored.DeferredCleanupPath);
            }
        }
        catch (Exception error)
        {
            if (IsCurrentOperationContext(database, generation))
            {
                TryRefreshAfterOperationFailure();
                ReportAssetOperationFailure("Trashから復元できませんでした: " + error.Message);
            }
        }
        finally
        {
            CompleteAssetPathMutation(mutation, succeeded);
        }
    }

    private async void OnEmptyTrash(object sender, RoutedEventArgs e)
    {
        using IDisposable lifecycle = _assetOperationLifecycles.Enter();
        if (_assetDatabase == null ||
            !CanStartAssetOperation("Inspect Trash"))
        {
            return;
        }
        AssetDatabase database = _assetDatabase;
        int generation = _projectRefreshGeneration;
        IReadOnlyList<AssetTrashEntry> entries;
        try
        {
            entries = await RunAssetOperationAsync(() =>
                new AssetTrashWorkflow(database).ListEntries());
            if (!IsCurrentOperationContext(database, generation)) return;
        }
        catch (Exception error)
        {
            if (IsCurrentOperationContext(database, generation))
                ReportAssetOperationFailure("Trashを確認できませんでした: " + error.Message);
            return;
        }
        if (entries.Count == 0) return;
        long bytes = entries.Sum(static entry => entry.StoredBytes);
        if (MessageBox.Show(
                Window.GetWindow(this),
                $"{entries.Count}件（{FormatSize(bytes)}）を完全に削除します。" +
                "\nこの操作は元に戻せません。",
                "Empty Trash",
                MessageBoxButton.OKCancel,
                MessageBoxImage.Warning,
                MessageBoxResult.Cancel) != MessageBoxResult.OK)
        {
            return;
        }
        if (!CanStartAssetOperation("Empty Trash")) return;

        AssetBrowserSourcesStore? sourcesStore = _sourcesStore;
        try
        {
            TrashPurgeUiResult operation = await RunAssetOperationAsync(() =>
            {
                bool sourcesChanged = false;
                AssetTrashCleanupResult cleanup =
                    new AssetTrashWorkflow(database).EmptyTrash(paths =>
                        sourcesChanged = PersistPurgedTrashSources(
                            database,
                            sourcesStore,
                            paths));
                return new TrashPurgeUiResult(cleanup, sourcesChanged);
            });
            if (!IsCurrentOperationContext(database, generation)) return;
            AssetTrashCleanupResult result = operation.Cleanup;
            if (operation.SourcesChanged)
                RefreshSavedSources();
            Log?.Invoke(
                $"Emptied {result.RemovedEntries} Trash entries and reclaimed " +
                $"{FormatSize(result.ReclaimedBytes)}.");
            foreach (string path in result.DeferredPaths)
                Log?.Invoke("Trash cleanup deferred: " + path);
        }
        catch (Exception error)
        {
            if (IsCurrentOperationContext(database, generation))
                ReportAssetOperationFailure("Trashを空にできませんでした: " + error.Message);
        }
    }

    private static bool PersistPurgedTrashSources(
        AssetDatabase database,
        AssetBrowserSourcesStore? sourcesStore,
        IReadOnlyList<string> purgedOriginalRelativePaths)
    {
        if (sourcesStore == null ||
            purgedOriginalRelativePaths.Count == 0)
        {
            return false;
        }

        string[] deletedRoots = purgedOriginalRelativePaths
            .Select(relative => Path.GetFullPath(Path.Combine(
                database.AssetsRoot,
                relative.Replace('/', Path.DirectorySeparatorChar))))
            .ToArray();
        return sourcesStore.ApplyPathChanges(
            new AssetPathsChangedEventArgs(deletedRoots: deletedRoots));
    }

    private sealed record TrashMaintenanceResult(
        AssetTrashCleanupResult Cleanup,
        IReadOnlyList<string> DeferredTransactions,
        bool SourcesChanged);

    private sealed record TrashPurgeUiResult(
        AssetTrashCleanupResult Cleanup,
        bool SourcesChanged);
}
