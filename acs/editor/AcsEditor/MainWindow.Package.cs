// SPDX-License-Identifier: Apache-2.0

using System.Windows;

namespace AcsEditor;

public partial class MainWindow
{
    private async void OnPackageProject(object sender, RoutedEventArgs e)
    {
        if (_project == null)
        {
            BuildLog("プロジェクトがありません。");
            return;
        }
        if (_building)
        {
            BuildLog("別のビルドが実行中です。");
            return;
        }
        if (!EnsureBuildSceneCompatibility("Package"))
            return;

        ShowBottomTab("build");
        BuildLog($"==== Package Project: {_project.Name} ====");
        _building = true;
        SetBuildUiEnabled(false);
        try
        {
            // Source save now awaits autosave generation drain. Keep that await inside the same
            // build exclusion window so repeated shortcuts/clicks cannot start a second package.
            if (!await SaveSceneForBuildAsync())
                return;
            var dialog = new PackageProjectDialog(_project, BuildLog);
            await dialog.ShowModelessAsync(this);
        }
        finally
        {
            _building = false;
            SetBuildUiEnabled(true);
        }
    }
}
