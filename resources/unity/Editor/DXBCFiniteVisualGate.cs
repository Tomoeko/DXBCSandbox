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
    // Minimal managed rendering boundary for the portable C11 launcher. The
    // file is copied into a fresh project for one invocation. It must never be
    // installed into or execute against a caller-owned Unity project.
    public static class DXBCFiniteVisualGate
    {
        private const string ResultSchema =
            "dxbc-sandbox-unity-finite-visual/v1";
        private const string FixtureSchema =
            "dxbc-sandbox-finite-visual-fixture/v1";
        private const string BaselineAsset =
            "Assets/DXBCFiniteVisual/Baseline.shader";
        private const string CandidateAsset =
            "Assets/DXBCFiniteVisual/Candidate.shader";
        private const string ZeroSha =
            "0000000000000000000000000000000000000000000000000000000000000000";

        private enum BindingKind
        {
            Float,
            Vector,
            Matrix,
            Texture2D
        }

        private sealed class TextureValue
        {
            public int Width;
            public int Height;
            public bool Linear;
            public FilterMode Filter;
            public TextureWrapMode Wrap;
            public byte[] Bytes;
        }

        private sealed class Binding
        {
            public string Name;
            public BindingKind Kind;
            public float FloatValue;
            public Vector4 VectorValue;
            public Matrix4x4 MatrixValue;
            public TextureValue TextureValue;
        }

        private sealed class Fixture
        {
            public int Width = 1;
            public int Height = 1;
            public int PassIndex;
            public string ColorSpace = "linear";
            public Color Clear = new Color(0.0f, 0.0f, 0.0f, 0.0f);
            public bool RequireCompleteProperties = true;
            public readonly List<Binding> Properties = new List<Binding>();
            public readonly List<Binding> Globals = new List<Binding>();
            public readonly List<string> LocalKeywords = new List<string>();
            public readonly List<string> GlobalKeywords = new List<string>();
        }

        private sealed class Diagnostic
        {
            public string Side;
            public string Severity;
            public uint Line;
            public string File;
            public string Message;
            public string Platform;
            public string Details;
        }

        private sealed class Failure
        {
            public string Kind;
            public string Message;
        }

        private sealed class Result
        {
            public string Status = "unsupported-fixture";
            public string UnityVersion = String.Empty;
            public string Backend = "metal";
            public string DeviceName = String.Empty;
            public string DeviceVendor = String.Empty;
            public string DeviceVersion = String.Empty;
            public string RequestedColorSpace = "linear";
            public string ColorSpace = "gamma";
            public uint Width = 1U;
            public uint Height = 1U;
            public uint Pass;
            public string FixtureSha = ZeroSha;
            public string BridgeSha = ZeroSha;
            public string BaselineSourceSha = ZeroSha;
            public string CandidateSourceSha = ZeroSha;
            public bool BaselineImported;
            public bool CandidateImported;
            public bool BaselineStable;
            public bool CandidateStable;
            public bool PixelEqual;
            public ulong PixelByteCount = 16UL;
            public ulong FirstMismatchOffset = UInt64.MaxValue;
            public string BaselinePixelsSha = ZeroSha;
            public string CandidatePixelsSha = ZeroSha;
            public readonly List<Diagnostic> Diagnostics =
                new List<Diagnostic>();
            public readonly List<Failure> Failures = new List<Failure>();
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

        private static string Base64(string value)
        {
            return Convert.ToBase64String(
                new UTF8Encoding(false, true).GetBytes(value ?? String.Empty));
        }

        private static string DecodeName(string encoded)
        {
            byte[] bytes = Convert.FromBase64String(encoded);
            if (!String.Equals(Convert.ToBase64String(bytes), encoded,
                               StringComparison.Ordinal))
                throw new InvalidDataException("noncanonical base64 name");
            string value = new UTF8Encoding(false, true).GetString(bytes);
            if (String.IsNullOrEmpty(value) || value.IndexOf('\0') >= 0 ||
                value.IndexOf('\t') >= 0 || value.IndexOf('\r') >= 0 ||
                value.IndexOf('\n') >= 0)
                throw new InvalidDataException("invalid binding name");
            return value;
        }

        private static byte[] ReadStrictUtf8(string path, int maximum)
        {
            byte[] bytes = File.ReadAllBytes(path);
            if (bytes.Length == 0 || bytes.Length > maximum)
                throw new InvalidDataException("file size is outside bounds");
            if (bytes.Length >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb &&
                bytes[2] == 0xbf)
                throw new InvalidDataException("UTF-8 BOM is not canonical");
            new UTF8Encoding(false, true).GetString(bytes);
            return bytes;
        }

        private static string Sha256(byte[] bytes)
        {
            using (SHA256 hash = SHA256.Create())
            {
                byte[] digest = hash.ComputeHash(bytes);
                var output = new StringBuilder(64);
                foreach (byte value in digest)
                    output.Append(value.ToString("x2", CultureInfo.InvariantCulture));
                return output.ToString();
            }
        }

        private static uint ParseHexBits(string text)
        {
            if (text == null || text.Length != 8)
                throw new InvalidDataException("float bits must have eight hex digits");
            for (int index = 0; index < text.Length; ++index)
            {
                char value = text[index];
                if (!((value >= '0' && value <= '9') ||
                      (value >= 'a' && value <= 'f')))
                    throw new InvalidDataException(
                        "float bits must use lowercase hexadecimal");
            }
            return UInt32.Parse(text, NumberStyles.AllowHexSpecifier,
                                CultureInfo.InvariantCulture);
        }

        private static float ParseFiniteFloat(string text)
        {
            byte[] bytes = BitConverter.GetBytes(ParseHexBits(text));
            if (!BitConverter.IsLittleEndian) Array.Reverse(bytes);
            float value = BitConverter.ToSingle(bytes, 0);
            if (Single.IsNaN(value) || Single.IsInfinity(value))
                throw new InvalidDataException(
                    "non-finite fixture values are unsupported");
            return value;
        }

        private static int ParseBoundedInt(string text, int minimum,
                                           int maximum, string field)
        {
            int value;
            if (!Int32.TryParse(text, NumberStyles.None,
                                CultureInfo.InvariantCulture, out value) ||
                value < minimum || value > maximum ||
                !String.Equals(value.ToString(CultureInfo.InvariantCulture),
                               text, StringComparison.Ordinal))
                throw new InvalidDataException("invalid " + field);
            return value;
        }

        private static TextureValue ParseTexture(string[] fields)
        {
            if (fields.Length != 8)
                throw new InvalidDataException("invalid texture2d record");
            int width = ParseBoundedInt(fields[2], 1, 1024, "texture width");
            int height = ParseBoundedInt(fields[3], 1, 1024, "texture height");
            bool linear;
            if (fields[4] == "linear") linear = true;
            else if (fields[4] == "srgb") linear = false;
            else throw new InvalidDataException("invalid texture color space");
            FilterMode filter;
            if (fields[5] == "point") filter = FilterMode.Point;
            else if (fields[5] == "bilinear") filter = FilterMode.Bilinear;
            else throw new InvalidDataException("invalid texture filter");
            TextureWrapMode wrap;
            if (fields[6] == "clamp") wrap = TextureWrapMode.Clamp;
            else if (fields[6] == "repeat") wrap = TextureWrapMode.Repeat;
            else if (fields[6] == "mirror") wrap = TextureWrapMode.Mirror;
            else throw new InvalidDataException("invalid texture wrap");
            byte[] bytes = Convert.FromBase64String(fields[7]);
            if (!String.Equals(Convert.ToBase64String(bytes), fields[7],
                               StringComparison.Ordinal) ||
                bytes.LongLength != (long)width * height * 16L)
                throw new InvalidDataException("invalid RGBAFloat texture payload");
            for (int offset = 0; offset < bytes.Length; offset += 4)
            {
                byte[] word = new byte[4];
                Buffer.BlockCopy(bytes, offset, word, 0, 4);
                if (!BitConverter.IsLittleEndian) Array.Reverse(word);
                float value = BitConverter.ToSingle(word, 0);
                if (Single.IsNaN(value) || Single.IsInfinity(value))
                    throw new InvalidDataException(
                        "texture payload contains a non-finite value");
            }
            return new TextureValue {
                Width = width,
                Height = height,
                Linear = linear,
                Filter = filter,
                Wrap = wrap,
                Bytes = bytes
            };
        }

        private static Binding ParseBinding(string[] fields,
                                            BindingKind expectedKind)
        {
            if (fields.Length < 3)
                throw new InvalidDataException("truncated binding record");
            var binding = new Binding { Name = DecodeName(fields[1]) };
            binding.Kind = expectedKind;
            if (expectedKind == BindingKind.Texture2D)
            {
                binding.TextureValue = ParseTexture(fields);
            }
            else if (expectedKind == BindingKind.Float && fields.Length == 3)
            {
                binding.FloatValue = ParseFiniteFloat(fields[2]);
            }
            else if (expectedKind == BindingKind.Vector && fields.Length == 6)
            {
                binding.VectorValue = new Vector4(
                    ParseFiniteFloat(fields[2]), ParseFiniteFloat(fields[3]),
                    ParseFiniteFloat(fields[4]), ParseFiniteFloat(fields[5]));
            }
            else if (expectedKind == BindingKind.Matrix && fields.Length == 18)
            {
                Matrix4x4 matrix = new Matrix4x4();
                for (int row = 0; row < 4; ++row)
                    for (int column = 0; column < 4; ++column)
                        matrix[row, column] = ParseFiniteFloat(
                            fields[2 + row * 4 + column]);
                binding.MatrixValue = matrix;
            }
            else
            {
                throw new InvalidDataException("invalid binding arity");
            }
            return binding;
        }

        private static void AddUniqueBinding(List<Binding> bindings,
                                             Binding binding)
        {
            foreach (Binding known in bindings)
                if (String.Equals(known.Name, binding.Name,
                                  StringComparison.Ordinal))
                    throw new InvalidDataException(
                        "duplicate binding: " + binding.Name);
            bindings.Add(binding);
        }

        private static void AddUniqueKeyword(List<string> keywords,
                                             string keyword)
        {
            if (keywords.Contains(keyword))
                throw new InvalidDataException("duplicate keyword: " + keyword);
            keywords.Add(keyword);
        }

        private static Fixture ReadFixture(byte[] bytes)
        {
            string text = new UTF8Encoding(false, true).GetString(bytes);
            if (text.IndexOf('\r') >= 0 || text.IndexOf('\0') >= 0 ||
                !text.EndsWith("\n", StringComparison.Ordinal))
                throw new InvalidDataException(
                    "fixture must use canonical LF-terminated UTF-8");
            string[] lines = text.Substring(0, text.Length - 1).Split('\n');
            if (lines.Length < 7 || lines[0] != FixtureSchema ||
                lines[lines.Length - 1] != "end")
                throw new InvalidDataException("invalid fixture envelope");
            var fixture = new Fixture();
            bool seenSize = false;
            bool seenPass = false;
            bool seenColor = false;
            bool seenClear = false;
            bool seenComplete = false;
            for (int lineIndex = 1; lineIndex + 1 < lines.Length; ++lineIndex)
            {
                if (lines[lineIndex].Length == 0)
                    throw new InvalidDataException("empty fixture record");
                string[] fields = lines[lineIndex].Split('\t');
                switch (fields[0])
                {
                    case "size":
                        if (seenSize || fields.Length != 3)
                            throw new InvalidDataException("invalid size record");
                        fixture.Width = ParseBoundedInt(fields[1], 1, 1024,
                                                        "render width");
                        fixture.Height = ParseBoundedInt(fields[2], 1, 1024,
                                                         "render height");
                        seenSize = true;
                        break;
                    case "pass":
                        if (seenPass || fields.Length != 2)
                            throw new InvalidDataException("invalid pass record");
                        fixture.PassIndex = ParseBoundedInt(fields[1], 0, 65535,
                                                            "pass index");
                        seenPass = true;
                        break;
                    case "color_space":
                        if (seenColor || fields.Length != 2 ||
                            (fields[1] != "linear" && fields[1] != "gamma"))
                            throw new InvalidDataException(
                                "invalid color_space record");
                        fixture.ColorSpace = fields[1];
                        seenColor = true;
                        break;
                    case "clear":
                        if (seenClear || fields.Length != 5)
                            throw new InvalidDataException("invalid clear record");
                        fixture.Clear = new Color(
                            ParseFiniteFloat(fields[1]),
                            ParseFiniteFloat(fields[2]),
                            ParseFiniteFloat(fields[3]),
                            ParseFiniteFloat(fields[4]));
                        seenClear = true;
                        break;
                    case "require_complete_properties":
                        if (seenComplete || fields.Length != 2 || fields[1] != "1")
                            throw new InvalidDataException(
                                "v1 requires complete material properties");
                        fixture.RequireCompleteProperties = true;
                        seenComplete = true;
                        break;
                    case "property_float":
                        AddUniqueBinding(fixture.Properties,
                            ParseBinding(fields, BindingKind.Float));
                        break;
                    case "property_vector":
                        AddUniqueBinding(fixture.Properties,
                            ParseBinding(fields, BindingKind.Vector));
                        break;
                    case "property_matrix":
                        AddUniqueBinding(fixture.Properties,
                            ParseBinding(fields, BindingKind.Matrix));
                        break;
                    case "property_texture2d":
                        AddUniqueBinding(fixture.Properties,
                            ParseBinding(fields, BindingKind.Texture2D));
                        break;
                    case "global_float":
                        AddUniqueBinding(fixture.Globals,
                            ParseBinding(fields, BindingKind.Float));
                        break;
                    case "global_vector":
                        AddUniqueBinding(fixture.Globals,
                            ParseBinding(fields, BindingKind.Vector));
                        break;
                    case "global_matrix":
                        AddUniqueBinding(fixture.Globals,
                            ParseBinding(fields, BindingKind.Matrix));
                        break;
                    case "global_texture2d":
                        AddUniqueBinding(fixture.Globals,
                            ParseBinding(fields, BindingKind.Texture2D));
                        break;
                    case "local_keyword":
                        if (fields.Length != 2)
                            throw new InvalidDataException(
                                "invalid local_keyword record");
                        AddUniqueKeyword(fixture.LocalKeywords,
                                         DecodeName(fields[1]));
                        break;
                    case "global_keyword":
                        if (fields.Length != 2)
                            throw new InvalidDataException(
                                "invalid global_keyword record");
                        AddUniqueKeyword(fixture.GlobalKeywords,
                                         DecodeName(fields[1]));
                        break;
                    default:
                        throw new InvalidDataException(
                            "unknown fixture record: " + fields[0]);
                }
            }
            if (!seenSize || !seenPass || !seenColor || !seenClear ||
                !seenComplete)
                throw new InvalidDataException("fixture is missing required records");
            foreach (Binding property in fixture.Properties)
                if (fixture.Globals.Exists(value => String.Equals(
                        value.Name, property.Name, StringComparison.Ordinal)))
                    throw new InvalidDataException(
                        "binding appears in material and global scope: " +
                        property.Name);
            foreach (string keyword in fixture.LocalKeywords)
                if (fixture.GlobalKeywords.Contains(keyword))
                    throw new InvalidDataException(
                        "keyword appears in local and global scope: " + keyword);
            fixture.Properties.Sort((left, right) =>
                StringComparer.Ordinal.Compare(left.Name, right.Name));
            fixture.Globals.Sort((left, right) =>
                StringComparer.Ordinal.Compare(left.Name, right.Name));
            fixture.LocalKeywords.Sort(StringComparer.Ordinal);
            fixture.GlobalKeywords.Sort(StringComparer.Ordinal);
            return fixture;
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
                long line = Convert.ToInt64(value, CultureInfo.InvariantCulture);
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
                    Array messages = method.Invoke(null,
                        new object[] { shader }) as Array;
                    if (messages == null)
                        throw new InvalidDataException(
                            "GetShaderMessages returned a non-array");
                    return messages;
                }
            }
            throw new MissingMethodException(
                "ShaderUtil.GetShaderMessages(Shader)");
        }

        private static void CollectShaderDiagnostics(Result result,
                                                     Shader shader,
                                                     string side)
        {
            foreach (object message in GetShaderMessages(shader))
            {
                result.Diagnostics.Add(new Diagnostic {
                    Side = side,
                    Severity = NormalizeSeverity(message),
                    Line = MemberLine(message),
                    File = MemberString(message, "file"),
                    Message = MemberString(message, "message"),
                    Platform = MemberString(message, "platform"),
                    Details = MemberString(message, "messageDetails")
                });
            }
        }

        private static string NormalizeBackend(GraphicsDeviceType type)
        {
            switch (type)
            {
                case GraphicsDeviceType.Metal: return "metal";
                case GraphicsDeviceType.Direct3D11: return "d3d11";
                case GraphicsDeviceType.OpenGLCore: return "openglcore";
                case GraphicsDeviceType.Vulkan: return "vulkan";
                default: return "unsupported";
            }
        }

        private static void AddFailure(Result result, string kind,
                                       string message)
        {
            result.Failures.Add(new Failure {
                Kind = kind ?? String.Empty,
                Message = message ?? String.Empty
            });
        }

        private static Texture2D CreateTexture(TextureValue value)
        {
            var texture = new Texture2D(value.Width, value.Height,
                TextureFormat.RGBAFloat, false, value.Linear);
            texture.name = "DXBCFiniteVisualTexture";
            texture.filterMode = value.Filter;
            texture.wrapMode = value.Wrap;
            texture.anisoLevel = 0;
            texture.LoadRawTextureData(value.Bytes);
            texture.Apply(false, true);
            return texture;
        }

        private static void ApplyBinding(Material material, Binding binding,
                                         List<Texture2D> textures)
        {
            switch (binding.Kind)
            {
                case BindingKind.Float:
                    material.SetFloat(binding.Name, binding.FloatValue);
                    break;
                case BindingKind.Vector:
                    material.SetVector(binding.Name, binding.VectorValue);
                    break;
                case BindingKind.Matrix:
                    material.SetMatrix(binding.Name, binding.MatrixValue);
                    break;
                case BindingKind.Texture2D:
                    Texture2D texture = CreateTexture(binding.TextureValue);
                    textures.Add(texture);
                    material.SetTexture(binding.Name, texture);
                    material.SetTextureScale(binding.Name, Vector2.one);
                    material.SetTextureOffset(binding.Name, Vector2.zero);
                    break;
            }
        }

        private static void ApplyGlobal(Binding binding,
                                        List<Texture2D> textures)
        {
            switch (binding.Kind)
            {
                case BindingKind.Float:
                    Shader.SetGlobalFloat(binding.Name, binding.FloatValue);
                    break;
                case BindingKind.Vector:
                    Shader.SetGlobalVector(binding.Name, binding.VectorValue);
                    break;
                case BindingKind.Matrix:
                    Shader.SetGlobalMatrix(binding.Name, binding.MatrixValue);
                    break;
                case BindingKind.Texture2D:
                    Texture2D texture = CreateTexture(binding.TextureValue);
                    textures.Add(texture);
                    Shader.SetGlobalTexture(binding.Name, texture);
                    break;
            }
        }

        private static string PropertyKind(Shader shader, int index)
        {
            ShaderPropertyType type = shader.GetPropertyType(index);
            switch (type)
            {
                case ShaderPropertyType.Float:
                case ShaderPropertyType.Range:
                    return "float";
                case ShaderPropertyType.Color:
                case ShaderPropertyType.Vector:
                    return "vector";
                case ShaderPropertyType.Texture:
                    return shader.GetPropertyTextureDimension(index) ==
                        TextureDimension.Tex2D ? "texture2d" : "unsupported";
                default:
                    return "unsupported";
            }
        }

        private static string BindingKindName(Binding binding)
        {
            switch (binding.Kind)
            {
                case BindingKind.Float: return "float";
                case BindingKind.Vector: return "vector";
                case BindingKind.Texture2D: return "texture2d";
                default: return "matrix";
            }
        }

        private static bool ValidatePropertyClosure(Result result,
                                                    Fixture fixture,
                                                    Shader baseline,
                                                    Shader candidate)
        {
            var baselineProperties = new SortedDictionary<string, string>(
                StringComparer.Ordinal);
            var candidateProperties = new SortedDictionary<string, string>(
                StringComparer.Ordinal);
            for (int index = 0; index < baseline.GetPropertyCount(); ++index)
                baselineProperties.Add(baseline.GetPropertyName(index),
                                       PropertyKind(baseline, index));
            for (int index = 0; index < candidate.GetPropertyCount(); ++index)
                candidateProperties.Add(candidate.GetPropertyName(index),
                                        PropertyKind(candidate, index));
            if (baselineProperties.Count != candidateProperties.Count)
            {
                AddFailure(result, "property-closure",
                    "baseline and candidate declare different property counts");
                return false;
            }
            foreach (KeyValuePair<string, string> property in baselineProperties)
            {
                string candidateKind;
                if (!candidateProperties.TryGetValue(property.Key,
                                                      out candidateKind) ||
                    candidateKind != property.Value || property.Value == "unsupported")
                {
                    AddFailure(result, "property-closure",
                        "property signature differs or is unsupported: " +
                        property.Key);
                    return false;
                }
                Binding binding = fixture.Properties.Find(value =>
                    String.Equals(value.Name, property.Key,
                                  StringComparison.Ordinal));
                if (binding == null || BindingKindName(binding) != property.Value)
                {
                    AddFailure(result, "property-closure",
                        "fixture does not bind declared property: " + property.Key);
                    return false;
                }
            }
            foreach (Binding binding in fixture.Properties)
            {
                string kind;
                if (!baselineProperties.TryGetValue(binding.Name, out kind) ||
                    kind != BindingKindName(binding))
                {
                    AddFailure(result, "property-closure",
                        "fixture binds no identical declared property: " +
                        binding.Name);
                    return false;
                }
            }
            return true;
        }

        private static Mesh CreateCanonicalMesh()
        {
            var mesh = new Mesh();
            mesh.name = "DXBCFiniteVisualCanonicalQuad/v1";
            mesh.vertices = new[] {
                new Vector3(-1.0f, -1.0f, 0.0f),
                new Vector3( 1.0f, -1.0f, 0.0f),
                new Vector3(-1.0f,  1.0f, 0.0f),
                new Vector3( 1.0f,  1.0f, 0.0f)
            };
            mesh.uv = new[] {
                new Vector2(0.0f, 0.0f), new Vector2(1.0f, 0.0f),
                new Vector2(0.0f, 1.0f), new Vector2(1.0f, 1.0f)
            };
            mesh.normals = new[] {
                Vector3.back, Vector3.back, Vector3.back, Vector3.back
            };
            mesh.tangents = new[] {
                new Vector4(1.0f, 0.0f, 0.0f, 1.0f),
                new Vector4(1.0f, 0.0f, 0.0f, 1.0f),
                new Vector4(1.0f, 0.0f, 0.0f, 1.0f),
                new Vector4(1.0f, 0.0f, 0.0f, 1.0f)
            };
            mesh.colors = new[] { Color.white, Color.white, Color.white,
                                  Color.white };
            mesh.SetIndices(new[] { 0, 2, 1, 1, 2, 3 },
                            MeshTopology.Triangles, 0, false);
            mesh.bounds = new Bounds(Vector3.zero,
                                     new Vector3(2.0f, 2.0f, 0.0f));
            mesh.UploadMeshData(true);
            return mesh;
        }

        private static void SetCanonicalGlobals(Fixture fixture,
                                                List<Texture2D> textures)
        {
            foreach (GlobalKeyword keyword in Shader.enabledGlobalKeywords)
                Shader.DisableKeyword(keyword.name);
            Shader.SetGlobalVector("_Time", Vector4.zero);
            Shader.SetGlobalVector("_SinTime", Vector4.zero);
            Shader.SetGlobalVector("_CosTime", Vector4.one);
            Shader.SetGlobalVector("unity_DeltaTime", Vector4.zero);
            Shader.SetGlobalVector("_ProjectionParams",
                                   new Vector4(1.0f, 1.0f, 1.0f, 0.0f));
            Shader.SetGlobalVector("_ScreenParams",
                new Vector4(fixture.Width, fixture.Height,
                    1.0f + 1.0f / fixture.Width,
                    1.0f + 1.0f / fixture.Height));
            foreach (Binding binding in fixture.Globals)
                ApplyGlobal(binding, textures);
            foreach (string keyword in fixture.GlobalKeywords)
            {
                Shader.DisableKeyword(keyword);
                Shader.EnableKeyword(keyword);
            }
        }

        private static byte[] Capture(Material material, Fixture fixture,
                                      Mesh mesh, List<Texture2D> textures)
        {
            SetCanonicalGlobals(fixture, textures);
            RenderTexture target = null;
            Texture2D readback = null;
            RenderTexture previous = RenderTexture.active;
            try
            {
                target = new RenderTexture(fixture.Width, fixture.Height, 24,
                    RenderTextureFormat.ARGBFloat, RenderTextureReadWrite.Linear);
                target.name = "DXBCFiniteVisualARGBFloat/v1";
                target.antiAliasing = 1;
                target.useMipMap = false;
                target.autoGenerateMips = false;
                target.enableRandomWrite = false;
                target.filterMode = FilterMode.Point;
                target.wrapMode = TextureWrapMode.Clamp;
                if (!target.Create() || target.format != RenderTextureFormat.ARGBFloat)
                    throw new NotSupportedException(
                        "ARGBFloat RenderTexture creation failed");
                RenderTexture.active = target;
                GL.Viewport(new Rect(0, 0, fixture.Width, fixture.Height));
                GL.Clear(true, true, fixture.Clear, 1.0f);
                GL.PushMatrix();
                try
                {
                    GL.LoadIdentity();
                    GL.LoadProjectionMatrix(Matrix4x4.identity);
                    if (!material.SetPass(fixture.PassIndex))
                        throw new InvalidOperationException(
                            "Material.SetPass returned false");
                    Graphics.DrawMeshNow(mesh, Matrix4x4.identity, 0);
                }
                finally
                {
                    GL.PopMatrix();
                }
                readback = new Texture2D(fixture.Width, fixture.Height,
                    TextureFormat.RGBAFloat, false, true);
                readback.ReadPixels(new Rect(0, 0, fixture.Width,
                                             fixture.Height), 0, 0, false);
                readback.Apply(false, false);
                byte[] bytes = readback.GetRawTextureData();
                if (bytes.LongLength != (long)fixture.Width *
                    fixture.Height * 16L)
                    throw new InvalidDataException(
                        "RGBAFloat readback byte count mismatch");
                return bytes;
            }
            finally
            {
                RenderTexture.active = previous;
                if (target != null)
                {
                    target.Release();
                    UnityEngine.Object.DestroyImmediate(target);
                }
                if (readback != null)
                    UnityEngine.Object.DestroyImmediate(readback);
            }
        }

        private static Material CreateMaterial(Shader shader, Fixture fixture,
                                               List<Texture2D> textures)
        {
            var material = new Material(shader) {
                hideFlags = HideFlags.HideAndDontSave
            };
            material.shaderKeywords = fixture.LocalKeywords.ToArray();
            foreach (Binding binding in fixture.Properties)
                ApplyBinding(material, binding, textures);
            return material;
        }

        private static ulong FirstMismatch(byte[] baseline, byte[] candidate)
        {
            int count = Math.Min(baseline.Length, candidate.Length);
            for (int index = 0; index < count; ++index)
                if (baseline[index] != candidate[index]) return (ulong)index;
            return baseline.Length == candidate.Length ? UInt64.MaxValue :
                (ulong)count;
        }

        private static int CompareDiagnostics(Diagnostic left,
                                              Diagnostic right)
        {
            int order = StringComparer.Ordinal.Compare(left.Side, right.Side);
            if (order != 0) return order;
            order = StringComparer.Ordinal.Compare(left.Severity, right.Severity);
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

        private static void WriteResult(string path, Result result)
        {
            result.Diagnostics.Sort(CompareDiagnostics);
            result.Failures.Sort((left, right) => {
                int order = StringComparer.Ordinal.Compare(left.Kind, right.Kind);
                return order != 0 ? order :
                    StringComparer.Ordinal.Compare(left.Message, right.Message);
            });
            ulong errors = 0UL;
            ulong warnings = 0UL;
            foreach (Diagnostic diagnostic in result.Diagnostics)
            {
                if (diagnostic.Severity == "error") ++errors;
                else if (diagnostic.Severity == "warning") ++warnings;
            }
            var output = new StringBuilder();
            output.Append(ResultSchema).Append('\n');
            output.Append("status\t").Append(result.Status).Append('\n');
            output.Append("unity_version\t").Append(Base64(result.UnityVersion)).Append('\n');
            output.Append("backend\t").Append(result.Backend).Append('\n');
            output.Append("device_name\t").Append(Base64(result.DeviceName)).Append('\n');
            output.Append("device_vendor\t").Append(Base64(result.DeviceVendor)).Append('\n');
            output.Append("device_version\t").Append(Base64(result.DeviceVersion)).Append('\n');
            output.Append("requested_color_space\t")
                .Append(Base64(result.RequestedColorSpace)).Append('\n');
            output.Append("color_space\t").Append(Base64(result.ColorSpace)).Append('\n');
            output.Append("width\t").Append(result.Width.ToString(
                CultureInfo.InvariantCulture)).Append('\n');
            output.Append("height\t").Append(result.Height.ToString(
                CultureInfo.InvariantCulture)).Append('\n');
            output.Append("pass\t").Append(result.Pass.ToString(
                CultureInfo.InvariantCulture)).Append('\n');
            output.Append("fixture_sha256\t").Append(result.FixtureSha).Append('\n');
            output.Append("bridge_sha256\t").Append(result.BridgeSha).Append('\n');
            output.Append("baseline_source_sha256\t")
                .Append(result.BaselineSourceSha).Append('\n');
            output.Append("candidate_source_sha256\t")
                .Append(result.CandidateSourceSha).Append('\n');
            output.Append("baseline_imported\t")
                .Append(result.BaselineImported ? "1" : "0").Append('\n');
            output.Append("candidate_imported\t")
                .Append(result.CandidateImported ? "1" : "0").Append('\n');
            output.Append("baseline_stable\t")
                .Append(result.BaselineStable ? "1" : "0").Append('\n');
            output.Append("candidate_stable\t")
                .Append(result.CandidateStable ? "1" : "0").Append('\n');
            output.Append("pixel_equal\t")
                .Append(result.PixelEqual ? "1" : "0").Append('\n');
            output.Append("pixel_byte_count\t").Append(
                result.PixelByteCount.ToString(CultureInfo.InvariantCulture)).Append('\n');
            output.Append("first_mismatch_offset\t").Append(
                result.FirstMismatchOffset.ToString(CultureInfo.InvariantCulture)).Append('\n');
            output.Append("baseline_pixels_sha256\t")
                .Append(result.BaselinePixelsSha).Append('\n');
            output.Append("candidate_pixels_sha256\t")
                .Append(result.CandidatePixelsSha).Append('\n');
            output.Append("diagnostic_count\t").Append(
                result.Diagnostics.Count.ToString(CultureInfo.InvariantCulture)).Append('\n');
            output.Append("error_count\t").Append(
                errors.ToString(CultureInfo.InvariantCulture)).Append('\n');
            output.Append("warning_count\t").Append(
                warnings.ToString(CultureInfo.InvariantCulture)).Append('\n');
            output.Append("failure_count\t").Append(
                result.Failures.Count.ToString(CultureInfo.InvariantCulture)).Append('\n');
            foreach (Diagnostic diagnostic in result.Diagnostics)
            {
                output.Append("diagnostic\t").Append(diagnostic.Side)
                    .Append('\t').Append(diagnostic.Severity)
                    .Append('\t').Append(diagnostic.Line.ToString(
                        CultureInfo.InvariantCulture))
                    .Append('\t').Append(Base64(diagnostic.File))
                    .Append('\t').Append(Base64(diagnostic.Message))
                    .Append('\t').Append(Base64(diagnostic.Platform))
                    .Append('\t').Append(Base64(diagnostic.Details)).Append('\n');
            }
            foreach (Failure failure in result.Failures)
                output.Append("failure\t").Append(Base64(failure.Kind))
                    .Append('\t').Append(Base64(failure.Message)).Append('\n');
            output.Append("end\n");
            File.WriteAllText(path, output.ToString(),
                              new UTF8Encoding(false, true));
        }

        public static void Run()
        {
            string[] arguments = Environment.GetCommandLineArgs();
            string resultPath = null;
            var result = new Result();
            int exitCode = 2;
            try
            {
                resultPath = RequireArgument(arguments, "-dxbc-finite-result");
                string baselinePixelsPath = RequireArgument(arguments,
                    "-dxbc-finite-baseline-pixels");
                string candidatePixelsPath = RequireArgument(arguments,
                    "-dxbc-finite-candidate-pixels");
                string fixturePath = RequireArgument(arguments,
                    "-dxbc-finite-fixture");
                string expectedUnity = RequireArgument(arguments,
                    "-dxbc-finite-expected-unity");
                string expectedBackend = RequireArgument(arguments,
                    "-dxbc-finite-expected-backend");
                result.BridgeSha = RequireArgument(arguments,
                    "-dxbc-finite-bridge-sha256");
                result.UnityVersion = Application.unityVersion ?? String.Empty;
                result.Backend = NormalizeBackend(SystemInfo.graphicsDeviceType);
                result.DeviceName = SystemInfo.graphicsDeviceName ?? String.Empty;
                result.DeviceVendor = SystemInfo.graphicsDeviceVendor ?? String.Empty;
                result.DeviceVersion = SystemInfo.graphicsDeviceVersion ?? String.Empty;
                result.ColorSpace = QualitySettings.activeColorSpace ==
                    ColorSpace.Linear ? "linear" : "gamma";

                byte[] fixtureBytes = ReadStrictUtf8(fixturePath,
                                                      256 * 1024 * 1024);
                string projectRoot = Directory.GetParent(
                    Application.dataPath).FullName;
                byte[] baselineSource = File.ReadAllBytes(Path.Combine(
                    projectRoot, BaselineAsset));
                byte[] candidateSource = File.ReadAllBytes(Path.Combine(
                    projectRoot, CandidateAsset));
                result.FixtureSha = Sha256(fixtureBytes);
                result.BaselineSourceSha = Sha256(baselineSource);
                result.CandidateSourceSha = Sha256(candidateSource);
                Fixture fixture = ReadFixture(fixtureBytes);
                result.Width = (uint)fixture.Width;
                result.Height = (uint)fixture.Height;
                result.Pass = (uint)fixture.PassIndex;
                result.RequestedColorSpace = fixture.ColorSpace;
                result.PixelByteCount = (ulong)fixture.Width *
                    (ulong)fixture.Height * 16UL;

                if (!String.Equals(result.UnityVersion, expectedUnity,
                                   StringComparison.Ordinal))
                    AddFailure(result, "unity-version",
                        "actual Unity version does not match the requested version");
                if (!String.Equals(result.Backend, expectedBackend,
                                   StringComparison.Ordinal))
                    AddFailure(result, "graphics-backend",
                        "actual graphics backend does not match the requested backend");
                ColorSpace requestedColor = fixture.ColorSpace == "linear"
                    ? ColorSpace.Linear : ColorSpace.Gamma;
                if (PlayerSettings.colorSpace != requestedColor)
                {
                    PlayerSettings.colorSpace = requestedColor;
                    AssetDatabase.SaveAssets();
                    AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
                }
                result.ColorSpace = QualitySettings.activeColorSpace ==
                    ColorSpace.Linear ? "linear" : "gamma";
                if (!String.Equals(result.ColorSpace, fixture.ColorSpace,
                                   StringComparison.Ordinal))
                    AddFailure(result, "color-space",
                        "active color space does not match the fixture");
                if (!SystemInfo.SupportsRenderTextureFormat(
                        RenderTextureFormat.ARGBFloat) ||
                    !SystemInfo.SupportsTextureFormat(TextureFormat.RGBAFloat))
                    AddFailure(result, "render-format",
                        "ARGBFloat render target or RGBAFloat readback is unsupported");

                AssetDatabase.ImportAsset(BaselineAsset,
                    ImportAssetOptions.ForceSynchronousImport |
                    ImportAssetOptions.ForceUpdate);
                AssetDatabase.ImportAsset(CandidateAsset,
                    ImportAssetOptions.ForceSynchronousImport |
                    ImportAssetOptions.ForceUpdate);
                Shader baseline = AssetDatabase.LoadAssetAtPath<Shader>(BaselineAsset);
                Shader candidate = AssetDatabase.LoadAssetAtPath<Shader>(CandidateAsset);
                result.BaselineImported = baseline != null;
                result.CandidateImported = candidate != null;
                if (baseline == null)
                    result.Diagnostics.Add(new Diagnostic {
                        Side = "baseline", Severity = "error", Line = 0U,
                        File = BaselineAsset,
                        Message = "ShaderImporter produced no Shader asset",
                        Platform = "editor", Details = String.Empty
                    });
                else
                    CollectShaderDiagnostics(result, baseline, "baseline");
                if (candidate == null)
                    result.Diagnostics.Add(new Diagnostic {
                        Side = "candidate", Severity = "error", Line = 0U,
                        File = CandidateAsset,
                        Message = "ShaderImporter produced no Shader asset",
                        Platform = "editor", Details = String.Empty
                    });
                else
                    CollectShaderDiagnostics(result, candidate, "candidate");

                bool actionableDiagnostic = result.Diagnostics.Exists(value =>
                    value.Severity == "error" || value.Severity == "warning");
                if (!actionableDiagnostic && result.Failures.Count == 0 &&
                    baseline != null && candidate != null &&
                    ValidatePropertyClosure(result, fixture, baseline, candidate))
                {
                    if (fixture.PassIndex >= baseline.passCount ||
                        fixture.PassIndex >= candidate.passCount)
                    {
                        AddFailure(result, "pass-index",
                            "fixture pass is outside baseline or candidate pass count");
                    }
                    else
                    {
                        var textures = new List<Texture2D>();
                        Mesh mesh = null;
                        Material baselineMaterial = null;
                        Material candidateMaterial = null;
                        try
                        {
                            mesh = CreateCanonicalMesh();
                            baselineMaterial = CreateMaterial(baseline, fixture,
                                                              textures);
                            candidateMaterial = CreateMaterial(candidate, fixture,
                                                               textures);
                            byte[] baselineFirst = Capture(baselineMaterial,
                                fixture, mesh, textures);
                            byte[] candidateFirst = Capture(candidateMaterial,
                                fixture, mesh, textures);
                            byte[] baselineSecond = Capture(baselineMaterial,
                                fixture, mesh, textures);
                            byte[] candidateSecond = Capture(candidateMaterial,
                                fixture, mesh, textures);
                            File.WriteAllBytes(baselinePixelsPath, baselineFirst);
                            File.WriteAllBytes(candidatePixelsPath, candidateFirst);
                            result.BaselinePixelsSha = Sha256(baselineFirst);
                            result.CandidatePixelsSha = Sha256(candidateFirst);
                            result.BaselineStable = FirstMismatch(
                                baselineFirst, baselineSecond) == UInt64.MaxValue;
                            result.CandidateStable = FirstMismatch(
                                candidateFirst, candidateSecond) == UInt64.MaxValue;
                            result.FirstMismatchOffset = FirstMismatch(
                                baselineFirst, candidateFirst);
                            result.PixelEqual = result.FirstMismatchOffset ==
                                UInt64.MaxValue;
                        }
                        finally
                        {
                            if (baselineMaterial != null)
                                UnityEngine.Object.DestroyImmediate(baselineMaterial);
                            if (candidateMaterial != null)
                                UnityEngine.Object.DestroyImmediate(candidateMaterial);
                            if (mesh != null)
                                UnityEngine.Object.DestroyImmediate(mesh);
                            foreach (Texture2D texture in textures)
                                if (texture != null)
                                    UnityEngine.Object.DestroyImmediate(texture);
                        }
                    }
                }

                actionableDiagnostic = result.Diagnostics.Exists(value =>
                    value.Severity == "error" || value.Severity == "warning");
                if (actionableDiagnostic || !result.BaselineImported ||
                    !result.CandidateImported)
                    result.Status = "diagnostics-found";
                else if (result.Failures.Count != 0)
                    result.Status = "unsupported-fixture";
                else if (!result.BaselineStable || !result.CandidateStable)
                    result.Status = "nondeterministic";
                else if (!result.PixelEqual)
                    result.Status = "pixel-mismatch";
                else
                {
                    result.Status = "ok";
                    exitCode = 0;
                }
            }
            catch (Exception exception)
            {
                AddFailure(result, "bridge-exception",
                    exception.GetType().FullName + ": " + exception.Message);
                result.Status = "unsupported-fixture";
            }
            finally
            {
                if (resultPath != null)
                {
                    try { WriteResult(resultPath, result); }
                    catch (Exception exception)
                    {
                        Debug.LogException(exception);
                        exitCode = 1;
                    }
                }
                else
                {
                    exitCode = 1;
                }
                EditorApplication.Exit(exitCode);
            }
        }
    }
}
