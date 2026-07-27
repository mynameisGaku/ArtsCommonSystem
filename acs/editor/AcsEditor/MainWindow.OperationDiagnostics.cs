// SPDX-License-Identifier: Apache-2.0

namespace AcsEditor;

public partial class MainWindow
{
    private EditorOperationJournal? _editorOperationJournal;

    private EditorOperationJournal EditorOperations =>
        _editorOperationJournal ??= new EditorOperationJournal(
            completedCapacity: 128,
            observer: PublishEditorOperationDiagnostic);

    private EditorOperationSession BeginEditorOperation(
        EditorOperationService service,
        string startCode,
        string startMessage)
    {
        return EditorOperations.Begin(
            service,
            startCode,
            startMessage,
            assetId: _project?.CanonicalSceneAssetId,
            path: _project?.ProjectFilePath);
    }

    private void PublishEditorOperationDiagnostic(
        EditorOperationDiagnostic diagnostic) =>
        BuildLog(
            EditorOperationDiagnosticFormatting.LegacyLine(diagnostic));
}
