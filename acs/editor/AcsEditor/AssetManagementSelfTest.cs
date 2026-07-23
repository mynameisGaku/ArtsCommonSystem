// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace AcsEditor;

internal static class AssetManagementSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;
        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-asset-management-selftest-" + Guid.NewGuid().ToString("N"));
        string project = Path.Combine(root, "Project");
        string assets = Path.Combine(project, "Assets");

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

        static bool Throws<T>(Action action) where T : Exception
        {
            try
            {
                action();
                return false;
            }
            catch (T)
            {
                return true;
            }
        }

        static void Write(string path, string text)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, text, new UTF8Encoding(false));
        }

        static void WriteLegacyGraphMetadata(
            string graphPath,
            string source,
            string ownerPath)
        {
            Write(
                graphPath + AssetDatabase.MetadataSuffix,
                "{\n" +
                "  \"schemaVersion\": 1,\n" +
                $"  \"id\": \"{Guid.NewGuid():N}\",\n" +
                "  \"kind\": \"data\",\n" +
                $"  \"source\": \"{source}\",\n" +
                "  \"importer\": \"passthrough\",\n" +
                "  \"importerVersion\": 1,\n" +
                "  \"dependencies\": [],\n" +
                "  \"importSettings\": {\n" +
                $"    \"owner\": \"{ownerPath}\"\n" +
                "  }\n" +
                "}\n");
        }

        try
        {
            Directory.CreateDirectory(assets);
            string loose = Path.Combine(assets, "Loose.acsbp");
            Write(loose, "ACSBP 1\n");
            var database = new AssetDatabase(project, assets);
            database.Refresh(verifyContent: true);
            var workflow = new AssetManagementWorkflow(database);
            Check(database.TryGetByPath(loose, out AssetRecord? looseRecord) && looseRecord != null,
                "fixture asset is indexed before management operations");

            string looseId = looseRecord!.AssetId;
            database.UpdateImportMetadata(
                looseId,
                "generated://selftest",
                "blueprint",
                7,
                importSettings: new[] { new KeyValuePair<string, string>("mode", "strict") });
            string renamed = workflow.Rename(loose, looseId, false, "Renamed");
            Check(renamed == Path.Combine(assets, "Renamed.acsbp") &&
                  !File.Exists(loose) && File.Exists(renamed) &&
                  File.Exists(renamed + AssetDatabase.MetadataSuffix) &&
                  !File.Exists(loose + AssetDatabase.MetadataSuffix) &&
                  database.TryGetByPath(renamed, out AssetRecord? renamedRecord) &&
                  renamedRecord?.AssetId == looseId &&
                  renamedRecord.Metadata.ImporterVersion == 7,
                "file rename fixes the extension and preserves GUID plus authoritative metadata");
            Check(Throws<InvalidDataException>(() =>
                    workflow.Rename(renamed, looseId, false, "renamed")) &&
                  Throws<InvalidDataException>(() =>
                    AssetManagementWorkflow.ValidateBaseName("bad.tmp-name")) &&
                  Throws<InvalidDataException>(() =>
                    AssetManagementWorkflow.ValidateBaseName("CON")) &&
                  Throws<InvalidDataException>(() =>
                    AssetManagementWorkflow.ValidateBaseName("../escape")),
                "rename rejects case-only, temporary, device, and traversal names");

            IReadOnlyList<string> duplicates = workflow.Duplicate(new[] { renamed });
            string duplicate = Path.Combine(assets, "Renamed_Copy.acsbp");
            Check(duplicates.SequenceEqual(new[] { duplicate }) &&
                  File.ReadAllText(duplicate) == "ACSBP 1\n" &&
                  database.TryGetByPath(duplicate, out AssetRecord? duplicateRecord) &&
                  duplicateRecord != null && duplicateRecord.AssetId != looseId &&
                  duplicateRecord.Metadata.Importer == "blueprint" &&
                  duplicateRecord.Metadata.ImporterVersion == 7 &&
                  duplicateRecord.Metadata.ImportSettings.GetValueOrDefault("mode") == "strict",
                "file duplicate publishes identical content with a new GUID and preserved import metadata");
            IReadOnlyList<string> secondDuplicate = workflow.Duplicate(new[] { renamed });
            Check(secondDuplicate.Single().EndsWith("Renamed_Copy2.acsbp", StringComparison.Ordinal),
                "duplicate naming remains deterministic across collisions");

            string moveSource = Path.Combine(assets, "MoveSource.txt");
            string moveFolder = Path.Combine(assets, "Destination");
            Write(moveSource, "move me");
            Directory.CreateDirectory(moveFolder);
            database.Refresh(verifyContent: true);
            database.TryGetByPath(moveSource, out AssetRecord? moveSourceRecord);
            string movedPath = workflow.Move(new[] { moveSource }, moveFolder).Single();
            Check(movedPath == Path.Combine(moveFolder, "MoveSource.txt") &&
                  !File.Exists(moveSource) && File.Exists(movedPath) &&
                  database.TryGetByPath(movedPath, out AssetRecord? movedRecord) &&
                  movedRecord?.AssetId == moveSourceRecord!.AssetId,
                "cut/paste move preserves GUID and sidecar identity across folders");

            string pack = Path.Combine(assets, "Pack");
            string material = Path.Combine(pack, "Surface.acsmat");
            string materialGraph = material + ".graph.json";
            string blueprint = Path.Combine(pack, "Actor.acsbp");
            Write(material, "ACSMAT 1\nname Surface\nkind pbr\n");
            Write(
                materialGraph,
                "{\"material\":\"Assets/Pack/Surface.acsmat\"," +
                "\"layout\":\"Assets/Pack/Surface.acsmat.graph.json\"}\n");
            WriteLegacyGraphMetadata(
                materialGraph,
                "Assets/Pack/Surface.acsmat.graph.json",
                "Assets/Pack/Surface.acsmat");
            Write(blueprint, "ACSBP 1\n");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(material, out AssetRecord? materialRecord);
            database.TryGetByPath(blueprint, out AssetRecord? blueprintRecord);
            Check(!database.TryGetByPath(materialGraph, out _),
                "material graph layout companion is hidden from the authoritative asset index");
            Write(
                blueprint,
                "ACSBP 1\n" +
                "ref Assets/Pack/Surface.acsmat\n" +
                "json {\"path\":\"Assets\\\\Pack\\\\Surface.acsmat\"}\n" +
                $"asset-id {materialRecord!.AssetId}\n");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(blueprint, out blueprintRecord);
            database.UpdateImportMetadata(
                blueprintRecord!.AssetId,
                "Assets/Pack/Actor.acsbp",
                "blueprint",
                2,
                new[] { materialRecord.AssetId },
                new[]
                {
                    new KeyValuePair<string, string>(
                        "targetPath",
                        "Assets/Pack/Surface.acsmat"),
                    new KeyValuePair<string, string>("targetId", materialRecord.AssetId),
                });
            string copiedPack = workflow.Duplicate(new[] { pack, material }).Single();
            string copiedMaterial = Path.Combine(copiedPack, "Surface.acsmat");
            string copiedMaterialGraph = copiedMaterial + ".graph.json";
            string copiedBlueprint = Path.Combine(copiedPack, "Actor.acsbp");
            database.TryGetByPath(copiedMaterial, out AssetRecord? copiedMaterialRecord);
            database.TryGetByPath(copiedBlueprint, out AssetRecord? copiedBlueprintRecord);
            Check(copiedPack.EndsWith("Pack_Copy", StringComparison.Ordinal) &&
                  copiedMaterialRecord != null && copiedBlueprintRecord != null &&
                  copiedMaterialRecord.AssetId != materialRecord.AssetId &&
                  copiedBlueprintRecord.AssetId != blueprintRecord.AssetId &&
                  copiedBlueprintRecord.Metadata.Dependencies.SequenceEqual(
                      new[] { copiedMaterialRecord.AssetId }) &&
                  copiedBlueprintRecord.Metadata.Source ==
                      "Assets/Pack_Copy/Actor.acsbp" &&
                  copiedBlueprintRecord.Metadata.ImportSettings.GetValueOrDefault(
                      "targetPath") == "Assets/Pack_Copy/Surface.acsmat" &&
                  copiedBlueprintRecord.Metadata.ImportSettings.GetValueOrDefault(
                      "targetId") == copiedMaterialRecord.AssetId &&
                  File.ReadAllText(copiedBlueprint).Contains(
                      "Assets/Pack_Copy/Surface.acsmat",
                      StringComparison.Ordinal) &&
                  File.ReadAllText(copiedBlueprint).Contains(
                      "Assets\\\\Pack_Copy\\\\Surface.acsmat",
                      StringComparison.Ordinal) &&
                  File.ReadAllText(copiedBlueprint).Contains(
                      copiedMaterialRecord.AssetId,
                      StringComparison.Ordinal) &&
                  File.Exists(copiedMaterialGraph) &&
                  File.Exists(copiedMaterialGraph + AssetDatabase.MetadataSuffix) &&
                  File.ReadAllText(copiedMaterialGraph).Contains(
                      "Assets/Pack_Copy/Surface.acsmat.graph.json",
                      StringComparison.Ordinal) &&
                  File.ReadAllText(copiedMaterialGraph + AssetDatabase.MetadataSuffix).Contains(
                      "Assets/Pack_Copy/Surface.acsmat",
                      StringComparison.Ordinal) &&
                  !database.TryGetByPath(copiedMaterialGraph, out _),
                "folder duplicate drops nested selections and remaps internal IDs, paths, content, and metadata");

            string oldMaterialId = materialRecord.AssetId;
            string renamedPack = workflow.Rename(pack, "", true, "PackRenamed");
            string movedMaterial = Path.Combine(renamedPack, "Surface.acsmat");
            string movedBlueprint = Path.Combine(renamedPack, "Actor.acsbp");
            string movedGraph = movedMaterial + ".graph.json";
            database.TryGetByPath(movedBlueprint, out AssetRecord? movedBlueprintRecord);
            Check(Directory.Exists(renamedPack) && !Directory.Exists(pack) &&
                  database.TryGetByPath(movedMaterial, out AssetRecord? movedMaterialRecord) &&
                  movedMaterialRecord?.AssetId == oldMaterialId &&
                  movedBlueprintRecord?.AssetId == blueprintRecord.AssetId &&
                  movedBlueprintRecord.Metadata.Source ==
                      "Assets/PackRenamed/Actor.acsbp" &&
                  movedBlueprintRecord.Metadata.ImportSettings.GetValueOrDefault(
                      "targetPath") == "Assets/PackRenamed/Surface.acsmat" &&
                  movedBlueprintRecord.Metadata.ImportSettings.GetValueOrDefault(
                      "targetId") == oldMaterialId &&
                  movedBlueprintRecord.Metadata.Dependencies.SequenceEqual(
                      new[] { oldMaterialId }) &&
                  File.ReadAllText(movedBlueprint).Contains(
                      "Assets/PackRenamed/Surface.acsmat",
                      StringComparison.Ordinal) &&
                  !File.ReadAllText(movedBlueprint).Contains(
                      "Assets/Pack/Surface.acsmat",
                      StringComparison.Ordinal) &&
                  File.Exists(movedGraph) &&
                  File.Exists(movedGraph + AssetDatabase.MetadataSuffix) &&
                  File.ReadAllText(movedGraph).Contains(
                      "Assets/PackRenamed/Surface.acsmat.graph.json",
                      StringComparison.Ordinal) &&
                  File.ReadAllText(movedGraph + AssetDatabase.MetadataSuffix).Contains(
                      "Assets/PackRenamed/Surface.acsmat",
                      StringComparison.Ordinal),
                "folder rename preserves GUIDs while transactionally remapping content, metadata, and graph companions");

            string relocatedRoot = Path.Combine(assets, "Relocated");
            Directory.CreateDirectory(relocatedRoot);
            AssetMoveResult relocated = workflow.MoveWithMappings(
                new[] { renamedPack },
                relocatedRoot);
            string relocatedPack = Path.Combine(relocatedRoot, "PackRenamed");
            string relocatedMaterial = Path.Combine(relocatedPack, "Surface.acsmat");
            string relocatedBlueprint = Path.Combine(relocatedPack, "Actor.acsbp");
            string relocatedGraph = relocatedMaterial + ".graph.json";
            database.TryGetByPath(relocatedBlueprint, out AssetRecord? relocatedBlueprintRecord);
            Check(relocated.PublishedPaths.SequenceEqual(new[] { relocatedPack }) &&
                  relocated.Mappings.SequenceEqual(new[]
                  {
                      new AssetMoveMapping(renamedPack, relocatedPack),
                  }) &&
                  !Directory.Exists(renamedPack) && Directory.Exists(relocatedPack) &&
                  database.TryGetByPath(relocatedMaterial, out AssetRecord? relocatedMaterialRecord) &&
                  relocatedMaterialRecord?.AssetId == oldMaterialId &&
                  relocatedBlueprintRecord?.AssetId == blueprintRecord.AssetId &&
                  relocatedBlueprintRecord.Metadata.Source ==
                      "Assets/Relocated/PackRenamed/Actor.acsbp" &&
                  relocatedBlueprintRecord.Metadata.ImportSettings.GetValueOrDefault(
                      "targetPath") ==
                      "Assets/Relocated/PackRenamed/Surface.acsmat" &&
                  File.ReadAllText(relocatedBlueprint).Contains(
                      "Assets/Relocated/PackRenamed/Surface.acsmat",
                      StringComparison.Ordinal) &&
                  File.ReadAllText(relocatedGraph).Contains(
                      "Assets/Relocated/PackRenamed/Surface.acsmat.graph.json",
                      StringComparison.Ordinal) &&
                  File.ReadAllText(relocatedGraph + AssetDatabase.MetadataSuffix).Contains(
                      "Assets/Relocated/PackRenamed/Surface.acsmat",
                      StringComparison.Ordinal),
                "folder move publishes exact root mappings and remaps internal references without changing GUIDs");
            renamedPack = relocatedPack;

            string soloMaterial = Path.Combine(assets, "Solo.acsmat");
            string soloGraph = soloMaterial + ".graph.json";
            Write(
                soloMaterial,
                "ACSMAT 1\nname Solo\nsource Assets/Solo.acsmat\n");
            Write(
                soloGraph,
                "{\"material\":\"Assets/Solo.acsmat\"," +
                "\"layout\":\"Assets/Solo.acsmat.graph.json\"}\n");
            WriteLegacyGraphMetadata(
                soloGraph,
                "Assets/Solo.acsmat.graph.json",
                "Assets/Solo.acsmat");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(soloMaterial, out AssetRecord? soloRecord);
            string soloCopy = workflow.Duplicate(new[] { soloMaterial }).Single();
            string soloCopyGraph = soloCopy + ".graph.json";
            Check(File.Exists(soloCopyGraph) &&
                  File.Exists(soloCopyGraph + AssetDatabase.MetadataSuffix) &&
                  File.ReadAllText(soloCopyGraph).Contains(
                      "Assets/Solo_Copy.acsmat.graph.json",
                      StringComparison.Ordinal) &&
                  File.ReadAllText(soloCopyGraph + AssetDatabase.MetadataSuffix).Contains(
                      "Assets/Solo_Copy.acsmat",
                      StringComparison.Ordinal) &&
                  !database.TryGetByPath(soloCopyGraph, out _),
                "single material duplicate atomically publishes its graph companion without indexing it separately");

            string renamedSolo = workflow.Rename(
                soloMaterial,
                soloRecord!.AssetId,
                false,
                "SoloRenamed");
            string renamedSoloGraph = renamedSolo + ".graph.json";
            Check(!File.Exists(soloMaterial) && !File.Exists(soloGraph) &&
                  File.Exists(renamedSoloGraph) &&
                  File.Exists(renamedSoloGraph + AssetDatabase.MetadataSuffix) &&
                  File.ReadAllText(renamedSolo).Contains(
                      "Assets/SoloRenamed.acsmat",
                      StringComparison.Ordinal) &&
                  File.ReadAllText(renamedSoloGraph).Contains(
                      "Assets/SoloRenamed.acsmat.graph.json",
                      StringComparison.Ordinal) &&
                  File.ReadAllText(renamedSoloGraph + AssetDatabase.MetadataSuffix).Contains(
                      "Assets/SoloRenamed.acsmat",
                      StringComparison.Ordinal) &&
                  database.TryGetByPath(renamedSolo, out AssetRecord? renamedSoloRecord) &&
                  renamedSoloRecord?.AssetId == soloRecord.AssetId,
                "single material rename moves and remaps its graph companion with the authoritative GUID");

            string soloDestination = Path.Combine(assets, "SoloDestination");
            Directory.CreateDirectory(soloDestination);
            string movedSolo = workflow.Move(new[] { renamedSolo }, soloDestination).Single();
            string movedSoloGraph = movedSolo + ".graph.json";
            Check(!File.Exists(renamedSolo) && !File.Exists(renamedSoloGraph) &&
                  File.Exists(movedSoloGraph) &&
                  File.Exists(movedSoloGraph + AssetDatabase.MetadataSuffix) &&
                  File.ReadAllText(movedSoloGraph).Contains(
                      "Assets/SoloDestination/SoloRenamed.acsmat.graph.json",
                      StringComparison.Ordinal) &&
                  File.ReadAllText(movedSoloGraph + AssetDatabase.MetadataSuffix).Contains(
                      "Assets/SoloDestination/SoloRenamed.acsmat",
                      StringComparison.Ordinal) &&
                  database.TryGetByPath(movedSolo, out AssetRecord? movedSoloRecord) &&
                  movedSoloRecord?.AssetId == soloRecord.AssetId,
                "single material move keeps graph companion paths and GUID identity coherent");
            AssetDeleteResult soloDeleted = workflow.Delete(new[] { movedSolo });
            Check(soloDeleted.AssetCount == 1 &&
                  !File.Exists(movedSolo) &&
                  !File.Exists(movedSolo + AssetDatabase.MetadataSuffix) &&
                  !File.Exists(movedSoloGraph) &&
                  !File.Exists(movedSoloGraph + AssetDatabase.MetadataSuffix) &&
                  !database.TryGetByAssetId(soloRecord.AssetId, out _),
                "single material delete quarantines and removes its graph companion as one asset family");

            string invalidGraphMaterial = Path.Combine(assets, "InvalidGraph.acsmat");
            string invalidGraph = invalidGraphMaterial + ".graph.json";
            Write(invalidGraphMaterial, "ACSMAT 1\nname InvalidGraph\nkind pbr\n");
            File.WriteAllBytes(invalidGraph, new byte[] { 0xC3, 0x28 });
            WriteLegacyGraphMetadata(
                invalidGraph,
                "Assets/InvalidGraph.acsmat.graph.json",
                "Assets/InvalidGraph.acsmat");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(invalidGraphMaterial, out AssetRecord? invalidGraphRecord);
            string invalidGraphDestination = Path.Combine(assets, "InvalidGraphRenamed.acsmat");
            Check(Throws<InvalidDataException>(() => workflow.Rename(
                      invalidGraphMaterial,
                      invalidGraphRecord!.AssetId,
                      false,
                      "InvalidGraphRenamed")) &&
                  File.Exists(invalidGraphMaterial) &&
                  File.Exists(invalidGraphMaterial + AssetDatabase.MetadataSuffix) &&
                  File.Exists(invalidGraph) &&
                  File.Exists(invalidGraph + AssetDatabase.MetadataSuffix) &&
                  !File.Exists(invalidGraphDestination) &&
                  !File.Exists(invalidGraphDestination + AssetDatabase.MetadataSuffix) &&
                  !File.Exists(invalidGraphDestination + ".graph.json") &&
                  !File.Exists(
                      invalidGraphDestination + ".graph.json" + AssetDatabase.MetadataSuffix) &&
                  File.ReadAllBytes(invalidGraph).SequenceEqual(new byte[] { 0xC3, 0x28 }) &&
                  database.TryGetByPath(
                      invalidGraphMaterial,
                      out AssetRecord? restoredInvalidGraphRecord) &&
                  restoredInvalidGraphRecord?.AssetId == invalidGraphRecord!.AssetId &&
                  !database.TryGetByPath(invalidGraph, out _),
                "failed single material rename rolls main asset and graph companion family back together");
            workflow.Delete(new[] { invalidGraphMaterial });

            string rollbackPack = Path.Combine(assets, "RollbackPack");
            string rollbackBlueprint = Path.Combine(rollbackPack, "A.acsbp");
            string rollbackMaterial = Path.Combine(rollbackPack, "B.acsmat");
            string rollbackGraph = rollbackMaterial + ".graph.json";
            Write(
                rollbackBlueprint,
                "ACSBP 1\nref Assets/RollbackPack/B.acsmat\n");
            Write(rollbackMaterial, "ACSMAT 1\nname B\nkind pbr\n");
            Write(
                rollbackGraph,
                "{\"material\":\"Assets/RollbackPack/B.acsmat\"}\n");
            WriteLegacyGraphMetadata(
                rollbackGraph,
                "Assets/RollbackPack/B.acsmat.graph.json",
                "Assets/RollbackPack/B.acsmat");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(rollbackBlueprint, out AssetRecord? rollbackBlueprintRecord);
            database.TryGetByPath(rollbackMaterial, out AssetRecord? rollbackMaterialRecord);
            const string rollbackSourceToken = "Assets/RollbackPack/A.acsbp";
            string rollbackSource = string.Join(
                "|",
                Enumerable.Repeat(
                    rollbackSourceToken,
                    4096 / (rollbackSourceToken.Length + 1)));
            database.UpdateImportMetadata(
                rollbackBlueprintRecord!.AssetId,
                rollbackSource,
                "blueprint",
                1,
                new[] { rollbackMaterialRecord!.AssetId },
                new[]
                {
                    new KeyValuePair<string, string>(
                        "targetPath",
                        "Assets/RollbackPack/B.acsmat"),
                });
            byte[] rollbackBlueprintBytes = File.ReadAllBytes(rollbackBlueprint);
            byte[] rollbackBlueprintMetadataBytes = File.ReadAllBytes(
                rollbackBlueprint + AssetDatabase.MetadataSuffix);
            byte[] rollbackGraphBytes = File.ReadAllBytes(rollbackGraph);
            byte[] rollbackGraphMetadataBytes = File.ReadAllBytes(
                rollbackGraph + AssetDatabase.MetadataSuffix);
            long rollbackBlueprintTicks = File.GetLastWriteTimeUtc(rollbackBlueprint).Ticks;
            string rollbackDestination = Path.Combine(
                assets,
                "RollbackPackDestinationWithLongerName");
            Check(Throws<InvalidDataException>(() => workflow.Rename(
                      rollbackPack,
                      "",
                      true,
                      "RollbackPackDestinationWithLongerName")) &&
                  Directory.Exists(rollbackPack) &&
                  !Directory.Exists(rollbackDestination) &&
                  File.ReadAllBytes(rollbackBlueprint).SequenceEqual(rollbackBlueprintBytes) &&
                  File.ReadAllBytes(rollbackBlueprint + AssetDatabase.MetadataSuffix)
                      .SequenceEqual(rollbackBlueprintMetadataBytes) &&
                  File.ReadAllBytes(rollbackGraph).SequenceEqual(rollbackGraphBytes) &&
                  File.ReadAllBytes(rollbackGraph + AssetDatabase.MetadataSuffix)
                      .SequenceEqual(rollbackGraphMetadataBytes) &&
                  File.GetLastWriteTimeUtc(rollbackBlueprint).Ticks == rollbackBlueprintTicks &&
                  database.TryGetByPath(
                      rollbackBlueprint,
                      out AssetRecord? restoredRollbackBlueprint) &&
                  restoredRollbackBlueprint?.AssetId == rollbackBlueprintRecord.AssetId &&
                  restoredRollbackBlueprint.Metadata.Source == rollbackSource &&
                  restoredRollbackBlueprint.Metadata.ImportSettings.GetValueOrDefault(
                      "targetPath") == "Assets/RollbackPack/B.acsmat" &&
                  database.TryGetByPath(
                      rollbackMaterial,
                      out AssetRecord? restoredRollbackMaterial) &&
                  restoredRollbackMaterial?.AssetId == rollbackMaterialRecord.AssetId &&
                  !database.TryGetByPath(rollbackGraph, out _),
                "failed folder rename restores rewritten bytes, timestamps, metadata, companions, paths, and GUIDs");

            string protectedAsset = Path.Combine(assets, "Protected.acsmat");
            string referencer = Path.Combine(assets, "UsesProtected.acscene");
            Write(protectedAsset, "ACSMAT 1\nname Protected\nkind pbr\n");
            Write(referencer, "ACSCENE v1\nMAT 1 Assets/Protected.acsmat\n");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(protectedAsset, out AssetRecord? protectedRecord);
            AssetDeleteInspection protectedInspection = workflow.InspectDelete(
                new[] { protectedAsset });
            Check(!protectedInspection.CanDelete && protectedInspection.Blockers.Any() &&
                  Throws<AssetOperationBlockedException>(() =>
                      workflow.Rename(
                          protectedAsset,
                          protectedRecord!.AssetId,
                          false,
                          "Broken")) &&
                  Throws<AssetOperationBlockedException>(() =>
                      workflow.Delete(new[] { protectedAsset })) &&
                  File.Exists(protectedAsset),
                "path-referenced assets are blocked before rename or delete mutation");

            string graphProtected = Path.Combine(assets, "GraphProtected.acsmat");
            string graphProtectedReference = Path.Combine(assets, "UsesGraphProtected.json");
            Write(graphProtected, "ACSMAT 1\nname GraphProtected\nkind pbr\n");
            Write(
                graphProtectedReference,
                "{\"layout\":\"Assets/GraphProtected.acsmat.graph.json\"}\n");
            string metadataProtected = Path.Combine(assets, "MetadataProtected.acsmat");
            string metadataOwner = Path.Combine(assets, "MetadataOwner.txt");
            Write(metadataProtected, "ACSMAT 1\nname MetadataProtected\nkind pbr\n");
            Write(metadataOwner, "metadata owner\n");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(graphProtected, out AssetRecord? graphProtectedRecord);
            database.TryGetByPath(metadataProtected, out AssetRecord? metadataProtectedRecord);
            database.TryGetByPath(metadataOwner, out AssetRecord? metadataOwnerRecord);
            database.UpdateImportMetadata(
                metadataOwnerRecord!.AssetId,
                "Assets/MetadataOwner.txt",
                "passthrough",
                1,
                importSettings: new[]
                {
                    new KeyValuePair<string, string>(
                        "material",
                        "Assets/MetadataProtected.acsmat"),
                });
            Check(!workflow.InspectDelete(new[] { graphProtected }).CanDelete &&
                  Throws<AssetOperationBlockedException>(() => workflow.Rename(
                      graphProtected,
                      graphProtectedRecord!.AssetId,
                      false,
                      "GraphReferenceMustSurvive")) &&
                  !workflow.InspectDelete(new[] { metadataProtected }).CanDelete &&
                  Throws<AssetOperationBlockedException>(() => workflow.Rename(
                      metadataProtected,
                      metadataProtectedRecord!.AssetId,
                      false,
                      "MetadataReferenceMustSurvive")) &&
                  File.Exists(graphProtected) && File.Exists(metadataProtected),
                "graph-companion and external metadata references block destructive path changes");

            string prefixAsset = Path.Combine(assets, "Prefix.acsmat");
            string prefixMention = Path.Combine(assets, "MentionsPrefix.txt");
            string escapedAsset = Path.Combine(assets, "Escaped.acsmat");
            string escapedReference = Path.Combine(assets, "UsesEscaped.json");
            string idAsset = Path.Combine(assets, "IdProtected.acsmat");
            string idReference = Path.Combine(assets, "UsesId.cs");
            Write(prefixAsset, "ACSMAT 1\nname Prefix\nkind pbr\n");
            Write(
                prefixMention,
                "Not references: Assets/Prefix.acsmat.bak and Assets/Prefix.acsmaterial\n");
            Write(escapedAsset, "ACSMAT 1\nname Escaped\nkind pbr\n");
            Write(
                escapedReference,
                "{\"path\":\"Assets\\\\Escaped.acsmat\"}\n");
            Write(idAsset, "ACSMAT 1\nname IdProtected\nkind pbr\n");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(prefixAsset, out AssetRecord? prefixRecord);
            database.TryGetByPath(idAsset, out AssetRecord? idRecord);
            Write(idReference, $"const string AssetId = \"{idRecord!.AssetId}\";\n");
            database.Refresh(verifyContent: true);
            AssetDeleteInspection prefixInspection = workflow.InspectDelete(
                new[] { prefixAsset });
            AssetDeleteInspection escapedInspection = workflow.InspectDelete(
                new[] { escapedAsset });
            AssetDeleteInspection idInspection = workflow.InspectDelete(
                new[] { idAsset });
            Check(prefixInspection.CanDelete &&
                  !escapedInspection.CanDelete &&
                  !idInspection.CanDelete &&
                  Throws<InvalidDataException>(() => workflow.Rename(
                      prefixAsset,
                      idRecord.AssetId,
                      false,
                      "WrongIdentity")) &&
                  File.Exists(prefixAsset) && File.Exists(idAsset),
                "reference checks reject escaped paths and raw IDs without prefix false positives, and rename binds path to GUID");

            string disposable = Path.Combine(assets, "Disposable.txt");
            Write(disposable, "delete me");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(disposable, out AssetRecord? disposableRecord);
            AssetDeleteInspection disposableInspection = workflow.InspectDelete(
                new[] { disposable });
            AssetDeleteResult deleted = workflow.Delete(new[] { disposable });
            Check(disposableInspection.CanDelete && disposableInspection.AssetCount == 1 &&
                  deleted.AssetCount == 1 && !File.Exists(disposable) &&
                  !File.Exists(disposable + AssetDatabase.MetadataSuffix) &&
                  !database.TryGetByAssetId(disposableRecord!.AssetId, out _),
                "delete quarantines asset and sidecar before removing the indexed identity");

            string internalFolder = Path.Combine(assets, "InternalRefs");
            string internalA = Path.Combine(internalFolder, "A.acsbp");
            string internalB = Path.Combine(internalFolder, "B.acsmat");
            Write(internalA, "ACSBP 1\nref Assets/InternalRefs/B.acsmat\n");
            Write(internalB, "ACSMAT 1\nname B\nkind pbr\n");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(internalA, out AssetRecord? internalARecord);
            database.TryGetByPath(internalB, out AssetRecord? internalBRecord);
            database.UpdateImportMetadata(
                internalARecord!.AssetId,
                "generated://a",
                "blueprint",
                1,
                new[] { internalBRecord!.AssetId });
            AssetDeleteInspection folderInspection = workflow.InspectDelete(
                new[] { internalFolder, internalB });
            AssetDeleteResult folderDeleted = workflow.Delete(
                new[] { internalFolder, internalB });
            Check(folderInspection.CanDelete && folderInspection.AssetCount == 2 &&
                  folderDeleted.AssetCount == 2 && !Directory.Exists(internalFolder),
                "batch folder delete collapses nested selections and permits internal references");

            string copySourceA = Path.Combine(assets, "CopySourceA", "Same.TXT");
            string copySourceB = Path.Combine(assets, "CopySourceB", "same.txt");
            string copyDestination = Path.Combine(assets, "CopyDestination");
            Write(copySourceA, "first");
            Write(copySourceB, "second");
            Directory.CreateDirectory(copyDestination);
            database.Refresh(verifyContent: true);
            IReadOnlyList<string> collisionCopies = workflow.Duplicate(
                new[] { copySourceA, copySourceB },
                copyDestination);
            Check(collisionCopies.Count == 2 &&
                  collisionCopies.Distinct(StringComparer.OrdinalIgnoreCase).Count() == 2 &&
                  collisionCopies.All(File.Exists) &&
                  collisionCopies.Any(path =>
                      Path.GetExtension(path).Equals(".TXT", StringComparison.Ordinal)) &&
                  collisionCopies.Select(File.ReadAllText)
                      .OrderBy(static value => value, StringComparer.Ordinal)
                      .SequenceEqual(new[] { "first", "second" }),
                "multi-copy reserves case-insensitive destinations and preserves extensions");

            string moveSourceA = Path.Combine(assets, "MoveSourceA", "Same.txt");
            string moveSourceB = Path.Combine(assets, "MoveSourceB", "same.txt");
            string moveDestination = Path.Combine(assets, "MoveCollisionDestination");
            Write(moveSourceA, "move first");
            Write(moveSourceB, "move second");
            Directory.CreateDirectory(moveDestination);
            database.Refresh(verifyContent: true);
            Check(Throws<IOException>(() => workflow.Move(
                      new[] { moveSourceA, moveSourceB },
                      moveDestination)) &&
                  File.Exists(moveSourceA) && File.Exists(moveSourceB) &&
                  !Directory.EnumerateFileSystemEntries(moveDestination).Any(),
                "multi-move detects case-insensitive destination collisions before mutation");

            string unindexedProtected = Path.Combine(assets, "UnindexedProtected.acsmat");
            string unindexedReferencer = Path.Combine(assets, "BrokenReferencer.json");
            Write(unindexedProtected, "ACSMAT 1\nname UnindexedProtected\nkind pbr\n");
            Write(
                unindexedReferencer,
                "{\"path\":\"Assets/UnindexedProtected.acsmat\"}\n");
            Write(
                unindexedReferencer + AssetDatabase.MetadataSuffix,
                "{ invalid metadata");
            database.Refresh(verifyContent: true);
            Check(!workflow.InspectDelete(new[] { unindexedProtected }).CanDelete,
                "path scan includes physical text referencers rejected by the metadata index");

            string unindexedFolder = Path.Combine(assets, "UnindexedFolder");
            string unindexedAsset = Path.Combine(unindexedFolder, "Broken.txt");
            Write(unindexedAsset, "must survive");
            Write(unindexedAsset + AssetDatabase.MetadataSuffix, "{ invalid metadata");
            database.Refresh(verifyContent: true);
            Check(Throws<InvalidDataException>(() => workflow.Delete(
                      new[] { unindexedFolder })) &&
                  File.Exists(unindexedAsset) && Directory.Exists(unindexedFolder),
                "folder delete fails closed when a physical child is absent from the authoritative index");

            string outside = Path.Combine(root, "Outside.txt");
            Write(outside, "outside");
            Check(Throws<InvalidDataException>(() => workflow.ValidateExternalPath(outside)) &&
                  Throws<InvalidDataException>(() => workflow.Duplicate(
                      new[] { renamedPack }, renamedPack)) &&
                  Throws<InvalidDataException>(() => workflow.Move(
                      new[] { renamedPack }, renamedPack)),
                "external targets and copy/move-into-source operations fail before mutation");

            string linkedTarget = Path.Combine(root, "LinkedTarget");
            string linked = Path.Combine(assets, "Linked");
            Directory.CreateDirectory(linkedTarget);
            try
            {
                Directory.CreateSymbolicLink(linked, linkedTarget);
                Check(Throws<InvalidDataException>(() => workflow.Duplicate(new[] { linked })) &&
                      Throws<InvalidDataException>(() => workflow.Delete(new[] { linked })),
                    "reparse-point asset trees are rejected for copy and delete");
            }
            catch (Exception error) when (
                error is UnauthorizedAccessException or IOException or NotSupportedException)
            {
                output.WriteLine("SKIP: reparse-point runtime test: " + error.Message);
            }

            string operations = Path.Combine(
                assets,
                AssetDatabase.InternalDirectoryName,
                "operations");
            Check(!Directory.Exists(operations) ||
                  !Directory.EnumerateDirectories(operations).Any(),
                "successful operations leave no staging or quarantine transaction");
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
                if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
            }
            catch
            {
            }
        }

        output.WriteLine(
            $"Asset management self-test: {passed} PASS / {failed} failures");
        return failed;
    }
}
