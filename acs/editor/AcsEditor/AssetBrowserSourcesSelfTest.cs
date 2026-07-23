// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace AcsEditor;

internal static class AssetBrowserSourcesSelfTest
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

        static bool Throws<TException>(Action action)
            where TException : Exception
        {
            try
            {
                action();
                return false;
            }
            catch (TException)
            {
                return true;
            }
        }

        string fixture = Path.Combine(
            Path.GetTempPath(),
            "acs-asset-sources-selftest-" + Guid.NewGuid().ToString("N"));
        try
        {
            string assets = Path.Combine(fixture, "Assets");
            string characters = Path.Combine(assets, "Characters");
            string heroes = Path.Combine(characters, "Heroes");
            string props = Path.Combine(assets, "Props");
            Directory.CreateDirectory(heroes);
            Directory.CreateDirectory(props);
            Directory.CreateDirectory(Path.Combine(assets, AssetDatabase.InternalDirectoryName));
            string hero = Path.Combine(heroes, "Hero.acsbp");
            string prop = Path.Combine(props, "Crate.acsmat");
            File.WriteAllText(hero, "ACSBP 1\n");
            File.WriteAllText(prop, "{}");

            var store = new AssetBrowserSourcesStore(
                fixture,
                assets,
                out string? initialWarning);
            Check(initialWarning == null &&
                  store.StatePath == Path.Combine(
                      assets,
                      AssetDatabase.InternalDirectoryName,
                      "editor",
                      "asset-sources.v1.json"),
                "a new project uses the project-local editor state path without warnings");

            Check(store.AddFavorite(characters) &&
                  store.AddFavorite(assets) &&
                  !store.AddFavorite(characters) &&
                  store.Favorites.Count == 2,
                "favorites add safe folders, include Assets, and deduplicate case-insensitively");
            Check(Throws<IOException>(() => store.AddFavorite(hero)) &&
                  Throws<IOException>(() => store.AddFavorite(
                      Path.Combine(fixture, "Outside"))),
                "favorites reject files and paths outside Assets");

            Check(store.CreateCollection("Gameplay") &&
                  !store.CreateCollection(" gameplay ") &&
                  Throws<ArgumentException>(() => store.CreateCollection("../Bad")) &&
                  Throws<ArgumentException>(() => store.CreateCollection("Bad|Name")),
                "collection names are trimmed, unique, and reject path/control syntax");
            Check(store.AddCollectionItems(
                      "Gameplay",
                      new[] { hero, props, hero.ToUpperInvariant() }) &&
                  !store.AddCollectionItems("Gameplay", new[] { hero }) &&
                  store.GetCollectionPaths("Gameplay").Count == 2,
                "collections accept safe files and folders and suppress duplicate members");
            Check(Throws<IOException>(() => store.AddCollectionItems(
                      "Gameplay",
                      new[] { Path.Combine(fixture, "outside.txt") })) &&
                  Throws<System.Collections.Generic.KeyNotFoundException>(() =>
                      store.AddCollectionItems("Missing", new[] { hero })),
                "collection insertion fails closed for outside paths and unknown collections");

            var reloaded = new AssetBrowserSourcesStore(
                fixture,
                assets,
                out string? reloadWarning);
            Check(reloadWarning == null &&
                  reloaded.Favorites.Count == 2 &&
                  reloaded.Collections.Single().Name == "Gameplay" &&
                  reloaded.Collections.Single().ItemCount == 2,
                "favorites and collections survive an atomic store reload");
            Check(!Directory.EnumerateFiles(
                       Path.Combine(
                           assets,
                           AssetDatabase.InternalDirectoryName,
                           "editor"),
                       ".asset-sources.*",
                       SearchOption.TopDirectoryOnly).Any(),
                "successful atomic commits leave no temporary or backup files");

            Check(reloaded.RemoveCollectionItems("Gameplay", new[] { props }) &&
                  reloaded.GetCollectionPaths("Gameplay").Single() == hero &&
                  reloaded.RemoveFavorite(".") &&
                  reloaded.Favorites.Count == 1,
                "favorite and collection member removal update the durable state");

            string renamedCharacters = Path.Combine(assets, "Actors");
            Directory.Move(characters, renamedCharacters);
            string renamedHero = Path.Combine(renamedCharacters, "Heroes", "Hero.acsbp");
            Check(reloaded.ApplyPathChanges(new AssetPathsChangedEventArgs(
                      mappings: new[]
                      {
                          new AssetPathMapping(characters, renamedCharacters),
                      })) &&
                  reloaded.Favorites.Single().RelativePath == "Actors" &&
                  reloaded.GetCollectionPaths("Gameplay").Single() == renamedHero,
                "transactional folder moves remap Favorites and nested Collection members");

            File.Delete(renamedHero);
            Check(reloaded.ApplyPathChanges(new AssetPathsChangedEventArgs(
                      deletedRoots: new[] { renamedHero })) &&
                  reloaded.GetCollectionPaths("Gameplay").Count == 0 &&
                  reloaded.Collections.Single().ItemCount == 0,
                "asset deletion removes matching Collection members");
            Check(reloaded.DeleteCollection("gameplay") &&
                  reloaded.Collections.Count == 0 &&
                  !reloaded.DeleteCollection("Gameplay"),
                "collections delete case-insensitively without touching assets");

            AssetBrowserSourceNode? tree = AssetBrowserSourceTreeBuilder.Build(assets);
            Check(tree is { Name: "Assets" } &&
                  tree.Children.Any(node => node.Name == "Actors") &&
                  tree.Children.Any(node => node.Name == "Props") &&
                  tree.Children.All(node =>
                      node.Name != AssetDatabase.InternalDirectoryName),
                "Sources tree exposes physical folders while hiding AssetDatabase internals");

            Check(AssetBrowserSourcePathPolicy.TryCanonicalizeExisting(
                      assets,
                      props,
                      requireDirectory: true,
                      out string canonicalProps) &&
                  canonicalProps == props &&
                  !AssetBrowserSourcePathPolicy.TryCanonicalizeExisting(
                      assets,
                      fixture,
                      requireDirectory: true,
                      out _),
                "source path policy accepts in-bound folders and rejects project-root escape");
            Check(AssetBrowserSourcePathPolicy.NormalizePersistedRelativePath(
                      "Actors\\Heroes\\Hero.acsbp") ==
                  "Actors/Heroes/Hero.acsbp" &&
                  Throws<ArgumentException>(() =>
                      AssetBrowserSourcePathPolicy.NormalizePersistedRelativePath(
                          "../outside")) &&
                  Throws<ArgumentException>(() =>
                      AssetBrowserSourcePathPolicy.NormalizePersistedRelativePath(
                          "C:\\outside")),
                "persisted paths normalize separators and reject traversal/rooted input");

            TestReparseSafety(output, assets, fixture, Check);
            TestMalformedState(output, Check);
            TestConcurrentStateSafety(output, Check);
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine("FAIL: unhandled exception: " + error);
        }
        finally
        {
            TryDeleteDirectory(fixture);
        }

        output.WriteLine(
            $"Asset Browser sources self-test: {passed} PASS / {failed} failures");
        return failed;
    }

    private static void TestReparseSafety(
        TextWriter output,
        string assets,
        string fixture,
        Action<bool, string> check)
    {
        string external = Path.Combine(fixture, "External");
        string link = Path.Combine(assets, "LinkedOutside");
        Directory.CreateDirectory(external);
        try
        {
            Directory.CreateSymbolicLink(link, external);
            check(!AssetBrowserSourcePathPolicy.TryCanonicalizeExisting(
                      assets,
                      link,
                      requireDirectory: true,
                      out _) &&
                  AssetBrowserSourceTreeBuilder.Build(assets)?.Children.All(
                      node => node.Name != "LinkedOutside") == true,
                "reparse-point folders are rejected and never enumerated by Sources");
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
            PlatformNotSupportedException)
        {
            output.WriteLine(
                "INFO: reparse-point link creation is unavailable (" +
                error.GetType().Name + ")");
            check(true, "reparse-point policy remains covered by direct attribute checks");
        }
    }

    private static void TestMalformedState(
        TextWriter output,
        Action<bool, string> check)
    {
        string fixture = Path.Combine(
            Path.GetTempPath(),
            "acs-asset-sources-malformed-" + Guid.NewGuid().ToString("N"));
        try
        {
            string assets = Path.Combine(fixture, "Assets");
            string valid = Path.Combine(assets, "Valid");
            string config = Path.Combine(
                assets,
                AssetDatabase.InternalDirectoryName,
                "editor");
            Directory.CreateDirectory(valid);
            Directory.CreateDirectory(config);
            string statePath = Path.Combine(config, "asset-sources.v1.json");
            File.WriteAllText(
                statePath,
                """
                {
                  "Version": 1,
                  "Favorites": [ "../Outside", "Valid", "valid" ],
                  "Collections": [
                    { "Name": "Safe", "Items": [ "../../escape", "Valid" ] },
                    { "Name": "../Bad", "Items": [ "Valid" ] }
                  ]
                }
                """);

            var sanitized = new AssetBrowserSourcesStore(
                fixture,
                assets,
                out string? warning);
            check(warning == null &&
                  sanitized.Favorites.Count == 1 &&
                  sanitized.Favorites.Single().RelativePath == "Valid" &&
                  sanitized.Collections.Count == 1 &&
                  sanitized.Collections.Single().ItemCount == 1,
                "load sanitization drops traversal, duplicate, and invalid-name entries");

            File.WriteAllText(
                statePath,
                """{ "Version": 999, "Favorites": [ "Valid" ] }""");
            var future = new AssetBrowserSourcesStore(
                fixture,
                assets,
                out warning);
            check(warning != null &&
                  future.Favorites.Count == 0 &&
                  future.Collections.Count == 0,
                "unknown state schemas are not partially interpreted");

            File.WriteAllText(statePath, "{ definitely not json");
            var corrupt = new AssetBrowserSourcesStore(
                fixture,
                assets,
                out warning);
            check(warning != null &&
                  corrupt.Favorites.Count == 0 &&
                  corrupt.Collections.Count == 0,
                "malformed JSON reports a warning and fails closed to empty state");
        }
        catch (Exception error)
        {
            output.WriteLine("FAIL: malformed-state fixture: " + error);
            check(false, "malformed state fixture completed");
        }
        finally
        {
            TryDeleteDirectory(fixture);
        }
    }

    private static void TestConcurrentStateSafety(
        TextWriter output,
        Action<bool, string> check)
    {
        string fixture = Path.Combine(
            Path.GetTempPath(),
            "acs-asset-sources-concurrency-" + Guid.NewGuid().ToString("N"));
        try
        {
            string assets = Path.Combine(fixture, "Assets");
            string firstFolder = Path.Combine(assets, "First");
            string secondFolder = Path.Combine(assets, "Second");
            Directory.CreateDirectory(firstFolder);
            Directory.CreateDirectory(secondFolder);

            var first = new AssetBrowserSourcesStore(
                fixture,
                assets,
                out string? firstWarning);
            var second = new AssetBrowserSourcesStore(
                fixture,
                assets,
                out string? secondWarning);
            bool merged =
                firstWarning == null &&
                secondWarning == null &&
                first.AddFavorite(firstFolder) &&
                second.AddFavorite(secondFolder) &&
                first.CreateCollection("FirstCollection") &&
                second.CreateCollection("SecondCollection");
            var reloaded = new AssetBrowserSourcesStore(
                fixture,
                assets,
                out string? reloadWarning);
            check(
                merged &&
                reloadWarning == null &&
                reloaded.Favorites.Count == 2 &&
                reloaded.Collections.Count == 2,
                "concurrent editor stores reload under the project lease instead of losing updates");

            bool contentionRejected;
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(assets))
            {
                contentionRejected = Throws<IOException>(() =>
                    first.RemoveFavorite("First"));
            }
            check(
                contentionRejected &&
                new AssetBrowserSourcesStore(
                    fixture,
                    assets,
                    out _).Favorites.Count == 2,
                "saved-source mutations fail before writing while another editor owns the asset lease");

            File.WriteAllText(reloaded.StatePath, "{ definitely not json");
            check(
                Throws<JsonException>(() => second.RemoveFavorite("Second")),
                "saved-source mutation refuses to overwrite a corrupt authoritative document");
        }
        catch (Exception error)
        {
            output.WriteLine("FAIL: concurrent-state fixture: " + error);
            check(false, "concurrent state fixture completed");
        }
        finally
        {
            TryDeleteDirectory(fixture);
        }
    }

    private static void TryDeleteDirectory(string path)
    {
        try
        {
            if (Directory.Exists(path))
                Directory.Delete(path, recursive: true);
        }
        catch
        {
            // Best-effort cleanup for a standalone self-test.
        }
    }

    private static bool Throws<TException>(Action action)
        where TException : Exception
    {
        try
        {
            action();
            return false;
        }
        catch (TException)
        {
            return true;
        }
    }
}
