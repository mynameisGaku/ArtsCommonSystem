// SPDX-License-Identifier: Apache-2.0
using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;

// 実Editorの生成処理を隔離fixtureへ呼び、既存ユーザーsourceと生成の再実行を検査する。
internal static class DxcGeneratedProjectTest
{
    // ユーザーが編集したsourceに見立て、生成処理の前後で内容と更新日時を比較する。
    private const string UserGame = "// ユーザー所有のsentinel。生成処理で変更しない。\n#include <windows.h>\nextern \"C\" __declspec(dllexport) int UserSentinel() { return 73; }\nint WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return 0; }\n";

    // 指定実験場所内の新規fixtureに限定する。Editorのentry pointやProject.Openは呼ばない。
    private static int Main(string[] args)
    {
        try
        {
            if (args.Length != 3) throw new ArgumentException("Expected editor assembly, new fixture root, and permitted artifact root");
            string fixtureRoot = Path.GetFullPath(args[1]);
            string permittedRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(args[2]));
            if (!fixtureRoot.StartsWith(permittedRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || Directory.Exists(fixtureRoot)) throw new InvalidOperationException("Fixture must be new and inside the permitted experiment root");
            Assembly editor = Assembly.LoadFrom(Path.GetFullPath(args[0]));
            Type manager = editor.GetType("AcsEditor.ProjectManager", throwOnError: true)!;
            Type projectType = editor.GetType("AcsEditor.Project", throwOnError: true)!;
            MethodInfo template = manager.GetMethod("CMakeTemplate", BindingFlags.NonPublic | BindingFlags.Static)!;
            MethodInfo ensure = manager.GetMethod("EnsureBuildFiles", BindingFlags.Public | BindingFlags.Static)!;
            foreach (string fixtureName in new[] { "exe project", "reflect project" })
            {
                string projectRoot = Path.Combine(fixtureRoot, fixtureName);
                string sourceRoot = Path.Combine(projectRoot, "Source");
                Directory.CreateDirectory(sourceRoot);
                string gamePath = Path.Combine(sourceRoot, "Game.cpp");
                string headerPath = Path.Combine(sourceRoot, "UserHeader.h");
                string manifestPath = Path.Combine(projectRoot, "DxcFixture.acsproject");
                string cmakePath = Path.Combine(sourceRoot, "CMakeLists.txt");
                File.WriteAllText(gamePath, UserGame, new UTF8Encoding(false));
                File.WriteAllText(headerPath, "// ユーザー所有header\n", new UTF8Encoding(false));
                File.WriteAllText(manifestPath, "{\"userSentinel\":73}\n", new UTF8Encoding(false));
                File.WriteAllText(cmakePath, "# fixtureだけの旧build定義\n", new UTF8Encoding(false));
                string[] protectedPaths = { gamePath, headerPath, manifestPath };
                byte[][] protectedBytes = protectedPaths.Select(File.ReadAllBytes).ToArray();
                DateTime[] protectedTimes = protectedPaths.Select(File.GetLastWriteTimeUtc).ToArray();
                object project = Activator.CreateInstance(projectType)!;
                projectType.GetProperty("Name")!.SetValue(project, "DxcFixture");
                projectType.GetProperty("ProjectFilePath")!.SetValue(project, manifestPath);
                string expected = (string)template.Invoke(null, new object[] { "DxcFixture", projectRoot })!;
                if (!(bool)ensure.Invoke(null, new[] { project })!) throw new InvalidOperationException("First EnsureBuildFiles did not update the fixture");
                if (File.ReadAllText(cmakePath) != expected) throw new InvalidOperationException("EnsureBuildFiles output differs from the real CMakeTemplate");
                // 同一内容なら書込まない既存契約を、古い更新日時を固定して検査する。
                DateTime unchangedTime = new(2001, 2, 3, 4, 5, 6, DateTimeKind.Utc);
                File.SetLastWriteTimeUtc(cmakePath, unchangedTime);
                if ((bool)ensure.Invoke(null, new[] { project })! || File.GetLastWriteTimeUtc(cmakePath) != unchangedTime) throw new InvalidOperationException("Repeated EnsureBuildFiles rewrote an unchanged file");
                for (int index = 0; index < protectedPaths.Length; ++index)
                {
                    if (!protectedBytes[index].SequenceEqual(File.ReadAllBytes(protectedPaths[index])) || protectedTimes[index] != File.GetLastWriteTimeUtc(protectedPaths[index])) throw new InvalidOperationException("User-owned file changed: " + protectedPaths[index]);
                }
                Console.WriteLine("PASS: " + fixtureName + " actual template/EnsureBuildFiles, idempotency, user-file preservation");
            }
            // Game.cppが無いprojectでも、build定義の修復がsourceを新規生成しないことを検査する。
            string emptyRoot = Path.Combine(fixtureRoot, "empty project");
            object emptyProject = Activator.CreateInstance(projectType)!;
            projectType.GetProperty("Name")!.SetValue(emptyProject, "EmptyFixture");
            projectType.GetProperty("ProjectFilePath")!.SetValue(emptyProject, Path.Combine(emptyRoot, "EmptyFixture.acsproject"));
            if (!(bool)ensure.Invoke(null, new[] { emptyProject })! || File.Exists(Path.Combine(emptyRoot, "Source", "Game.cpp"))) throw new InvalidOperationException("Build repair generated an unrequested Game.cpp");
            Console.WriteLine("PASS: missing Game.cpp remains absent");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
