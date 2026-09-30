// SPDX-License-Identifier: GPL-3.0-only
// Place this evaluation helper in Assets/Editor of an isolated Unity project.
using System;
using System.IO;
using UnityEditor;
using UnityEngine;

public static class CaptureFixtureBundle
{
    // The shader-message API does not include every player-build diagnostic.
    // Compiler messages may also precede this entry point during initial import.
    internal static void CheckEditorLog(TextReader reader)
    {
        string line;
        while ((line = reader.ReadLine()) != null)
            if (IsErrorLogLine(line))
                throw new InvalidOperationException("The Editor log contains an error: " + line);
    }

    private static bool IsErrorLogLine(string line)
    {
        string message = (line ?? string.Empty).TrimStart();
        return message.StartsWith("Shader error in ", StringComparison.OrdinalIgnoreCase) ||
            message.StartsWith("Compute shader error", StringComparison.OrdinalIgnoreCase) ||
            message.StartsWith("Error:", StringComparison.OrdinalIgnoreCase) ||
            message.StartsWith("Error ", StringComparison.OrdinalIgnoreCase) ||
            message.StartsWith("[Error]", StringComparison.OrdinalIgnoreCase) ||
            message.StartsWith("Fatal error", StringComparison.OrdinalIgnoreCase);
    }

    private sealed class Diagnostics
    {
        private readonly object sync = new object();
        private string firstError;

        internal void Capture(string message, string stackTrace, LogType type)
        {
            if (type != LogType.Error && type != LogType.Assert &&
                type != LogType.Exception && !IsErrorLogLine(message)) return;
            lock (sync)
                if (firstError == null) firstError = message ?? "Editor reported an error.";
        }

        internal void Check()
        {
            lock (sync)
                if (firstError != null)
                    throw new InvalidOperationException(firstError);
        }
    }

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

        Diagnostics diagnostics = new Diagnostics();
        Application.logMessageReceivedThreaded += diagnostics.Capture;
        try
        {
            BuildChecked(outputDirectory, diagnostics);
        }
        finally
        {
            Application.logMessageReceivedThreaded -= diagnostics.Capture;
        }
    }

    private static void BuildChecked(string outputDirectory, Diagnostics diagnostics)
    {
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
        diagnostics.Check();
        string logPath = Application.consoleLogPath;
        if (string.IsNullOrEmpty(logPath))
            throw new InvalidOperationException("An explicit readable Editor log is required.");
        using (FileStream stream = new FileStream(logPath, FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite))
        using (StreamReader reader = new StreamReader(stream))
            CheckEditorLog(reader);
        File.WriteAllLines(Path.Combine(outputDirectory, "source-assets.txt"), paths);
        diagnostics.Check();
        Debug.Log("CONTROLLED_FIXTURE_RELEASE_OK " + paths.Length);
    }
}
