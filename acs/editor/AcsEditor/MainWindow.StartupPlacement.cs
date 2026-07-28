// SPDX-License-Identifier: Apache-2.0

namespace AcsEditor;

public partial class MainWindow
{
    private bool _startupMonitorPlacementRequested;

    internal void SuppressSavedWindowPlacementForStartupMonitor()
    {
        _startupMonitorPlacementRequested = true;
        WindowStartupLocation = System.Windows.WindowStartupLocation.Manual;
    }
}
