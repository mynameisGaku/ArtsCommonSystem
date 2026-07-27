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
            unpacked3D == subsystem3D,
            "transaction envelope round-trips both compatibility payloads exactly");
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
