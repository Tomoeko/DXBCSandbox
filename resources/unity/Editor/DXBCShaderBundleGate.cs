using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

namespace DXBCSandbox.Editor
{
    // Managed entry point for the portable C11 isolated-bundle launcher.  This
    // file is copied into a fresh project for one invocation; it must never be
    // installed into or run against a caller-owned Unity project.
    public static class DXBCShaderBundleGate
    {
        private const string Schema =
            "dxbc-sandbox-unity-shader-bundle/v1";
        private const string BundleName = "dxbc-release-shaders.bundle";

        private sealed class Candidate
        {
            public string AssetPath;
            public string SourcePath;
        }

        private sealed class Diagnostic
        {
            public string Severity;
            public string File;
            public uint Line;
            public string Message;
            public string Platform;
            public string Details;
        }

        private sealed class Failure
        {
            public string Kind;
            public string Message;
        }

        private sealed class ShaderResult
        {
            public string SourcePath;
            public string AssetPath;
            public string BundleAssetName;
            public string ShaderName;
            public bool Imported;
            public readonly List<Diagnostic> Messages = new List<Diagnostic>();
        }

        private static string RequireArgument(string[] arguments, string name)
        {
            string value = null;
            for (int index = 0; index < arguments.Length; ++index)
            {
                if (!String.Equals(arguments[index], name,
                                   StringComparison.Ordinal))
                    continue;
                if (value != null || index + 1 >= arguments.Length ||
                    String.IsNullOrEmpty(arguments[index + 1]))
                    throw new InvalidOperationException(
                        "missing or duplicate " + name);
                value = arguments[++index];
            }
            if (value == null)
                throw new InvalidOperationException("missing " + name);
            return value;
        }

        private static List<Candidate> ReadManifest(string path)
        {
            string[] lines = File.ReadAllLines(path,
                new UTF8Encoding(false, true));
            var candidates = new List<Candidate>(lines.Length);
            var knownAssets = new HashSet<string>(StringComparer.Ordinal);
            var strictUtf8 = new UTF8Encoding(false, true);
            foreach (string line in lines)
            {
                int separator = line.IndexOf('\t');
                if (separator <= 0 || separator != line.LastIndexOf('\t') ||
                    separator + 1 >= line.Length)
                    throw new InvalidDataException(
                        "invalid candidate manifest record");
                string assetPath = line.Substring(0, separator);
                if (!assetPath.StartsWith("Assets/DXBCReleaseBundle/",
                                          StringComparison.Ordinal) ||
                    !assetPath.EndsWith(".shader", StringComparison.Ordinal) ||
                    assetPath.IndexOf("..", StringComparison.Ordinal) >= 0 ||
                    !knownAssets.Add(assetPath))
                    throw new InvalidDataException(
                        "invalid or duplicate candidate asset path");
                string sourcePath = strictUtf8.GetString(
                    Convert.FromBase64String(line.Substring(separator + 1)));
                if (String.IsNullOrEmpty(sourcePath))
                    throw new InvalidDataException("empty candidate source path");
                candidates.Add(new Candidate {
                    AssetPath = assetPath,
                    SourcePath = sourcePath
                });
            }
            if (candidates.Count == 0)
                throw new InvalidDataException("candidate manifest is empty");

            string directory = Path.Combine(Directory.GetCurrentDirectory(),
                "Assets", "DXBCReleaseBundle");
            string[] files = Directory.GetFiles(directory, "*.shader",
                                                SearchOption.TopDirectoryOnly);
            Array.Sort(files, StringComparer.Ordinal);
            if (files.Length != candidates.Count)
                throw new InvalidDataException(
                    "candidate manifest/file count mismatch");
            for (int index = 0; index < files.Length; ++index)
            {
                string relative = "Assets/DXBCReleaseBundle/" +
                    Path.GetFileName(files[index]);
                if (!String.Equals(relative, candidates[index].AssetPath,
                                   StringComparison.Ordinal))
                    throw new InvalidDataException(
                        "candidate manifest/file order mismatch");
            }
            return candidates;
        }

        private static List<ShaderResult> CreateResults(
            List<Candidate> candidates)
        {
            var results = new List<ShaderResult>(candidates.Count);
            foreach (Candidate candidate in candidates)
            {
                results.Add(new ShaderResult {
                    SourcePath = candidate.SourcePath,
                    AssetPath = candidate.AssetPath,
                    BundleAssetName = candidate.AssetPath,
                    ShaderName = String.Empty,
                    Imported = false
                });
            }
            return results;
        }

        private static object ReadMember(object instance, string name)
        {
            if (instance == null) return null;
            Type type = instance.GetType();
            const BindingFlags flags = BindingFlags.Instance |
                BindingFlags.Public | BindingFlags.NonPublic;
            PropertyInfo property = type.GetProperty(name, flags);
            if (property != null && property.GetIndexParameters().Length == 0)
                return property.GetValue(instance, null);
            FieldInfo field = type.GetField(name, flags);
            return field == null ? null : field.GetValue(instance);
        }

        private static string MemberString(object instance, string name)
        {
            object value = ReadMember(instance, name);
            return value == null ? String.Empty :
                Convert.ToString(value, CultureInfo.InvariantCulture) ??
                String.Empty;
        }

        private static uint MemberLine(object instance)
        {
            object value = ReadMember(instance, "line");
            if (value == null) return 0U;
            try
            {
                long line = Convert.ToInt64(value,
                                            CultureInfo.InvariantCulture);
                return line <= 0L ? 0U : line >= UInt32.MaxValue
                    ? UInt32.MaxValue : (uint)line;
            }
            catch (Exception) { return 0U; }
        }

        private static string NormalizeSeverity(object message)
        {
            string raw = MemberString(message, "severity").ToLowerInvariant();
            if (raw.IndexOf("error", StringComparison.Ordinal) >= 0)
                return "error";
            if (raw.IndexOf("warning", StringComparison.Ordinal) >= 0)
                return "warning";
            return "info";
        }

        private static Array GetShaderMessages(Shader shader)
        {
            MethodInfo selected = null;
            foreach (MethodInfo method in typeof(ShaderUtil).GetMethods(
                BindingFlags.Static | BindingFlags.Public |
                BindingFlags.NonPublic))
            {
                if (!String.Equals(method.Name, "GetShaderMessages",
                                   StringComparison.Ordinal))
                    continue;
                ParameterInfo[] parameters = method.GetParameters();
                if (parameters.Length == 1 &&
                    parameters[0].ParameterType == typeof(Shader))
                {
                    selected = method;
                    break;
                }
            }
            if (selected == null)
                throw new MissingMethodException(
                    "ShaderUtil.GetShaderMessages(Shader)");
            Array messages = selected.Invoke(null, new object[] { shader }) as
                Array;
            if (messages == null)
                throw new InvalidDataException(
                    "GetShaderMessages returned a non-array");
            return messages;
        }

        private static Diagnostic InfrastructureDiagnostic(string message)
        {
            return new Diagnostic {
                Severity = "error",
                File = String.Empty,
                Line = 0U,
                Message = message,
                Platform = "editor",
                Details = String.Empty
            };
        }

        private static void Import(Candidate candidate, ShaderResult result)
        {
            try
            {
                AssetDatabase.ImportAsset(candidate.AssetPath,
                    ImportAssetOptions.ForceSynchronousImport |
                    ImportAssetOptions.ForceUpdate);
                Shader shader = AssetDatabase.LoadAssetAtPath<Shader>(
                    candidate.AssetPath);
                if (shader == null)
                {
                    result.Messages.Add(InfrastructureDiagnostic(
                        "ShaderImporter produced no Shader asset"));
                    return;
                }
                result.Imported = true;
                result.ShaderName = shader.name ?? String.Empty;
                if (String.IsNullOrEmpty(result.ShaderName))
                    result.Messages.Add(InfrastructureDiagnostic(
                        "ShaderImporter produced a Shader with no name"));
                foreach (object message in GetShaderMessages(shader))
                {
                    result.Messages.Add(new Diagnostic {
                        Severity = NormalizeSeverity(message),
                        File = MemberString(message, "file"),
                        Line = MemberLine(message),
                        Message = MemberString(message, "message"),
                        Platform = MemberString(message, "platform"),
                        Details = MemberString(message, "messageDetails")
                    });
                }
            }
            catch (Exception exception)
            {
                result.Messages.Add(InfrastructureDiagnostic(
                    exception.GetType().FullName + ": " + exception.Message));
            }
            result.Messages.Sort(CompareDiagnostics);
        }

        private static int CompareDiagnostics(Diagnostic left,
                                              Diagnostic right)
        {
            int order = StringComparer.Ordinal.Compare(left.Severity,
                                                       right.Severity);
            if (order != 0) return order;
            order = StringComparer.Ordinal.Compare(left.File, right.File);
            if (order != 0) return order;
            order = left.Line.CompareTo(right.Line);
            if (order != 0) return order;
            order = StringComparer.Ordinal.Compare(left.Platform,
                                                    right.Platform);
            if (order != 0) return order;
            order = StringComparer.Ordinal.Compare(left.Message,
                                                    right.Message);
            return order != 0 ? order : StringComparer.Ordinal.Compare(
                left.Details, right.Details);
        }

        private static BuildTarget ParseBuildTarget(string value)
        {
            if (value == "StandaloneOSX") return BuildTarget.StandaloneOSX;
            if (value == "StandaloneWindows64")
                return BuildTarget.StandaloneWindows64;
            if (value == "StandaloneLinux64")
                return BuildTarget.StandaloneLinux64;
            throw new InvalidOperationException("unsupported build target: " +
                                                value);
        }

        private static GraphicsDeviceType ParseBackend(string target,
                                                       string backend)
        {
            if (target == "StandaloneOSX" && backend == "metal")
                return GraphicsDeviceType.Metal;
            if (target == "StandaloneOSX" && backend == "openglcore")
                return GraphicsDeviceType.OpenGLCore;
            if (target == "StandaloneWindows64" && backend == "d3d11")
                return GraphicsDeviceType.Direct3D11;
            if ((target == "StandaloneWindows64" ||
                 target == "StandaloneLinux64") && backend == "vulkan")
                return GraphicsDeviceType.Vulkan;
            if ((target == "StandaloneWindows64" ||
                 target == "StandaloneLinux64") &&
                backend == "openglcore")
                return GraphicsDeviceType.OpenGLCore;
            throw new InvalidOperationException(
                "graphics backend is not defined for target: " + backend +
                "/" + target);
        }

        private static void RequireSupportedBackend(
            BuildTarget target, GraphicsDeviceType backend)
        {
            MethodInfo method = typeof(PlayerSettings).GetMethod(
                "GetSupportedGraphicsAPIs",
                BindingFlags.Static | BindingFlags.NonPublic,
                null, new[] { typeof(BuildTarget) }, null);
            if (method == null)
                throw new MissingMethodException(
                    "PlayerSettings.GetSupportedGraphicsAPIs(BuildTarget)");
            GraphicsDeviceType[] supported = method.Invoke(
                null, new object[] { target }) as GraphicsDeviceType[];
            if (supported == null)
                throw new InvalidDataException(
                    "GetSupportedGraphicsAPIs returned a non-array");
            foreach (GraphicsDeviceType candidate in supported)
                if (candidate == backend) return;
            throw new NotSupportedException(
                "requested graphics backend is unavailable for the target");
        }

        private static bool PolicyBreak(List<ShaderResult> shaders,
                                        List<Diagnostic> buildMessages,
                                        string warningPolicy)
        {
            foreach (ShaderResult shader in shaders)
            {
                foreach (Diagnostic diagnostic in shader.Messages)
                {
                    if (diagnostic.Severity == "error" ||
                        (warningPolicy == "fail" &&
                         diagnostic.Severity == "warning"))
                        return true;
                }
            }
            foreach (Diagnostic diagnostic in buildMessages)
            {
                if (diagnostic.Severity == "error" ||
                    (warningPolicy == "fail" &&
                     diagnostic.Severity == "warning"))
                    return true;
            }
            return false;
        }

        private static string Sha256(string path)
        {
            using (FileStream stream = new FileStream(path, FileMode.Open,
                FileAccess.Read, FileShare.Read))
            using (SHA256 hash = SHA256.Create())
            {
                byte[] digest = hash.ComputeHash(stream);
                var output = new StringBuilder(digest.Length * 2);
                foreach (byte value in digest)
                    output.Append(value.ToString("x2",
                        CultureInfo.InvariantCulture));
                return output.ToString();
            }
        }

        private static List<Diagnostic> ActiveBuildMessages;

        private static void CaptureBuildLog(string condition, string stackTrace,
                                            LogType type)
        {
            if (ActiveBuildMessages == null || type == LogType.Log)
                return;
            string severity = type == LogType.Warning ? "warning" : "error";
            ActiveBuildMessages.Add(new Diagnostic {
                Severity = severity,
                File = String.Empty,
                Line = 0U,
                Message = condition ?? String.Empty,
                Platform = "build",
                Details = stackTrace ?? String.Empty
            });
        }

        private static void StopDiagnosticCapture(List<Diagnostic> messages)
        {
            if (ActiveBuildMessages != null)
            {
                Application.logMessageReceived -= CaptureBuildLog;
                ActiveBuildMessages = null;
            }
            messages.Sort(CompareDiagnostics);
        }

        private static void JsonString(StringBuilder output, string value)
        {
            output.Append('"');
            foreach (char character in value ?? String.Empty)
            {
                switch (character)
                {
                    case '"': output.Append("\\\""); break;
                    case '\\': output.Append("\\\\"); break;
                    case '\b': output.Append("\\b"); break;
                    case '\f': output.Append("\\f"); break;
                    case '\n': output.Append("\\n"); break;
                    case '\r': output.Append("\\r"); break;
                    case '\t': output.Append("\\t"); break;
                    default:
                        if (character < 0x20 || Char.IsSurrogate(character))
                            output.Append("\\u").Append(
                                ((int)character).ToString("x4"));
                        else
                            output.Append(character);
                        break;
                }
            }
            output.Append('"');
        }

        private static void AppendDiagnostic(StringBuilder output,
                                             Diagnostic diagnostic)
        {
            output.Append("{\"severity\":");
            JsonString(output, diagnostic.Severity);
            output.Append(",\"file\":"); JsonString(output, diagnostic.File);
            output.Append(",\"line\":").Append(diagnostic.Line);
            output.Append(",\"message\":");
            JsonString(output, diagnostic.Message);
            output.Append(",\"platform\":");
            JsonString(output, diagnostic.Platform);
            output.Append(",\"details\":");
            JsonString(output, diagnostic.Details);
            output.Append('}');
        }

        private static string BuildReport(
            List<ShaderResult> shaders, List<Diagnostic> buildMessages,
            List<Failure> failures, string target, string backend,
            string warningPolicy, string status, string outputBundlePath,
            bool bundleBuilt, ulong bundleSize, string bundleSha256)
        {
            ulong messageCount = 0U;
            ulong errorCount = 0U;
            ulong warningCount = 0U;
            foreach (ShaderResult shader in shaders)
            {
                foreach (Diagnostic diagnostic in shader.Messages)
                {
                    ++messageCount;
                    if (diagnostic.Severity == "error") ++errorCount;
                    if (diagnostic.Severity == "warning") ++warningCount;
                }
            }
            foreach (Diagnostic diagnostic in buildMessages)
            {
                ++messageCount;
                if (diagnostic.Severity == "error") ++errorCount;
                if (diagnostic.Severity == "warning") ++warningCount;
            }
            var output = new StringBuilder(8192);
            output.Append("{\n  \"schema\":"); JsonString(output, Schema);
            output.Append(",\n  \"unity_version\":");
            JsonString(output, Application.unityVersion);
            output.Append(",\n  \"build_target\":"); JsonString(output, target);
            output.Append(",\n  \"graphics_backend\":");
            JsonString(output, backend);
            output.Append(",\n  \"warning_policy\":");
            JsonString(output, warningPolicy);
            output.Append(",\n  \"candidate_count\":").Append(shaders.Count);
            output.Append(",\n  \"message_count\":").Append(messageCount);
            output.Append(",\n  \"error_count\":").Append(errorCount);
            output.Append(",\n  \"warning_count\":").Append(warningCount);
            output.Append(",\n  \"failure_count\":").Append(failures.Count);
            output.Append(",\n  \"status\":"); JsonString(output, status);
            output.Append(",\n  \"output_bundle_path\":");
            JsonString(output, outputBundlePath);
            output.Append(",\n  \"bundle_built\":").Append(
                bundleBuilt ? "true" : "false");
            output.Append(",\n  \"bundle_size\":").Append(bundleSize);
            output.Append(",\n  \"bundle_sha256\":");
            JsonString(output, bundleSha256);
            output.Append(",\n  \"shaders\":[");
            for (int shaderIndex = 0; shaderIndex < shaders.Count;
                 ++shaderIndex)
            {
                ShaderResult shader = shaders[shaderIndex];
                output.Append(shaderIndex == 0 ? "\n    {" : ",\n    {");
                output.Append("\n      \"source_path\":");
                JsonString(output, shader.SourcePath);
                output.Append(",\n      \"asset_path\":");
                JsonString(output, shader.AssetPath);
                output.Append(",\n      \"bundle_asset_name\":");
                JsonString(output, shader.BundleAssetName);
                output.Append(",\n      \"shader_name\":");
                JsonString(output, shader.ShaderName);
                output.Append(",\n      \"imported\":").Append(
                    shader.Imported ? "true" : "false");
                output.Append(",\n      \"messages\":[");
                for (int messageIndex = 0;
                     messageIndex < shader.Messages.Count; ++messageIndex)
                {
                    output.Append(messageIndex == 0 ? "\n        " :
                                                        ",\n        ");
                    AppendDiagnostic(output, shader.Messages[messageIndex]);
                }
                if (shader.Messages.Count != 0)
                    output.Append('\n').Append("      ");
                output.Append("]\n    }");
            }
            if (shaders.Count != 0) output.Append('\n').Append("  ");
            output.Append("],\n  \"build_messages\":[");
            for (int index = 0; index < buildMessages.Count; ++index)
            {
                output.Append(index == 0 ? "\n    " : ",\n    ");
                AppendDiagnostic(output, buildMessages[index]);
            }
            if (buildMessages.Count != 0) output.Append('\n').Append("  ");
            output.Append("],\n  \"failures\":[");
            for (int index = 0; index < failures.Count; ++index)
            {
                Failure failure = failures[index];
                output.Append(index == 0 ? "\n    {\"kind\":" :
                    ",\n    {\"kind\":");
                JsonString(output, failure.Kind);
                output.Append(",\"message\":");
                JsonString(output, failure.Message);
                output.Append('}');
            }
            if (failures.Count != 0) output.Append('\n').Append("  ");
            output.Append("]\n}\n");
            return output.ToString();
        }

        private static void WriteReport(
            string path, List<ShaderResult> shaders,
            List<Diagnostic> buildMessages, List<Failure> failures,
            string target, string backend, string warningPolicy,
            string status, string outputBundlePath, bool bundleBuilt,
            ulong bundleSize, string bundleSha256)
        {
            File.WriteAllText(path, BuildReport(shaders, buildMessages,
                failures, target, backend, warningPolicy, status,
                outputBundlePath, bundleBuilt, bundleSize, bundleSha256),
                new UTF8Encoding(false, true));
        }

        private static bool HasError(List<Diagnostic> messages)
        {
            foreach (Diagnostic diagnostic in messages)
                if (diagnostic.Severity == "error") return true;
            return false;
        }

        private static void DeleteBundleIfPresent(string path)
        {
            try { if (File.Exists(path)) File.Delete(path); }
            catch (Exception exception) { Debug.LogException(exception); }
        }

        public static void Run()
        {
            string resultPath = null;
            string targetName = "StandaloneOSX";
            string backendName = "metal";
            string warningPolicy = "fail";
            string publishedBundlePath = String.Empty;
            string internalBundlePath = String.Empty;
            string status = "build_failed";
            bool bundleBuilt = false;
            ulong bundleSize = 0U;
            string bundleSha256 = String.Empty;
            var results = new List<ShaderResult>();
            var buildMessages = new List<Diagnostic>();
            var failures = new List<Failure>();
            int exitCode = 3;
            try
            {
                string[] arguments = Environment.GetCommandLineArgs();
                resultPath = RequireArgument(arguments, "-dxbc-bundle-result");
                string manifestPath = RequireArgument(arguments,
                    "-dxbc-bundle-manifest");
                string outputDirectory = RequireArgument(arguments,
                    "-dxbc-bundle-output-dir");
                publishedBundlePath = RequireArgument(arguments,
                    "-dxbc-published-bundle-path");
                string expectedVersion = RequireArgument(arguments,
                    "-dxbc-expected-unity-version");
                warningPolicy = RequireArgument(arguments,
                    "-dxbc-warning-policy");
                targetName = RequireArgument(arguments, "-dxbc-build-target");
                backendName = RequireArgument(arguments,
                    "-dxbc-graphics-backend");
                if (warningPolicy != "allow" && warningPolicy != "fail")
                    throw new InvalidOperationException(
                        "invalid warning policy");
                List<Candidate> candidates = ReadManifest(manifestPath);
                results = CreateResults(candidates);
                if (!String.Equals(Application.unityVersion, expectedVersion,
                                   StringComparison.Ordinal))
                    throw new InvalidOperationException(
                        "Unity version mismatch: expected " + expectedVersion +
                        ", running " + Application.unityVersion);

                BuildTarget target = ParseBuildTarget(targetName);
                BuildTargetGroup group = BuildPipeline.GetBuildTargetGroup(target);
                if (group == BuildTargetGroup.Unknown ||
                    !BuildPipeline.IsBuildTargetSupported(group, target))
                {
                    failures.Add(new Failure {
                        Kind = "target_unavailable",
                        Message = "BuildTarget support module is not installed: " +
                            targetName
                    });
                    status = "target_unavailable";
                    exitCode = 4;
                }
                else
                {
                    ActiveBuildMessages = buildMessages;
                    Application.logMessageReceived += CaptureBuildLog;
                    GraphicsDeviceType backend;
                    try
                    {
                        backend = ParseBackend(targetName, backendName);
                        RequireSupportedBackend(target, backend);
                        PlayerSettings.SetUseDefaultGraphicsAPIs(target, false);
                        PlayerSettings.SetGraphicsAPIs(target,
                            new[] { backend });
                        GraphicsDeviceType[] selected =
                            PlayerSettings.GetGraphicsAPIs(target);
                        if (PlayerSettings.GetUseDefaultGraphicsAPIs(target) ||
                            selected == null || selected.Length != 1 ||
                            selected[0] != backend)
                            throw new InvalidOperationException(
                                "Unity did not retain the requested graphics API");
                    }
                    catch (Exception exception)
                    {
                        failures.Add(new Failure {
                            Kind = "backend_unavailable",
                            Message = exception.GetType().FullName + ": " +
                                exception.Message
                        });
                        status = "backend_unavailable";
                        exitCode = 5;
                        backend = GraphicsDeviceType.Null;
                    }

                    if (exitCode != 5)
                    {
                        AssetDatabase.Refresh(
                            ImportAssetOptions.ForceSynchronousImport |
                            ImportAssetOptions.ForceUpdate);
                        for (int index = 0; index < candidates.Count; ++index)
                            Import(candidates[index], results[index]);
                        if (PolicyBreak(results, buildMessages, warningPolicy))
                        {
                            status = "diagnostics_found";
                            exitCode = 2;
                        }
                        else
                        {
                            Directory.CreateDirectory(outputDirectory);
                            internalBundlePath = Path.Combine(outputDirectory,
                                                             BundleName);
                            var build = new AssetBundleBuild {
                                assetBundleName = BundleName,
                                assetNames = candidates.ConvertAll(
                                    candidate => candidate.AssetPath).ToArray()
                            };
                            AssetBundleManifest builtManifest =
                                BuildPipeline.BuildAssetBundles(
                                outputDirectory, new[] { build },
                                BuildAssetBundleOptions.ForceRebuildAssetBundle |
                                BuildAssetBundleOptions.UncompressedAssetBundle |
                                BuildAssetBundleOptions.DeterministicAssetBundle |
                                BuildAssetBundleOptions.StrictMode,
                                target);
                            if (builtManifest == null ||
                                !File.Exists(internalBundlePath))
                            {
                                failures.Add(new Failure {
                                    Kind = "build_failed",
                                    Message = builtManifest == null
                                        ? "BuildPipeline.BuildAssetBundles returned null"
                                        : "requested AssetBundle was not produced"
                                });
                                DeleteBundleIfPresent(internalBundlePath);
                                status = "build_failed";
                                exitCode = 6;
                            }
                            else if (HasError(buildMessages))
                            {
                                failures.Add(new Failure {
                                    Kind = "build_failed",
                                    Message = "AssetBundle build emitted an error"
                                });
                                DeleteBundleIfPresent(internalBundlePath);
                                status = "build_failed";
                                exitCode = 6;
                            }
                            else if (PolicyBreak(results, buildMessages,
                                                 warningPolicy))
                            {
                                DeleteBundleIfPresent(internalBundlePath);
                                status = "diagnostics_found";
                                exitCode = 2;
                            }
                            else
                            {
                                var information = new FileInfo(
                                    internalBundlePath);
                                if (information.Length <= 0L)
                                    throw new InvalidDataException(
                                        "produced AssetBundle is empty");
                                bundleSize = (ulong)information.Length;
                                bundleSha256 = Sha256(internalBundlePath);
                                bundleBuilt = true;
                                status = "passed";
                                exitCode = 0;
                            }
                        }
                    }
                    StopDiagnosticCapture(buildMessages);
                }
                WriteReport(resultPath, results, buildMessages, failures,
                    targetName, backendName, warningPolicy, status,
                    publishedBundlePath, bundleBuilt, bundleSize,
                    bundleSha256);
            }
            catch (Exception exception)
            {
                StopDiagnosticCapture(buildMessages);
                Debug.LogException(exception);
                failures.Add(new Failure {
                    Kind = "infrastructure_failure",
                    Message = exception.GetType().FullName + ": " +
                        exception.Message
                });
                status = "build_failed";
                bundleBuilt = false;
                bundleSize = 0U;
                bundleSha256 = String.Empty;
                DeleteBundleIfPresent(internalBundlePath);
                if (!String.IsNullOrEmpty(resultPath) && results.Count != 0)
                {
                    try
                    {
                        WriteReport(resultPath, results, buildMessages, failures,
                            targetName, backendName, warningPolicy, status,
                            publishedBundlePath, false, 0U, String.Empty);
                    }
                    catch (Exception reportException)
                    {
                        Debug.LogException(reportException);
                    }
                }
                exitCode = 3;
            }
            finally
            {
                StopDiagnosticCapture(buildMessages);
            }
            EditorApplication.Exit(exitCode);
        }
    }
}
