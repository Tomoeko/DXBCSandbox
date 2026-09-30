// SPDX-License-Identifier: GPL-3.0-only
// Compile with CaptureFixtureBundle.cs; these stubs exercise capture decisions,
// not Unity compilation or released shader bytes.
using System;
using System.IO;

internal static class TestCaptureFixtureBundle
{
    private static int checks;

    private static void Require(bool condition)
    {
        ++checks;
        if (!condition) throw new Exception("Capture decision test failed.");
    }

    private static void LogCase(string text, bool error)
    {
        bool rejected = false;
        try { CaptureFixtureBundle.CheckEditorLog(new StringReader(text)); }
        catch (InvalidOperationException) { rejected = true; }
        Require(rejected == error);
    }

    private static void BuildCase(string log, UnityEngine.LogType? eventType,
                                  bool shaderError, bool error,
                                  string eventMessage = "Build diagnostic")
    {
        string scratch = Path.Combine(Path.GetTempPath(), "fixture-capture-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(scratch);
        string output = Path.Combine(scratch, "release");
        string logPath = Path.Combine(scratch, "editor.log");
        File.WriteAllText(logPath, log);
        Environment.SetEnvironmentVariable("DXBC_FIXTURE_BUNDLE_DIR", output);
        UnityEngine.Application.consoleLogPath = logPath;
        UnityEditor.BuildPipeline.EventType = eventType;
        UnityEngine.Application.EventMessage = eventMessage;
        UnityEditor.ShaderUtil.HasError = shaderError;
        UnityEngine.Debug.Success = false;
        bool rejected = false;
        try { CaptureFixtureBundle.Build(); }
        catch (InvalidOperationException) { rejected = true; }
        try
        {
            Require(rejected == error);
            Require(UnityEngine.Debug.Success == !error);
            Require(File.Exists(Path.Combine(output, "source-assets.txt")) == !error);
            Require(UnityEngine.Application.CallbackCount == 0);
            // A bundle is not sufficient evidence of a successful capture.
            Require(File.Exists(Path.Combine(output, "high-level-fixtures.bundle")));
        }
        finally { Directory.Delete(scratch, true); }
    }

    public static int Main(string[] args)
    {
        LogCase("Shader warning in 'Fixture/Test': unused value\r\nCONTROLLED_FIXTURE_RELEASE_OK 1\r\n", false);
        LogCase("LogAssemblyErrors (0ms)\nError X3000: invalid input\n", true);
        LogCase("CONTROLLED_FIXTURE_RELEASE_OK 1\nShader error in 'Fixture/Test': error X3000\n", true);
        LogCase("  Compute shader error in 'Fixture/Test': invalid kernel\r\n", true);
        LogCase("Error: bundle build failed\n", true);
        LogCase("[ERROR] compiler failed\n", true);
        LogCase("Fatal error: compiler stopped\n", true);
        LogCase("LogAssemblyErrors (0ms)\nShader warning in 'Fixture/Test': warning X3206\n", false);
        BuildCase("Editor ready\n", null, false, false);
        BuildCase("Shader warning in 'Fixture/Test': unused value\n", UnityEngine.LogType.Warning, false, false);
        BuildCase("Shader error in 'Fixture/Test': invalid input\nCONTROLLED_FIXTURE_RELEASE_OK 1\n", null, false, true);
        BuildCase("Compute shader error in 'Fixture/Test': invalid kernel\n", null, false, true);
        BuildCase("Editor ready\n", UnityEngine.LogType.Error, false, true);
        BuildCase("Editor ready\n", UnityEngine.LogType.Assert, false, true);
        BuildCase("Editor ready\n", UnityEngine.LogType.Exception, false, true);
        BuildCase("Editor ready\n", UnityEngine.LogType.Log, false, true,
            "Shader error in 'Fixture/Test': invalid input");
        BuildCase("Editor ready\n", null, true, true);
        foreach (string path in args)
        {
            bool rejected = false;
            try
            {
                using (StreamReader reader = new StreamReader(path))
                    CaptureFixtureBundle.CheckEditorLog(reader);
            }
            catch (InvalidOperationException) { rejected = true; }
            Require(rejected);
        }
        Console.WriteLine("Capture decision checks passed: " + checks);
        return 0;
    }
}

namespace UnityEngine
{
    public enum LogType { Error, Assert, Warning, Log, Exception }
    public sealed class Shader { }
    public sealed class AssetBundleManifest { }
    public static class Application
    {
        public static string unityVersion = "2021.3.35f1";
        public static string consoleLogPath;
        public static string EventMessage;
        public delegate void LogCallback(string message, string stack, LogType type);
        public static event LogCallback logMessageReceivedThreaded;
        public static int CallbackCount { get { return logMessageReceivedThreaded == null ? 0 : logMessageReceivedThreaded.GetInvocationList().Length; } }
        public static void Emit(LogType type)
        {
            if (logMessageReceivedThreaded != null)
                logMessageReceivedThreaded(EventMessage, "", type);
        }
    }
    public static class Debug
    {
        public static bool Success;
        public static void Log(string message) { Success = message.StartsWith("CONTROLLED_FIXTURE_RELEASE_OK ", StringComparison.Ordinal); }
    }
}

namespace UnityEditor
{
    public enum ImportAssetOptions { ForceSynchronousImport }
    [Flags] public enum BuildAssetBundleOptions { ForceRebuildAssetBundle = 1, UncompressedAssetBundle = 2 }
    public enum BuildTarget { StandaloneWindows64 }
    public sealed class AssetBundleBuild { public string assetBundleName; public string[] assetNames; }
    public static class AssetDatabase
    {
        public static void Refresh(ImportAssetOptions options) { }
        public static string[] FindAssets(string filter, string[] folders) { return new[] { "fixture" }; }
        public static string GUIDToAssetPath(string identifier) { return "Assets/Fixtures/test.shader"; }
        public static T LoadAssetAtPath<T>(string path) where T : new() { return new T(); }
    }
    public static class BuildPipeline
    {
        public static UnityEngine.LogType? EventType;
        public static UnityEngine.AssetBundleManifest BuildAssetBundles(string output, AssetBundleBuild[] builds, BuildAssetBundleOptions options, BuildTarget target)
        {
            File.WriteAllText(Path.Combine(output, builds[0].assetBundleName), "Synthetic test bundle");
            if (EventType.HasValue) UnityEngine.Application.Emit(EventType.Value);
            return new UnityEngine.AssetBundleManifest();
        }
    }
    public enum MessageSeverity { Error, Warning }
    public sealed class ShaderMessage { public MessageSeverity severity = MessageSeverity.Error; public string message = "Shader diagnostic"; }
    public static class ShaderUtil
    {
        public static bool HasError;
        public static ShaderMessage[] GetShaderMessages(UnityEngine.Shader shader) { return HasError ? new[] { new ShaderMessage() } : new ShaderMessage[0]; }
    }
}
