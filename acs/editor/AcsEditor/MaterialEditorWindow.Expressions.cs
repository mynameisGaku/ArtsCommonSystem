using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace AcsEditor;

public partial class MaterialEditorWindow
{
    private const double ExpressionNodeWidth = 198.0;
    private const int ExpressionValueTypeInvalid = 0;
    private const int ExpressionValueTypeFloat1 = 1;

    private sealed class ExpressionNode
    {
        public string StableId { get; set; } = Guid.NewGuid().ToString("N");
        public int Op { get; set; }
        public int DeclaredType { get; set; }
        public int TextureSlot { get; set; }
        public int TextureFlags { get; set; }
        public int ComponentIndex { get; set; }
        public int[] Inputs { get; set; } = { -1, -1, -1 };
        public uint ParameterId { get; set; }
        public uint TextureAssetIdLow { get; set; }
        public uint TextureAssetIdHigh { get; set; }
        public float[] Value { get; set; } = new float[4];
        public string ParameterName { get; set; } = "";
        public double X { get; set; }
        public double Y { get; set; }
    }

    private sealed class ExpressionNodeLayout
    {
        public int RuntimeIndex { get; set; }
        public string StableId { get; set; } = "";
        public string ParameterName { get; set; } = "";
        public double X { get; set; }
        public double Y { get; set; }
    }

    private readonly record struct ExpressionValueTag(int Node, int Lane);
    private readonly record struct ExpressionInputTag(int Node, int Input);
    private readonly List<ExpressionNode> _expressionNodes = new();
    private readonly string[] _expressionTexturePaths = new string[4];
    private int _expressionRoot = -1;
    private int _expressionMaxNodes = 64;
    private int _expressionTextureSlots = 4;
    private int _selectedExpression = -1;
    private int _expressionWireSource = -1;
    private int _dragExpressionNode = -1;
    private bool _draggingExpressionNode;
    private Point _expressionDragOrigin;
    private string _expressionCompileSummary = "No scalar expressions";

    private static readonly string[] ExpressionOpNames =
    {
        "Constant", "Scalar Parameter", "Vector Parameter", "Texture Sample 2D",
        "UV0", "Time", "World Position", "World Normal", "Add", "Multiply",
        "Lerp", "Clamp", "Power", "Dot", "Normalize", "Noise", "Component"
    };

    private static readonly string[] ExpressionErrorNames =
    {
        "None", "Empty Graph", "Too Many Nodes", "Invalid Root", "Invalid Opcode",
        "Invalid Declared Type", "Missing Input", "Unexpected Input",
        "Invalid Input Index", "Cycle", "Non-Finite Value", "Value Out Of Range",
        "Texture Slot Out Of Range", "Invalid Texture Flags",
        "Parameter Type Conflict", "Texture Slot Conflict", "Type Mismatch",
        "Invalid Compiled Program", "Evaluation Node Unavailable",
        "Non-Finite Context Value", "Too Many Parameters"
    };

    private void LoadExpressionGraph()
    {
        _expressionNodes.Clear();
        foreach (GraphNode closure in _graphNodes)
            closure.ExpressionRoots = Enumerable.Repeat(-1, SlabScalarCount).ToArray();
        Array.Fill(_expressionTexturePaths, "");

        try
        {
            _expressionMaxNodes = Math.Max(
                1, EngineInterop.acs_editor_material_expression_max_nodes());
            _expressionTextureSlots = Math.Clamp(
                EngineInterop.acs_editor_material_expression_texture_slots(), 1, 4);
            if (EngineInterop.acs_editor_material_expression_get_header(
                    _path, out _expressionRoot, out int count) != 0)
            {
                count = Math.Clamp(count, 0, _expressionMaxNodes);
                for (int i = 0; i < count; ++i)
                {
                    var value = new float[4];
                    if (EngineInterop.acs_editor_material_expression_get_node(
                            _path, i, out int op, out int declaredType,
                            out int textureSlot, out int textureFlags,
                            out int componentIndex, out int input0, out int input1,
                            out int input2, out uint parameterId,
                            out uint assetLow, out uint assetHigh, value) == 0)
                        continue;
                    _expressionNodes.Add(new ExpressionNode
                    {
                        Op = Math.Clamp(op, 0, ExpressionOpNames.Length - 1),
                        DeclaredType = Math.Clamp(declaredType, 0, 4),
                        TextureSlot = Math.Clamp(textureSlot, 0, _expressionTextureSlots - 1),
                        TextureFlags = textureFlags,
                        ComponentIndex = Math.Clamp(componentIndex, 0, 3),
                        Inputs = new[] { input0, input1, input2 },
                        ParameterId = parameterId,
                        TextureAssetIdLow = assetLow,
                        TextureAssetIdHigh = assetHigh,
                        Value = value,
                        ParameterName = op is 1 or 2
                            ? $"Parameter_{parameterId:X8}"
                            : "",
                        X = 70 + (i % 4) * 225,
                        Y = 40 + (i / 4) * 150
                    });
                }
            }
            _expressionRoot = ValidExpressionIndex(_expressionRoot)
                ? _expressionRoot
                : -1;

            for (int closureIndex = 0; closureIndex < _graphNodes.Count; ++closureIndex)
            {
                var roots = Enumerable.Repeat(-1, SlabScalarCount).ToArray();
                if (EngineInterop.acs_editor_material_expression_get_bindings(
                        _path, closureIndex, roots) != 0)
                    _graphNodes[closureIndex].ExpressionRoots = roots;
            }
            for (int slot = 0; slot < _expressionTextureSlots; ++slot)
            {
                var utf8 = new byte[1024];
                if (EngineInterop.acs_editor_material_expression_get_texture_path(
                        _path, slot, utf8, utf8.Length) == 0)
                    continue;
                int end = Array.IndexOf(utf8, (byte)0);
                if (end < 0) end = utf8.Length;
                _expressionTexturePaths[slot] = Encoding.UTF8.GetString(utf8, 0, end);
            }
        }
        catch (EntryPointNotFoundException)
        {
            DiagnosticsList.Items.Add(
                "WARNING  Typed expression ABI is unavailable in the loaded editor runtime.");
        }
        catch (DllNotFoundException)
        {
            DiagnosticsList.Items.Add(
                "WARNING  Typed expression graph could not load because the runtime DLL is missing.");
        }
        UpdateGraphStatus();
    }

    private bool ValidExpressionIndex(int index) =>
        index >= 0 && index < _expressionNodes.Count;

    private static int ExpressionInputCount(int op) => op switch
    {
        3 or 14 or 15 or 16 => 1,
        8 or 9 or 12 or 13 => 2,
        10 or 11 => 3,
        _ => 0
    };

    private static string ExpressionInputName(int op, int input) => op switch
    {
        3 => "UV",
        8 or 9 or 13 => input == 0 ? "A" : "B",
        10 => input switch { 0 => "A", 1 => "B", _ => "Alpha" },
        11 => input switch { 0 => "Value", 1 => "Min", _ => "Max" },
        12 => input == 0 ? "Base" : "Exponent",
        14 => "Value",
        15 => "Coordinates",
        16 => "Vector",
        _ => $"Input {input}"
    };

    private static int DefaultDeclaredType(int op) => op switch
    {
        0 or 1 or 5 => 1,
        4 => 2,
        6 or 7 => 3,
        2 or 3 => 4,
        _ => 0
    };

    private static string ExpressionTypeName(int type) => type switch
    {
        1 => "Float1",
        2 => "Float2",
        3 => "Float3",
        4 => "Float4",
        _ => "Invalid"
    };

    private static string ExpressionErrorName(int error) =>
        error >= 0 && error < ExpressionErrorNames.Length
            ? ExpressionErrorNames[error]
            : $"Error {error}";

    private static int SlabScalarLane(int scalar)
    {
        if (scalar <= 8) return scalar % 3;
        if (scalar is >= 13 and <= 18) return (scalar - 13) % 3;
        if (scalar is >= 20 and <= 25) return (scalar - 20) % 3;
        if (scalar is >= 27 and <= 29) return scalar - 27;
        if (scalar is >= 35 and <= 37) return scalar - 35;
        return 0;
    }

    private static string SlabBindingRequirement(int scalar)
    {
        int lane = SlabScalarLane(scalar);
        string channel = lane switch { 0 => "X / R", 1 => "Y / G", _ => "Z / B" };
        return $"Float1 broadcast or vector lane {channel}";
    }

    private string SlabBindingDisplay(int expression, int scalar)
    {
        if (!ValidExpressionIndex(expression)) return "";
        int width = InferExpressionType(expression);
        if (width == 1) return $"E{expression} (broadcast)";
        string lane = SlabScalarLane(scalar) switch
        {
            0 => ".r",
            1 => ".g",
            _ => ".b"
        };
        return $"E{expression}{lane}";
    }

    private static uint ParameterId(string name)
    {
        uint hash = 2166136261u;
        foreach (byte b in Encoding.UTF8.GetBytes(name ?? ""))
        {
            hash ^= b;
            hash *= 16777619u;
        }
        return hash;
    }

    private static ulong AssetId(string path)
    {
        ulong hash = 14695981039346656037UL;
        foreach (byte b in Encoding.UTF8.GetBytes(path ?? ""))
        {
            hash ^= b;
            hash *= 1099511628211UL;
        }
        return string.IsNullOrWhiteSpace(path) ? 0 : hash;
    }

    private int InferExpressionType(int index)
    {
        var visiting = new HashSet<int>();
        return Infer(index);

        int Infer(int current)
        {
            if (!ValidExpressionIndex(current) || !visiting.Add(current)) return 0;
            ExpressionNode node = _expressionNodes[current];
            int result;
            switch (node.Op)
            {
                case 0:
                    result = node.DeclaredType is >= 1 and <= 4 ? node.DeclaredType : 1;
                    break;
                case 1:
                case 5:
                    result = 1;
                    break;
                case 2:
                    result = node.DeclaredType is >= 2 and <= 4 ? node.DeclaredType : 4;
                    break;
                case 3:
                    result = 4;
                    break;
                case 4:
                    result = 2;
                    break;
                case 6:
                case 7:
                    result = 3;
                    break;
                case 8:
                case 9:
                case 12:
                    result = CombineExpressionTypes(Infer(node.Inputs[0]), Infer(node.Inputs[1]));
                    break;
                case 10:
                case 11:
                    result = CombineExpressionTypes(
                        CombineExpressionTypes(Infer(node.Inputs[0]), Infer(node.Inputs[1])),
                        Infer(node.Inputs[2]));
                    break;
                case 13:
                case 15:
                case 16:
                    result = 1;
                    break;
                case 14:
                    result = Infer(node.Inputs[0]);
                    break;
                default:
                    result = 0;
                    break;
            }
            visiting.Remove(current);
            return result;
        }
    }

    private static int CombineExpressionTypes(int a, int b)
    {
        if (a is < 1 or > 4 || b is < 1 or > 4) return 0;
        if (a == b) return a;
        if (a == 1) return b;
        if (b == 1) return a;
        return 0;
    }

    private static Brush ExpressionTypeBrush(int type) =>
        new SolidColorBrush(type switch
        {
            1 => Color.FromRgb(104, 198, 112),
            2 => Color.FromRgb(75, 188, 207),
            3 => Color.FromRgb(222, 185, 75),
            4 => Color.FromRgb(199, 99, 190),
            _ => Color.FromRgb(125, 130, 139)
        });

    private static string ExpressionTypeColor(int type) => type switch
    {
        1 => "#FF68C670",
        2 => "#FF4BBCCF",
        3 => "#FFDEB94B",
        4 => "#FFC763BE",
        _ => "#FF7D828B"
    };

    private static double ExpressionNodeHeight(ExpressionNode node) =>
        78 + Math.Max(1, ExpressionInputCount(node.Op)) * 21;

    private Point ExpressionOutputPoint(int index)
    {
        ExpressionNode node = _expressionNodes[index];
        return new Point(node.X + ExpressionNodeWidth, node.Y + 49);
    }

    private Point ExpressionInputPoint(int index, int input)
    {
        ExpressionNode node = _expressionNodes[index];
        return new Point(node.X, node.Y + 76 + input * 21);
    }

    private int ExpressionNodeAt(Point point)
    {
        for (int i = _expressionNodes.Count - 1; i >= 0; --i)
        {
            ExpressionNode node = _expressionNodes[i];
            if (point.X >= node.X && point.X <= node.X + ExpressionNodeWidth &&
                point.Y >= node.Y && point.Y <= node.Y + ExpressionNodeHeight(node))
                return i;
        }
        return -1;
    }

    private void RenderExpressionWires()
    {
        for (int destination = 0; destination < _expressionNodes.Count; ++destination)
        {
            ExpressionNode node = _expressionNodes[destination];
            int inputs = ExpressionInputCount(node.Op);
            for (int input = 0; input < inputs; ++input)
            {
                int source = node.Inputs[input];
                if (!ValidExpressionIndex(source)) continue;
                int type = InferExpressionType(source);
                AddWire(ExpressionOutputPoint(source),
                    ExpressionInputPoint(destination, input),
                    ExpressionTypeColor(type), 2.0);
            }
        }
        for (int closure = 0; closure < _graphNodes.Count; ++closure)
        {
            GraphNode node = _graphNodes[closure];
            if (node.Type != 0 || !node.IsExpanded) continue;
            for (int scalar = 0; scalar < SlabScalarCount; ++scalar)
            {
                int source = node.ExpressionRoots[scalar];
                if (!ValidExpressionIndex(source)) continue;
                AddWire(ExpressionOutputPoint(source),
                    SlabScalarInputPoint(closure, scalar),
                    ExpressionTypeColor(InferExpressionType(source)), 1.8);
            }
        }
    }

    private void RenderExpressionNodes()
    {
        for (int i = 0; i < _expressionNodes.Count; ++i)
            AddExpressionNodeVisual(i);
        if (ValidExpressionIndex(_expressionWireSource))
            AddWire(ExpressionOutputPoint(_expressionWireSource), _wireMouse,
                ExpressionTypeColor(InferExpressionType(_expressionWireSource)),
                2.0, dashed: true);
    }

    private void AddExpressionNodeVisual(int index)
    {
        ExpressionNode node = _expressionNodes[index];
        int outputType = InferExpressionType(index);
        int inputCount = ExpressionInputCount(node.Op);
        var root = new Grid();
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(29) });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(30) });
        root.RowDefinitions.Add(new RowDefinition());

        var header = new Border
        {
            Background = new SolidColorBrush(node.Op switch
            {
                <= 3 => Color.FromRgb(63, 91, 126),
                <= 7 => Color.FromRgb(78, 99, 104),
                _ => Color.FromRgb(100, 69, 117)
            }),
            Padding = new Thickness(8, 0, 7, 0)
        };
        var headerGrid = new Grid();
        headerGrid.ColumnDefinitions.Add(new ColumnDefinition());
        headerGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        headerGrid.Children.Add(new TextBlock
        {
            Text = ExpressionOpNames[node.Op],
            Foreground = Brushes.White,
            FontWeight = FontWeights.SemiBold,
            VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis
        });
        var id = new TextBlock
        {
            Text = $"E{index}",
            Foreground = new SolidColorBrush(Color.FromArgb(200, 255, 255, 255)),
            FontFamily = new FontFamily("Consolas"),
            FontSize = 9.5,
            VerticalAlignment = VerticalAlignment.Center
        };
        Grid.SetColumn(id, 1);
        headerGrid.Children.Add(id);
        header.Child = headerGrid;
        root.Children.Add(header);

        string summary = node.Op switch
        {
            0 => string.Join(", ", node.Value.Take(Math.Max(1, outputType))
                .Select(v => v.ToString("0.###", CultureInfo.InvariantCulture))),
            1 or 2 => string.IsNullOrWhiteSpace(node.ParameterName)
                ? $"ID 0x{node.ParameterId:X8}"
                : node.ParameterName,
            3 => $"Texture slot {node.TextureSlot}",
            16 => "xyzw"[Math.Clamp(node.ComponentIndex, 0, 3)].ToString(),
            _ => ExpressionTypeName(outputType)
        };
        var summaryText = NodeBodyText(summary);
        summaryText.Margin = new Thickness(10, 6, 18, 2);
        Grid.SetRow(summaryText, 1);
        root.Children.Add(summaryText);

        var inputs = new StackPanel { Margin = new Thickness(10, 5, 8, 5) };
        Grid.SetRow(inputs, 2);
        if (inputCount == 0)
        {
            inputs.Children.Add(NodeBodyText("Source"));
        }
        else
        {
            for (int input = 0; input < inputCount; ++input)
            {
                int source = node.Inputs[input];
                inputs.Children.Add(NodeBodyText(
                    $"{ExpressionInputName(node.Op, input)}   " +
                    (ValidExpressionIndex(source)
                        ? $"E{source} {ExpressionTypeName(InferExpressionType(source))}"
                        : "Not connected")));
            }
        }
        root.Children.Add(inputs);

        var border = new Border
        {
            Width = ExpressionNodeWidth,
            Height = ExpressionNodeHeight(node),
            Background = new SolidColorBrush(Color.FromRgb(31, 35, 42)),
            BorderBrush = index == _selectedExpression
                ? (Brush)FindResource("Accent")
                : new SolidColorBrush(Color.FromRgb(63, 70, 81)),
            BorderThickness = new Thickness(index == _selectedExpression ? 2 : 1),
            CornerRadius = new CornerRadius(2),
            Child = root,
            Effect = new System.Windows.Media.Effects.DropShadowEffect
            {
                Color = Colors.Black,
                BlurRadius = 7,
                Opacity = 0.35,
                ShadowDepth = 2
            }
        };
        Canvas.SetLeft(border, node.X);
        Canvas.SetTop(border, node.Y);
        GraphCanvas.Children.Add(border);

        AddSocket(ExpressionOutputPoint(index), ExpressionTypeColor(outputType));
        for (int input = 0; input < inputCount; ++input)
        {
            int expected = ExpressionExpectedInputType(index, input);
            AddSocket(ExpressionInputPoint(index, input), ExpressionTypeColor(expected));
        }
    }

    private int ExpressionExpectedInputType(int index, int input)
    {
        if (!ValidExpressionIndex(index)) return 0;
        ExpressionNode node = _expressionNodes[index];
        if (node.Op == 3) return 2;
        if (node.Op == 16) return 0;
        if (node.Op is 8 or 9 or 10 or 11 or 12 or 13)
        {
            for (int other = 0; other < ExpressionInputCount(node.Op); ++other)
            {
                if (other == input || !ValidExpressionIndex(node.Inputs[other])) continue;
                return InferExpressionType(node.Inputs[other]);
            }
        }
        return 0;
    }

    private void AddExpressionNode(PaletteEntry entry, Point location)
    {
        if (_expressionNodes.Count >= _expressionMaxNodes)
        {
            SetDiagnostics(new[]
            {
                $"ERROR  Shader expression capacity is {_expressionMaxNodes} nodes."
            });
            return;
        }
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Add Shader Expression");
        int op = entry.ExpressionOp;
        var node = new ExpressionNode
        {
            Op = op,
            DeclaredType = DefaultDeclaredType(op),
            TextureFlags = op == 3 ? 1 | 8 : 0,
            ParameterName = op == 1 ? "ScalarParameter" :
                op == 2 ? "VectorParameter" : "",
            X = Math.Clamp(location.X - ExpressionNodeWidth * 0.5,
                0, GraphCanvas.Width - ExpressionNodeWidth),
            Y = Math.Clamp(location.Y - 28, 0, GraphCanvas.Height - 110)
        };
        if (op == 3)
        {
            ExpressionNode? shared = _expressionNodes.FirstOrDefault(
                candidate => candidate.Op == 3 && candidate.TextureSlot == node.TextureSlot);
            if (shared != null)
            {
                node.TextureFlags = shared.TextureFlags;
                node.TextureAssetIdLow = shared.TextureAssetIdLow;
                node.TextureAssetIdHigh = shared.TextureAssetIdHigh;
            }
        }
        if (op is 0 or 1 or 2 or 3)
        {
            node.Value[0] = 1;
            if (op is 2 or 3)
                node.Value[1] = node.Value[2] = node.Value[3] = 1;
        }
        if (op is 1 or 2) node.ParameterId = ParameterId(node.ParameterName);
        _expressionNodes.Add(node);
        int added = _expressionNodes.Count - 1;
        if (_expressionRoot < 0) _expressionRoot = added;
        MarkGraphDirty();
        SelectExpressionNode(added);
        SetDiagnostics(new[]
        {
            $"INFO   Added [expr {added}] {ExpressionOpNames[op]}. " +
            "Connect its typed output to another expression or an expanded Slab input."
        });
    }

    private void ConnectExpressionInput(int source, int destination, int input)
    {
        if (!ValidExpressionIndex(source) || !ValidExpressionIndex(destination) ||
            source == destination || input < 0 ||
            input >= ExpressionInputCount(_expressionNodes[destination].Op))
            return;
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Connect Shader Expressions");
        ExpressionNode destinationNode = _expressionNodes[destination];
        if (!IsExpressionInputTypeCompatible(
                source, destination, input, out int sourceType, out int expectedType))
        {
            SetDiagnostics(new[]
            {
                $"ERROR  [expr {source}] outputs {ExpressionTypeName(sourceType)}, but " +
                $"[expr {destination}] {ExpressionInputName(destinationNode.Op, input)} " +
                $"expects {ExpressionTypeName(expectedType)}.",
                "INFO   Use a Component node for vector lane extraction; only Float1 splat is implicit."
            });
            StatusText.Text = "Typed connection rejected";
            StatusText.Foreground = (Brush)FindResource("WarnFg");
            return;
        }
        int previous = destinationNode.Inputs[input];
        destinationNode.Inputs[input] = source;
        if (ExpressionGraphHasCycle())
        {
            destinationNode.Inputs[input] = previous;
            SetDiagnostics(new[]
            {
                "ERROR  Expression connection would create a cycle and was rejected."
            });
            StatusText.Text = "Cyclic connection rejected";
            StatusText.Foreground = (Brush)FindResource("WarnFg");
            return;
        }
        MarkGraphDirty();
        SelectExpressionNode(destination);
    }

    private bool IsExpressionInputTypeCompatible(
        int source, int destination, int input,
        out int sourceType, out int expectedType)
    {
        sourceType = InferExpressionType(source);
        expectedType = ExpressionExpectedInputType(destination, input);
        if (!ValidExpressionIndex(destination) || sourceType <= 0) return false;
        ExpressionNode destinationNode = _expressionNodes[destination];
        bool exact = destinationNode.Op is 3 or 13;
        bool compatible = expectedType == 0 || sourceType == expectedType ||
            (!exact && (sourceType == 1 || expectedType == 1));
        if (destinationNode.Op == 16 &&
            destinationNode.ComponentIndex >= sourceType)
            compatible = false;
        return compatible;
    }

    private void BindExpressionToSlab(int source, int closure, int scalar)
    {
        if (!ValidExpressionIndex(source) || !ValidNodeIndex(closure) ||
            _graphNodes[closure].Type != 0 || scalar < 0 || scalar >= SlabScalarCount)
            return;
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Bind Shader Expression");
        int type = InferExpressionType(source);
        int lane = SlabScalarLane(scalar);
        if (type < 1 || (type != 1 && lane >= type))
        {
            SetDiagnostics(new[]
            {
                $"ERROR  [expr {source}] outputs {ExpressionTypeName(type)}, which has no " +
                $"{SlabBindingRequirement(scalar)} for {SlabPropertyNames[scalar]}.",
                "INFO   Use a wider vector or a Component node for explicit lane extraction."
            });
            StatusText.Text = "Slab binding rejected";
            StatusText.Foreground = (Brush)FindResource("WarnFg");
            return;
        }
        _graphNodes[closure].ExpressionRoots[scalar] = source;
        MarkGraphDirty();
        SelectGraphNode(closure);
        SetDiagnostics(new[]
        {
            $"OK     Bound {SlabBindingDisplay(source, scalar)} to closure #{closure} / " +
            $"{SlabPropertyNames[scalar]} ({SlabBindingRequirement(scalar)})."
        });
    }

    private bool TryCompleteExpressionConnection(Point point)
    {
        if (!ValidExpressionIndex(_expressionWireSource)) return false;
        int source = _expressionWireSource;
        for (int destination = 0; destination < _expressionNodes.Count; ++destination)
        {
            int count = ExpressionInputCount(_expressionNodes[destination].Op);
            for (int input = 0; input < count; ++input)
            {
                if (!Near(point, ExpressionInputPoint(destination, input))) continue;
                _expressionWireSource = -1;
                ConnectExpressionInput(source, destination, input);
                return true;
            }
        }
        for (int closure = 0; closure < _graphNodes.Count; ++closure)
        {
            GraphNode node = _graphNodes[closure];
            if (node.Type != 0 || !node.IsExpanded) continue;
            for (int scalar = 0; scalar < SlabScalarCount; ++scalar)
            {
                if (!Near(point, SlabScalarInputPoint(closure, scalar), 10)) continue;
                _expressionWireSource = -1;
                BindExpressionToSlab(source, closure, scalar);
                return true;
            }
        }
        return false;
    }

    private bool TryBeginExpressionWire(Point point)
    {
        for (int index = _expressionNodes.Count - 1; index >= 0; --index)
        {
            if (!Near(point, ExpressionOutputPoint(index))) continue;
            _expressionWireSource = index;
            _wireSource = -1;
            _wireMouse = point;
            GraphCanvas.CaptureMouse();
            StatusText.Text =
                $"Connecting [expr {index}] {ExpressionTypeName(InferExpressionType(index))}";
            RenderGraph();
            return true;
        }
        return false;
    }

    private bool ExpressionGraphHasCycle()
    {
        var state = new byte[_expressionNodes.Count];
        bool Visit(int index)
        {
            if (!ValidExpressionIndex(index)) return false;
            if (state[index] == 1) return true;
            if (state[index] == 2) return false;
            state[index] = 1;
            ExpressionNode node = _expressionNodes[index];
            for (int input = 0; input < ExpressionInputCount(node.Op); ++input)
                if (ValidExpressionIndex(node.Inputs[input]) && Visit(node.Inputs[input]))
                    return true;
            state[index] = 2;
            return false;
        }
        for (int i = 0; i < _expressionNodes.Count; ++i)
            if (Visit(i)) return true;
        return false;
    }

    private List<string> ValidateExpressionGraph()
    {
        var result = new List<string>();
        if (_expressionNodes.Count > _expressionMaxNodes)
            result.Add($"ERROR  Expression graph has {_expressionNodes.Count} nodes; " +
                       $"runtime capacity is {_expressionMaxNodes}.");
        if (_expressionRoot >= _expressionNodes.Count)
            result.Add("ERROR  Expression preview root is outside the graph.");
        for (int i = 0; i < _expressionNodes.Count; ++i)
        {
            ExpressionNode node = _expressionNodes[i];
            if (node.Op < 0 || node.Op >= ExpressionOpNames.Length)
                result.Add($"ERROR  [expr {i}] has an invalid opcode.");
            int arity = ExpressionInputCount(node.Op);
            for (int input = 0; input < arity; ++input)
            {
                if (!ValidExpressionIndex(node.Inputs[input]))
                {
                    result.Add($"ERROR  [expr {i}] {ExpressionOpNames[node.Op]} " +
                               $"requires {ExpressionInputName(node.Op, input)}.");
                }
                else if (!IsExpressionInputTypeCompatible(
                             node.Inputs[input], i, input,
                             out int sourceType, out int expectedType))
                {
                    result.Add(
                        $"ERROR  [expr {i}] {ExpressionInputName(node.Op, input)} " +
                        $"cannot accept {ExpressionTypeName(sourceType)} from " +
                        $"[expr {node.Inputs[input]}]" +
                        (expectedType > 0
                            ? $"; expected {ExpressionTypeName(expectedType)}."
                            : "."));
                }
            }
            for (int input = arity; input < 3; ++input)
                if (node.Inputs[input] >= 0)
                    result.Add($"ERROR  [expr {i}] has an unexpected input {input}.");
            if (node.Value.Any(value => !float.IsFinite(value)))
                result.Add($"ERROR  [expr {i}] contains a non-finite value.");
            if (node.Op == 3 &&
                (node.TextureSlot < 0 || node.TextureSlot >= _expressionTextureSlots))
                result.Add($"ERROR  [expr {i}] texture slot is out of range.");
            if (node.Op == 3 && (node.TextureFlags & ~15) != 0)
                result.Add($"ERROR  [expr {i}] has invalid texture sampling flags.");
            if (node.Op == 16 && node.ComponentIndex is < 0 or > 3)
                result.Add($"ERROR  [expr {i}] component lane is out of range.");
            if (node.Op is 8 or 9 or 10 or 11 or 12 &&
                Enumerable.Range(0, arity).All(input =>
                    ValidExpressionIndex(node.Inputs[input])) &&
                InferExpressionType(i) == 0)
            {
                result.Add(
                    $"ERROR  [expr {i}] {ExpressionOpNames[node.Op]} inputs have " +
                    "incompatible vector widths; only Float1 splat is implicit.");
            }
        }
        var parameterTypes = new Dictionary<uint, int>();
        for (int i = 0; i < _expressionNodes.Count; ++i)
        {
            ExpressionNode node = _expressionNodes[i];
            if (node.Op is not (1 or 2)) continue;
            int type = node.Op == 1
                ? 1
                : node.DeclaredType is >= 2 and <= 4 ? node.DeclaredType : 4;
            if (parameterTypes.TryGetValue(node.ParameterId, out int existingType) &&
                existingType != type)
            {
                result.Add(
                    $"ERROR  [expr {i}] parameter ID 0x{node.ParameterId:X8} is " +
                    $"declared as both {ExpressionTypeName(existingType)} and " +
                    $"{ExpressionTypeName(type)}.");
            }
            else
            {
                parameterTypes[node.ParameterId] = type;
            }
        }
        if (parameterTypes.Count > 32)
            result.Add(
                $"ERROR  Material uses {parameterTypes.Count} distinct parameter IDs; " +
                "the runtime limit is 32.");
        var textureStates = new Dictionary<int, (int Flags, uint Low, uint High, int Node)>();
        for (int i = 0; i < _expressionNodes.Count; ++i)
        {
            ExpressionNode node = _expressionNodes[i];
            if (node.Op != 3) continue;
            if (textureStates.TryGetValue(node.TextureSlot, out var state) &&
                (state.Flags != node.TextureFlags ||
                 state.Low != node.TextureAssetIdLow ||
                 state.High != node.TextureAssetIdHigh))
            {
                result.Add(
                    $"ERROR  [expr {i}] and [expr {state.Node}] use texture slot " +
                    $"{node.TextureSlot} with conflicting asset or sampler state.");
            }
            else
            {
                textureStates[node.TextureSlot] = (
                    node.TextureFlags, node.TextureAssetIdLow,
                    node.TextureAssetIdHigh, i);
            }
        }
        if (ExpressionGraphHasCycle())
            result.Add("ERROR  Shader expression graph contains a cycle.");

        for (int closure = 0; closure < _graphNodes.Count; ++closure)
        {
            GraphNode node = _graphNodes[closure];
            for (int scalar = 0; scalar < SlabScalarCount; ++scalar)
            {
                int root = node.ExpressionRoots[scalar];
                if (root < 0) continue;
                if (!ValidExpressionIndex(root))
                {
                    result.Add($"ERROR  Closure #{closure} / {SlabPropertyNames[scalar]} " +
                               "references a missing expression.");
                }
                else
                {
                    int width = InferExpressionType(root);
                    int lane = SlabScalarLane(scalar);
                    if (width < 1 || (width != 1 && lane >= width))
                    {
                        result.Add(
                            $"ERROR  Closure #{closure} / {SlabPropertyNames[scalar]} " +
                            $"needs {SlabBindingRequirement(scalar)}, but [expr {root}] " +
                            $"outputs {ExpressionTypeName(width)}.");
                    }
                }
            }
        }
        bool hasBinding = _graphNodes.Any(
            node => node.ExpressionRoots.Any(ValidExpressionIndex));
        if (hasBinding)
        {
            if (!ValidNodeIndex(_substrateRoot) ||
                _graphNodes[_substrateRoot].Type != 0)
            {
                result.Add(
                    "ERROR  Dynamic Slab expressions require Front Material to reference " +
                    "a Slab directly; closure-operator roots are not supported by the current GPU path.");
            }
            for (int closure = 0; closure < _graphNodes.Count; ++closure)
            {
                if (closure == _substrateRoot) continue;
                if (_graphNodes[closure].ExpressionRoots.Any(ValidExpressionIndex))
                {
                    result.Add(
                        $"ERROR  Closure #{closure} has dynamic scalar bindings but is not the " +
                        "direct Front Material Slab.");
                }
            }
        }
        return result;
    }

    private void PackExpressionGraph(
        out int[] ops, out int[] declaredTypes, out int[] textureSlots,
        out int[] textureFlags, out int[] componentIndices,
        out int[] input0, out int[] input1, out int[] input2,
        out uint[] parameterIds, out uint[] assetLows, out uint[] assetHighs,
        out float[] values4)
    {
        int count = _expressionNodes.Count;
        ops = new int[count];
        declaredTypes = new int[count];
        textureSlots = new int[count];
        textureFlags = new int[count];
        componentIndices = new int[count];
        input0 = new int[count];
        input1 = new int[count];
        input2 = new int[count];
        parameterIds = new uint[count];
        assetLows = new uint[count];
        assetHighs = new uint[count];
        values4 = new float[count * 4];
        for (int i = 0; i < count; ++i)
        {
            ExpressionNode node = _expressionNodes[i];
            ops[i] = node.Op;
            declaredTypes[i] = node.DeclaredType;
            textureSlots[i] = node.TextureSlot;
            textureFlags[i] = node.TextureFlags;
            componentIndices[i] = node.ComponentIndex;
            input0[i] = node.Inputs[0];
            input1[i] = node.Inputs[1];
            input2[i] = node.Inputs[2];
            parameterIds[i] = node.ParameterId;
            assetLows[i] = node.TextureAssetIdLow;
            assetHighs[i] = node.TextureAssetIdHigh;
            Array.Copy(node.Value, 0, values4, i * 4, 4);
        }
    }

    private int[] PackExpressionBindings()
    {
        var roots = Enumerable.Repeat(-1, _graphNodes.Count * SlabScalarCount).ToArray();
        for (int i = 0; i < _graphNodes.Count; ++i)
            if (_graphNodes[i].ExpressionRoots.Length == SlabScalarCount)
                Array.Copy(_graphNodes[i].ExpressionRoots, 0, roots,
                    i * SlabScalarCount, SlabScalarCount);
        return roots;
    }

    private bool SaveCombinedRuntimeGraph(
        int[] types, int[] inputsA, int[] inputsB, float[] factors,
        uint[] flags, float[] slabs)
    {
        PackExpressionGraph(
            out int[] ops, out int[] declaredTypes, out int[] textureSlots,
            out int[] textureFlags, out int[] componentIndices,
            out int[] input0, out int[] input1, out int[] input2,
            out uint[] parameterIds, out uint[] assetLows, out uint[] assetHighs,
            out float[] values4);
        return EngineInterop.acs_editor_material_substrate_expression_save(
            _path, 1, _substrateRoot, _graphNodes.Count,
            types, inputsA, inputsB, factors, flags, slabs,
            PackExpressionBindings(),
            _expressionRoot, _expressionNodes.Count,
            ops, declaredTypes, textureSlots, textureFlags, componentIndices,
            input0, input1, input2, parameterIds, assetLows, assetHighs, values4,
            _expressionTexturePaths[0], _expressionTexturePaths[1],
            _expressionTexturePaths[2], _expressionTexturePaths[3]) != 0;
    }

    private bool CompileExpressionGraph(out List<string> messages)
    {
        messages = new List<string>();
        if (_expressionNodes.Count == 0)
        {
            _expressionCompileSummary = "0 instructions";
            return true;
        }
        PackExpressionGraph(
            out int[] ops, out int[] declaredTypes, out int[] textureSlots,
            out int[] textureFlags, out int[] componentIndices,
            out int[] input0, out int[] input1, out int[] input2,
            out uint[] parameterIds, out uint[] assetLows, out uint[] assetHighs,
            out float[] values4);
        int called = EngineInterop.acs_editor_material_expression_compile_arrays(
            _expressionRoot, _expressionNodes.Count,
            ops, declaredTypes, textureSlots, textureFlags, componentIndices,
            input0, input1, input2, parameterIds, assetLows, assetHighs, values4,
            out int error, out int errorNode, out int errorInput,
            out int expected, out int actual, out int instructions, out int folds,
            out uint hashLow, out uint hashHigh);
        if (called == 0 || error != 0)
        {
            string input = errorInput >= 0 ? $", input {errorInput}" : "";
            string type = expected > 0 || actual > 0
                ? $", expected {ExpressionTypeName(expected)}, got {ExpressionTypeName(actual)}"
                : "";
            messages.Add(
                $"ERROR  [expr {errorNode}] {ExpressionErrorName(error)}{input}{type}.");
            _expressionCompileSummary = "Expression compile failed";
            return false;
        }
        _expressionCompileSummary =
            $"{instructions} instruction(s), {folds} fold(s), hash {hashHigh:X8}{hashLow:X8}";
        messages.Add($"INFO   Scalar expressions: {_expressionCompileSummary}.");
        return true;
    }

    private void DeleteSelectedExpression()
    {
        if (!ValidExpressionIndex(_selectedExpression)) return;
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Delete Shader Expression");
        int removed = _selectedExpression;
        _expressionNodes.RemoveAt(removed);
        foreach (ExpressionNode node in _expressionNodes)
            for (int input = 0; input < 3; ++input)
                node.Inputs[input] = RemapAfterDelete(node.Inputs[input], removed);
        foreach (GraphNode closure in _graphNodes)
            for (int scalar = 0; scalar < SlabScalarCount; ++scalar)
                closure.ExpressionRoots[scalar] =
                    RemapAfterDelete(closure.ExpressionRoots[scalar], removed);
        _expressionRoot = RemapAfterDelete(_expressionRoot, removed);
        if (_expressionRoot < 0 && _expressionNodes.Count > 0)
            _expressionRoot = Math.Min(removed, _expressionNodes.Count - 1);
        _selectedExpression = Math.Min(removed, _expressionNodes.Count - 1);
        _expressionWireSource = -1;
        MarkGraphDirty();
        if (ValidExpressionIndex(_selectedExpression))
            SelectExpressionNode(_selectedExpression);
        else
            SelectGraphNode(-1);
    }

    private void SelectExpressionNode(int index)
    {
        _selectedExpression = ValidExpressionIndex(index) ? index : -1;
        _selectedNode = -1;
        _graphUiSync = true;
        PbrPanel.Visibility = Visibility.Collapsed;
        SelectedSlabPanel.Visibility = Visibility.Collapsed;
        SelectedOperatorPanel.Visibility = Visibility.Collapsed;
        SelectedExpressionPanel.Visibility =
            ValidExpressionIndex(_selectedExpression)
                ? Visibility.Visible
                : Visibility.Collapsed;
        if (ValidExpressionIndex(_selectedExpression))
        {
            ExpressionNode node = _expressionNodes[_selectedExpression];
            NodeDetailsTitle.Text =
                $"E{_selectedExpression}  {ExpressionOpNames[node.Op]}";
            NodeDetailsText.Text =
                $"Typed {ExpressionTypeName(InferExpressionType(_selectedExpression))} " +
                "shader expression. Connect it to an expanded Slab scalar input; " +
                "the Preview Root only drives direct expression inspection. Dynamic GPU " +
                "bindings require Front Material to reference that bound Slab directly.";
            RebuildExpressionDetails(node);
        }
        _graphUiSync = false;
        BuildLiveParameterList();
        RenderGraph();
    }

    private void RebuildExpressionDetails(ExpressionNode node)
    {
        ExpressionPropertyPanel.Children.Clear();
        AddExpressionDetailsText("Opcode", ExpressionOpNames[node.Op]);
        AddExpressionDetailsText(
            "Output Type", ExpressionTypeName(InferExpressionType(_selectedExpression)));

        if (node.Op is 0 or 2)
        {
            var typeBox = new ComboBox { Tag = _selectedExpression };
            if (node.Op == 0)
            {
                typeBox.Items.Add("Float1");
                typeBox.Items.Add("Float2");
                typeBox.Items.Add("Float3");
                typeBox.Items.Add("Float4");
                typeBox.SelectedIndex = Math.Clamp(
                    (node.DeclaredType == 0 ? 1 : node.DeclaredType) - 1, 0, 3);
            }
            else
            {
                typeBox.Items.Add("Float2");
                typeBox.Items.Add("Float3");
                typeBox.Items.Add("Float4");
                typeBox.SelectedIndex = Math.Clamp(
                    (node.DeclaredType == 0 ? 4 : node.DeclaredType) - 2, 0, 2);
            }
            typeBox.SelectionChanged += OnExpressionDeclaredTypeChanged;
            AddExpressionDetailsRow("Declared Type", typeBox);
        }

        if (node.Op is 0 or 1 or 2 or 3)
        {
            var values = new StackPanel { Orientation = Orientation.Horizontal };
            int width = node.Op is 0 or 2 or 3
                ? Math.Max(1, node.DeclaredType == 0 ? DefaultDeclaredType(node.Op) : node.DeclaredType)
                : 1;
            if (node.Op == 3) width = 4;
            for (int lane = 0; lane < width; ++lane)
            {
                var box = new TextBox
                {
                    Width = 48,
                    Margin = new Thickness(lane == 0 ? 0 : 3, 0, 0, 0),
                    Text = node.Value[lane].ToString("0.###", CultureInfo.InvariantCulture),
                    Tag = new ExpressionValueTag(_selectedExpression, lane)
                };
                box.TextChanged += OnExpressionValueChanged;
                values.Children.Add(box);
            }
            if (width >= 3)
            {
                var swatch = new Border
                {
                    Width = 23,
                    Height = 23,
                    Margin = new Thickness(5, 0, 0, 0),
                    Background = new SolidColorBrush(PickerColor(
                        node.Value[0], node.Value[1], node.Value[2],
                        width >= 4 ? node.Value[3] : 1)),
                    BorderBrush = (Brush)FindResource("CtrlBorder"),
                    BorderThickness = new Thickness(1),
                    Cursor = Cursors.Hand,
                    Tag = _selectedExpression,
                    ToolTip = width >= 4
                        ? "Edit RGB and alpha with the shared color picker"
                        : "Edit RGB with the shared color picker"
                };
                swatch.MouseLeftButtonDown += OnExpressionColorClicked;
                values.Children.Add(swatch);
            }
            AddExpressionDetailsRow(
                node.Op == 3 ? "Fallback RGBA" : "Default Value", values);
        }

        if (node.Op is 1 or 2)
        {
            var name = new TextBox
            {
                Text = node.ParameterName,
                Tag = _selectedExpression,
                ToolTip = "Display metadata is stored in the editor sidecar; the stable FNV-1a ID is stored in ACSMAT."
            };
            name.TextChanged += OnExpressionParameterNameChanged;
            AddExpressionDetailsRow("Parameter", name);
            AddExpressionDetailsText("Stable ID", $"0x{node.ParameterId:X8}");
        }

        if (node.Op == 3)
        {
            var slot = new ComboBox { Tag = _selectedExpression };
            for (int i = 0; i < _expressionTextureSlots; ++i)
                slot.Items.Add($"Texture Slot {i}");
            slot.SelectedIndex = Math.Clamp(node.TextureSlot, 0, _expressionTextureSlots - 1);
            slot.SelectionChanged += OnExpressionTextureSlotChanged;
            AddExpressionDetailsRow("Slot", slot);

            var path = new Grid();
            path.ColumnDefinitions.Add(new ColumnDefinition());
            path.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            path.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            path.Children.Add(new TextBlock
            {
                Text = string.IsNullOrWhiteSpace(_expressionTexturePaths[node.TextureSlot])
                    ? "None"
                    : Path.GetFileName(_expressionTexturePaths[node.TextureSlot]),
                Foreground = (Brush)FindResource("TextDim"),
                TextTrimming = TextTrimming.CharacterEllipsis,
                VerticalAlignment = VerticalAlignment.Center,
                ToolTip = _expressionTexturePaths[node.TextureSlot]
            });
            var browse = new Button
            {
                Content = "Browse",
                Padding = new Thickness(7, 2, 7, 2),
                Margin = new Thickness(4, 0, 0, 0),
                Tag = _selectedExpression
            };
            browse.Click += OnExpressionTextureBrowse;
            Grid.SetColumn(browse, 1);
            path.Children.Add(browse);
            var clear = new Button
            {
                Content = "Clear",
                Padding = new Thickness(7, 2, 7, 2),
                Margin = new Thickness(3, 0, 0, 0),
                Tag = _selectedExpression
            };
            clear.Click += OnExpressionTextureClear;
            Grid.SetColumn(clear, 2);
            path.Children.Add(clear);
            AddExpressionDetailsRow("Texture", path);

            var flags = new WrapPanel();
            string[] flagNames = { "Linear", "Clamp U", "Clamp V", "sRGB" };
            for (int bit = 0; bit < flagNames.Length; ++bit)
            {
                var check = new CheckBox
                {
                    Content = flagNames[bit],
                    IsChecked = (node.TextureFlags & (1 << bit)) != 0,
                    Tag = bit,
                    Margin = new Thickness(0, 0, 8, 3)
                };
                check.Checked += OnExpressionTextureFlagChanged;
                check.Unchecked += OnExpressionTextureFlagChanged;
                flags.Children.Add(check);
            }
            AddExpressionDetailsRow("Sampling", flags);
        }

        if (node.Op == 16)
        {
            var component = new ComboBox { Tag = _selectedExpression };
            component.Items.Add("X / R");
            component.Items.Add("Y / G");
            component.Items.Add("Z / B");
            component.Items.Add("W / A");
            component.SelectedIndex = Math.Clamp(node.ComponentIndex, 0, 3);
            component.SelectionChanged += OnExpressionComponentChanged;
            AddExpressionDetailsRow("Component", component);
        }

        int inputs = ExpressionInputCount(node.Op);
        if (inputs > 0)
        {
            ExpressionPropertyPanel.Children.Add(new TextBlock
            {
                Text = "CONNECTIONS",
                Style = (Style)FindResource("MaterialSection")
            });
            for (int input = 0; input < inputs; ++input)
            {
                int source = node.Inputs[input];
                var row = new Grid();
                row.ColumnDefinitions.Add(new ColumnDefinition());
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                row.Children.Add(new TextBlock
                {
                    Text = ValidExpressionIndex(source)
                        ? $"E{source}  {ExpressionOpNames[_expressionNodes[source].Op]} " +
                          $"({ExpressionTypeName(InferExpressionType(source))})"
                        : "Not connected",
                    Foreground = ValidExpressionIndex(source)
                        ? ExpressionTypeBrush(InferExpressionType(source))
                        : (Brush)FindResource("TextDim"),
                    VerticalAlignment = VerticalAlignment.Center,
                    TextTrimming = TextTrimming.CharacterEllipsis
                });
                var disconnect = new Button
                {
                    Content = "Disconnect",
                    IsEnabled = ValidExpressionIndex(source),
                    Padding = new Thickness(6, 1, 6, 1),
                    Tag = new ExpressionInputTag(_selectedExpression, input)
                };
                disconnect.Click += OnExpressionInputDisconnected;
                Grid.SetColumn(disconnect, 1);
                row.Children.Add(disconnect);
                AddExpressionDetailsRow(ExpressionInputName(node.Op, input), row);
            }
        }

        var previewRoot = new Button
        {
            Content = _expressionRoot == _selectedExpression
                ? "Expression Preview Root (Current)"
                : "Use as Expression Preview Root",
            IsEnabled = _expressionRoot != _selectedExpression,
            Padding = new Thickness(8, 4, 8, 4),
            Margin = new Thickness(0, 9, 0, 0),
            ToolTip = "This direct root is for expression inspection only. Slab scalar bindings drive the material."
        };
        previewRoot.Click += OnSetExpressionRoot;
        ExpressionPropertyPanel.Children.Add(previewRoot);
    }

    private void AddExpressionDetailsText(string label, string value) =>
        AddExpressionDetailsRow(label, new TextBlock
        {
            Text = value,
            Foreground = (Brush)FindResource("Text"),
            FontFamily = new FontFamily("Consolas"),
            VerticalAlignment = VerticalAlignment.Center
        });

    private void AddExpressionDetailsRow(string label, FrameworkElement control)
    {
        var row = new Grid { Margin = new Thickness(0, 2, 0, 2) };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(112) });
        row.ColumnDefinitions.Add(new ColumnDefinition());
        row.Children.Add(new TextBlock
        {
            Text = label,
            Style = (Style)FindResource("MaterialFieldLabel")
        });
        Grid.SetColumn(control, 1);
        row.Children.Add(control);
        ExpressionPropertyPanel.Children.Add(row);
    }

    private void OnExpressionDeclaredTypeChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_graphUiSync || sender is not ComboBox { Tag: int index } box ||
            !ValidExpressionIndex(index))
            return;
        ExpressionNode node = _expressionNodes[index];
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Change Shader Expression Type");
        node.DeclaredType = node.Op == 0 ? box.SelectedIndex + 1 : box.SelectedIndex + 2;
        MarkGraphDirty();
        SelectExpressionNode(index);
    }

    private void OnExpressionValueChanged(object sender, TextChangedEventArgs e)
    {
        if (_graphUiSync || sender is not TextBox { Tag: ExpressionValueTag tag } box ||
            !ValidExpressionIndex(tag.Node))
            return;
        if (!float.TryParse(box.Text, NumberStyles.Float, CultureInfo.InvariantCulture,
                out float value) || !float.IsFinite(value))
        {
            box.BorderBrush = (Brush)FindResource("WarnFg");
            return;
        }
        box.ClearValue(Border.BorderBrushProperty);
        ExpressionNode node = _expressionNodes[tag.Node];
        using GraphHistoryScope history = BeginGraphHistoryChange(
            "Edit Shader Expression Value",
            $"expression:{node.StableId}:value:{tag.Lane}");
        node.Value[tag.Lane] = value;
        MarkGraphDirty();
        BuildLiveParameterList();
        RenderGraph();
    }

    private void OnExpressionColorClicked(object sender, MouseButtonEventArgs e)
    {
        if (sender is not Border { Tag: int index } ||
            !ValidExpressionIndex(index))
            return;
        ExpressionNode node = _expressionNodes[index];
        int width = node.Op == 3
            ? 4
            : Math.Max(1, node.DeclaredType == 0
                ? DefaultDeclaredType(node.Op)
                : node.DeclaredType);
        if (width < 3) return;
        Color initial = PickerColor(
            node.Value[0], node.Value[1], node.Value[2],
            width >= 4 ? node.Value[3] : 1);
        if (!ColorPickerDialog.TryPick(
                this, initial, width >= 4, out Color picked))
            return;
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Edit Shader Expression Color");
        node.Value[0] = picked.R / 255f;
        node.Value[1] = picked.G / 255f;
        node.Value[2] = picked.B / 255f;
        if (width >= 4) node.Value[3] = picked.A / 255f;
        MarkGraphDirty();
        SelectExpressionNode(index);
        e.Handled = true;
    }

    private void OnExpressionParameterNameChanged(object sender, TextChangedEventArgs e)
    {
        if (_graphUiSync || sender is not TextBox { Tag: int index } box ||
            !ValidExpressionIndex(index))
            return;
        ExpressionNode node = _expressionNodes[index];
        using GraphHistoryScope history = BeginGraphHistoryChange(
            "Rename Shader Parameter",
            $"expression:{node.StableId}:parameter-name");
        node.ParameterName = (box.Text ?? "").Trim();
        node.ParameterId = ParameterId(node.ParameterName);
        MarkGraphDirty();
        BuildLiveParameterList();
        NodeDetailsText.Text =
            $"{node.ParameterName} uses stable ID 0x{node.ParameterId:X8}. " +
            "The runtime path is override-ready; this editor does not fabricate an instance override.";
        RenderGraph();
    }

    private void OnExpressionTextureSlotChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_graphUiSync || sender is not ComboBox { Tag: int index } box ||
            !ValidExpressionIndex(index))
            return;
        ExpressionNode node = _expressionNodes[index];
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Change Texture Slot");
        node.TextureSlot = Math.Clamp(box.SelectedIndex, 0, _expressionTextureSlots - 1);
        ExpressionNode? shared = null;
        for (int candidateIndex = 0;
             candidateIndex < _expressionNodes.Count;
             ++candidateIndex)
        {
            ExpressionNode candidate = _expressionNodes[candidateIndex];
            if (candidateIndex == index || candidate.Op != 3 ||
                candidate.TextureSlot != node.TextureSlot)
                continue;
            shared = candidate;
            break;
        }
        if (shared != null)
        {
            node.TextureFlags = shared.TextureFlags;
            node.TextureAssetIdLow = shared.TextureAssetIdLow;
            node.TextureAssetIdHigh = shared.TextureAssetIdHigh;
        }
        else
        {
            ulong pathId = AssetId(_expressionTexturePaths[node.TextureSlot]);
            node.TextureAssetIdLow = (uint)pathId;
            node.TextureAssetIdHigh = (uint)(pathId >> 32);
        }
        MarkGraphDirty();
        SelectExpressionNode(index);
    }

    private void OnExpressionTextureBrowse(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: int index } || !ValidExpressionIndex(index))
            return;
        ExpressionNode node = _expressionNodes[index];
        var dialog = new Microsoft.Win32.OpenFileDialog
        {
            Title = $"Choose Texture for Slot {node.TextureSlot}",
            Filter = "Texture files (*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds)|*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds|All files (*.*)|*.*"
        };
        if (dialog.ShowDialog(this) != true) return;
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Assign Shader Texture");
        _expressionTexturePaths[node.TextureSlot] = dialog.FileName;
        ulong id = AssetId(dialog.FileName);
        PropagateTextureSlotState(
            node.TextureSlot, node.TextureFlags, (uint)id, (uint)(id >> 32));
        MarkGraphDirty();
        SelectExpressionNode(index);
    }

    private void OnExpressionTextureClear(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: int index } || !ValidExpressionIndex(index))
            return;
        ExpressionNode node = _expressionNodes[index];
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Clear Shader Texture");
        _expressionTexturePaths[node.TextureSlot] = "";
        PropagateTextureSlotState(node.TextureSlot, node.TextureFlags, 0, 0);
        MarkGraphDirty();
        SelectExpressionNode(index);
    }

    private void OnExpressionTextureFlagChanged(object sender, RoutedEventArgs e)
    {
        if (_graphUiSync || !ValidExpressionIndex(_selectedExpression) ||
            sender is not CheckBox { Tag: int bit } check)
            return;
        ExpressionNode node = _expressionNodes[_selectedExpression];
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Change Texture Sampling");
        if (check.IsChecked == true) node.TextureFlags |= 1 << bit;
        else node.TextureFlags &= ~(1 << bit);
        PropagateTextureSlotState(
            node.TextureSlot, node.TextureFlags,
            node.TextureAssetIdLow, node.TextureAssetIdHigh);
        MarkGraphDirty();
    }

    private void PropagateTextureSlotState(
        int slot, int flags, uint assetLow, uint assetHigh)
    {
        foreach (ExpressionNode texture in _expressionNodes)
        {
            if (texture.Op != 3 || texture.TextureSlot != slot) continue;
            texture.TextureFlags = flags;
            texture.TextureAssetIdLow = assetLow;
            texture.TextureAssetIdHigh = assetHigh;
        }
    }

    private void OnExpressionComponentChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_graphUiSync || sender is not ComboBox { Tag: int index } box ||
            !ValidExpressionIndex(index))
            return;
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Change Component Lane");
        _expressionNodes[index].ComponentIndex = Math.Clamp(box.SelectedIndex, 0, 3);
        MarkGraphDirty();
        RenderGraph();
    }

    private void OnExpressionInputDisconnected(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: ExpressionInputTag tag } ||
            !ValidExpressionIndex(tag.Node))
            return;
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Disconnect Shader Input");
        _expressionNodes[tag.Node].Inputs[tag.Input] = -1;
        MarkGraphDirty();
        SelectExpressionNode(tag.Node);
    }

    private void OnSetExpressionRoot(object sender, RoutedEventArgs e)
    {
        if (!ValidExpressionIndex(_selectedExpression)) return;
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Set Shader Preview Root");
        _expressionRoot = _selectedExpression;
        MarkGraphDirty();
        SelectExpressionNode(_selectedExpression);
    }

    private void OnSlabExpressionDisconnected(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: int scalar } ||
            !ValidNodeIndex(_selectedNode) || scalar < 0 || scalar >= SlabScalarCount)
            return;
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Disconnect Slab Binding");
        _graphNodes[_selectedNode].ExpressionRoots[scalar] = -1;
        MarkGraphDirty();
        SelectGraphNode(_selectedNode);
    }

    private void BuildLiveParameterList()
    {
        _parameters.Clear();
        for (int i = 0; i < _expressionNodes.Count; ++i)
        {
            ExpressionNode node = _expressionNodes[i];
            if (node.Op is not (1 or 2)) continue;
            int width = node.Op == 1 ? 1 :
                Math.Clamp(node.DeclaredType == 0 ? 4 : node.DeclaredType, 2, 4);
            string value = string.Join(", ", node.Value.Take(width)
                .Select(v => v.ToString("0.###", CultureInfo.InvariantCulture)));
            string name = string.IsNullOrWhiteSpace(node.ParameterName)
                ? $"0x{node.ParameterId:X8}"
                : node.ParameterName;
            _parameters.Add(new ParameterEntry(
                name, $"{ExpressionTypeName(width)}  {value}", -1, i));
        }
    }

    private void UpdateGraphStatus()
    {
        if (GraphStatusText == null) return;
        int parameters = _expressionNodes
            .Where(node => node.Op is 1 or 2)
            .Select(node => node.ParameterId)
            .Distinct()
            .Count();
        int textures = _expressionNodes
            .Where(node => node.Op == 3)
            .Select(node => node.TextureSlot)
            .Distinct()
            .Count();
        GraphStatusText.Text =
            $"{_graphNodes.Count}/{_runtimeMaxNodes} closures  |  " +
            $"{_expressionNodes.Count}/{_expressionMaxNodes} shader expressions  |  " +
            $"{parameters}/32 parameter IDs  |  {textures}/{_expressionTextureSlots} texture slots\n" +
            $"Front: {NodeDisplayName(_substrateRoot)}  |  " +
            "Dynamic GPU bindings: direct Front Slab only";
    }

    // Deterministic authoring fixture used by --matshot expr for visual and
    // persistence regression checks. Every node is a real runtime expression.
    internal void BuildExpressionGraphForTest(bool unsupportedOperatorRoot = false)
    {
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Build Shader Expression Fixture");
        _graphNodes.Clear();
        var slab = new GraphNode
        {
            Type = 0,
            Slab = BuildLegacySlab(),
            IsExpanded = true,
            X = 1110,
            Y = 45
        };
        _graphNodes.Add(slab);
        _substrateRoot = 0;

        _expressionNodes.Clear();
        Array.Fill(_expressionTexturePaths, "");
        ExpressionNode Add(int op, double x, double y, int type = 0)
        {
            var node = new ExpressionNode
            {
                Op = op,
                DeclaredType = type == 0 ? DefaultDeclaredType(op) : type,
                X = x,
                Y = y
            };
            _expressionNodes.Add(node);
            return node;
        }

        Add(4, 45, 65);                                           // E0 UV0
        ExpressionNode texture = Add(3, 270, 55, 4);              // E1 texture
        texture.Inputs[0] = 0;
        texture.TextureFlags = 1 | 8;
        texture.Value = new[] { 0.16f, 0.42f, 0.82f, 1.0f };
        ExpressionNode red = Add(16, 505, 35);                    // E2 R
        red.Inputs[0] = 1;
        ExpressionNode green = Add(16, 505, 155);                 // E3 G
        green.Inputs[0] = 1;
        green.ComponentIndex = 1;
        ExpressionNode blue = Add(16, 505, 275);                  // E4 B
        blue.Inputs[0] = 1;
        blue.ComponentIndex = 2;
        Add(5, 45, 280);                                         // E5 Time
        ExpressionNode parameter = Add(1, 270, 300, 1);          // E6 scalar
        parameter.ParameterName = "WaveRoughness";
        parameter.ParameterId = ParameterId(parameter.ParameterName);
        parameter.Value[0] = 0.35f;
        ExpressionNode noise = Add(15, 505, 405);                 // E7 Noise
        noise.Inputs[0] = 5;
        ExpressionNode multiply = Add(9, 745, 325);               // E8 Multiply
        multiply.Inputs[0] = 7;
        multiply.Inputs[1] = 6;

        slab.ExpressionRoots[0] = 1;
        slab.ExpressionRoots[1] = 1;
        slab.ExpressionRoots[2] = 1;
        slab.ExpressionRoots[3] = 2;
        slab.ExpressionRoots[4] = 3;
        slab.ExpressionRoots[5] = 4;
        slab.ExpressionRoots[9] = 8;
        _expressionRoot = 8;

        if (unsupportedOperatorRoot)
        {
            _graphNodes.Add(new GraphNode
            {
                Type = 1,
                InputA = 0,
                Factor = 1,
                X = 1430,
                Y = 320
            });
            _substrateRoot = 1;
        }
        _outputX = unsupportedOperatorRoot ? 1650 : 1450;
        _outputY = 390;
        MarkGraphDirty();
        SelectExpressionNode(1);
        CompileGraph(userInitiated: true);
        OnFitGraphClicked(this, new RoutedEventArgs());
    }

    internal bool SaveExpressionGraphForTest() =>
        SaveRuntimeGraph(showDiagnostics: true) &&
        CompileGraph(userInitiated: true);

    internal void DeleteExpressionForTest(int index)
    {
        if (!ValidExpressionIndex(index)) return;
        SelectExpressionNode(index);
        DeleteSelectedExpression();
        CompileGraph(userInitiated: true);
    }

    internal void TriggerExpressionConnectionErrorForTest(bool cycle)
    {
        if (_expressionNodes.Count < 9) BuildExpressionGraphForTest();
        if (cycle)
            ConnectExpressionInput(8, 7, 0);
        else
            ConnectExpressionInput(2, 1, 0);
    }

    internal bool ConfigureSharedTextureForTest()
    {
        if (_expressionNodes.Count < 9) BuildExpressionGraphForTest();
        using GraphHistoryScope history =
            BeginGraphHistoryChange("Configure Shared Texture Fixture");
        const string testPath = "Assets/QA/shared_substrate_texture.dds";
        _expressionTexturePaths[0] = testPath;
        ulong id = AssetId(testPath);
        PropagateTextureSlotState(0, 1 | 2 | 4 | 8, (uint)id, (uint)(id >> 32));
        var second = new ExpressionNode
        {
            Op = 3,
            DeclaredType = 4,
            TextureSlot = 0,
            TextureFlags = 1 | 2 | 4 | 8,
            TextureAssetIdLow = (uint)id,
            TextureAssetIdHigh = (uint)(id >> 32),
            Inputs = new[] { 0, -1, -1 },
            Value = new[] { 0.5f, 0.5f, 1.0f, 1.0f },
            X = 745,
            Y = 90
        };
        _expressionNodes.Add(second);
        MarkGraphDirty();
        SelectExpressionNode(1);
        OnFitGraphClicked(this, new RoutedEventArgs());
        return SaveExpressionGraphForTest();
    }
}
