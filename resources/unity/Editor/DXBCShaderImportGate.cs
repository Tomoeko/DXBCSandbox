using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using UnityEditor;
using UnityEngine;

namespace DXBCSandbox.Editor
{
    // Unity Editor APIs have no native entry point. This intentionally small
    // bridge is copied into a fresh project by the C11 launcher for each run.
    public static class DXBCShaderImportGate
    {
        private const string Schema = "dxbc-sandbox-unity-shader-import/v1";

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

        private sealed class ShaderResult
        {
            public string SourcePath;
            public string AssetPath;
            public string ShaderName;
            public bool Imported;
            public readonly List<Diagnostic> Messages = new List<Diagnostic>();
        }

        private static string RequireArgument(string[] arguments, string name)
        {
            string value = null;
            for (int index = 0; index < arguments.Length; ++index)
            {
                if (!String.Equals(arguments[index], name, StringComparison.Ordinal))
                    continue;
                if (value != null || index + 1 >= arguments.Length ||
                    String.IsNullOrEmpty(arguments[index + 1]))
                    throw new InvalidOperationException("missing or duplicate " + name);
                value = arguments[++index];
            }
            if (value == null)
                throw new InvalidOperationException("missing " + name);
            return value;
        }

        private static List<Candidate> ReadManifest(string path)
        {
            string[] lines = File.ReadAllLines(path, new UTF8Encoding(false, true));
            var candidates = new List<Candidate>(lines.Length);
            var knownAssets = new HashSet<string>(StringComparer.Ordinal);
            var strictUtf8 = new UTF8Encoding(false, true);
            foreach (string line in lines)
            {
                int separator = line.IndexOf('\t');
                if (separator <= 0 || separator != line.LastIndexOf('\t') ||
                    separator + 1 >= line.Length)
                    throw new InvalidDataException("invalid candidate manifest record");
                string assetPath = line.Substring(0, separator);
                if (!assetPath.StartsWith("Assets/DXBCImportGate/", StringComparison.Ordinal) ||
                    !assetPath.EndsWith(".shader", StringComparison.Ordinal) ||
                    assetPath.IndexOf("..", StringComparison.Ordinal) >= 0 ||
                    !knownAssets.Add(assetPath))
                    throw new InvalidDataException("invalid or duplicate candidate asset path");
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

            string candidateDirectory = Path.Combine(
                Directory.GetCurrentDirectory(), "Assets", "DXBCImportGate");
            string[] files = Directory.GetFiles(
                candidateDirectory, "*.shader", SearchOption.TopDirectoryOnly);
            Array.Sort(files, StringComparer.Ordinal);
            if (files.Length != candidates.Count)
                throw new InvalidDataException("candidate manifest/file count mismatch");
            for (int index = 0; index < files.Length; ++index)
            {
                string relative = "Assets/DXBCImportGate/" + Path.GetFileName(files[index]);
                if (!String.Equals(relative, candidates[index].AssetPath,
                                   StringComparison.Ordinal))
                    throw new InvalidDataException("candidate manifest/file order mismatch");
            }
            return candidates;
        }

        private static object ReadMember(object instance, string name)
        {
            if (instance == null)
                return null;
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
                Convert.ToString(value, CultureInfo.InvariantCulture) ?? String.Empty;
        }

        private static uint MemberLine(object instance)
        {
            object value = ReadMember(instance, "line");
            if (value == null)
                return 0U;
            try
            {
                long line = Convert.ToInt64(value, CultureInfo.InvariantCulture);
                return line <= 0L ? 0U :
                    line >= UInt32.MaxValue ? UInt32.MaxValue : (uint)line;
            }
            catch (Exception)
            {
                return 0U;
            }
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
                BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic))
            {
                if (!String.Equals(method.Name, "GetShaderMessages",
                                   StringComparison.Ordinal))
                    continue;
                ParameterInfo[] parameters = method.GetParameters();
                if (parameters.Length == 1 && parameters[0].ParameterType == typeof(Shader))
                {
                    selected = method;
                    break;
                }
            }
            if (selected == null)
                throw new MissingMethodException("ShaderUtil.GetShaderMessages(Shader)");
            object result = selected.Invoke(null, new object[] { shader });
            Array messages = result as Array;
            if (messages == null)
                throw new InvalidDataException("GetShaderMessages returned a non-array");
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

        private static ShaderResult Import(Candidate candidate)
        {
            var result = new ShaderResult {
                SourcePath = candidate.SourcePath,
                AssetPath = candidate.AssetPath,
                ShaderName = String.Empty,
                Imported = false
            };
            try
            {
                AssetDatabase.ImportAsset(candidate.AssetPath,
                    ImportAssetOptions.ForceSynchronousImport |
                    ImportAssetOptions.ForceUpdate);
                Shader shader = AssetDatabase.LoadAssetAtPath<Shader>(candidate.AssetPath);
                if (shader == null)
                {
                    result.Messages.Add(InfrastructureDiagnostic(
                        "ShaderImporter produced no Shader asset"));
                    return result;
                }
                result.Imported = true;
                result.ShaderName = shader.name ?? String.Empty;
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
            return result;
        }

        private static int CompareDiagnostics(Diagnostic left, Diagnostic right)
        {
            int order = StringComparer.Ordinal.Compare(left.Severity, right.Severity);
            if (order != 0) return order;
            order = StringComparer.Ordinal.Compare(left.File, right.File);
            if (order != 0) return order;
            order = left.Line.CompareTo(right.Line);
            if (order != 0) return order;
            order = StringComparer.Ordinal.Compare(left.Platform, right.Platform);
            if (order != 0) return order;
            order = StringComparer.Ordinal.Compare(left.Message, right.Message);
            return order != 0 ? order :
                StringComparer.Ordinal.Compare(left.Details, right.Details);
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
                            output.Append("\\u").Append(((int)character).ToString("x4"));
                        else
                            output.Append(character);
                        break;
                }
            }
            output.Append('"');
        }

        private static string BuildReport(List<ShaderResult> shaders,
                                          string warningPolicy)
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
            bool passed = errorCount == 0U &&
                (warningPolicy == "allow" || warningCount == 0U);
            var output = new StringBuilder(4096);
            output.Append("{\n  \"schema\":"); JsonString(output, Schema);
            output.Append(",\n  \"unity_version\":");
            JsonString(output, Application.unityVersion);
            output.Append(",\n  \"warning_policy\":"); JsonString(output, warningPolicy);
            output.Append(",\n  \"candidate_count\":").Append(shaders.Count);
            output.Append(",\n  \"message_count\":").Append(messageCount);
            output.Append(",\n  \"error_count\":").Append(errorCount);
            output.Append(",\n  \"warning_count\":").Append(warningCount);
            output.Append(",\n  \"status\":"); JsonString(output, passed ? "passed" : "failed");
            output.Append(",\n  \"shaders\":[");
            for (int shaderIndex = 0; shaderIndex < shaders.Count; ++shaderIndex)
            {
                ShaderResult shader = shaders[shaderIndex];
                output.Append(shaderIndex == 0 ? "\n    {" : ",\n    {");
                output.Append("\n      \"source_path\":"); JsonString(output, shader.SourcePath);
                output.Append(",\n      \"asset_path\":"); JsonString(output, shader.AssetPath);
                output.Append(",\n      \"shader_name\":"); JsonString(output, shader.ShaderName);
                output.Append(",\n      \"imported\":").Append(shader.Imported ? "true" : "false");
                output.Append(",\n      \"messages\":[");
                for (int messageIndex = 0; messageIndex < shader.Messages.Count;
                     ++messageIndex)
                {
                    Diagnostic diagnostic = shader.Messages[messageIndex];
                    output.Append(messageIndex == 0 ? "\n        {" : ",\n        {");
                    output.Append("\"severity\":"); JsonString(output, diagnostic.Severity);
                    output.Append(",\"file\":"); JsonString(output, diagnostic.File);
                    output.Append(",\"line\":").Append(diagnostic.Line);
                    output.Append(",\"message\":"); JsonString(output, diagnostic.Message);
                    output.Append(",\"platform\":"); JsonString(output, diagnostic.Platform);
                    output.Append(",\"details\":"); JsonString(output, diagnostic.Details);
                    output.Append('}');
                }
                if (shader.Messages.Count != 0) output.Append('\n').Append("      ");
                output.Append("]\n    }");
            }
            if (shaders.Count != 0) output.Append('\n').Append("  ");
            output.Append("]\n}\n");
            return output.ToString();
        }

        private static void WriteReport(string path, List<ShaderResult> shaders,
                                        string warningPolicy)
        {
            File.WriteAllText(path, BuildReport(shaders, warningPolicy),
                              new UTF8Encoding(false, true));
        }

        public static void Run()
        {
            string resultPath = null;
            string warningPolicy = "allow";
            var results = new List<ShaderResult>();
            int exitCode = 3;
            try
            {
                string[] arguments = Environment.GetCommandLineArgs();
                resultPath = RequireArgument(arguments, "-dxbc-import-result");
                string manifestPath = RequireArgument(arguments, "-dxbc-import-manifest");
                string expectedVersion = RequireArgument(
                    arguments, "-dxbc-expected-unity-version");
                warningPolicy = RequireArgument(arguments, "-dxbc-warning-policy");
                if (warningPolicy != "allow" && warningPolicy != "fail")
                    throw new InvalidOperationException("invalid warning policy");
                if (!String.Equals(Application.unityVersion, expectedVersion,
                                   StringComparison.Ordinal))
                    throw new InvalidOperationException(
                        "Unity version mismatch: expected " + expectedVersion +
                        ", running " + Application.unityVersion);
                List<Candidate> candidates = ReadManifest(manifestPath);
                AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport |
                                      ImportAssetOptions.ForceUpdate);
                foreach (Candidate candidate in candidates)
                    results.Add(Import(candidate));
                WriteReport(resultPath, results, warningPolicy);

                bool failed = false;
                foreach (ShaderResult shader in results)
                {
                    foreach (Diagnostic diagnostic in shader.Messages)
                    {
                        if (diagnostic.Severity == "error" ||
                            (warningPolicy == "fail" &&
                             diagnostic.Severity == "warning"))
                            failed = true;
                    }
                }
                exitCode = failed ? 2 : 0;
            }
            catch (Exception exception)
            {
                Debug.LogException(exception);
                if (!String.IsNullOrEmpty(resultPath))
                {
                    var fatal = new ShaderResult {
                        SourcePath = "<gate>",
                        AssetPath = "Assets/Editor/DXBCShaderImportGate.cs",
                        ShaderName = String.Empty,
                        Imported = false
                    };
                    fatal.Messages.Add(InfrastructureDiagnostic(
                        exception.GetType().FullName + ": " + exception.Message));
                    results.Add(fatal);
                    try { WriteReport(resultPath, results, warningPolicy); }
                    catch (Exception reportException) { Debug.LogException(reportException); }
                }
                exitCode = 3;
            }
            EditorApplication.Exit(exitCode);
        }
    }
}
