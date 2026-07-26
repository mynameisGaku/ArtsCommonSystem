// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;

namespace AcsEditor;

internal static class MaterialWorkflowSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;
        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-material-workflow-selftest-" + Guid.NewGuid().ToString("N"));
        string assets = Path.Combine(root, "Assets");

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

        try
        {
            Directory.CreateDirectory(Path.Combine(assets, "Characters"));
            Directory.CreateDirectory(Path.Combine(assets, "Environment"));
            string alpha = Path.Combine(assets, "Characters", "Alpha.acsmat");
            string brass = Path.Combine(assets, "Environment", "Brass.ACSMAT");
            string zinc = Path.Combine(assets, "Zinc.acsmat");
            File.WriteAllText(zinc, "zinc");
            File.WriteAllText(brass, "brass");
            File.WriteAllText(alpha, "alpha");
            File.WriteAllText(Path.Combine(assets, "ignore.txt"), "not a material");

            MaterialAssetCatalog catalogue =
                MaterialAssetWorkflow.BuildCatalog(assets, brass);
            Check(catalogue.Choices.Count == 3,
                "catalogue includes only .acsmat assets");
            Check(catalogue.Choices.Select(choice => choice.DisplayName).SequenceEqual(
                    new[]
                    {
                        "Characters/Alpha.acsmat",
                        "Environment/Brass.ACSMAT",
                        "Zinc.acsmat",
                    },
                    StringComparer.OrdinalIgnoreCase),
                "nested material assets are sorted deterministically by project path");
            Check(catalogue.SelectedIndex == 1,
                "current project material is selected by normalized path");
            Check(catalogue.Choices.All(choice => choice.IsProjectAsset),
                "catalogued assets are marked as project assets");
            string[] firstOrder = catalogue.Choices
                .Select(choice => choice.FullPath)
                .ToArray();
            Check(Enumerable.Range(0, 8).All(_ =>
                    MaterialAssetWorkflow.BuildCatalog(assets, brass).Choices
                        .Select(choice => choice.FullPath)
                        .SequenceEqual(firstOrder, StringComparer.Ordinal)),
                "repeated catalogue enumeration has a stable exact order");

            string outside = Path.Combine(root, "Shared", "External.acsmat");
            MaterialAssetCatalog external =
                MaterialAssetWorkflow.BuildCatalog(assets, outside);
            Check(external.Choices.Count == 4 &&
                  external.SelectedIndex == 3 &&
                  external.Choices[3].DisplayName == "External.acsmat",
                "assigned external or missing material remains visible in the slot");
            Check(!external.Choices[3].IsProjectAsset,
                "external material is not mislabeled as a project asset");
            Check(MaterialAssetWorkflow.DisplayName(
                    assets,
                    Path.Combine(root, "AssetsShared", "Sibling.acsmat")) ==
                  "Sibling.acsmat",
                "sibling directories with the Assets prefix are not shown as project-relative");

            string emptyRoot = Path.Combine(root, "MissingAssets");
            MaterialAssetCatalog missingRoot =
                MaterialAssetWorkflow.BuildCatalog(emptyRoot, outside);
            Check(missingRoot.Choices.Count == 1 && missingRoot.SelectedIndex == 0,
                "missing Assets directory still preserves the assigned material");

            File.WriteAllText(Path.Combine(assets, "Material.acsmat"), "0");
            File.WriteAllText(Path.Combine(assets, "Material1.acsmat"), "1");
            Directory.CreateDirectory(Path.Combine(assets, "Material2.acsmat"));
            Check(
                MaterialAssetWorkflow.NextAvailablePath(assets).EndsWith(
                    Path.Combine("Assets", "Material3.acsmat"),
                    StringComparison.OrdinalIgnoreCase),
                "new material naming skips existing files and directories without overwrite");

            string reserved = MaterialAssetWorkflow.ReserveNextAvailablePath(assets);
            string reservedAgain = MaterialAssetWorkflow.ReserveNextAvailablePath(assets);
            Check(File.Exists(reserved) && File.Exists(reservedAgain) &&
                  !MaterialAssetWorkflow.SamePath(reserved, reservedAgain) &&
                  reserved.EndsWith("Material3.acsmat", StringComparison.OrdinalIgnoreCase) &&
                  reservedAgain.EndsWith("Material4.acsmat", StringComparison.OrdinalIgnoreCase),
                "material name reservation is atomic and collision-free");
            Check(MaterialAssetWorkflow.SamePath(
                    Path.Combine(assets, ".", "Zinc.acsmat"),
                    zinc),
                "material selection compares normalized paths");
            Check(!MaterialAssetWorkflow.SamePath(null, zinc),
                "empty material references never compare equal");

            string link = Path.Combine(root, "LinkedAssets");
            try
            {
                Directory.CreateSymbolicLink(link, assets);
                Check(MaterialAssetWorkflow.BuildCatalog(link, outside).Choices.Count == 1,
                    "catalogue does not traverse a reparse-point Assets root");
                bool reparseWriteRejected = false;
                try
                {
                    MaterialAssetWorkflow.ReserveNextAvailablePath(link);
                }
                catch (InvalidDataException)
                {
                    reparseWriteRejected = true;
                }
                Check(reparseWriteRejected,
                    "material creation refuses a reparse-point Assets root");
            }
            catch (Exception error) when (
                error is UnauthorizedAccessException or IOException or NotSupportedException)
            {
                output.WriteLine(
                    "SKIP: reparse-point material safety runtime test: " + error.Message);
            }
            Check(MaterialAssetWorkflow.IsRecoverableCreationFailure(
                    new InvalidDataException("reparse-point Assets root")),
                "reparse validation failures are handled by the UI creation path");

            bool invalidNameRejected = false;
            try
            {
                MaterialAssetWorkflow.NextAvailablePath(assets, "bad/name");
            }
            catch (ArgumentException)
            {
                invalidNameRejected = true;
            }
            Check(invalidNameRejected,
                "generated material names reject directory traversal");

            (int duplicatePassed, int duplicateFailed) =
                MaterialEditorWindow.RunDuplicationContractSelfTest(output);
            passed += duplicatePassed;
            failed += duplicateFailed;
            (int historyPassed, int historyFailed) =
                MaterialEditorWindow.RunHistoryContractSelfTest(output);
            passed += historyPassed;
            failed += historyFailed;
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine("FAIL: unhandled exception: " + error);
        }
        finally
        {
            try
            {
                if (Directory.Exists(root))
                    Directory.Delete(root, recursive: true);
            }
            catch
            {
                // The directory is unique; cleanup failure is not a product failure.
            }
        }

        output.WriteLine(
            $"Material workflow self-test: {passed} PASS / {failed} failures");
        return failed;
    }
}
