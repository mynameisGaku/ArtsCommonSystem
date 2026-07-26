using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
using IOPath = System.IO.Path;
using ShapePath = System.Windows.Shapes.Path;

namespace AcsEditor;

public partial class MaterialEditorWindow
{
    private const int SlabScalarCount = 39;
    private const double GraphNodeWidth = 204.0;
    private const double GraphNodeHeight = 112.0;
    private const double GraphOutputWidth = 190.0;
    private const double GraphOutputHeight = 88.0;

    private sealed class GraphNode
    {
        public string StableId { get; set; } = Guid.NewGuid().ToString("N");
        public int Type { get; set; }
        public int InputA { get; set; } = -1;
        public int InputB { get; set; } = -1;
        public float Factor { get; set; } = 0.5f;
        public uint Flags { get; set; }
        public float[] Slab { get; set; } = new float[SlabScalarCount];
        public int[] ExpressionRoots { get; set; } =
            Enumerable.Repeat(-1, SlabScalarCount).ToArray();
        public bool IsExpanded { get; set; }
        public double X { get; set; }
        public double Y { get; set; }
    }

    private sealed record PaletteEntry(
        string Category, string Name, int Type, uint Flags, string Description,
        int ExpressionOp = -1)
    {
        public override string ToString() => $"{Category}   {Name}";
    }

    private sealed record ParameterEntry(
        string Name, string Value, int ScalarIndex = -1, int ExpressionIndex = -1);

    private sealed class GraphLayout
    {
        public int Version { get; set; } = 3;
        public double Zoom { get; set; } = 1.0;
        public double HorizontalOffset { get; set; }
        public double VerticalOffset { get; set; }
        public double OutputX { get; set; } = 900;
        public double OutputY { get; set; } = 430;
        public string? SelectedNodeId { get; set; }
        public string? SelectedExpressionId { get; set; }
        public List<GraphNodeLayout> Nodes { get; set; } = new();
        public List<ExpressionNodeLayout> Expressions { get; set; } = new();
    }

    private sealed class GraphNodeLayout
    {
        public int RuntimeIndex { get; set; }
        public string StableId { get; set; } = "";
        public double X { get; set; }
        public double Y { get; set; }
        public bool IsExpanded { get; set; }
    }

    private static readonly PaletteEntry[] AllPaletteEntries =
    {
        new("BSDF", "Substrate Slab", 0, 0u << 8,
            "Full physically based Substrate slab closure."),
        new("BUILDING BLOCKS", "Simple Clear Coat", 0, 1u << 8,
            "A production-ready coated dielectric slab."),
        new("BUILDING BLOCKS", "Unlit", 0, 2u << 8,
            "Emissive-only closure that is unaffected by scene lighting."),
        new("BUILDING BLOCKS", "Single Layer Water", 0, 3u << 8,
            "Thin participating water surface with transmittance."),
        new("OPERATORS", "Coverage Weight", 1, 0,
            "Scales the coverage of one material closure."),
        new("OPERATORS", "Horizontal Blend", 2, 0,
            "Mixes two closures side by side using a weight."),
        new("OPERATORS", "Vertical Layer", 3, 0,
            "Places top input A over bottom input B using a physical top thickness."),
        new("OPERATORS", "Add", 4, 0,
            "Adds the energy of two material closures."),
        new("OPERATORS", "Select", 5, 0,
            "Selects input A or B from a scalar selector."),
        new("VALUE", "Constant", 0, 0,
            "A typed scalar or vector literal.", 0),
        new("PARAMETERS", "Scalar Parameter", 0, 0,
            "A named Float1 parameter with a stable override-ready ID.", 1),
        new("PARAMETERS", "Vector Parameter", 0, 0,
            "A named Float2, Float3, or Float4 parameter with a stable ID.", 2),
        new("TEXTURES", "Texture Sample 2D", 0, 0,
            "Samples one of four material texture slots from a Float2 UV input.", 3),
        new("COORDINATES", "UV0", 0, 0,
            "Primary mesh texture coordinates (Float2).", 4),
        new("COORDINATES", "Time", 0, 0,
            "Material time in seconds (Float1).", 5),
        new("COORDINATES", "World Position", 0, 0,
            "Shaded world position (Float3).", 6),
        new("COORDINATES", "World Normal", 0, 0,
            "Shaded world normal (Float3).", 7),
        new("MATH", "Add", 0, 0,
            "Adds two values with scalar splat support.", 8),
        new("MATH", "Multiply", 0, 0,
            "Multiplies two values with scalar splat support.", 9),
        new("MATH", "Lerp", 0, 0,
            "Interpolates A and B by Alpha.", 10),
        new("MATH", "Clamp", 0, 0,
            "Clamps Value between Min and Max.", 11),
        new("MATH", "Power", 0, 0,
            "Raises Base to Exponent.", 12),
        new("MATH", "Dot", 0, 0,
            "Dot product of two equal-width vectors, returning Float1.", 13),
        new("MATH", "Normalize", 0, 0,
            "Normalizes a scalar or vector value.", 14),
        new("MATH", "Noise", 0, 0,
            "Deterministic procedural noise returning Float1.", 15),
        new("MATH", "Component", 0, 0,
            "Extracts one x/y/z/w (r/g/b/a) lane as Float1.", 16)
    };

    private static readonly string[] SlabPropertyNames =
    {
        "Diffuse R", "Diffuse G", "Diffuse B",
        "F0 R", "F0 G", "F0 B",
        "F90 R", "F90 G", "F90 B",
        "Roughness", "Second Roughness", "Second Lobe Weight", "Anisotropy",
        "Tangent X", "Tangent Y", "Tangent Z",
        "SSS Mean Free Path R (cm)", "SSS Mean Free Path G (cm)", "SSS Mean Free Path B (cm)",
        "SSS Phase",
        "Emissive R", "Emissive G", "Emissive B",
        "Transmittance R", "Transmittance G", "Transmittance B",
        "Thickness (cm)",
        "Fuzz R", "Fuzz G", "Fuzz B", "Fuzz Amount", "Fuzz Roughness",
        "Thin Film Weight", "Thin Film Thickness (nm)", "Thin Film IOR",
        "Normal X", "Normal Y", "Normal Z", "Normal Strength"
    };

    private readonly List<GraphNode> _graphNodes = new();
    private readonly ObservableCollection<ParameterEntry> _parameters = new();
    private int _substrateEnabled;
    private int _substrateRoot = -1;
    private int _runtimeMaxNodes = 32;
    private int _selectedNode = -1;
    private int _wireSource = -1;
    private int _dragNode = -1;
    private bool _draggingNode;
    private bool _draggingOutput;
    private bool _panningGraph;
    private bool _graphUiSync;
    private bool _graphDirty;
    private bool _suppressClosePromptForAutomation;
    private bool _loadedGraphLayout;
    private Point _dragAnchor;
    private Point _nodeDragOrigin;
    private Point _outputDragOrigin;
    private Point _panAnchor;
    private double _panHorizontalOrigin;
    private double _panVerticalOrigin;
    private Point _wireMouse;
    private double _graphZoom = 0.9;
    private double _outputX = 900;
    private double _outputY = 430;
    private Point _paletteDragStart;
    private PaletteEntry? _dragPaletteEntry;

    private string GraphLayoutPath => _path + ".graph.json";

    private void InitializeGraphEditor()
    {
        ParameterList.ItemsSource = _parameters;
        PopulatePalette("");
        LoadSubstrateGraph();
        LoadExpressionGraph();
        LoadGraphLayout();
        UpdateGraphZoom();
        RenderGraph();
        if (ValidExpressionIndex(_selectedExpression))
            SelectExpressionNode(_selectedExpression);
        else
            SelectGraphNode(_selectedNode);
        CompileGraph(userInitiated: false);
        if (!_loadedGraphLayout)
            Loaded += (_, _) => OnFitGraphClicked(this, new RoutedEventArgs());
    }

    private void PopulatePalette(string query)
    {
        string q = (query ?? "").Trim();
        PaletteList.Items.Clear();
        foreach (PaletteEntry entry in AllPaletteEntries)
        {
            if (q.Length == 0 ||
                entry.Name.Contains(q, StringComparison.OrdinalIgnoreCase) ||
                entry.Category.Contains(q, StringComparison.OrdinalIgnoreCase) ||
                entry.Description.Contains(q, StringComparison.OrdinalIgnoreCase))
            {
                PaletteList.Items.Add(entry);
            }
        }
        if (PaletteList.Items.Count > 0) PaletteList.SelectedIndex = 0;
    }

    private void LoadSubstrateGraph()
    {
        _graphNodes.Clear();
        try
        {
            _runtimeMaxNodes = Math.Max(1, EngineInterop.acs_editor_material_substrate_max_nodes());
            int runtimeSlabScalars = EngineInterop.acs_editor_material_substrate_slab_scalar_count();
            if (runtimeSlabScalars != SlabScalarCount)
                throw new InvalidOperationException(
                    $"Editor expects {SlabScalarCount} Slab values, runtime reports {runtimeSlabScalars}.");
            if (EngineInterop.acs_editor_material_substrate_get_header(
                    _path, out _substrateEnabled, out _substrateRoot, out int count) != 0)
            {
                count = Math.Clamp(count, 0, _runtimeMaxNodes);
                for (int i = 0; i < count; ++i)
                {
                    var slab = new float[SlabScalarCount];
                    if (EngineInterop.acs_editor_material_substrate_get_node(
                            _path, i, out int type, out int a, out int b, out float factor,
                            out uint flags, slab) == 0)
                    {
                        continue;
                    }
                    _graphNodes.Add(new GraphNode
                    {
                        Type = Math.Clamp(type, 0, 5),
                        InputA = a,
                        InputB = b,
                        Factor = float.IsFinite(factor) ? factor : 0.5f,
                        Flags = flags,
                        Slab = slab,
                        X = 60 + (i % 5) * 240,
                        Y = 180 + (i / 5) * 170
                    });
                }
            }
        }
        catch (EntryPointNotFoundException)
        {
            _substrateEnabled = 0;
            _substrateRoot = -1;
            DiagnosticsList.Items.Add("WARNING  Substrate runtime ABI is not present in the loaded editor DLL.");
        }
        catch (DllNotFoundException)
        {
            _substrateEnabled = 0;
            _substrateRoot = -1;
            DiagnosticsList.Items.Add("WARNING  Editor runtime DLL was not found.");
        }
        catch (InvalidOperationException ex)
        {
            _substrateEnabled = 0;
            _substrateRoot = -1;
            DiagnosticsList.Items.Add($"ERROR  {ex.Message}");
        }

        if (_graphNodes.Count == 0)
        {
            _graphNodes.Add(new GraphNode
            {
                Type = 0,
                Flags = 0,
                Slab = BuildLegacySlab(),
                X = 430,
                Y = 390
            });
            _substrateRoot = 0;
            _selectedNode = 0;
            GraphAssetStateText.Text = "Legacy surface preview - Save converts to Substrate";
        }
        else
        {
            _substrateRoot = Math.Clamp(_substrateRoot, 0, _graphNodes.Count - 1);
            _selectedNode = _substrateRoot;
            GraphAssetStateText.Text = _substrateEnabled != 0
                ? "ACSMAT Substrate DAG - legacy compatible"
                : "Legacy surface preview - Save converts to Substrate";
        }
        GraphStatusText.Text = $"{_graphNodes.Count} / {_runtimeMaxNodes} expressions  |  Front Material: {NodeDisplayName(_substrateRoot)}";
    }

    private float[] BuildLegacySlab()
    {
        var s = new float[SlabScalarCount];
        float baseR = P(BcR.Text, 1), baseG = P(BcG.Text, 1), baseB = P(BcB.Text, 1);
        float metallic = Math.Clamp(P(Metallic.Text), 0, 1);
        s[0] = baseR * (1 - metallic); s[1] = baseG * (1 - metallic); s[2] = baseB * (1 - metallic);
        float dielectricF0 = Math.Clamp(P(SpecLvl.Text, 0.5f), 0, 1) * 0.08f;
        s[3] = Lerp(dielectricF0, baseR, metallic);
        s[4] = Lerp(dielectricF0, baseG, metallic);
        s[5] = Lerp(dielectricF0, baseB, metallic);
        float specularTint = Math.Clamp(P(SpecTint.Text), 0, 1);
        s[6] = Lerp(1, baseR, specularTint);
        s[7] = Lerp(1, baseG, specularTint);
        s[8] = Lerp(1, baseB, specularTint);
        s[9] = Math.Clamp(P(Roughness.Text, 0.5f), 0.02f, 1);
        s[10] = s[9];
        s[11] = 0;
        s[12] = Math.Clamp(P(Aniso.Text), -1, 1);
        s[13] = 1; s[14] = 0; s[15] = 0;
        float sss = Math.Clamp(P(Subsurf.Text), 0, 1);
        s[16] = sss * Math.Clamp(P(SsR.Text, 1), 0, 1);
        s[17] = sss * Math.Clamp(P(SsG.Text, 0.3f), 0, 1);
        s[18] = sss * Math.Clamp(P(SsB.Text, 0.2f), 0, 1);
        s[19] = 0;
        s[20] = P(EmR.Text) * P(EmStr.Text);
        s[21] = P(EmG.Text) * P(EmStr.Text);
        s[22] = P(EmB.Text) * P(EmStr.Text);
        s[23] = 0; s[24] = 0; s[25] = 0;
        s[26] = 0.01f;
        s[27] = P(ShR.Text, 1); s[28] = P(ShG.Text, 1); s[29] = P(ShB.Text, 1);
        s[30] = Math.Clamp(P(Sheen.Text), 0, 1);
        s[31] = Math.Clamp(P(SheenRough.Text, 0.3f), 0.02f, 1);
        s[32] = 0; s[33] = 400; s[34] = 1.4f;
        s[35] = 0; s[36] = 0; s[37] = 1; s[38] = Math.Clamp(P(NormalStr.Text, 1), 0, 4);
        return s;
    }

    private static float Lerp(float a, float b, float t) => a + (b - a) * t;

    private float[] BuildPresetSlab(uint flags)
    {
        int mode = (int)((flags >> 8) & 0xF);
        if (mode == 0) return BuildLegacySlab();

        var s = new float[SlabScalarCount];
        s[6] = 1; s[7] = 1; s[8] = 1;
        s[9] = 0.5f; s[10] = 0.5f;
        s[13] = 1;
        s[26] = 0.01f;
        s[27] = 1; s[28] = 1; s[29] = 1; s[31] = 0.5f;
        s[33] = 400; s[34] = 1.4f;
        s[37] = 1; s[38] = 1;

        switch (mode)
        {
            case 1: // Simple Clear Coat
                s[3] = s[4] = s[5] = 0.04f;
                s[9] = s[10] = 0.08f;
                s[23] = s[24] = s[25] = 1;
                s[26] = 0.001f;
                break;
            case 2: // Unlit
                s[9] = s[10] = 1;
                s[20] = Math.Max(0.05f, P(BcR.Text, 1));
                s[21] = Math.Max(0.05f, P(BcG.Text, 1));
                s[22] = Math.Max(0.05f, P(BcB.Text, 1));
                break;
            case 3: // Single Layer Water, IOR 1.333 -> F0 ~= 0.0204
                s[3] = s[4] = s[5] = 0.0204f;
                s[9] = s[10] = 0.08f;
                s[16] = 24.5f; s[17] = 65.0f; s[18] = 200.0f;
                s[23] = 0.96f; s[24] = 0.985f; s[25] = 0.995f;
                s[26] = 1.0f;
                break;
        }
        return s;
    }

    private void LoadGraphLayout()
    {
        if (!File.Exists(GraphLayoutPath)) return;
        try
        {
            GraphLayout? layout = JsonSerializer.Deserialize<GraphLayout>(File.ReadAllText(GraphLayoutPath));
            if (layout == null) return;
            _loadedGraphLayout = true;
            _graphZoom = Math.Clamp(layout.Zoom, 0.35, 2.25);
            _outputX = Math.Clamp(layout.OutputX, 0, GraphCanvas.Width - GraphOutputWidth);
            _outputY = Math.Clamp(layout.OutputY, 0, GraphCanvas.Height - GraphOutputHeight);
            foreach (GraphNodeLayout saved in layout.Nodes)
            {
                if (saved.RuntimeIndex < 0 || saved.RuntimeIndex >= _graphNodes.Count) continue;
                GraphNode node = _graphNodes[saved.RuntimeIndex];
                node.StableId = string.IsNullOrWhiteSpace(saved.StableId)
                    ? node.StableId
                    : saved.StableId;
                node.X = Math.Clamp(saved.X, 0, GraphCanvas.Width - GraphNodeWidth);
                node.Y = Math.Clamp(saved.Y, 0, GraphCanvas.Height - GraphNodeHeight);
                node.IsExpanded = saved.IsExpanded;
            }
            if (!string.IsNullOrWhiteSpace(layout.SelectedNodeId))
            {
                int index = _graphNodes.FindIndex(n => n.StableId == layout.SelectedNodeId);
                if (index >= 0) _selectedNode = index;
            }
            foreach (ExpressionNodeLayout saved in layout.Expressions)
            {
                if (saved.RuntimeIndex < 0 || saved.RuntimeIndex >= _expressionNodes.Count) continue;
                ExpressionNode node = _expressionNodes[saved.RuntimeIndex];
                node.StableId = string.IsNullOrWhiteSpace(saved.StableId)
                    ? node.StableId
                    : saved.StableId;
                node.X = Math.Clamp(saved.X, 0, GraphCanvas.Width - ExpressionNodeWidth);
                node.Y = Math.Clamp(saved.Y, 0, GraphCanvas.Height - ExpressionNodeHeight(node));
                if (!string.IsNullOrWhiteSpace(saved.ParameterName))
                    node.ParameterName = saved.ParameterName;
            }
            if (!string.IsNullOrWhiteSpace(layout.SelectedExpressionId))
            {
                int index = _expressionNodes.FindIndex(
                    n => n.StableId == layout.SelectedExpressionId);
                if (index >= 0)
                {
                    _selectedExpression = index;
                    _selectedNode = -1;
                }
            }
            Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            {
                GraphViewport.ScrollToHorizontalOffset(Math.Max(0, layout.HorizontalOffset));
                GraphViewport.ScrollToVerticalOffset(Math.Max(0, layout.VerticalOffset));
            }));
        }
        catch (Exception ex)
        {
            DiagnosticsList.Items.Add($"WARNING  Layout sidecar ignored: {ex.Message}");
        }
    }

    private void SaveGraphLayout()
    {
        if (!CanAccessAssetPath) return;
        try
        {
            var layout = new GraphLayout
            {
                Zoom = _graphZoom,
                HorizontalOffset = GraphViewport.HorizontalOffset,
                VerticalOffset = GraphViewport.VerticalOffset,
                OutputX = _outputX,
                OutputY = _outputY,
                SelectedNodeId = _selectedNode >= 0 && _selectedNode < _graphNodes.Count
                    ? _graphNodes[_selectedNode].StableId
                    : null,
                SelectedExpressionId =
                    _selectedExpression >= 0 && _selectedExpression < _expressionNodes.Count
                        ? _expressionNodes[_selectedExpression].StableId
                        : null,
                Nodes = _graphNodes.Select((n, i) => new GraphNodeLayout
                {
                    RuntimeIndex = i,
                    StableId = n.StableId,
                    X = n.X,
                    Y = n.Y,
                    IsExpanded = n.IsExpanded
                }).ToList(),
                Expressions = _expressionNodes.Select((n, i) => new ExpressionNodeLayout
                {
                    RuntimeIndex = i,
                    StableId = n.StableId,
                    X = n.X,
                    Y = n.Y,
                    ParameterName = n.ParameterName
                }).ToList()
            };
            string temp = GraphLayoutPath + ".tmp";
            File.WriteAllText(temp, JsonSerializer.Serialize(layout, new JsonSerializerOptions { WriteIndented = true }));
            File.Move(temp, GraphLayoutPath, true);
        }
        catch (Exception ex)
        {
            DiagnosticsList.Items.Add($"WARNING  Could not save graph layout: {ex.Message}");
        }
    }

    private bool SaveRuntimeGraph(bool showDiagnostics)
    {
        if (!CanAccessAssetPath)
        {
            if (showDiagnostics)
                SetDiagnostics(new[] { "ERROR  The material asset is unavailable while its path is changing." });
            return false;
        }
        List<string> local = ValidateGraph();
        if (local.Any(m => m.StartsWith("ERROR", StringComparison.Ordinal)))
        {
            if (showDiagnostics) SetDiagnostics(local);
            StatusText.Text = "Graph has validation errors";
            StatusText.Foreground = (Brush)FindResource("WarnFg");
            return false;
        }

        PackGraph(out int[] types, out int[] inputsA, out int[] inputsB,
                  out float[] factors, out uint[] flags, out float[] slabs);
        int count = _graphNodes.Count;

        try
        {
            bool ok = SaveCombinedRuntimeGraph(
                types, inputsA, inputsB, factors, flags, slabs);
            if (!ok)
            {
                if (showDiagnostics) SetDiagnostics(new[] { "ERROR  Runtime rejected the Substrate graph." });
                StatusText.Text = "Substrate save failed";
                StatusText.Foreground = (Brush)FindResource("WarnFg");
                return false;
            }
            _substrateEnabled = 1;
            _graphDirty = false;
            SaveGraphLayout();
            GraphAssetStateText.Text = "ACSMAT Substrate DAG - saved";
            StatusText.Text = "Saved";
            StatusText.Foreground = (Brush)FindResource("OkFg");
            PreviewStateText.Text = "Preview: last applied asset";
            return true;
        }
        catch (EntryPointNotFoundException)
        {
            if (showDiagnostics) SetDiagnostics(new[] { "ERROR  The loaded editor DLL does not expose the Substrate graph ABI." });
            return false;
        }
    }

    private void PackGraph(out int[] types, out int[] inputsA, out int[] inputsB,
                           out float[] factors, out uint[] flags, out float[] slabs)
    {
        int count = _graphNodes.Count;
        types = new int[count];
        inputsA = new int[count];
        inputsB = new int[count];
        factors = new float[count];
        flags = new uint[count];
        slabs = new float[count * SlabScalarCount];
        for (int i = 0; i < count; ++i)
        {
            GraphNode n = _graphNodes[i];
            types[i] = n.Type;
            inputsA[i] = n.InputA;
            inputsB[i] = n.InputB;
            factors[i] = float.IsFinite(n.Factor) ? n.Factor : 0;
            flags[i] = n.Flags;
            if (n.Slab.Length == SlabScalarCount)
                Array.Copy(n.Slab, 0, slabs, i * SlabScalarCount, SlabScalarCount);
        }
    }

    private List<string> ValidateGraph()
    {
        var result = new List<string>();
        if (_graphNodes.Count == 0)
        {
            result.Add("ERROR  Graph has no material expressions.");
            return result;
        }
        if (_graphNodes.Count > _runtimeMaxNodes)
            result.Add($"ERROR  Graph has {_graphNodes.Count} nodes; runtime capacity is {_runtimeMaxNodes}.");
        if (_substrateRoot < 0 || _substrateRoot >= _graphNodes.Count)
            result.Add("ERROR  Front Material is not connected.");

        for (int i = 0; i < _graphNodes.Count; ++i)
        {
            GraphNode n = _graphNodes[i];
            int required = RequiredInputCount(n.Type);
            if (required >= 1 && !ValidNodeIndex(n.InputA))
                result.Add($"ERROR  [{i}] {NodeDisplayName(i)} requires input A.");
            if (required >= 2 && !ValidNodeIndex(n.InputB))
                result.Add($"ERROR  [{i}] {NodeDisplayName(i)} requires input B.");
            if (n.InputA == i || n.InputB == i)
                result.Add($"ERROR  [{i}] {NodeDisplayName(i)} cannot reference itself.");
            if (n.Type == 3 && n.Factor < 0)
                result.Add($"ERROR  [{i}] Vertical Layer thickness cannot be negative.");
            if (n.Type is 1 or 2 or 5 && (n.Factor < 0 || n.Factor > 1))
                result.Add($"ERROR  [{i}] {NodeDisplayName(i)} factor must be between 0 and 1.");
            if (n.Slab.Any(v => !float.IsFinite(v)))
                result.Add($"ERROR  [{i}] Slab contains a non-finite value.");
        }

        var state = new byte[_graphNodes.Count];
        bool Visit(int i)
        {
            if (!ValidNodeIndex(i)) return false;
            if (state[i] == 1) return true;
            if (state[i] == 2) return false;
            state[i] = 1;
            GraphNode n = _graphNodes[i];
            if (ValidNodeIndex(n.InputA) && Visit(n.InputA)) return true;
            if (ValidNodeIndex(n.InputB) && Visit(n.InputB)) return true;
            state[i] = 2;
            return false;
        }
        for (int i = 0; i < _graphNodes.Count; ++i)
        {
            if (Visit(i))
            {
                result.Add("ERROR  Closure graph contains a cycle.");
                break;
            }
        }

        result.AddRange(ValidateExpressionGraph());
        if (result.Count == 0)
            result.Add(
                $"OK  Local graph validation passed ({_graphNodes.Count} closures, " +
                $"{_expressionNodes.Count} shader expressions).");
        return result;
    }

    private bool CompileGraph(bool userInitiated)
    {
        Stopwatch timer = Stopwatch.StartNew();
        List<string> local = ValidateGraph();
        if (local.Any(m => m.StartsWith("ERROR", StringComparison.Ordinal)))
        {
            SetDiagnostics(local);
            return false;
        }
        try
        {
            PackGraph(out int[] types, out int[] inputsA, out int[] inputsB,
                      out float[] factors, out uint[] flags, out float[] slabs);
            int called = EngineInterop.acs_editor_material_substrate_compile_arrays(
                1, _substrateRoot, _graphNodes.Count,
                types, inputsA, inputsB, factors, flags, slabs,
                out int errorCode, out int errorNode, out uint features,
                out int closures, out int complexity, out int bytesPerPixel);
            timer.Stop();
            if (called == 0 || errorCode != 0)
            {
                SetDiagnostics(new[]
                {
                    $"ERROR  Substrate compiler rejected node {errorNode} (code {errorCode}).",
                    "INFO   Double-click the diagnostic node in the graph after fixing its inputs."
                });
                StatClosures.Text = "—";
                StatComplexity.Text = "Failed";
                StatBytesPerPixel.Text = "—";
                StatFeatures.Text = $"0x{features:X}";
                StatusText.Text = "Compile failed";
                StatusText.Foreground = (Brush)FindResource("WarnFg");
                return false;
            }
            bool expressionsOk = CompileExpressionGraph(out List<string> expressionMessages);
            var diagnostics = new List<string>
            {
                $"OK     Compiled in {timer.Elapsed.TotalMilliseconds:0.0} ms.",
                $"INFO   Root {_substrateRoot}: {NodeDisplayName(_substrateRoot)}",
                $"INFO   {closures} closure(s), complexity {complexity}, {bytesPerPixel} bytes/pixel."
            };
            diagnostics.AddRange(expressionMessages);
            SetDiagnostics(diagnostics);
            StatClosures.Text = closures.ToString();
            StatComplexity.Text = ComplexityLabel(complexity);
            StatBytesPerPixel.Text = bytesPerPixel.ToString();
            StatFeatures.Text = FeatureLabel(features);
            StatusText.Text = expressionsOk
                ? userInitiated ? "Compile succeeded" : "Ready"
                : "Expression compile failed";
            StatusText.Foreground = expressionsOk
                ? (Brush)FindResource("OkFg")
                : (Brush)FindResource("WarnFg");
            return expressionsOk;
        }
        catch (EntryPointNotFoundException)
        {
            SetDiagnostics(local.Concat(new[]
            {
                "WARNING  Runtime compiler ABI is unavailable; only local validation ran."
            }));
            StatClosures.Text = "—";
            StatComplexity.Text = "Local";
            StatBytesPerPixel.Text = "—";
            StatFeatures.Text = "ABI pending";
            return false;
        }
    }

    private static string ComplexityLabel(int value) => value switch
    {
        0 => "Simple",
        1 => "Single",
        2 => "Complex",
        3 => "Complex Special",
        _ => $"Unknown ({value})"
    };

    private static string FeatureLabel(uint features)
    {
        if (features == 0) return "Standard";
        string[] names =
        {
            "F90", "Second Roughness", "Anisotropy", "SSS", "Emissive",
            "Transmission", "Fuzz", "Thin Film", "Custom Normal", "Multiple Closure",
            "Vertical Layer", "Non-Physical Add", "Parameter Blend", "Clear Coat",
            "Single Layer Water"
        };
        var active = new List<string>();
        for (int bit = 0; bit < names.Length; ++bit)
            if ((features & (1u << bit)) != 0) active.Add(names[bit]);
        uint knownMask = (1u << names.Length) - 1u;
        if ((features & ~knownMask) != 0) active.Add($"0x{features & ~knownMask:X}");
        return string.Join(", ", active);
    }

    private void SetDiagnostics(IEnumerable<string> messages)
    {
        DiagnosticsList.Items.Clear();
        foreach (string message in messages)
        {
            Brush foreground = message.StartsWith("ERROR", StringComparison.Ordinal)
                ? (Brush)FindResource("WarnFg")
                : message.StartsWith("OK", StringComparison.Ordinal)
                    ? (Brush)FindResource("OkFg")
                    : message.StartsWith("WARNING", StringComparison.Ordinal)
                        ? new SolidColorBrush(Color.FromRgb(221, 177, 91))
                        : (Brush)FindResource("TextDim");
            DiagnosticsList.Items.Add(new TextBlock
            {
                Text = message,
                Foreground = foreground,
                FontFamily = message.StartsWith("INFO", StringComparison.Ordinal)
                    ? new FontFamily("Consolas")
                    : new FontFamily("Segoe UI"),
                FontSize = 10.5
            });
        }
    }

    private void OnDiagnosticDoubleClick(object sender, MouseButtonEventArgs e)
    {
        string? message = DiagnosticsList.SelectedItem switch
        {
            string text => text,
            TextBlock block => block.Text,
            _ => null
        };
        if (message == null) return;
        Match expressionMatch = Regex.Match(
            message, @"\[expr\s+(?<expr>\d+)\]",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        if (expressionMatch.Success &&
            int.TryParse(expressionMatch.Groups["expr"].Value, out int expressionIndex) &&
            ValidExpressionIndex(expressionIndex))
        {
            SelectExpressionNode(expressionIndex);
            ExpressionNode expression = _expressionNodes[expressionIndex];
            GraphViewport.ScrollToHorizontalOffset(Math.Max(
                0, (expression.X + ExpressionNodeWidth * 0.5) * _graphZoom -
                   GraphViewport.ViewportWidth * 0.5));
            GraphViewport.ScrollToVerticalOffset(Math.Max(
                0, (expression.Y + ExpressionNodeHeight(expression) * 0.5) * _graphZoom -
                   GraphViewport.ViewportHeight * 0.5));
            return;
        }
        Match match = Regex.Match(message, @"(?:\[(?<index>\d+)\]|node\s+(?<node>\d+))",
                                  RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        string value = match.Groups["index"].Success
            ? match.Groups["index"].Value
            : match.Groups["node"].Value;
        if (!int.TryParse(value, out int index) || !ValidNodeIndex(index)) return;
        SelectGraphNode(index);
        GraphNode node = _graphNodes[index];
        GraphViewport.ScrollToHorizontalOffset(Math.Max(
            0, (node.X + GraphNodeWidth * 0.5) * _graphZoom - GraphViewport.ViewportWidth * 0.5));
        GraphViewport.ScrollToVerticalOffset(Math.Max(
            0, (node.Y + GraphNodeHeight * 0.5) * _graphZoom - GraphViewport.ViewportHeight * 0.5));
    }

    private void MarkGraphDirty()
    {
        _graphDirty = true;
        GraphAssetStateText.Text = "ACSMAT Substrate DAG - unsaved";
        PreviewStateText.Text = "Preview: last applied asset";
        StatusText.Text = "Modified";
        StatusText.Foreground = (Brush)FindResource("InfoFg");
        UpdateGraphStatus();
    }

    private bool ValidNodeIndex(int index) => index >= 0 && index < _graphNodes.Count;

    private static int RequiredInputCount(int type) => type switch
    {
        1 => 1,
        2 or 3 or 4 or 5 => 2,
        _ => 0
    };

    private static string TypeName(int type) => type switch
    {
        0 => "Substrate Slab",
        1 => "Coverage Weight",
        2 => "Horizontal Blend",
        3 => "Vertical Layer",
        4 => "Add",
        5 => "Select",
        _ => "Unknown"
    };

    private string NodeDisplayName(int index)
    {
        if (!ValidNodeIndex(index)) return "None";
        GraphNode n = _graphNodes[index];
        if (n.Type != 0) return TypeName(n.Type);
        int mode = (int)((n.Flags >> 8) & 0xF);
        return mode switch
        {
            1 => "Simple Clear Coat",
            2 => "Unlit",
            3 => "Single Layer Water",
            _ => "Substrate Slab"
        };
    }

    private static Brush NodeHeaderBrush(GraphNode n)
    {
        if (n.Type == 0)
        {
            int mode = (int)((n.Flags >> 8) & 0xF);
            return new SolidColorBrush(mode switch
            {
                1 => Color.FromRgb(56, 115, 137),
                2 => Color.FromRgb(76, 95, 124),
                3 => Color.FromRgb(41, 113, 142),
                _ => Color.FromRgb(55, 91, 137)
            });
        }
        return new SolidColorBrush(n.Type switch
        {
            1 => Color.FromRgb(92, 112, 69),
            2 => Color.FromRgb(94, 70, 128),
            3 => Color.FromRgb(142, 91, 45),
            4 => Color.FromRgb(45, 116, 111),
            5 => Color.FromRgb(122, 66, 111),
            _ => Color.FromRgb(80, 84, 91)
        });
    }

    private void RenderGraph()
    {
        GraphCanvas.Children.Clear();

        // Wires are deliberately rendered first so selected node cards and sockets remain crisp.
        RenderExpressionWires();
        for (int dest = 0; dest < _graphNodes.Count; ++dest)
        {
            GraphNode n = _graphNodes[dest];
            if (ValidNodeIndex(n.InputA))
                AddWire(OutputPoint(n.InputA), InputPoint(dest, 0), "#FF7F9FC1", 2.2);
            if (ValidNodeIndex(n.InputB))
                AddWire(OutputPoint(n.InputB), InputPoint(dest, 1), "#FFB58AC8", 2.2);
        }
        if (ValidNodeIndex(_substrateRoot))
            AddWire(OutputPoint(_substrateRoot), new Point(_outputX, _outputY + 56), "#FF79C18A", 2.8);

        RenderExpressionNodes();
        for (int i = 0; i < _graphNodes.Count; ++i) AddNodeVisual(i);
        AddFrontMaterialVisual();

        if (ValidNodeIndex(_wireSource))
            AddWire(OutputPoint(_wireSource), _wireMouse, "#FFF2C76A", 2.0, dashed: true);
    }

    private void AddWire(Point start, Point end, string color, double thickness, bool dashed = false)
    {
        double tangent = Math.Max(54, Math.Abs(end.X - start.X) * 0.45);
        var figure = new PathFigure { StartPoint = start, IsClosed = false };
        figure.Segments.Add(new BezierSegment(
            new Point(start.X + tangent, start.Y),
            new Point(end.X - tangent, end.Y),
            end, true));
        var path = new ShapePath
        {
            Data = new PathGeometry(new[] { figure }),
            Stroke = (Brush)new BrushConverter().ConvertFromString(color)!,
            StrokeThickness = thickness,
            IsHitTestVisible = false,
            SnapsToDevicePixels = true
        };
        if (dashed) path.StrokeDashArray = new DoubleCollection { 4, 3 };
        GraphCanvas.Children.Add(path);
    }

    private void AddNodeVisual(int index)
    {
        GraphNode n = _graphNodes[index];
        var grid = new Grid();
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(29) });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

        var header = new Border
        {
            Background = NodeHeaderBrush(n),
            Padding = new Thickness(9, 0, 7, 0),
            Child = new Grid()
        };
        var headerGrid = (Grid)header.Child;
        headerGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        headerGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        headerGrid.Children.Add(new TextBlock
        {
            Text = NodeDisplayName(index),
            Foreground = Brushes.White,
            FontWeight = FontWeights.SemiBold,
            VerticalAlignment = VerticalAlignment.Center
        });
        var indexText = new TextBlock
        {
            Text = n.Type == 0
                ? $"{(n.IsExpanded ? "▾" : "▸")}  #{index}"
                : $"#{index}",
            Foreground = new SolidColorBrush(Color.FromArgb(190, 255, 255, 255)),
            FontFamily = new FontFamily("Consolas"),
            FontSize = 10,
            VerticalAlignment = VerticalAlignment.Center
        };
        Grid.SetColumn(indexText, 1);
        headerGrid.Children.Add(indexText);
        grid.Children.Add(header);

        var body = new StackPanel { Margin = new Thickness(10, 8, 10, 6) };
        Grid.SetRow(body, 1);
        if (n.Type == 0)
        {
            if (n.IsExpanded)
            {
                body.Margin = new Thickness(12, 7, 8, 4);
                for (int scalar = 0; scalar < SlabScalarCount; ++scalar)
                {
                    int expression = n.ExpressionRoots[scalar];
                    var row = NodeBodyText(
                        $"{SlabPropertyNames[scalar]}   " +
                        (ValidExpressionIndex(expression)
                            ? $"← {SlabBindingDisplay(expression, scalar)}"
                            : n.Slab[scalar].ToString("0.###")));
                    row.Height = 18;
                    row.Foreground = ValidExpressionIndex(expression)
                        ? ExpressionTypeBrush(ExpressionValueTypeFloat1)
                        : new SolidColorBrush(Color.FromRgb(173, 180, 190));
                    body.Children.Add(row);
                }
            }
            else
            {
                body.Children.Add(NodeBodyText($"Roughness  {n.Slab[9]:0.###}"));
                body.Children.Add(NodeBodyText(
                    $"F0  {n.Slab[3]:0.###}, {n.Slab[4]:0.###}, {n.Slab[5]:0.###}"));
                body.Children.Add(NodeBodyText("Double-click title to expose 39 scalar inputs"));
            }
        }
        else
        {
            body.Children.Add(NodeBodyText($"A   {NodeDisplayName(n.InputA)}"));
            if (RequiredInputCount(n.Type) > 1)
                body.Children.Add(NodeBodyText($"B   {NodeDisplayName(n.InputB)}"));
            string factorLabel = n.Type == 3 ? "Top Thickness" : n.Type == 5 ? "Selector" : "Weight";
            if (n.Type != 4) body.Children.Add(NodeBodyText($"{factorLabel}   {n.Factor:0.###}"));
        }
        grid.Children.Add(body);

        var outputLabel = new TextBlock
        {
            Text = "Material",
            Foreground = new SolidColorBrush(Color.FromRgb(150, 187, 159)),
            FontSize = 10,
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Bottom,
            Margin = new Thickness(0, 0, 12, 6)
        };
        Grid.SetRow(outputLabel, 2);
        grid.Children.Add(outputLabel);

        var border = new Border
        {
            Width = GraphNodeWidthFor(n),
            Height = GraphNodeHeightFor(n),
            Background = new SolidColorBrush(Color.FromRgb(31, 35, 42)),
            BorderBrush = index == _selectedNode
                ? (Brush)FindResource("Accent")
                : new SolidColorBrush(Color.FromRgb(63, 70, 81)),
            BorderThickness = new Thickness(index == _selectedNode ? 2 : 1),
            CornerRadius = new CornerRadius(2),
            Child = grid,
            Effect = new System.Windows.Media.Effects.DropShadowEffect
            {
                Color = Colors.Black,
                BlurRadius = 8,
                Opacity = 0.38,
                ShadowDepth = 2
            }
        };
        Canvas.SetLeft(border, n.X);
        Canvas.SetTop(border, n.Y);
        GraphCanvas.Children.Add(border);

        if (RequiredInputCount(n.Type) >= 1)
            AddSocket(InputPoint(index, 0), "#FF80A3C5");
        if (RequiredInputCount(n.Type) >= 2)
            AddSocket(InputPoint(index, 1), "#FFB68DCA");
        if (n.Type == 0 && n.IsExpanded)
            for (int scalar = 0; scalar < SlabScalarCount; ++scalar)
            {
                int expression = n.ExpressionRoots[scalar];
                AddSocket(SlabScalarInputPoint(index, scalar),
                    ExpressionTypeColor(ValidExpressionIndex(expression)
                        ? InferExpressionType(expression)
                        : SlabScalarLane(scalar) + 1), 4.5);
            }
        AddSocket(OutputPoint(index), "#FF78BF88");
    }

    private static TextBlock NodeBodyText(string text) => new()
    {
        Text = text,
        Foreground = new SolidColorBrush(Color.FromRgb(173, 180, 190)),
        FontSize = 10.5,
        Margin = new Thickness(0, 0, 0, 3),
        TextTrimming = TextTrimming.CharacterEllipsis
    };

    private void AddFrontMaterialVisual()
    {
        var grid = new Grid();
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(31) });
        grid.RowDefinitions.Add(new RowDefinition());
        var header = new Border
        {
            Background = new SolidColorBrush(Color.FromRgb(52, 119, 72)),
            Padding = new Thickness(10, 0, 0, 0),
            Child = new TextBlock
            {
                Text = "Front Material",
                Foreground = Brushes.White,
                FontWeight = FontWeights.SemiBold,
                VerticalAlignment = VerticalAlignment.Center
            }
        };
        grid.Children.Add(header);
        var text = new TextBlock
        {
            Text = ValidNodeIndex(_substrateRoot)
                ? $"Surface  <-  #{_substrateRoot} {NodeDisplayName(_substrateRoot)}"
                : "Surface  <-  Connect a closure",
            Foreground = new SolidColorBrush(Color.FromRgb(181, 194, 184)),
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(13, 0, 8, 0),
            TextTrimming = TextTrimming.CharacterEllipsis
        };
        Grid.SetRow(text, 1);
        grid.Children.Add(text);
        var border = new Border
        {
            Width = GraphOutputWidth,
            Height = GraphOutputHeight,
            Background = new SolidColorBrush(Color.FromRgb(31, 38, 34)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(73, 115, 82)),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(2),
            Child = grid
        };
        Canvas.SetLeft(border, _outputX);
        Canvas.SetTop(border, _outputY);
        GraphCanvas.Children.Add(border);
        AddSocket(new Point(_outputX, _outputY + 56), "#FF78BF88", 7);
    }

    private void AddSocket(Point point, string color, double radius = 6)
    {
        var ellipse = new Ellipse
        {
            Width = radius * 2,
            Height = radius * 2,
            Fill = new SolidColorBrush(Color.FromRgb(24, 27, 32)),
            Stroke = (Brush)new BrushConverter().ConvertFromString(color)!,
            StrokeThickness = 2,
            IsHitTestVisible = false
        };
        Canvas.SetLeft(ellipse, point.X - radius);
        Canvas.SetTop(ellipse, point.Y - radius);
        GraphCanvas.Children.Add(ellipse);
    }

    private Point OutputPoint(int index)
    {
        GraphNode n = _graphNodes[index];
        return new Point(
            n.X + GraphNodeWidthFor(n),
            n.Y + GraphNodeHeightFor(n) - 24);
    }

    private Point InputPoint(int index, int input)
    {
        GraphNode n = _graphNodes[index];
        return new Point(n.X, n.Y + (input == 0 ? 57 : 80));
    }

    private static double GraphNodeWidthFor(GraphNode node) =>
        node.Type == 0 && node.IsExpanded ? 286 : GraphNodeWidth;

    private static double GraphNodeHeightFor(GraphNode node) =>
        node.Type == 0 && node.IsExpanded
            ? 29 + 14 + SlabScalarCount * 18 + 30
            : GraphNodeHeight;

    private Point SlabScalarInputPoint(int closureIndex, int scalar)
    {
        GraphNode node = _graphNodes[closureIndex];
        return new Point(node.X, node.Y + 44 + scalar * 18);
    }

    private static bool Near(Point a, Point b, double radius = 13) =>
        Math.Abs(a.X - b.X) <= radius && Math.Abs(a.Y - b.Y) <= radius;

    private int NodeAt(Point p)
    {
        for (int i = _graphNodes.Count - 1; i >= 0; --i)
        {
            GraphNode n = _graphNodes[i];
            if (p.X >= n.X && p.X <= n.X + GraphNodeWidthFor(n) &&
                p.Y >= n.Y && p.Y <= n.Y + GraphNodeHeightFor(n))
                return i;
        }
        return -1;
    }

    private void SelectGraphNode(int index)
    {
        _selectedNode = ValidNodeIndex(index) ? index : -1;
        _selectedExpression = -1;
        _graphUiSync = true;
        PbrPanel.Visibility = Visibility.Collapsed;
        SelectedSlabPanel.Visibility = Visibility.Collapsed;
        SelectedOperatorPanel.Visibility = Visibility.Collapsed;
        SelectedExpressionPanel.Visibility = Visibility.Collapsed;

        if (!ValidNodeIndex(_selectedNode))
        {
            PbrPanel.Visibility = _kind == 0 ? Visibility.Visible : Visibility.Collapsed;
            NodeDetailsTitle.Text = "Material";
            NodeDetailsText.Text = "Select an expression to edit it.";
            BuildLiveParameterList();
        }
        else
        {
            GraphNode n = _graphNodes[_selectedNode];
            NodeDetailsTitle.Text = $"#{_selectedNode}  {NodeDisplayName(_selectedNode)}";
            NodeDetailsText.Text = n.Type == 0
                ? "A Substrate BSDF slab. Its physical closure properties are compiled into the Front Material."
                : n.Type == 3
                    ? "Layers top input A over bottom input B. Factor is physical top-layer thickness in centimeters."
                    : $"{TypeName(n.Type)} combines closure inputs without falling back to legacy PBR.";
            if (n.Type == 0)
            {
                SelectedSlabPanel.Visibility = Visibility.Visible;
                RebuildSlabPropertyPanel(n);
            }
            else
            {
                SelectedOperatorPanel.Visibility = Visibility.Visible;
                OperatorInputAText.Text = NodeDisplayName(n.InputA);
                OperatorInputBText.Text = RequiredInputCount(n.Type) > 1 ? NodeDisplayName(n.InputB) : "Not used";
                OperatorFactorSlider.IsEnabled = n.Type != 4;
                OperatorFactorBox.IsEnabled = n.Type != 4;
                OperatorFactorSlider.Minimum = 0;
                OperatorFactorSlider.Maximum = n.Type == 3 ? Math.Max(10, n.Factor) : 1;
                OperatorFactorSlider.Value = Math.Clamp(n.Factor, 0, (float)OperatorFactorSlider.Maximum);
                OperatorFactorBox.Text = F(n.Factor);
                OperatorUseParameter.IsChecked = (n.Flags & 1u) != 0;
                BuildParameterList(n);
            }
        }
        _graphUiSync = false;
        RenderGraph();
    }

    private void RebuildSlabPropertyPanel(GraphNode node)
    {
        SlabPropertyPanel.Children.Clear();
        var modeRow = new Grid { Margin = new Thickness(0, 2, 0, 5) };
        modeRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(102) });
        modeRow.ColumnDefinitions.Add(new ColumnDefinition());
        modeRow.Children.Add(new TextBlock
        {
            Text = "Building Block",
            Style = (Style)FindResource("MaterialFieldLabel")
        });
        var modeBox = new ComboBox { SelectedIndex = Math.Clamp((int)((node.Flags >> 8) & 0xF), 0, 3) };
        modeBox.Items.Add("Standard Slab");
        modeBox.Items.Add("Simple Clear Coat");
        modeBox.Items.Add("Unlit");
        modeBox.Items.Add("Single Layer Water");
        modeBox.SelectionChanged += OnSlabModeChanged;
        Grid.SetColumn(modeBox, 1);
        modeRow.Children.Add(modeBox);
        SlabPropertyPanel.Children.Add(modeRow);

        for (int i = 0; i < SlabScalarCount; ++i)
        {
            if (i is 0 or 3 or 6 or 9 or 13 or 16 or 20 or 23 or 27 or 32 or 35)
            {
                string group = i switch
                {
                    0 => "Diffuse",
                    3 => "Specular",
                    6 => "Grazing",
                    9 => "Microfacet Lobes",
                    13 => "Anisotropy",
                    16 => "Subsurface",
                    20 => "Emission",
                    23 => "Transmission",
                    27 => "Fuzz",
                    32 => "Thin Film",
                    _ => "Normal"
                };
                var groupRow = new Grid { Margin = new Thickness(0, 8, 0, 3) };
                groupRow.ColumnDefinitions.Add(new ColumnDefinition());
                groupRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                groupRow.Children.Add(new TextBlock
                {
                    Text = group.ToUpperInvariant(),
                    Foreground = (Brush)FindResource("SectionFg"),
                    FontWeight = FontWeights.SemiBold,
                    FontSize = 9.5,
                    VerticalAlignment = VerticalAlignment.Center
                });
                if (i is 0 or 3 or 6 or 23 or 27)
                {
                    var pick = new Button
                    {
                        Content = "Pick Color",
                        Tag = i,
                        Padding = new Thickness(6, 1, 6, 1),
                        FontSize = 9.5,
                        ToolTip = $"Edit {group} RGB values"
                    };
                    pick.Click += OnSlabColorClicked;
                    Grid.SetColumn(pick, 1);
                    groupRow.Children.Add(pick);
                }
                SlabPropertyPanel.Children.Add(groupRow);
            }
            var row = new Grid { Margin = new Thickness(0, 2, 0, 2) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(142) });
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.Children.Add(new TextBlock
            {
                Text = SlabPropertyNames[i],
                Style = (Style)FindResource("MaterialFieldLabel")
            });
            var box = new TextBox
            {
                Text = F(node.Slab[i]),
                Tag = i,
                MinHeight = 24,
                IsEnabled = !ValidExpressionIndex(node.ExpressionRoots[i]),
                ToolTip = ValidExpressionIndex(node.ExpressionRoots[i])
                    ? "This literal is currently overridden by a typed shader expression."
                    : "Literal fallback value stored in the Slab."
            };
            box.TextChanged += OnSlabPropertyChanged;
            Grid.SetColumn(box, 1);
            row.Children.Add(box);
            int expression = node.ExpressionRoots[i];
            var binding = new Button
            {
                Content = ValidExpressionIndex(expression)
                    ? $"{SlabBindingDisplay(expression, i)}  ×"
                    : "f(x)",
                IsEnabled = ValidExpressionIndex(expression),
                Tag = i,
                Padding = new Thickness(5, 1, 5, 1),
                Margin = new Thickness(4, 0, 0, 0),
                Foreground = ValidExpressionIndex(expression)
                    ? ExpressionTypeBrush(ExpressionValueTypeFloat1)
                    : (Brush)FindResource("TextDim"),
                ToolTip = ValidExpressionIndex(expression)
                    ? $"Disconnect [expr {expression}] from {SlabPropertyNames[i]}"
                    : $"Expand this Slab and connect {SlabBindingRequirement(i)}."
            };
            binding.Click += OnSlabExpressionDisconnected;
            Grid.SetColumn(binding, 2);
            row.Children.Add(binding);
            SlabPropertyPanel.Children.Add(row);
        }
        BuildParameterList(node);
    }

    private void BuildParameterList(GraphNode node)
    {
        BuildLiveParameterList();
    }

    private void AddNode(PaletteEntry entry, Point location)
    {
        if (entry.ExpressionOp >= 0)
        {
            AddExpressionNode(entry, location);
            return;
        }
        if (_graphNodes.Count >= _runtimeMaxNodes)
        {
            SetDiagnostics(new[] { $"ERROR  Runtime capacity is {_runtimeMaxNodes} material expressions." });
            return;
        }
        var node = new GraphNode
        {
            Type = entry.Type,
            Flags = entry.Flags,
            Factor = entry.Type == 3 ? 0.1f : 0.5f,
            Slab = BuildPresetSlab(entry.Flags),
            X = Math.Clamp(location.X - GraphNodeWidth * 0.5, 0, GraphCanvas.Width - GraphNodeWidth),
            Y = Math.Clamp(location.Y - 28, 0, GraphCanvas.Height - GraphNodeHeight)
        };
        _graphNodes.Add(node);
        int added = _graphNodes.Count - 1;
        if (_substrateRoot < 0) _substrateRoot = added;
        MarkGraphDirty();
        SelectGraphNode(added);
        SetDiagnostics(new[] { $"INFO   Added #{added} {NodeDisplayName(added)}. Drag from a green output socket to an input." });
    }

    private static double DuplicateCoordinate(double value, double maximum)
    {
        const double offset = 32.0;
        double forward = value + offset;
        return forward <= maximum
            ? forward
            : Math.Max(0, value - offset);
    }

    private static GraphNode CloneGraphNode(GraphNode source, double maximumX, double maximumY) =>
        new()
        {
            StableId = Guid.NewGuid().ToString("N"),
            Type = source.Type,
            InputA = source.InputA,
            InputB = source.InputB,
            Factor = source.Factor,
            Flags = source.Flags,
            Slab = (float[])source.Slab.Clone(),
            ExpressionRoots = (int[])source.ExpressionRoots.Clone(),
            IsExpanded = source.IsExpanded,
            X = DuplicateCoordinate(source.X, maximumX),
            Y = DuplicateCoordinate(source.Y, maximumY)
        };

    private static ExpressionNode CloneExpressionNode(
        ExpressionNode source, double maximumX, double maximumY) =>
        new()
        {
            StableId = Guid.NewGuid().ToString("N"),
            Op = source.Op,
            DeclaredType = source.DeclaredType,
            TextureSlot = source.TextureSlot,
            TextureFlags = source.TextureFlags,
            ComponentIndex = source.ComponentIndex,
            Inputs = (int[])source.Inputs.Clone(),
            ParameterId = source.ParameterId,
            TextureAssetIdLow = source.TextureAssetIdLow,
            TextureAssetIdHigh = source.TextureAssetIdHigh,
            Value = (float[])source.Value.Clone(),
            ParameterName = source.ParameterName,
            X = DuplicateCoordinate(source.X, maximumX),
            Y = DuplicateCoordinate(source.Y, maximumY)
        };

    private void DuplicateSelectedNode()
    {
        if (ValidExpressionIndex(_selectedExpression))
        {
            if (_expressionNodes.Count >= _expressionMaxNodes)
            {
                SetDiagnostics(new[]
                {
                    $"ERROR  Shader expression capacity is {_expressionMaxNodes} nodes."
                });
                return;
            }

            ExpressionNode source = _expressionNodes[_selectedExpression];
            ExpressionNode copy = CloneExpressionNode(
                source,
                GraphCanvas.Width - ExpressionNodeWidth,
                GraphCanvas.Height - ExpressionNodeHeight(source));
            _expressionNodes.Add(copy);
            int added = _expressionNodes.Count - 1;
            MarkGraphDirty();
            SelectExpressionNode(added);
            SetDiagnostics(new[]
            {
                $"INFO   Duplicated shader expression as [expr {added}]."
            });
            return;
        }

        if (!ValidNodeIndex(_selectedNode))
            return;
        if (_graphNodes.Count >= _runtimeMaxNodes)
        {
            SetDiagnostics(new[]
            {
                $"ERROR  Runtime capacity is {_runtimeMaxNodes} material expressions."
            });
            return;
        }

        GraphNode graphSource = _graphNodes[_selectedNode];
        GraphNode graphCopy = CloneGraphNode(
            graphSource,
            GraphCanvas.Width - GraphNodeWidthFor(graphSource),
            GraphCanvas.Height - GraphNodeHeightFor(graphSource));
        _graphNodes.Add(graphCopy);
        int graphAdded = _graphNodes.Count - 1;
        MarkGraphDirty();
        SelectGraphNode(graphAdded);
        SetDiagnostics(new[]
        {
            $"INFO   Duplicated material expression as #{graphAdded}."
        });
    }

    internal static (int Passed, int Failed) RunDuplicationContractSelfTest(
        TextWriter output)
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

        var graph = new GraphNode
        {
            StableId = "source-closure",
            Type = 3,
            InputA = 4,
            InputB = 7,
            Factor = 0.25f,
            Flags = 0x300u,
            Slab = Enumerable.Range(0, SlabScalarCount).Select(i => (float)i).ToArray(),
            ExpressionRoots = Enumerable.Range(0, SlabScalarCount).ToArray(),
            IsExpanded = true,
            X = 100,
            Y = 200
        };
        GraphNode graphCopy = CloneGraphNode(graph, 1000, 1000);
        Check(
            graphCopy.StableId != graph.StableId &&
            graphCopy.Type == graph.Type &&
            graphCopy.InputA == graph.InputA &&
            graphCopy.InputB == graph.InputB &&
            graphCopy.Factor == graph.Factor &&
            graphCopy.Flags == graph.Flags &&
            graphCopy.IsExpanded == graph.IsExpanded,
            "material-expression duplicate preserves editable topology and properties");
        Check(
            graphCopy.X == 132 && graphCopy.Y == 232,
            "material-expression duplicate receives a deterministic visible offset");
        Check(
            !ReferenceEquals(graphCopy.Slab, graph.Slab) &&
            !ReferenceEquals(graphCopy.ExpressionRoots, graph.ExpressionRoots) &&
            graphCopy.Slab.SequenceEqual(graph.Slab) &&
            graphCopy.ExpressionRoots.SequenceEqual(graph.ExpressionRoots),
            "material-expression duplicate deep-copies slab and dynamic bindings");
        graphCopy.Slab[0] = -1;
        graphCopy.ExpressionRoots[0] = -1;
        Check(
            graph.Slab[0] == 0 && graph.ExpressionRoots[0] == 0,
            "editing a duplicated material expression cannot mutate its source arrays");

        var expression = new ExpressionNode
        {
            StableId = "source-expression",
            Op = 10,
            DeclaredType = 3,
            TextureSlot = 2,
            TextureFlags = 9,
            ComponentIndex = 1,
            Inputs = new[] { 1, 2, 3 },
            ParameterId = 0x12345678,
            TextureAssetIdLow = 0x89ABCDEF,
            TextureAssetIdHigh = 0x76543210,
            Value = new[] { 0.1f, 0.2f, 0.3f, 0.4f },
            ParameterName = "Tint",
            X = 980,
            Y = 980
        };
        ExpressionNode expressionCopy =
            CloneExpressionNode(expression, 1000, 1000);
        Check(
            expressionCopy.StableId != expression.StableId &&
            expressionCopy.Op == expression.Op &&
            expressionCopy.DeclaredType == expression.DeclaredType &&
            expressionCopy.TextureSlot == expression.TextureSlot &&
            expressionCopy.TextureFlags == expression.TextureFlags &&
            expressionCopy.ComponentIndex == expression.ComponentIndex &&
            expressionCopy.ParameterId == expression.ParameterId &&
            expressionCopy.TextureAssetIdLow == expression.TextureAssetIdLow &&
            expressionCopy.TextureAssetIdHigh == expression.TextureAssetIdHigh &&
            expressionCopy.ParameterName == expression.ParameterName,
            "shader-expression duplicate preserves typed parameter and texture identity");
        Check(
            expressionCopy.X == 948 && expressionCopy.Y == 948,
            "duplicate offset remains visible when the source is at the graph edge");
        Check(
            !ReferenceEquals(expressionCopy.Inputs, expression.Inputs) &&
            !ReferenceEquals(expressionCopy.Value, expression.Value) &&
            expressionCopy.Inputs.SequenceEqual(expression.Inputs) &&
            expressionCopy.Value.SequenceEqual(expression.Value),
            "shader-expression duplicate deep-copies connections and literal values");
        expressionCopy.Inputs[0] = -1;
        expressionCopy.Value[0] = -1;
        Check(
            expression.Inputs[0] == 1 && expression.Value[0] == 0.1f,
            "editing a duplicated shader expression cannot mutate its source arrays");

        return (passed, failed);
    }

    private void ConnectNodes(int source, int destination, int input)
    {
        if (!ValidNodeIndex(source) || !ValidNodeIndex(destination) || source == destination) return;
        GraphNode n = _graphNodes[destination];
        if (input == 0) n.InputA = source;
        else n.InputB = source;
        List<string> validation = ValidateGraph();
        if (validation.Any(m => m.Contains("cycle", StringComparison.OrdinalIgnoreCase)))
        {
            if (input == 0) n.InputA = -1;
            else n.InputB = -1;
            SetDiagnostics(new[] { "ERROR  Connection would create a cycle and was rejected." });
            return;
        }
        MarkGraphDirty();
        SelectGraphNode(destination);
    }

    private void DeleteSelectedNode()
    {
        if (ValidExpressionIndex(_selectedExpression))
        {
            DeleteSelectedExpression();
            return;
        }
        if (!ValidNodeIndex(_selectedNode)) return;
        int removed = _selectedNode;
        _graphNodes.RemoveAt(removed);
        foreach (GraphNode n in _graphNodes)
        {
            n.InputA = RemapAfterDelete(n.InputA, removed);
            n.InputB = RemapAfterDelete(n.InputB, removed);
        }
        _substrateRoot = RemapAfterDelete(_substrateRoot, removed);
        if (_substrateRoot < 0 && _graphNodes.Count > 0) _substrateRoot = 0;
        _selectedNode = Math.Min(removed, _graphNodes.Count - 1);
        _wireSource = -1;
        MarkGraphDirty();
        SelectGraphNode(_selectedNode);
    }

    private static int RemapAfterDelete(int index, int removed) =>
        index < 0 ? -1 : index == removed ? -1 : index > removed ? index - 1 : index;

    private void SetRoot(int index)
    {
        if (!ValidNodeIndex(index)) return;
        _substrateRoot = index;
        MarkGraphDirty();
        RenderGraph();
    }

    private void OnPaletteSearchChanged(object sender, TextChangedEventArgs e) =>
        PopulatePalette(PaletteSearch.Text);

    private void OnPaletteDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (PaletteList.SelectedItem is not PaletteEntry entry) return;
        Point location = new(
            GraphViewport.HorizontalOffset / _graphZoom + GraphViewport.ViewportWidth / _graphZoom * 0.48,
            GraphViewport.VerticalOffset / _graphZoom + GraphViewport.ViewportHeight / _graphZoom * 0.45);
        AddNode(entry, location);
    }

    private void OnPaletteMouseDown(object sender, MouseButtonEventArgs e)
    {
        _paletteDragStart = e.GetPosition(PaletteList);
        _dragPaletteEntry = PaletteList.SelectedItem as PaletteEntry;
    }

    private void OnPaletteMouseMove(object sender, MouseEventArgs e)
    {
        if (e.LeftButton != MouseButtonState.Pressed || _dragPaletteEntry == null) return;
        Point now = e.GetPosition(PaletteList);
        if (Math.Abs(now.X - _paletteDragStart.X) < SystemParameters.MinimumHorizontalDragDistance &&
            Math.Abs(now.Y - _paletteDragStart.Y) < SystemParameters.MinimumVerticalDragDistance)
            return;
        DragDrop.DoDragDrop(PaletteList, _dragPaletteEntry, DragDropEffects.Copy);
        _dragPaletteEntry = null;
    }

    private void OnGraphDrop(object sender, DragEventArgs e)
    {
        if (e.Data.GetData(typeof(PaletteEntry)) is not PaletteEntry entry) return;
        AddNode(entry, e.GetPosition(GraphCanvas));
        e.Handled = true;
    }

    private void OnGraphMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        Point p = e.GetPosition(GraphCanvas);
        GraphCanvas.Focus();

        if (ValidExpressionIndex(_expressionWireSource) &&
            TryCompleteExpressionConnection(p))
        {
            RenderGraph();
            e.Handled = true;
            return;
        }

        if (ValidNodeIndex(_wireSource))
        {
            if (Near(p, new Point(_outputX, _outputY + 56)))
            {
                SetRoot(_wireSource);
                _wireSource = -1;
                RenderGraph();
                e.Handled = true;
                return;
            }
            for (int dest = 0; dest < _graphNodes.Count; ++dest)
            {
                if (RequiredInputCount(_graphNodes[dest].Type) >= 1 && Near(p, InputPoint(dest, 0)))
                {
                    ConnectNodes(_wireSource, dest, 0);
                    _wireSource = -1;
                    RenderGraph();
                    e.Handled = true;
                    return;
                }
                if (RequiredInputCount(_graphNodes[dest].Type) >= 2 && Near(p, InputPoint(dest, 1)))
                {
                    ConnectNodes(_wireSource, dest, 1);
                    _wireSource = -1;
                    RenderGraph();
                    e.Handled = true;
                    return;
                }
            }
        }

        if (TryBeginExpressionWire(p))
        {
            e.Handled = true;
            return;
        }

        for (int i = _graphNodes.Count - 1; i >= 0; --i)
        {
            if (!Near(p, OutputPoint(i))) continue;
            _wireSource = i;
            _wireMouse = p;
            GraphCanvas.CaptureMouse();
            RenderGraph();
            StatusText.Text = $"Connecting from #{i} - release on an input socket";
            e.Handled = true;
            return;
        }

        if (p.X >= _outputX && p.X <= _outputX + GraphOutputWidth &&
            p.Y >= _outputY && p.Y <= _outputY + GraphOutputHeight)
        {
            _draggingOutput = true;
            _dragAnchor = p;
            _outputDragOrigin = new Point(_outputX, _outputY);
            GraphCanvas.CaptureMouse();
            e.Handled = true;
            return;
        }

        int hit = NodeAt(p);
        if (hit >= 0)
        {
            GraphNode hitNode = _graphNodes[hit];
            if (e.ClickCount == 2 && hitNode.Type == 0 &&
                p.Y <= hitNode.Y + 31)
            {
                hitNode.IsExpanded = !hitNode.IsExpanded;
                MarkGraphDirty();
                SelectGraphNode(hit);
                SaveGraphLayout();
                e.Handled = true;
                return;
            }
            SelectGraphNode(hit);
            _dragNode = hit;
            _draggingNode = true;
            _dragAnchor = p;
            _nodeDragOrigin = new Point(_graphNodes[hit].X, _graphNodes[hit].Y);
            GraphCanvas.CaptureMouse();
            e.Handled = true;
        }
        else if (ExpressionNodeAt(p) is int expressionHit && expressionHit >= 0)
        {
            SelectExpressionNode(expressionHit);
            _dragExpressionNode = expressionHit;
            _draggingExpressionNode = true;
            _dragAnchor = p;
            _expressionDragOrigin = new Point(
                _expressionNodes[expressionHit].X,
                _expressionNodes[expressionHit].Y);
            GraphCanvas.CaptureMouse();
            e.Handled = true;
        }
        else
        {
            SelectGraphNode(-1);
        }
    }

    private void OnGraphMouseMove(object sender, MouseEventArgs e)
    {
        Point p = e.GetPosition(GraphCanvas);
        if (_panningGraph)
        {
            Point windowPoint = e.GetPosition(GraphViewport);
            GraphViewport.ScrollToHorizontalOffset(_panHorizontalOrigin - (windowPoint.X - _panAnchor.X));
            GraphViewport.ScrollToVerticalOffset(_panVerticalOrigin - (windowPoint.Y - _panAnchor.Y));
            return;
        }
        if (ValidNodeIndex(_wireSource) || ValidExpressionIndex(_expressionWireSource))
        {
            _wireMouse = p;
            RenderGraph();
            return;
        }
        if (_draggingOutput && e.LeftButton == MouseButtonState.Pressed)
        {
            _outputX = Math.Clamp(_outputDragOrigin.X + p.X - _dragAnchor.X,
                                  0, GraphCanvas.Width - GraphOutputWidth);
            _outputY = Math.Clamp(_outputDragOrigin.Y + p.Y - _dragAnchor.Y,
                                  0, GraphCanvas.Height - GraphOutputHeight);
            RenderGraph();
            return;
        }
        if (_draggingExpressionNode && ValidExpressionIndex(_dragExpressionNode) &&
            e.LeftButton == MouseButtonState.Pressed)
        {
            ExpressionNode expression = _expressionNodes[_dragExpressionNode];
            expression.X = Math.Clamp(
                _expressionDragOrigin.X + p.X - _dragAnchor.X,
                0, GraphCanvas.Width - ExpressionNodeWidth);
            expression.Y = Math.Clamp(
                _expressionDragOrigin.Y + p.Y - _dragAnchor.Y,
                0, GraphCanvas.Height - ExpressionNodeHeight(expression));
            RenderGraph();
            return;
        }
        if (!_draggingNode || !ValidNodeIndex(_dragNode) || e.LeftButton != MouseButtonState.Pressed) return;
        GraphNode n = _graphNodes[_dragNode];
        n.X = Math.Clamp(_nodeDragOrigin.X + p.X - _dragAnchor.X,
                         0, GraphCanvas.Width - GraphNodeWidthFor(n));
        n.Y = Math.Clamp(_nodeDragOrigin.Y + p.Y - _dragAnchor.Y,
                         0, GraphCanvas.Height - GraphNodeHeightFor(n));
        RenderGraph();
    }

    private void OnGraphMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        Point p = e.GetPosition(GraphCanvas);
        if (ValidExpressionIndex(_expressionWireSource))
        {
            int source = _expressionWireSource;
            bool completed = TryCompleteExpressionConnection(p);
            if (!completed && !Near(p, ExpressionOutputPoint(source)))
                _expressionWireSource = -1;
        }
        if (ValidNodeIndex(_wireSource))
        {
            if (Near(p, new Point(_outputX, _outputY + 56)))
            {
                SetRoot(_wireSource);
                _wireSource = -1;
            }
            else
            {
                for (int dest = 0; dest < _graphNodes.Count && ValidNodeIndex(_wireSource); ++dest)
                {
                    if (RequiredInputCount(_graphNodes[dest].Type) >= 1 && Near(p, InputPoint(dest, 0)))
                    {
                        int source = _wireSource;
                        _wireSource = -1;
                        ConnectNodes(source, dest, 0);
                    }
                    else if (RequiredInputCount(_graphNodes[dest].Type) >= 2 && Near(p, InputPoint(dest, 1)))
                    {
                        int source = _wireSource;
                        _wireSource = -1;
                        ConnectNodes(source, dest, 1);
                    }
                }
            }
        }
        if (_draggingNode)
        {
            _draggingNode = false;
            _dragNode = -1;
            SaveGraphLayout();
        }
        if (_draggingExpressionNode)
        {
            _draggingExpressionNode = false;
            _dragExpressionNode = -1;
            SaveGraphLayout();
        }
        if (_draggingOutput)
        {
            _draggingOutput = false;
            SaveGraphLayout();
        }
        GraphCanvas.ReleaseMouseCapture();
        RenderGraph();
    }

    private void OnGraphMouseRightButtonDown(object sender, MouseButtonEventArgs e)
    {
        _panningGraph = true;
        _panAnchor = e.GetPosition(GraphViewport);
        _panHorizontalOrigin = GraphViewport.HorizontalOffset;
        _panVerticalOrigin = GraphViewport.VerticalOffset;
        GraphCanvas.CaptureMouse();
        GraphCanvas.Cursor = Cursors.ScrollAll;
        e.Handled = true;
    }

    private void OnGraphMouseRightButtonUp(object sender, MouseButtonEventArgs e)
    {
        _panningGraph = false;
        GraphCanvas.ReleaseMouseCapture();
        GraphCanvas.Cursor = Cursors.Arrow;
        SaveGraphLayout();
        e.Handled = true;
    }

    private void OnGraphMouseWheel(object sender, MouseWheelEventArgs e)
    {
        double previous = _graphZoom;
        _graphZoom = Math.Clamp(_graphZoom * (e.Delta > 0 ? 1.12 : 1.0 / 1.12), 0.35, 2.25);
        if (Math.Abs(previous - _graphZoom) < 0.001) return;
        UpdateGraphZoom();
        GraphStatusText.Text = $"{_graphNodes.Count} / {_runtimeMaxNodes} expressions  |  Zoom {_graphZoom * 100:0}%";
        e.Handled = true;
    }

    private void UpdateGraphZoom() =>
        GraphCanvas.LayoutTransform = new ScaleTransform(_graphZoom, _graphZoom);

    private void OnDuplicateNodeClicked(object sender, RoutedEventArgs e) =>
        DuplicateSelectedNode();

    private void OnDeleteNodeClicked(object sender, RoutedEventArgs e) =>
        DeleteSelectedNode();

    private void OnSetRootClicked(object sender, RoutedEventArgs e) => SetRoot(_selectedNode);

    private void OnFitGraphClicked(object sender, RoutedEventArgs e)
    {
        if (_graphNodes.Count == 0 && _expressionNodes.Count == 0) return;
        var minXs = _graphNodes.Select(n => n.X)
            .Concat(_expressionNodes.Select(n => n.X))
            .Append(_outputX);
        var minYs = _graphNodes.Select(n => n.Y)
            .Concat(_expressionNodes.Select(n => n.Y))
            .Append(_outputY);
        var maxXs = _graphNodes.Select(n => n.X + GraphNodeWidthFor(n))
            .Concat(_expressionNodes.Select(n => n.X + ExpressionNodeWidth))
            .Append(_outputX + GraphOutputWidth);
        var maxYs = _graphNodes.Select(n => n.Y + GraphNodeHeightFor(n))
            .Concat(_expressionNodes.Select(n => n.Y + ExpressionNodeHeight(n)))
            .Append(_outputY + GraphOutputHeight);
        double minX = minXs.Min() - 80;
        double minY = minYs.Min() - 80;
        double maxX = maxXs.Max() + 80;
        double maxY = maxYs.Max() + 80;
        double availableW = Math.Max(200, GraphViewport.ActualWidth - 24);
        double availableH = Math.Max(160, GraphViewport.ActualHeight - 24);
        _graphZoom = Math.Clamp(Math.Min(availableW / Math.Max(1, maxX - minX),
                                        availableH / Math.Max(1, maxY - minY)), 0.35, 1.25);
        UpdateGraphZoom();
        Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() =>
        {
            GraphViewport.ScrollToHorizontalOffset(Math.Max(0, minX * _graphZoom));
            GraphViewport.ScrollToVerticalOffset(Math.Max(0, minY * _graphZoom));
        }));
    }

    private void OnHomeGraphClicked(object sender, RoutedEventArgs e)
    {
        _graphZoom = 1;
        UpdateGraphZoom();
        Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() =>
        {
            GraphViewport.ScrollToHorizontalOffset(Math.Max(0, _outputX - GraphViewport.ViewportWidth * 0.62));
            GraphViewport.ScrollToVerticalOffset(Math.Max(0, _outputY - GraphViewport.ViewportHeight * 0.45));
        }));
    }

    private void OnOperatorFactorSlider(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_graphUiSync || !ValidNodeIndex(_selectedNode) || _graphNodes[_selectedNode].Type == 0) return;
        _graphUiSync = true;
        GraphNode n = _graphNodes[_selectedNode];
        n.Factor = (float)e.NewValue;
        OperatorFactorBox.Text = F(n.Factor);
        _graphUiSync = false;
        MarkGraphDirty();
        BuildParameterList(n);
        RenderGraph();
    }

    private void OnOperatorFactorChanged(object sender, TextChangedEventArgs e)
    {
        if (_graphUiSync || !ValidNodeIndex(_selectedNode) || _graphNodes[_selectedNode].Type == 0) return;
        GraphNode n = _graphNodes[_selectedNode];
        float value = P(OperatorFactorBox.Text, n.Factor);
        if (n.Type != 3) value = Math.Clamp(value, 0, 1);
        else value = Math.Max(0, value);
        n.Factor = value;
        _graphUiSync = true;
        OperatorFactorSlider.Maximum = n.Type == 3 ? Math.Max(10, value) : 1;
        OperatorFactorSlider.Value = Math.Clamp(value, 0, (float)OperatorFactorSlider.Maximum);
        _graphUiSync = false;
        MarkGraphDirty();
        BuildParameterList(n);
        RenderGraph();
    }

    private void OnOperatorFlagsChanged(object sender, RoutedEventArgs e)
    {
        if (_graphUiSync || !ValidNodeIndex(_selectedNode)) return;
        GraphNode n = _graphNodes[_selectedNode];
        if (OperatorUseParameter.IsChecked == true) n.Flags |= 1u;
        else n.Flags &= ~1u;
        MarkGraphDirty();
        BuildParameterList(n);
    }

    private static Color PickerColor(float r, float g, float b, float a = 1)
    {
        static byte ToByte(float value) =>
            (byte)Math.Clamp(Math.Round(Math.Clamp(value, 0, 1) * 255), 0, 255);
        return Color.FromArgb(ToByte(a), ToByte(r), ToByte(g), ToByte(b));
    }

    private void OnColorSwatchClicked(object sender, MouseButtonEventArgs e)
    {
        if (sender is not Border { Tag: string target }) return;
        TextBox[] boxes;
        bool allowAlpha;
        Action save;
        switch (target)
        {
            case "Base":
                boxes = new[] { BcR, BcG, BcB, BcA }; allowAlpha = true; save = SavePbrAndReload; break;
            case "Emissive":
                boxes = new[] { EmR, EmG, EmB }; allowAlpha = false; save = SavePbrAndReload; break;
            case "Fuzz":
                boxes = new[] { ShR, ShG, ShB }; allowAlpha = false; save = SaveExtAndReload; break;
            case "SSS":
                boxes = new[] { SsR, SsG, SsB }; allowAlpha = false; save = SaveExtAndReload; break;
            case "Shade1":
                boxes = new[] { S1R, S1G, S1B }; allowAlpha = false; save = SaveToonAndReload; break;
            case "Shade2":
                boxes = new[] { S2R, S2G, S2B }; allowAlpha = false; save = SaveToonAndReload; break;
            case "Rim":
                boxes = new[] { RimR, RimG, RimB }; allowAlpha = false; save = SaveToonAndReload; break;
            case "Effect":
                boxes = new[] { ColR, ColG, ColB, ColA }; allowAlpha = true; save = SaveAndReload; break;
            default:
                return;
        }

        Color initial = PickerColor(P(boxes[0].Text), P(boxes[1].Text), P(boxes[2].Text),
            boxes.Length > 3 ? P(boxes[3].Text, 1) : 1);
        if (!ColorPickerDialog.TryPick(this, initial, allowAlpha, out Color picked)) return;

        bool previousLoading = _loading;
        _loading = true;
        boxes[0].Text = F(picked.R / 255f);
        boxes[1].Text = F(picked.G / 255f);
        boxes[2].Text = F(picked.B / 255f);
        if (boxes.Length > 3) boxes[3].Text = F(picked.A / 255f);
        _loading = previousLoading;
        if (previousLoading) return;
        UpdatePbrSwatches();
        UpdateExtSwatches();
        UpdateToonSwatches();
        UpdateSwatch();
        save();
        e.Handled = true;
    }

    private void OnSlabColorClicked(object sender, RoutedEventArgs e)
    {
        if (!ValidNodeIndex(_selectedNode) || _graphNodes[_selectedNode].Type != 0 ||
            sender is not Button { Tag: int start } || start < 0 || start + 2 >= SlabScalarCount)
            return;
        GraphNode node = _graphNodes[_selectedNode];
        Color initial = PickerColor(node.Slab[start], node.Slab[start + 1], node.Slab[start + 2]);
        if (!ColorPickerDialog.TryPick(this, initial, false, out Color picked)) return;
        node.Slab[start] = picked.R / 255f;
        node.Slab[start + 1] = picked.G / 255f;
        node.Slab[start + 2] = picked.B / 255f;
        MarkGraphDirty();
        _graphUiSync = true;
        RebuildSlabPropertyPanel(node);
        _graphUiSync = false;
        RenderGraph();
    }

    private void OnSlabPropertyChanged(object sender, TextChangedEventArgs e)
    {
        if (_graphUiSync || !ValidNodeIndex(_selectedNode) ||
            _graphNodes[_selectedNode].Type != 0 || sender is not TextBox box ||
            box.Tag is not int scalarIndex)
            return;
        GraphNode n = _graphNodes[_selectedNode];
        if (!float.TryParse(box.Text, System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out float value) || !float.IsFinite(value))
        {
            box.BorderBrush = (Brush)FindResource("WarnFg");
            return;
        }
        box.ClearValue(Border.BorderBrushProperty);
        n.Slab[scalarIndex] = value;
        MarkGraphDirty();
        BuildParameterList(n);
        RenderGraph();
    }

    private void OnSlabModeChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_graphUiSync || !ValidNodeIndex(_selectedNode) ||
            _graphNodes[_selectedNode].Type != 0 || sender is not ComboBox box)
            return;
        GraphNode n = _graphNodes[_selectedNode];
        int previousMode = (int)((n.Flags >> 8) & 0xF);
        int nextMode = Math.Clamp(box.SelectedIndex, 0, 3);
        if (previousMode == nextMode) return;
        MessageBoxResult choice = MessageBox.Show(this,
            "Apply the physically based defaults for this building block?\n\n" +
            "Yes: replace this Slab's values with the preset.\n" +
            "No: keep authored values and only change BSDF classification.",
            "Change Substrate Building Block",
            MessageBoxButton.YesNoCancel,
            MessageBoxImage.Question);
        if (choice == MessageBoxResult.Cancel)
        {
            _graphUiSync = true;
            box.SelectedIndex = previousMode;
            _graphUiSync = false;
            return;
        }
        n.Flags = (n.Flags & ~(0xFu << 8)) | ((uint)nextMode << 8);
        if (choice == MessageBoxResult.Yes)
        {
            n.Slab = BuildPresetSlab(n.Flags);
            _graphUiSync = true;
            RebuildSlabPropertyPanel(n);
            _graphUiSync = false;
        }
        MarkGraphDirty();
        NodeDetailsTitle.Text = $"#{_selectedNode}  {NodeDisplayName(_selectedNode)}";
        RenderGraph();
    }

    private void OnParameterSelected(object sender, SelectionChangedEventArgs e)
    {
        if (ParameterList.SelectedItem is not ParameterEntry parameter) return;
        if (ValidExpressionIndex(parameter.ExpressionIndex))
        {
            SelectExpressionNode(parameter.ExpressionIndex);
            return;
        }
        NodeDetailsText.Text = $"{parameter.Name}: {parameter.Value}. Edit the corresponding field below.";
        if (parameter.ScalarIndex < 0) return;
        foreach (object child in SlabPropertyPanel.Children)
        {
            if (child is not Grid row) continue;
            TextBox? box = row.Children.OfType<TextBox>()
                .FirstOrDefault(candidate => candidate.Tag is int i && i == parameter.ScalarIndex);
            if (box == null) continue;
            box.BringIntoView();
            box.Focus();
            box.SelectAll();
            break;
        }
    }

    private void OnNameCommitted(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (_kind == 0) SavePbrAndReload();
        else SaveAndReload();
    }

    private void OnCompileClicked(object sender, RoutedEventArgs e) =>
        CompileGraph(userInitiated: true);

    private void OnSaveClicked(object sender, RoutedEventArgs e)
    {
        if (_kind == 0) SavePbrAndReload();
        else SaveAndReload();
        SaveRuntimeGraph(showDiagnostics: true);
    }

    private void OnApplyClicked(object sender, RoutedEventArgs e)
    {
        if (!SaveRuntimeGraph(showDiagnostics: true)) return;
        if (!CompileGraph(userInitiated: true)) return;
        ReloadMaterialInViewport();
        RefreshPreview();
        PreviewStateText.Text = "Preview: applied asset";
    }

    private void OnMaterialEditorClosing(object? sender, CancelEventArgs e)
    {
        BeginCloseAttempt();
        // A deleted document must never recreate either the .acsmat file or its graph sidecar.
        if (!_assetPathAvailable)
            return;
        // Headless visual-regression runs must neither persist test mutations
        // nor block shutdown on an off-screen modal prompt.
        if (_suppressClosePromptForAutomation)
            return;
        if (_graphDirty)
        {
            MessageBoxResult result = MessageBox.Show(
                this,
                "The material graph has unsaved changes.\n\nSave before closing?",
                "Unsaved Material",
                MessageBoxButton.YesNoCancel,
                MessageBoxImage.Warning);
            if (result == MessageBoxResult.Cancel)
            {
                e.Cancel = true;
                CancelCloseAttempt();
                return;
            }
            if (result == MessageBoxResult.Yes)
            {
                if (!SaveRuntimeGraph(showDiagnostics: true))
                {
                    e.Cancel = true;
                    CancelCloseAttempt();
                }
                return;
            }

            // "No" means discard the graph edit as well as its topology-dependent
            // sidecar. Saving the current layout here could overwrite positions for
            // the still-persisted graph with a layout from the discarded graph.
            return;
        }
        SaveGraphLayout();
    }

    internal void SuppressClosePromptForAutomation() =>
        _suppressClosePromptForAutomation = true;

    private void OnMaterialEditorKeyDown(object sender, KeyEventArgs e)
    {
        // Save/apply are asset-level commands and remain available from property editors.
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Control) && e.Key == Key.S)
        {
            OnSaveClicked(sender, new RoutedEventArgs());
            e.Handled = true;
            return;
        }
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Control) && e.Key == Key.Enter)
        {
            OnApplyClicked(sender, new RoutedEventArgs());
            e.Handled = true;
            return;
        }

        // Delete/F/Escape are graph-local. Window-level PreviewKeyDown used to steal these keys
        // from TextBox/ComboBox property editing even when the graph was not active.
        if (!GraphViewport.IsKeyboardFocusWithin ||
            Keyboard.FocusedElement is System.Windows.Controls.Primitives.TextBoxBase ||
            Keyboard.FocusedElement is ComboBox ||
            Keyboard.FocusedElement is PasswordBox)
            return;

        if (Keyboard.Modifiers == ModifierKeys.Control && e.Key == Key.D)
        {
            DuplicateSelectedNode();
            e.Handled = true;
        }
        else if (e.Key == Key.Delete)
        {
            DeleteSelectedNode();
            e.Handled = true;
        }
        else if (e.Key == Key.F && Keyboard.Modifiers == ModifierKeys.None)
        {
            OnFitGraphClicked(sender, new RoutedEventArgs());
            e.Handled = true;
        }
        else if (e.Key == Key.Escape &&
                 (ValidNodeIndex(_wireSource) ||
                  ValidExpressionIndex(_expressionWireSource)))
        {
            _wireSource = -1;
            _expressionWireSource = -1;
            RenderGraph();
            e.Handled = true;
        }
    }
}
