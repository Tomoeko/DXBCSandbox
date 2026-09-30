// SPDX-License-Identifier: GPL-3.0-only
// Place this evaluation helper in Assets/Editor of an isolated Unity project.
using System;
using System.IO;
using UnityEditor;
using UnityEngine;

public static class CaptureFixtureBundle
{
    public static void Build()
    {
        if (Application.unityVersion != "2021.3.35f1")
            throw new InvalidOperationException("Select the required Unity toolchain.");

        string outputDirectory = Environment.GetEnvironmentVariable("DXBC_FIXTURE_BUNDLE_DIR");
        if (string.IsNullOrEmpty(outputDirectory) || !Path.IsPathRooted(outputDirectory))
            throw new InvalidOperationException("DXBC_FIXTURE_BUNDLE_DIR must be absolute.");
        outputDirectory = Path.GetFullPath(outputDirectory);
        if (Directory.Exists(outputDirectory) && Directory.GetFileSystemEntries(outputDirectory).Length != 0)
            throw new InvalidOperationException("The capture directory must be new or empty.");

        AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
        string[] identifiers = AssetDatabase.FindAssets("t:Shader t:ComputeShader", new[] { "Assets/Fixtures" });
        string[] paths = new string[identifiers.Length];
        for (int index = 0; index < identifiers.Length; ++index)
            paths[index] = AssetDatabase.GUIDToAssetPath(identifiers[index]);
        Array.Sort(paths, StringComparer.Ordinal);
        if (paths.Length == 0)
            throw new InvalidOperationException("No controlled shader fixtures found.");

        Directory.CreateDirectory(outputDirectory);
        AssetBundleBuild build = new AssetBundleBuild
        {
            assetBundleName = "high-level-fixtures.bundle",
            assetNames = paths
        };
        AssetBundleManifest manifest = BuildPipeline.BuildAssetBundles(
            outputDirectory, new[] { build },
            BuildAssetBundleOptions.ForceRebuildAssetBundle | BuildAssetBundleOptions.UncompressedAssetBundle,
            BuildTarget.StandaloneWindows64);
        if (manifest == null)
            throw new InvalidOperationException("The controlled release bundle build failed.");

        foreach (string path in paths)
        {
            Shader shader = AssetDatabase.LoadAssetAtPath<Shader>(path);
            if (shader == null) continue;
            foreach (var message in ShaderUtil.GetShaderMessages(shader))
                if (message.severity.ToString() == "Error")
                    throw new InvalidOperationException(path + ": " + message.message);
        }
        File.WriteAllLines(Path.Combine(outputDirectory, "source-assets.txt"), paths);
        Debug.Log("CONTROLLED_FIXTURE_RELEASE_OK " + paths.Length);
    }
}
