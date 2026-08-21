// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using AcsEditor.Packaging;

namespace AcsEditor;

/// <summary>
/// Executable fixture for the accepted single-scene contract. It keeps the
/// project template, editor view, transaction envelope, and package bootstrap
/// boundaries from drifting independently while the legacy source adapters
/// are still in service.
/// </summary>
internal static class SceneContractFixtureSelfTest
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
                output.WriteLine("PASS  " + label);
            }
            else
            {
                failed++;
                output.WriteLine("FAIL  " + label);
            }
        }

        ProjectManager.NewProjectScenePlan perspective =
            ProjectManager.PlanNewProjectScene("3d");
        ProjectManager.NewProjectScenePlan orthographic =
            ProjectManager.PlanNewProjectScene("2d");
        ProjectManager.NewProjectScenePlan blankAlias =
            ProjectManager.PlanNewProjectScene("blank");

        Check(
            perspective.InitialScene == "Assets/main.acs3d" &&
            orthographic.InitialScene == perspective.InitialScene &&
            blankAlias.InitialScene == perspective.InitialScene &&
            perspective.SourceMode == SceneDocumentMode.ThreeD &&
            orthographic.SourceMode == SceneDocumentMode.ThreeD &&
            blankAlias.SourceMode == SceneDocumentMode.ThreeD,
            "new 2D and 3D projects persist one ACS3D scene source");
        Check(
            !perspective.StartsOrthographic &&
            orthographic.StartsOrthographic &&
            !blankAlias.StartsOrthographic &&
            EditorSceneViewModePolicy.InitialForProject(
                perspective.InitialScene,
                perspective.Template) == EditorSceneViewMode.Perspective &&
            EditorSceneViewModePolicy.InitialForProject(
                orthographic.InitialScene,
                orthographic.Template) == EditorSceneViewMode.Orthographic,
            "2D is an Orthographic view preset, not a second document kind");

        EditorSceneStartupPlan noProjectStartup =
            EditorSceneStartupPolicy.Resolve(null, null);
        EditorSceneStartupPlan blankStartup =
            EditorSceneStartupPolicy.Resolve("   ", "blank");
        EditorSceneStartupPlan twoDProjectStartup =
            EditorSceneStartupPolicy.Resolve(
                "Assets/main.acs3d",
                "2d");
        EditorSceneStartupPlan legacyStartup =
            EditorSceneStartupPolicy.Resolve(
                "Assets/legacy.acscene",
                "3d");
        Check(
            noProjectStartup is
            {
                SourceMode: SceneDocumentMode.ThreeD,
                ViewMode: EditorSceneViewMode.Perspective,
                SourceExtension: ".acs3d",
                Uses3D: true,
            } &&
            blankStartup == noProjectStartup &&
            twoDProjectStartup is
            {
                SourceMode: SceneDocumentMode.ThreeD,
                ViewMode: EditorSceneViewMode.Orthographic,
                Uses3D: true,
            } &&
            legacyStartup is
            {
                SourceMode: SceneDocumentMode.TwoD,
                ViewMode: EditorSceneViewMode.Orthographic,
                SourceExtension: ".acscene",
                Uses3D: false,
            },
            "startup without a source is blank ACS3D/Perspective while legacy ACSCENE remains an explicit adapter");

        Check(
            perspective.SceneText.StartsWith(
                "ACS3D v2\n",
                StringComparison.Ordinal) &&
            orthographic.SceneText.StartsWith(
                "ACS3D v2\n",
                StringComparison.Ordinal) &&
            blankAlias.Template == "3d",
            "project templates emit the accepted ACS3D v2 source contract");

        CanonicalSceneAdapterInspection perspectiveInspection =
            CanonicalSceneAdapter.InspectText(
                perspective.SceneText,
                ".acs3d");
        CanonicalSceneAdapterInspection orthographicInspection =
            CanonicalSceneAdapter.InspectText(
                orthographic.SceneText,
                ".acs3d");
        Check(
            !perspectiveInspection.HasErrors &&
            !orthographicInspection.HasErrors &&
            perspectiveInspection.Envelope.path ==
                CanonicalSceneAdapter.BootstrapPath &&
            perspectiveInspection.Envelope.contract ==
                CanonicalSceneAdapter.BootstrapContract &&
            perspectiveInspection.Envelope.sourceFormat ==
                CanonicalSceneAdapter.LegacyScene3DFormat,
            "new project sources satisfy the canonical package adapter");
        Check(
            perspectiveInspection.Envelope.adapterProjectionHint ==
                "perspective" &&
            EditorSceneViewModePolicy.InitialForProject(
                orthographic.InitialScene,
                orthographic.Template) == EditorSceneViewMode.Orthographic,
            "package adapter projection hint cannot override the authored view preset");

        const string cameraScene =
            "ACS3D v2\n" +
            "N3D 10 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 GameplayCamera\n" +
            "CAM3D 10 gameplay.main 0 20 1 65 12 0.05 5000\n" +
            "N3D 20 -1 -1 0 2 -8 0 0 0 1 1 1 1 1 1 1 CinematicCamera\n" +
            "CAM3D 20 cinematic-a 1 20 1 45 18 0.1 8000\n";
        CanonicalSceneAdapterInspection cameraInspection =
            CanonicalSceneAdapter.InspectText(cameraScene, ".acs3d");
        Check(
            !cameraInspection.HasErrors &&
            cameraInspection.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_CAMERA_MULTIPLE_ACTIVE" &&
                diagnostic.Severity ==
                    CanonicalSceneAdapterSeverity.Warning &&
                diagnostic.Message.Contains(
                    "priority, stable camera id, then node id",
                    StringComparison.Ordinal)),
            "CAM3D accepts multiple cameras and specifies deterministic active-camera ordering");

        const string duplicateCameraScene =
            "ACS3D v2\n" +
            "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 A\n" +
            "CAM3D 1 duplicate 0 0 1 60 10 0.1 1000\n" +
            "N3D 2 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 B\n" +
            "CAM3D 2 duplicate 0 1 0 60 10 0.1 1000\n";
        CanonicalSceneAdapterInspection duplicateCameraInspection =
            CanonicalSceneAdapter.InspectText(
                duplicateCameraScene,
                ".acs3d");
        CanonicalSceneAdapterInspection invalidCameraInspection =
            CanonicalSceneAdapter.InspectText(
                "ACS3D v2\n" +
                "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Camera\n" +
                "CAM3D 1 bad/id 0 0 1 60 10 1 1\n",
                ".acs3d");
        Check(
            duplicateCameraInspection.HasErrors &&
            duplicateCameraInspection.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_CAMERA_DUPLICATE") &&
            invalidCameraInspection.HasErrors &&
            invalidCameraInspection.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_CAMERA_INVALID"),
            "CAM3D duplicate identity and malformed optics fail closed");

        const string polygonScene =
            "ACS3D v2\n" +
            "N3D 7 -1 3 0 0 0 0 0 0 1 1 1 0.2 0.4 0.8 1 RuntimePolygon\n" +
            "PLY3D 7 4 -2 -1 2 -1 1 3 -1 2\n";
        CanonicalSceneAdapterInspection polygonInspection =
            CanonicalSceneAdapter.InspectText(polygonScene, ".acs3d");
        string cookedPolygon = CanonicalSceneAdapter.RewriteReferences(
            polygonScene,
            ".acs3d",
            static _ => throw new InvalidOperationException(
                "PLY3D must not create an asset reference."));
        Check(
            !polygonInspection.HasErrors &&
            polygonInspection.References.Count == 0 &&
            cookedPolygon == polygonScene,
            "PLY3D passes the package boundary and Cook preserves its deterministic point payload");

        CanonicalSceneAdapterInspection invalidPolygon =
            CanonicalSceneAdapter.InspectText(
                "ACS3D v2\n" +
                "N3D 7 -1 3 0 0 0 0 0 0 1 1 1 1 1 1 1 Polygon\n" +
                "PLY3D 7 3 0 0 1 nan 0 1\n",
                ".acs3d");
        CanonicalSceneAdapterInspection wrongPolygonPrimitive =
            CanonicalSceneAdapter.InspectText(
                "ACS3D v2\n" +
                "N3D 7 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Cube\n" +
                "PLY3D 7 3 0 0 1 0 0 1\n",
                ".acs3d");
        CanonicalSceneAdapterInspection duplicatePolygonGeometry =
            CanonicalSceneAdapter.InspectText(
                "ACS3D v2\n" +
                "N3D 7 -1 3 0 0 0 0 0 0 1 1 1 1 1 1 1 Polygon\n" +
                "MSH3D 7 mesh.acmesh\n" +
                "PLY3D 7 3 0 0 1 0 0 1\n",
                ".acs3d");
        Check(
            invalidPolygon.HasErrors &&
            invalidPolygon.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_POLYGON_INVALID") &&
            wrongPolygonPrimitive.HasErrors &&
            wrongPolygonPrimitive.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_POLYGON_PRIMITIVE_INVALID") &&
            duplicatePolygonGeometry.HasErrors &&
            duplicatePolygonGeometry.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_REFERENCE_DUPLICATE"),
            "PLY3D package validation rejects invalid points, non-Mesh owners, and duplicate geometry");

        const string spriteScene =
            "ACS3D v2\n" +
            "N3D 5 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Sign\n" +
            "SPR3D 5 Textures/sign.png\n";
        CanonicalSceneAdapterInspection spriteInspection =
            CanonicalSceneAdapter.InspectText(spriteScene, ".acs3d");
        string cookedSprite = CanonicalSceneAdapter.RewriteReferences(
            spriteScene,
            ".acs3d",
            static reference =>
                reference.Kind == CanonicalSceneReferenceKind.Texture
                    ? "Assets/Textures/sign.png"
                    : throw new InvalidOperationException(
                        "SPR3D must expose one texture reference."));
        Check(
            !spriteInspection.HasErrors &&
            spriteInspection.References.Count == 1 &&
            spriteInspection.References[0].Kind ==
                CanonicalSceneReferenceKind.Texture &&
            cookedSprite.Contains(
                "SPR3D 5 Assets/Textures/sign.png\n",
                StringComparison.Ordinal),
            "SPR3D passes package validation and Cook rewrites its texture path");

        CanonicalSceneAdapterInspection duplicateSprite =
            CanonicalSceneAdapter.InspectText(
                spriteScene + "SPR3D 5 Textures/other.png\n",
                ".acs3d");
        CanonicalSceneAdapterInspection missingSpriteNode =
            CanonicalSceneAdapter.InspectText(
                "ACS3D v2\nSPR3D 99 Textures/sign.png\n",
                ".acs3d");
        Check(
            duplicateSprite.HasErrors &&
            duplicateSprite.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_REFERENCE_DUPLICATE") &&
            missingSpriteNode.HasErrors &&
            missingSpriteNode.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_REFERENCE_INVALID"),
            "SPR3D duplicate and missing-node references fail closed");

        const string prefabScene =
            "ACS3D v2\n" +
            "N3D 8 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Vehicle\n" +
            "CMP3D 8 AWaterSurface3DComponent\n" +
            "CPROP3D 8 0 4 0.25 0 0 0\n" +
            "PFAB3D 8 Prefabs/vehicle.acsprefab\n" +
            "PINS3D 8 0123456789abcdef0123456789abcdef\n" +
            "POVR3D 8 5\n" +
            "PCOVR3D 8 0 4\n" +
            "N3D 9 8 0 0 0 0 0 0 0 1 1 1 0.4 0.5 0.6 1 Wheel\n" +
            "PSID3D 9 fedcba9876543210fedcba9876543210\n" +
            "PNOVR3D 9 127\n";
        CanonicalSceneAdapterInspection prefabInspection =
            CanonicalSceneAdapter.InspectText(prefabScene, ".acs3d");
        string cookedPrefab = CanonicalSceneAdapter.RewriteReferences(
            prefabScene,
            ".acs3d",
            static reference =>
                reference.Kind == CanonicalSceneReferenceKind.Prefab
                    ? "Assets/Prefabs/vehicle.acsprefab"
                    : throw new InvalidOperationException(
                        "PFAB3D must expose one Prefab reference."));
        Check(
            !prefabInspection.HasErrors &&
            prefabInspection.References.Count == 1 &&
            prefabInspection.References[0].Kind ==
                CanonicalSceneReferenceKind.Prefab &&
            cookedPrefab.Contains(
                "PFAB3D 8 Assets/Prefabs/vehicle.acsprefab\n",
                StringComparison.Ordinal) &&
            cookedPrefab.Contains(
                "PINS3D 8 0123456789abcdef0123456789abcdef\n",
                StringComparison.Ordinal) &&
            cookedPrefab.Contains(
                "POVR3D 8 5\n",
                StringComparison.Ordinal) &&
            cookedPrefab.Contains(
                "PCOVR3D 8 0 4\n",
                StringComparison.Ordinal) &&
            cookedPrefab.Contains(
                "PNOVR3D 9 127\n",
                StringComparison.Ordinal),
            "PFAB3D/PINS3D/PSID3D/POVR3D/PNOVR3D/PCOVR3D pass package validation while Cook rewrites only the source link");

        CanonicalSceneAdapterInspection duplicatePrefab =
            CanonicalSceneAdapter.InspectText(
                prefabScene + "PFAB3D 8 Prefabs/other.acsprefab\n",
                ".acs3d");
        CanonicalSceneAdapterInspection invalidPrefabExtension =
            CanonicalSceneAdapter.InspectText(
                "ACS3D v2\n" +
                "N3D 8 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Vehicle\n" +
                "PFAB3D 8 Prefabs/vehicle.txt\n",
                ".acs3d");
        Check(
            duplicatePrefab.HasErrors &&
            duplicatePrefab.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_REFERENCE_DUPLICATE") &&
            invalidPrefabExtension.HasErrors &&
            invalidPrefabExtension.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_FORMAT_UNSUPPORTED"),
            "PFAB3D duplicate links and unsupported source extensions fail closed");

        CanonicalSceneAdapterInspection invalidPrefabIdentity =
            CanonicalSceneAdapter.InspectText(
                "ACS3D v2\n" +
                "N3D 8 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Vehicle\n" +
                "PINS3D 8 0123456789ABCDEF0123456789ABCDEF\n",
                ".acs3d");
        CanonicalSceneAdapterInspection duplicatePrefabIdentity =
            CanonicalSceneAdapter.InspectText(
                prefabScene +
                "N3D 10 8 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 WheelInstance\n" +
                "PFAB3D 10 Prefabs/wheel.acsprefab\n" +
                "PINS3D 10 0123456789abcdef0123456789abcdef\n",
                ".acs3d");
        Check(
            invalidPrefabIdentity.HasErrors &&
            invalidPrefabIdentity.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_INSTANCE_ID_INVALID") &&
            duplicatePrefabIdentity.HasErrors &&
            duplicatePrefabIdentity.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_INSTANCE_ID_DUPLICATE"),
            "PINS3D orphan, malformed, and duplicate identities fail closed");

        CanonicalSceneAdapterInspection invalidPrefabOverride =
            CanonicalSceneAdapter.InspectText(
                prefabScene.Replace("POVR3D 8 5\n", "POVR3D 8 8\n", StringComparison.Ordinal),
                ".acs3d");
        CanonicalSceneAdapterInspection duplicatePrefabOverride =
            CanonicalSceneAdapter.InspectText(
                prefabScene + "POVR3D 8 1\n",
                ".acs3d");
        CanonicalSceneAdapterInspection orphanPrefabOverride =
            CanonicalSceneAdapter.InspectText(
                "ACS3D v2\n" +
                "N3D 8 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Vehicle\n" +
                "POVR3D 8 1\n",
                ".acs3d");
        Check(
            invalidPrefabOverride.HasErrors &&
            invalidPrefabOverride.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_ROOT_OVERRIDE_INVALID") &&
            duplicatePrefabOverride.HasErrors &&
            duplicatePrefabOverride.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_ROOT_OVERRIDE_DUPLICATE") &&
            orphanPrefabOverride.HasErrors &&
            orphanPrefabOverride.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_ROOT_OVERRIDE_INVALID"),
            "POVR3D unknown bits, duplicates, and orphan records fail closed");

        CanonicalSceneAdapterInspection invalidPrefabNodeOverride =
            CanonicalSceneAdapter.InspectText(
                prefabScene.Replace("PNOVR3D 9 127\n", "PNOVR3D 9 128\n", StringComparison.Ordinal),
                ".acs3d");
        CanonicalSceneAdapterInspection duplicatePrefabNodeOverride =
            CanonicalSceneAdapter.InspectText(
                prefabScene + "PNOVR3D 9 1\n",
                ".acs3d");
        CanonicalSceneAdapterInspection orphanPrefabNodeOverride =
            CanonicalSceneAdapter.InspectText(
                "ACS3D v2\n" +
                "N3D 9 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Wheel\n" +
                "PSID3D 9 fedcba9876543210fedcba9876543210\n" +
                "PNOVR3D 9 1\n",
                ".acs3d");
        Check(
            invalidPrefabNodeOverride.HasErrors &&
            invalidPrefabNodeOverride.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_NODE_OVERRIDE_INVALID") &&
            duplicatePrefabNodeOverride.HasErrors &&
            duplicatePrefabNodeOverride.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_NODE_OVERRIDE_DUPLICATE") &&
            orphanPrefabNodeOverride.HasErrors &&
            orphanPrefabNodeOverride.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_NODE_OVERRIDE_SCOPE_INVALID"),
            "PNOVR3D unknown bits, duplicates, and orphan scopes fail closed");

        CanonicalSceneAdapterInspection invalidPrefabComponentOverride =
            CanonicalSceneAdapter.InspectText(
                prefabScene.Replace(
                    "PCOVR3D 8 0 4\n",
                    "PCOVR3D 8 1 4\n",
                    StringComparison.Ordinal),
                ".acs3d");
        CanonicalSceneAdapterInspection duplicatePrefabComponentOverride =
            CanonicalSceneAdapter.InspectText(
                prefabScene + "PCOVR3D 8 0 4\n",
                ".acs3d");
        CanonicalSceneAdapterInspection orphanPrefabComponentOverride =
            CanonicalSceneAdapter.InspectText(
                "ACS3D v2\n" +
                "N3D 8 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Vehicle\n" +
                "CMP3D 8 AWaterSurface3DComponent\n" +
                "PCOVR3D 8 0 4\n",
                ".acs3d");
        Check(
            invalidPrefabComponentOverride.HasErrors &&
            invalidPrefabComponentOverride.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_COMPONENT_OVERRIDE_INVALID") &&
            duplicatePrefabComponentOverride.HasErrors &&
            duplicatePrefabComponentOverride.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_COMPONENT_OVERRIDE_DUPLICATE") &&
            orphanPrefabComponentOverride.HasErrors &&
            orphanPrefabComponentOverride.Diagnostics.Any(static diagnostic =>
                diagnostic.Code == "SCENE3D_PREFAB_COMPONENT_OVERRIDE_INVALID"),
            "PCOVR3D missing slots, duplicates, and orphan records fail closed");

        CanonicalSceneAdapterInspection legacy2D =
            CanonicalSceneAdapter.InspectText(
                "ACSCENE v1\n",
                ".acscene");
        Check(
            !legacy2D.HasErrors &&
            legacy2D.Envelope.path ==
                CanonicalSceneAdapter.BootstrapPath &&
            legacy2D.Envelope.contract ==
                CanonicalSceneAdapter.BootstrapContract &&
            legacy2D.Envelope.sourceFormat ==
                CanonicalSceneAdapter.LegacyScene2DFormat &&
            legacy2D.Envelope.adapterProjectionHint == "orthographic",
            "legacy ACSCENE remains an explicit compatibility adapter");

        CanonicalSceneAdapterInspection unsupported =
            CanonicalSceneAdapter.InspectText(
                "ACS3D v2\n",
                ".scene");
        Check(
            unsupported.HasErrors &&
            unsupported.Diagnostics.Any(static diagnostic =>
                diagnostic.Code ==
                "SCENE_ADAPTER_EXTENSION_UNSUPPORTED"),
            "unknown scene source formats fail closed at the package boundary");

        string subsystem2D =
            "ACSCENE v1\nSPRT 1 Assets/Textures/snow-\u96ea.png\n";
        string subsystem3D =
            "ACS3D v2\nN3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Player-\ud83c\udfae\n";
        string packed =
            SceneWorldDocumentEnvelope.Pack(subsystem2D, subsystem3D);
        SceneWorldDocumentEnvelope.Unpack(
            packed,
            out string unpacked2D,
            out string unpacked3D);
        Check(
            unpacked2D == subsystem2D &&
            unpacked3D == subsystem3D &&
            SceneWorldDocumentEnvelope.SelectSubsystem(
                packed,
                use3D: false) == subsystem2D &&
            SceneWorldDocumentEnvelope.SelectSubsystem(
                packed,
                use3D: true) == subsystem3D,
            "transaction envelope round-trips and selects both compatibility payloads exactly");

        var canonicalView = new EditorSceneViewState(
            SceneDocumentMode.ThreeD,
            EditorSceneViewMode.Perspective);
        bool canonicalOrthoAccepted = canonicalView.TryChangeView(
            EditorSceneViewMode.Orthographic,
            out EditorSceneViewState canonicalOrtho);
        var legacyView = new EditorSceneViewState(
            SceneDocumentMode.TwoD,
            EditorSceneViewMode.Orthographic);
        bool legacyPerspectiveRejected = !legacyView.TryChangeView(
            EditorSceneViewMode.Perspective,
            out EditorSceneViewState unchangedLegacy);
        Check(
            canonicalOrthoAccepted &&
            canonicalOrtho.SourceMode == SceneDocumentMode.ThreeD &&
            canonicalOrtho.ActivePayloadKey == "acs3d" &&
            legacyPerspectiveRejected &&
            unchangedLegacy == legacyView &&
            unchangedLegacy.ActivePayloadKey == "acscene",
            "view changes preserve the canonical or legacy source adapter without hidden migration");

        bool everySceneGameViewTransitionIsPresentationOnly = true;
        foreach (bool gameView in new[] { false, true })
        {
            foreach (int playState in new[] { 0, 1, 2 })
            {
                EditorViewSwitchPlan plan =
                    EditorViewSwitchPolicy.Plan(gameView, playState);
                everySceneGameViewTransitionIsPresentationOnly &=
                    !plan.StartPlay &&
                    !plan.StopPlay &&
                    !plan.MutateEditorNavigationCamera;
            }
        }
        Check(
            everySceneGameViewTransitionIsPresentationOnly,
            "Scene/Game presentation never owns Play lifetime or the editor navigation camera");

        Check(
            RejectEnvelope("ACS_EDITOR_WORLD 2\n0\n0\n") &&
            RejectEnvelope("ACS_EDITOR_WORLD 1\n1\n0\n") &&
            RejectEnvelope("ACS_EDITOR_WORLD 1\n0\n0\ntrailing"),
            "transaction envelope rejects version, length, and trailing-data drift");

        bool invalidTemplateRejected = false;
        try
        {
            _ = ProjectManager.PlanNewProjectScene("separate-2d-scene");
        }
        catch (ArgumentException)
        {
            invalidTemplateRejected = true;
        }
        Check(
            invalidTemplateRejected,
            "project creation rejects a second dimensional source format");

        output.WriteLine(
            $"Scene contract fixture self-test: passed={passed} failed={failed}");
        return failed;
    }

    private static bool RejectEnvelope(string payload)
    {
        try
        {
            SceneWorldDocumentEnvelope.Unpack(
                payload,
                out _,
                out _);
            return false;
        }
        catch (InvalidDataException)
        {
            return true;
        }
    }
}
