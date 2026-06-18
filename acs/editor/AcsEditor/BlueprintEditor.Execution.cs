using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Media;

namespace AcsEditor;

public partial class BlueprintEditor
{
    private void OnRun(object sender, RoutedEventArgs e) { _stepIdx = -1; _debugCurrent = -1; RunGraph(); }

    // ----- ライブデバッグ (実行トレースのステップ再生 + ブレークポイント) -----
    /// <summary>ステップ実行: 初回は実行してトレースを取り、1 ノードずつ進めて現在ノードを強調する。</summary>
    private void OnDebugStep(object sender, RoutedEventArgs e)
    {
        if (_stepIdx < 0) { RunGraph(); _stepIdx = -1; }   // 実行してトレース取得 (RunGraph が _stepIdx をリセットしないよう後で -1)
        _stepIdx++;
        ApplyStep();
    }

    /// <summary>続行: 次のブレークポイント (無ければ末尾) まで一気に進める。</summary>
    private void OnDebugContinue(object sender, RoutedEventArgs e)
    {
        if (_stepIdx < 0) { RunGraph(); _stepIdx = 0; } else _stepIdx++;
        while (_stepIdx < _trace.Count && !_breakpoints.Contains(_trace[_stepIdx])) _stepIdx++;
        ApplyStep();
    }

    private void OnDebugStop(object sender, RoutedEventArgs e)
    {
        _stepIdx = -1; _debugCurrent = -1;
        Rebuild();
        LogSink?.Invoke("デバッグ終了。");
    }

    private void ApplyStep()
    {
        if (_trace.Count == 0) { LogSink?.Invoke("実行トレースがありません。"); _stepIdx = -1; return; }
        if (_stepIdx >= _trace.Count)   // 末尾に到達 → デバッグ終了
        {
            _stepIdx = -1; _debugCurrent = -1; Rebuild();
            LogSink?.Invoke("■ トレース末尾に到達。");
            return;
        }
        _debugCurrent = _trace[_stepIdx];
        var n = NodeById(_debugCurrent);
        LogSink?.Invoke($"⏸ ステップ {_stepIdx + 1}/{_trace.Count}: {(n != null ? n.Title.Trim() : "#" + _debugCurrent)}");
        CenterOnNode(n);
        Rebuild();
    }

    /// <summary>ノードが画面中央に来るようパンする (デバッグの現在ノード追従)。</summary>
    private void CenterOnNode(BpNode? n)
    {
        if (n == null) return;
        double vw = ActualWidth > 0 ? ActualWidth : 800, vh = ActualHeight > 0 ? ActualHeight : 600;
        _pan.X = vw / 2 - _zoom.ScaleX * (n.X + NodeW / 2);
        _pan.Y = vh / 2 - _zoom.ScaleY * (n.Y + NodeHeight(n) / 2);
        UpdateGrid();
    }

    private void ToggleBreakpoint(BpNode n)
    {
        if (!_breakpoints.Add(n.Id)) _breakpoints.Remove(n.Id);
        LogSink?.Invoke(_breakpoints.Contains(n.Id) ? $"● ブレークポイント設定: {n.Title.Trim()}" : $"○ ブレークポイント解除: {n.Title.Trim()}");
        Rebuild();
    }

    /// <summary>
    /// イベントノード (exec 出力を持ち exec 入力を持たないノード) を起点に exec を辿って実行する。
    /// データ入力は «プル評価» で上流をたどる (例: Spawn の spawned → SetPosition の target)。
    /// 個々のノードの作用は今はトレース出力 (将来はエンジンの反射メソッド/サブシステムへ束縛)。
    /// </summary>
    public void RunGraph()
    {
        foreach (var n in _nodes) n.Executed = false;
        _spawnHandles.Clear(); _returns.Clear(); _spawnSeq = 0; _execBudget = 4000;
        _vars.Clear(); _watch.Clear(); _firedOut.Clear(); _showWatch = true;
        _flowState.Clear(); _trace.Clear();
        _callStack.Clear(); _argStack.Clear(); _callResult.Clear();
        foreach (var v in Variables)   // BP 変数を _vars へ種まき (Get Variable が既定値を読める)
            if (!string.IsNullOrWhiteSpace(v.Name)) _vars[v.Name.Trim()] = v.Value ?? "";

        // イベント起点は «関数を展開する前» の Event Graph ノードから集める (Function Entry を auto-run しない)。
        var entries = _nodes
            .Where(n => n.Outputs.Any(p => p.Kind == PinKind.Exec) && !n.Inputs.Any(p => p.Kind == PinKind.Exec)
                     && !n.Title.StartsWith("On Event") && n.Title != "Custom Event"
                     && n.Title != "On Key" && n.Title != "On Overlap")   // 直接呼び出し/入力駆動は自動起動しない
            .OrderBy(n => n.Inherited ? 0 : 1).ThenBy(n => n.Id).ToList();   // 親(継承)→子の順 = Super 相当

        LoadFunctionsForRun();   // Function サブグラフを一時展開 (Call Function から呼べるように)
        Trace($"▶ Blueprint 実行開始 ({entries.Count} イベント)");
        try { foreach (var ev in entries) ExecFrom(ev); }
        finally { RemoveFunctionsAfterRun(); }
        Trace("■ Blueprint 実行終了");
        Rebuild();   // 実行済みノードを枠ハイライト
    }

    /// <summary>全 Function サブグラフを大きな id オフセットで _nodes/_conns へ一時展開し、
    /// 関数名→Function Entry の id を _funcEntry へ記録する (Call Function から呼ぶため)。</summary>
    private void LoadFunctionsForRun()
    {
        _funcEntry.Clear(); _runtimeFuncIds.Clear();
        SyncActiveGraph();
        int idx = 0;
        foreach (var name in _graphOrder)
        {
            if (name == EventGraphName || !_graphIsFunc.Contains(name)) continue;
            int offset = 3_000_000 + (++idx) * 100_000;
            int beforeN = _nodes.Count;
            ParseGraph(_graphTexts.TryGetValue(name, out var b) ? b : "", offset, inherited: false);
            for (int i = beforeN; i < _nodes.Count; i++)
            {
                _runtimeFuncIds.Add(_nodes[i].Id);
                if (_nodes[i].Title == "Function Entry" && !_funcEntry.ContainsKey(name)) _funcEntry[name] = _nodes[i].Id;
            }
        }
    }
    private void RemoveFunctionsAfterRun()
    {
        if (_runtimeFuncIds.Count == 0) return;
        _nodes.RemoveAll(n => _runtimeFuncIds.Contains(n.Id));
        _conns.RemoveAll(c => _runtimeFuncIds.Contains(c.FromNode) || _runtimeFuncIds.Contains(c.ToNode));
        _runtimeFuncIds.Clear(); _funcEntry.Clear();
    }

    private int _lastEnteredPin = -1;   // 直前に発火した接続の «入力ピン index» (Gate の Enter/Open/Close 判定用)

    private void ExecFrom(BpNode n)
    {
        if (_execBudget-- <= 0) { Trace("  … (ステップ上限に達したため停止)"); return; }
        int enteredPin = _lastEnteredPin; _lastEnteredPin = -1;   // どの exec 入力から来たか (この呼び出し用に確定)
        n.Executed = true;
        _trace.Add(n.Id);   // ライブデバッグのステップ再生用に実行順を記録

        // 無効化ノード: 作用をスキップし、最初の exec 出力だけ «素通し» する。
        if (n.Disabled)
        {
            Trace("  → (無効) " + n.Title.Trim());
            for (int po = 0; po < n.Outputs.Count; po++)
                if (n.Outputs[po].Kind == PinKind.Exec) { FireExec(n, po); break; }
            return;
        }

        Trace("  → " + ActionLine(n));

        // Branch は cond を評価し True / False 出力のうち «一方だけ» を発火する。
        if (n.Title.StartsWith("Branch"))
        {
            string want = Truthy(EvalInputByName(n, "cond")) ? "True" : "False";
            FireExecByName(n, want);
            return;
        }
        // Gate: 開いている時だけ Exit を通す。Open/Close/Toggle 入力で状態切替。状態は _flowState。
        if (n.Title == "Gate")
        {
            bool open = _flowState.TryGetValue(n.Id, out var gs) ? gs != 0 : !Truthy(EvalInputByName(n, "Start Closed"));
            string ip = enteredPin >= 0 && enteredPin < n.Inputs.Count ? n.Inputs[enteredPin].Name : "Enter";
            if (ip == "Open") open = true;
            else if (ip == "Close") open = false;
            else if (ip == "Toggle") open = !open;
            else if (open) FireExecByName(n, "Exit");   // Enter
            _flowState[n.Id] = open ? 1 : 0;
            return;
        }
        // DoOnce: 初回だけ Completed を通す。Reset 入力で再武装。
        if (n.Title == "DoOnce")
        {
            string ip = enteredPin >= 0 && enteredPin < n.Inputs.Count ? n.Inputs[enteredPin].Name : "▶";
            if (ip == "Reset") { _flowState[n.Id] = 0; return; }
            bool fired = _flowState.TryGetValue(n.Id, out var ds) && ds != 0;
            if (!fired) { _flowState[n.Id] = 1; FireExecByName(n, "Completed"); }
            return;
        }
        // FlipFlop: A と B を交互に発火。is A を data 出力へ。
        if (n.Title == "FlipFlop")
        {
            bool isA = !(_flowState.TryGetValue(n.Id, out var fs) && fs != 0);   // 初回 = A
            _returns[n.Id] = isA ? "true" : "false";
            FireExecByName(n, isA ? "A" : "B");
            _flowState[n.Id] = isA ? 1 : 0;
            return;
        }
        // Switch on Int: selector に一致する case を、無ければ Default を発火。
        if (n.Title == "Switch on Int")
        {
            int sel = (int)ParseNum(EvalInputByName(n, "selector"));
            bool matched = n.Outputs.Any(p => p.Kind == PinKind.Exec && p.Name == sel.ToString());
            FireExecByName(n, matched ? sel.ToString() : "Default");
            return;
        }
        // ForEach: array(カンマ区切り) を反復し、element/index を出力。終わりに Completed。
        if (n.Title == "ForEach")
        {
            var arr = SplitArray(EvalInputByName(n, "array"));
            for (int i = 0; i < arr.Length; i++)
            {
                if (_execBudget-- <= 0) { Trace("  … (ステップ上限)"); break; }
                _returns[n.Id] = arr[i];               // element (outPin の名前で出し分けは EvalOutput が担当)
                _flowState[n.Id] = i;                  // index
                FireExecByName(n, "Loop Body");
            }
            FireExecByName(n, "Completed");
            return;
        }
        // Cast: object が «それらしい» 値なら Success(+As)、無ければ Failed。
        if (n.Title.StartsWith("Cast"))
        {
            string obj = EvalInputByName(n, "object");
            bool ok = obj.Length > 0 && obj != "(未接続)" && obj != "(なし)";
            if (ok) _returns[n.Id] = obj;
            FireExecByName(n, ok ? "Success" : "Failed");
            return;
        }
        // Delay: 同期シミュレーションでは即時 (実コード生成ではタイマー)。Completed を通す。
        if (n.Title == "Delay")
        {
            FireExecByName(n, "Completed");
            return;
        }
        // Return: data 入力 (戻り値) を呼出元 Call ノードのスロットへ書いて関数を抜ける。
        if (n.Title == "Return")
        {
            if (_callStack.Count > 0)
            {
                int callId = _callStack.Peek(); int r = 0;
                for (int i = 0; i < n.Inputs.Count; i++)
                    if (n.Inputs[i].Kind == PinKind.Data) _callResult[(callId, r++)] = EvalInput(n, i);
            }
            return;   // Return は下流を持たない
        }
        // For Loop: first..last を反復し Loop Body を毎回発火、index を data 出力へ。終わりに Completed。
        if (n.Title == "For Loop")
        {
            int first = (int)ParseNum(EvalInputByName(n, "first"));
            int last  = (int)ParseNum(EvalInputByName(n, "last"));
            for (int i = first; i <= last; i++)
            {
                if (_execBudget-- <= 0) { Trace("  … (ステップ上限)"); break; }
                _returns[n.Id] = i.ToString();   // index 出力 (pull)
                FireExecByName(n, "Loop Body");
            }
            FireExecByName(n, "Completed");
            return;
        }
        // While: cond が真の間 Loop Body を反復。終わりに Completed (budget で無限ループ防止)。
        if (n.Title == "While")
        {
            while (Truthy(EvalInputByName(n, "cond")))
            {
                if (_execBudget-- <= 0) { Trace("  … (ステップ上限)"); break; }
                FireExecByName(n, "Loop Body");
            }
            FireExecByName(n, "Completed");
            return;
        }
        // 通常ノード: 全 exec 出力を上から順に発火 (Sequence 等)。
        for (int po = 0; po < n.Outputs.Count; po++)
            if (n.Outputs[po].Kind == PinKind.Exec)
                FireExec(n, po);
    }

    /// <summary>ノード n の po 番出力 exec ピンに繋がる下流をすべて実行する。</summary>
    private void FireExec(BpNode n, int po)
    {
        bool any = false;
        foreach (var c in _conns)
            if (c.FromNode == n.Id && c.FromPin == po)
            {
                var to = NodeById(c.ToNode);
                if (to != null) { any = true; _lastEnteredPin = c.ToPin; ExecFrom(to); }
            }
        if (any) _firedOut.Add((n.Id, po));   // 発火した exec 出力 → 配線ハイライト
    }

    /// <summary>名前付き exec 出力ピンを発火する (Branch の True/False, Loop Body/Completed 等)。</summary>
    private void FireExecByName(BpNode n, string pinName)
    {
        for (int po = 0; po < n.Outputs.Count; po++)
            if (n.Outputs[po].Kind == PinKind.Exec && n.Outputs[po].Name == pinName)
                FireExec(n, po);
    }

    /// <summary>文字列を真偽として評価 (空/false/0/no/off/未接続=偽、数値は非0=真、他は真)。</summary>
    private static bool Truthy(string s)
    {
        s = (s ?? "").Trim().ToLowerInvariant();
        if (s.Length == 0 || s == "false" || s == "0" || s == "no" || s == "off" || s == "(未接続)") return false;
        if (double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var d)) return d != 0;
        return true;
    }

    /// <summary>a op b を比較して真偽を返す (両方数値なら数値比較、それ以外は ==/!= の文字列比較)。</summary>
    private static bool EvalCompare(string a, string op, string b)
    {
        op = (op ?? "").Trim();
        if (op.Length == 0) op = "==";
        bool na = double.TryParse((a ?? "").Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var da);
        bool nb = double.TryParse((b ?? "").Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var db);
        if (na && nb)
            switch (op)
            {
                case "==": return da == db;
                case "!=": return da != db;
                case "<":  return da <  db;
                case "<=": return da <= db;
                case ">":  return da >  db;
                case ">=": return da >= db;
            }
        string sa = (a ?? "").Trim(), sb = (b ?? "").Trim();
        return op == "!=" ? sa != sb : sa == sb;
    }

    /// <summary>channel に一致する On Event ノードをすべて発火する (Publish からの 1対多 通知)。発火数を返す。</summary>
    private int FireEventSubscribers(string channel)
    {
        channel = (channel ?? "").Trim();
        int count = 0;
        foreach (var ev in _nodes.Where(x => x.Title.StartsWith("On Event")).ToList())
            if (EvalInputByName(ev, "channel").Trim() == channel) { ExecFrom(ev); count++; }
        return count;
    }

    private string ActionLine(BpNode n)
    {
        string t = n.Title;
        if (t.StartsWith("On Event")) return $"イベント On Event(channel={EvalInputByName(n, "channel")})";
        if (t.StartsWith("On ") || t.StartsWith("Event")) return "イベント " + t.Trim();
        if (t.StartsWith("Spawn"))
        {
            string path = EvalInputByName(n, "path");
            string pos  = EvalInputByName(n, "pos");
            string? newId = SpawnPrefab?.Invoke(path, pos);   // 実プレハブを生成し新ノード id を得る
            if (newId != null) _returns[n.Id] = newId;        // spawned 出力 = 実ノード id (下流へ流れる)
            string handle = newId ?? SpawnHandle(n);
            return $"Spawn Prefab(path={path}, pos={pos}) ⇒ {handle}{(newId != null ? " [生成 OK]" : "")}";
        }
        if (t.StartsWith("Set Position"))
        {
            string target = EvalInputByName(n, "target"), x = EvalInputByName(n, "x"), y = EvalInputByName(n, "y");
            string? r = BuiltinOp?.Invoke("SetPosition", new[] { target, x, y });   // 実ノードへ適用 (永続)
            return $"Set Position(target={target}, x={x}, y={y}) [{(r != null ? "適用 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Get Position"))
        {
            string target = EvalInputByName(n, "target");
            string? r = BuiltinOp?.Invoke("GetPosition", new[] { target });
            if (r != null) _returns[n.Id] = r;   // "x,y" を pos 出力→下流へ
            return $"Get Position(target={target}) ⇒ {r ?? "対象なし"}";
        }
        if (t.StartsWith("Destroy"))
        {
            string target = EvalInputByName(n, "target");
            string? r = BuiltinOp?.Invoke("Destroy", new[] { target });   // 実ノードを削除
            return $"Destroy(target={target}) [{(r != null ? "削除 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Set Color"))
        {
            string target = EvalInputByName(n, "target");
            string cr = EvalInputByName(n, "r"), cg = EvalInputByName(n, "g"), cb = EvalInputByName(n, "b");
            string? r = BuiltinOp?.Invoke("SetColor", new[] { target, cr, cg, cb });
            return $"Set Color(target={target}, r={cr}, g={cg}, b={cb}) [{(r != null ? "適用 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Set Visible"))
        {
            string target = EvalInputByName(n, "target"), v = EvalInputByName(n, "visible");
            string? r = BuiltinOp?.Invoke("SetVisible", new[] { target, v });
            return $"Set Visible(target={target}, visible={v}) [{(r != null ? "適用 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Set Scale"))
        {
            string target = EvalInputByName(n, "target"), a = EvalInputByName(n, "sx"), b = EvalInputByName(n, "sy");
            string? r = BuiltinOp?.Invoke("SetScale", new[] { target, a, b });
            return $"Set Scale(target={target}, sx={a}, sy={b}) [{(r != null ? "適用 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Set Rotation"))
        {
            string target = EvalInputByName(n, "target"), d = EvalInputByName(n, "deg");
            string? r = BuiltinOp?.Invoke("SetRotation", new[] { target, d });
            return $"Set Rotation(target={target}, deg={d}) [{(r != null ? "適用 OK" : "対象なし")}]";
        }
        if (t.StartsWith("Reparent"))
        {
            string target = EvalInputByName(n, "target"), p = EvalInputByName(n, "parent");
            string? r = BuiltinOp?.Invoke("Reparent", new[] { target, p });
            return $"Reparent(target={target}, parent={p}) [{(r != null ? "OK" : "対象なし")}]";
        }
        if (t.StartsWith("Set Variable"))
        {
            string name = EvalInputByName(n, "name").Trim(), val = EvalInputByName(n, "value");
            if (name.Length > 0) _vars[name] = val;
            return $"Set Variable({name} = {val})";
        }
        if (t.StartsWith("Publish"))
        {
            string ch = EvalInputByName(n, "channel");
            if (ch == "(なし)") return t.Trim();   // channel 入力の無い旧 Publish ノードは従来通り
            int fired = FireEventSubscribers(ch);
            return $"Publish Event(channel={ch}) → {fired} 購読";
        }
        if (t == "Custom Event") return $"Custom Event({EvalInputByName(n, "name")})";
        if (t == "Call Function")   // Function サブグラフ / 同名 Custom Event を同期呼び出し (型付き引数/戻り値)
        {
            string fn = EvalInputByName(n, "name").Trim();
            int fired = 0;
            if (_funcEntry.TryGetValue(fn, out int entryId))   // Function サブグラフを実行
            {
                var ent = NodeById(entryId);
                if (ent != null)
                {
                    // «引数» = name の後ろの data 入力 (idx 2..) を評価してフレームへ積む。
                    var args = new List<string>();
                    for (int i = 2; i < n.Inputs.Count; i++) if (n.Inputs[i].Kind == PinKind.Data) args.Add(EvalInput(n, i));
                    _argStack.Push(args.ToArray()); _callStack.Push(n.Id);
                    ExecFrom(ent);
                    _callStack.Pop(); _argStack.Pop();
                    fired++;
                }
            }
            foreach (var ce in _nodes.Where(x => x.Title == "Custom Event").ToList())   // 同名 Custom Event も発火 (後方互換)
                if (EvalInputByName(ce, "name").Trim() == fn) { ExecFrom(ce); fired++; }
            return $"Call({fn}) → {fired} 関数";
        }
        if (t == "Function Entry") return $"Function Entry";
        if (t == "Return")         return $"Return";
        if (t == "For Loop") return $"For Loop(first={EvalInputByName(n, "first")}, last={EvalInputByName(n, "last")})";
        if (t == "While")    return $"While(cond={EvalInputByName(n, "cond")})";
        if (t.StartsWith("Print"))    return "Print: " + EvalInputByName(n, "text");
        if (t.StartsWith("Compare"))
        {
            string ca = EvalInputByName(n, "a"), cop = EvalInputByName(n, "op"), cb = EvalInputByName(n, "b");
            bool res = EvalCompare(ca, cop, cb);
            _returns[n.Id] = res ? "true" : "false";   // result 出力 → 下流 (Branch.cond 等) へ
            return $"Compare({ca} {cop} {cb}) ⇒ {(res ? "true" : "false")}";
        }
        if (t.StartsWith("Branch"))
        {
            string cv = EvalInputByName(n, "cond");
            return $"Branch(cond={cv}) → {(Truthy(cv) ? "True" : "False")}";
        }
        if (t.StartsWith("Sequence")) return "Sequence";
        if (n.Inputs.Any(p => p.Name == "target") && t.Contains('.'))   // 反射関数ノード "Owner.Method"
        {
            var parts = t.Split('.', 2);
            string owner = parts[0], method = parts[1];
            string target = EvalInputByName(n, "target");
            bool hasArg = n.Inputs.Any(p => p.Name == "arg");
            string arg = hasArg ? EvalInputByName(n, "arg") : "";
            string? ret = InvokeMethod?.Invoke(owner, method, target, arg);   // target ノード (無指定なら選択) へ実呼出
            bool ok = ret != null;
            if (ok && ret!.Length > 0) _returns[n.Id] = ret;                  // 戻り値を data 出力プル用に保持
            string argStr = hasArg ? $", arg={arg}" : "";
            string retStr = ok && ret!.Length > 0 ? $" = {ret}" : "";
            return $"Call {t}(target={target}{argStr}){retStr} [{(ok ? "実呼出 OK" : "未束縛")}]";
        }
        return t;
    }

    private string SpawnHandle(BpNode n)
    {
        if (!_spawnHandles.TryGetValue(n.Id, out var h)) { h = "spawned#" + (++_spawnSeq); _spawnHandles[n.Id] = h; }
        return h;
    }

    private string EvalInputByName(BpNode n, string pinName)
    {
        int idx = n.Inputs.FindIndex(p => p.Name == pinName);
        return idx < 0 ? "(なし)" : EvalInput(n, idx);
    }

    /// <summary>入力ピンの値を評価する: 接続があれば上流出力、無ければ定数、それも無ければ印。</summary>
    private string EvalInput(BpNode n, int inPin)
    {
        foreach (var c in _conns)
            if (c.ToNode == n.Id && c.ToPin == inPin)
            {
                var from = NodeById(c.FromNode);
                if (from != null) return EvalOutput(from, c.FromPin);
            }
        if (n.Literals.TryGetValue(inPin, out var lit) && lit.Length > 0) return lit;   // 定数
        return "(未接続)";
    }

    private string EvalOutput(BpNode from, int outPin)
    {
        string val = EvalOutputRaw(from, outPin);
        if (_showWatch && outPin >= 0 && outPin < from.Outputs.Count && from.Outputs[outPin].Kind == PinKind.Data)
            _watch[(from.Id, outPin)] = val;   // 値ウォッチ (実行後に出力ピン近傍へ表示)
        return val;
    }

    private string EvalOutputRaw(BpNode from, int outPin)
    {
        // pure 計算ノード (Add/Make Vector/To Float 等) は要求時に評価する。
        var pure = ComputePure(from, outPin);
        if (pure != null) return pure;
        if (from.Title.StartsWith("Reroute")) return EvalInputByName(from, "in");   // 配線中継 = 入力素通し
        // Function Entry の data 出力 (引数) = 現フレームの引数値。
        if (from.Title == "Function Entry" && outPin >= 1 && _argStack.Count > 0)
        {
            var a = _argStack.Peek(); int pi = outPin - 1;
            return pi >= 0 && pi < a.Length ? a[pi] : "";
        }
        // Call Function の data 出力 (戻り値) = Return が書いた値。
        if (from.Title == "Call Function" && outPin >= 1)
            return _callResult.TryGetValue((from.Id, outPin - 1), out var cr) ? cr : "";
        // ForEach は element / index の 2 つの data 出力を別管理する。
        if (from.Title == "ForEach")
        {
            string pn = outPin >= 0 && outPin < from.Outputs.Count ? from.Outputs[outPin].Name : "";
            if (pn == "index")   return _flowState.TryGetValue(from.Id, out var ix) ? ix.ToString() : "0";
            if (pn == "element") return _returns.TryGetValue(from.Id, out var el) ? el : "";
        }
        // Get Variable は要求時に変数ストアから読む (pure ノード = exec で実行しない)。
        if (from.Title.StartsWith("Get Variable"))
            return _vars.TryGetValue(EvalInputByName(from, "name").Trim(), out var gv) ? gv : "";
        // Get Self は «self» マーカーを返す。MainWindow.ResolveTarget が実行中インスタンス(_bpSelfId)へ解決する。
        if (from.Title.StartsWith("Get Self")) return "self";
        // 関数ノードの data 出力 = 実行時に得た戻り値 (キャッシュ) を返す。
        if (outPin >= 0 && outPin < from.Outputs.Count && from.Outputs[outPin].Kind == PinKind.Data
            && _returns.TryGetValue(from.Id, out var rv)) return rv;
        if (from.Title.StartsWith("Spawn")) return SpawnHandle(from);
        string pin = outPin >= 0 && outPin < from.Outputs.Count ? from.Outputs[outPin].Name : "out";
        return $"{from.Title.Trim()}.{pin}";
    }

    private static double ParseNum(string s) => double.TryParse((s ?? "").Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : 0;
    private static string Fmt(double d) => d.ToString("0.######", CultureInfo.InvariantCulture);
    /// <summary>カンマ区切りの «配列» 文字列を要素配列へ (空/印は除外)。</summary>
    private static string[] SplitArray(string s) => (s ?? "").Split(',')
        .Select(x => x.Trim()).Where(x => x.Length > 0 && x != "(未接続)" && x != "(なし)").ToArray();

    /// <summary>pure 計算ノード (演算/論理/ベクトル/変換) を評価する。非対象なら null。
    /// 反射メソッド名 ("Owner.Add" 等) との誤衝突を避けるため «厳密一致» で判定する。</summary>
    private string? ComputePure(BpNode from, int outPin)
    {
        double A() => ParseNum(EvalInputByName(from, "a"));
        double B() => ParseNum(EvalInputByName(from, "b"));
        double V() => ParseNum(EvalInputByName(from, "value"));
        string Clean(string s) => (s == "(未接続)" || s == "(なし)") ? "" : s;
        (double x, double y) V2Of(string name) { var p = EvalInputByName(from, name).Split(','); return (p.Length > 0 ? ParseNum(p[0]) : 0, p.Length > 1 ? ParseNum(p[1]) : 0); }
        switch (from.Title)
        {
            case "Add":      return Fmt(A() + B());
            case "Subtract": return Fmt(A() - B());
            case "Multiply": return Fmt(A() * B());
            case "Divide":   { double b = B(); return Fmt(b == 0 ? 0 : A() / b); }
            case "Modulo":   { double b = B(); return Fmt(b == 0 ? 0 : A() % b); }
            case "And": return (Truthy(EvalInputByName(from, "a")) && Truthy(EvalInputByName(from, "b"))) ? "true" : "false";
            case "Or":  return (Truthy(EvalInputByName(from, "a")) || Truthy(EvalInputByName(from, "b"))) ? "true" : "false";
            case "Not": return Truthy(EvalInputByName(from, "in")) ? "false" : "true";
            case "Make Vector": return $"{Fmt(ParseNum(EvalInputByName(from, "x")))},{Fmt(ParseNum(EvalInputByName(from, "y")))}";
            case "Break Vector":
            {
                var v = EvalInputByName(from, "in").Split(',');
                double x = v.Length > 0 ? ParseNum(v[0]) : 0, y = v.Length > 1 ? ParseNum(v[1]) : 0;
                return Fmt(outPin == 0 ? x : y);
            }
            case "To String": { var s = EvalInputByName(from, "in"); return s == "(未接続)" ? "" : s; }
            case "To Float":  return Fmt(ParseNum(EvalInputByName(from, "in")));
            case "To Int":    return ((long)ParseNum(EvalInputByName(from, "in"))).ToString(CultureInfo.InvariantCulture);
            case "To Bool":   return Truthy(EvalInputByName(from, "in")) ? "true" : "false";
            // 配列 (カンマ区切り文字列で表現)。
            case "Make Array":
            {
                var items = new[] { "a", "b", "c" }.Select(p => EvalInputByName(from, p)).Where(s => s != "(未接続)" && s != "(なし)" && s.Length > 0);
                return string.Join(",", items);
            }
            case "Array Length": return SplitArray(EvalInputByName(from, "array")).Length.ToString(CultureInfo.InvariantCulture);
            case "Array Get":    { var a = SplitArray(EvalInputByName(from, "array")); int i = (int)ParseNum(EvalInputByName(from, "index")); return i >= 0 && i < a.Length ? a[i] : ""; }
            case "Array Add":    { var a = SplitArray(EvalInputByName(from, "array")).ToList(); var e = EvalInputByName(from, "element"); if (e != "(未接続)" && e != "(なし)" && e.Length > 0) a.Add(e); return string.Join(",", a); }
            case "Select":       return Truthy(EvalInputByName(from, "pick")) ? EvalInputByName(from, "a") : EvalInputByName(from, "b");
            // Compare は exec でも使うが、result は pull でも評価できるようにする (pure 兼用)。
            case "Compare":      return EvalCompare(EvalInputByName(from, "a"), EvalInputByName(from, "op"), EvalInputByName(from, "b")) ? "true" : "false";
            // 数学 (単項)。入力は "value"。
            case "Abs":      return Fmt(Math.Abs(V()));
            case "Negate":   return Fmt(-V());
            case "Sqrt":     { double v = V(); return Fmt(v < 0 ? 0 : Math.Sqrt(v)); }
            case "Floor":    return Fmt(Math.Floor(V()));
            case "Ceil":     return Fmt(Math.Ceiling(V()));
            case "Round":    return Fmt(Math.Round(V(), MidpointRounding.AwayFromZero));
            case "Sign":     return Fmt(Math.Sign(V()));
            case "Sin":      return Fmt(Math.Sin(V()));
            case "Cos":      return Fmt(Math.Cos(V()));
            // 数学 (二項/多項)。
            case "Min":      return Fmt(Math.Min(A(), B()));
            case "Max":      return Fmt(Math.Max(A(), B()));
            case "Power":    return Fmt(Math.Pow(ParseNum(EvalInputByName(from, "base")), ParseNum(EvalInputByName(from, "exp"))));
            case "Clamp":    { double v = V(), lo = ParseNum(EvalInputByName(from, "min")), hi = ParseNum(EvalInputByName(from, "max")); return Fmt(Math.Min(Math.Max(v, lo), hi)); }
            case "Lerp":     { double a = A(), b = B(), t = ParseNum(EvalInputByName(from, "t")); return Fmt(a + (b - a) * t); }
            // 比較 (各演算子ごとの独立ノード)。
            case "Greater":       return A() >  B() ? "true" : "false";
            case "Less":          return A() <  B() ? "true" : "false";
            case "Greater Equal": return A() >= B() ? "true" : "false";
            case "Less Equal":    return A() <= B() ? "true" : "false";
            case "Equal":         return A() == B() ? "true" : "false";
            case "Not Equal":     return A() != B() ? "true" : "false";
            // 数学 追加。
            case "Tan":   return Fmt(Math.Tan(V()));
            case "Atan2": return Fmt(Math.Atan2(ParseNum(EvalInputByName(from, "y")), ParseNum(EvalInputByName(from, "x"))));
            case "Exp":   return Fmt(Math.Exp(V()));
            case "Log":   { double v = V(); return Fmt(v <= 0 ? 0 : Math.Log(v)); }
            case "Deg To Rad": return Fmt(ParseNum(EvalInputByName(from, "deg")) * Math.PI / 180.0);
            case "Rad To Deg": return Fmt(ParseNum(EvalInputByName(from, "rad")) * 180.0 / Math.PI);
            case "Map Range":  { double v = V(), i0 = ParseNum(EvalInputByName(from, "inMin")), i1 = ParseNum(EvalInputByName(from, "inMax")), o0 = ParseNum(EvalInputByName(from, "outMin")), o1 = ParseNum(EvalInputByName(from, "outMax")); double t = i1 == i0 ? 0 : (v - i0) / (i1 - i0); return Fmt(o0 + (o1 - o0) * t); }
            case "Move Towards": { double c = ParseNum(EvalInputByName(from, "current")), tg = ParseNum(EvalInputByName(from, "target")), st = ParseNum(EvalInputByName(from, "step")); double d = tg - c; return Fmt(Math.Abs(d) <= st ? tg : c + Math.Sign(d) * st); }
            case "Wrap":     { double v = V(), lo = ParseNum(EvalInputByName(from, "min")), hi = ParseNum(EvalInputByName(from, "max")); double r = hi - lo; return Fmt(r <= 0 ? lo : lo + (((v - lo) % r) + r) % r); }
            case "PingPong": { double t = ParseNum(EvalInputByName(from, "t")), len = ParseNum(EvalInputByName(from, "length")); if (len <= 0) return "0"; double m = ((t % (2 * len)) + 2 * len) % (2 * len); return Fmt(m <= len ? m : 2 * len - m); }
            case "SmoothStep": { double a = A(), b = B(), t = ParseNum(EvalInputByName(from, "t")); double x = Math.Min(Math.Max(t, 0), 1); return Fmt(a + (b - a) * (x * x * (3 - 2 * x))); }
            // ベクトル (Vector=Vec2 / Vector3=Vec3。カンマ区切り文字列で表現)。
            case "Make Vector3": return $"{Fmt(ParseNum(EvalInputByName(from, "x")))},{Fmt(ParseNum(EvalInputByName(from, "y")))},{Fmt(ParseNum(EvalInputByName(from, "z")))}";
            case "Break Vector3": { var v = EvalInputByName(from, "in").Split(','); double x = v.Length > 0 ? ParseNum(v[0]) : 0, y = v.Length > 1 ? ParseNum(v[1]) : 0, z = v.Length > 2 ? ParseNum(v[2]) : 0; return Fmt(outPin == 0 ? x : outPin == 1 ? y : z); }
            case "To Vector3": { var (vx, vy) = V2Of("in"); return $"{Fmt(vx)},{Fmt(vy)},0"; }
            case "To Vector2": { var (vx, vy) = V2Of("in"); return $"{Fmt(vx)},{Fmt(vy)}"; }
            case "Vector Add":      { var (ax, ay) = V2Of("a"); var (bx, by) = V2Of("b"); return $"{Fmt(ax + bx)},{Fmt(ay + by)}"; }
            case "Vector Subtract": { var (ax, ay) = V2Of("a"); var (bx, by) = V2Of("b"); return $"{Fmt(ax - bx)},{Fmt(ay - by)}"; }
            case "Vector Scale":    { var (vx, vy) = V2Of("v"); double s = ParseNum(EvalInputByName(from, "s")); return $"{Fmt(vx * s)},{Fmt(vy * s)}"; }
            case "Vector Length":   { var (vx, vy) = V2Of("v"); return Fmt(Math.Sqrt(vx * vx + vy * vy)); }
            case "Vector Distance": { var (ax, ay) = V2Of("a"); var (bx, by) = V2Of("b"); return Fmt(Math.Sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by))); }
            case "Vector Dot":      { var (ax, ay) = V2Of("a"); var (bx, by) = V2Of("b"); return Fmt(ax * bx + ay * by); }
            case "Vector Normalize":{ var (vx, vy) = V2Of("v"); double l = Math.Sqrt(vx * vx + vy * vy); return l == 0 ? "0,0" : $"{Fmt(vx / l)},{Fmt(vy / l)}"; }
            // 文字列。
            case "Append":      return Clean(EvalInputByName(from, "a")) + Clean(EvalInputByName(from, "b"));
            case "String Length": return Clean(EvalInputByName(from, "in")).Length.ToString(CultureInfo.InvariantCulture);
            case "To Upper":    return Clean(EvalInputByName(from, "in")).ToUpperInvariant();
            case "To Lower":    return Clean(EvalInputByName(from, "in")).ToLowerInvariant();
            case "Contains":    return Clean(EvalInputByName(from, "in")).Contains(Clean(EvalInputByName(from, "sub"))) ? "true" : "false";
            case "Replace":     return Clean(EvalInputByName(from, "in")).Replace(Clean(EvalInputByName(from, "from")), Clean(EvalInputByName(from, "to")));
            case "Substring":   { var s = Clean(EvalInputByName(from, "in")); int st = (int)ParseNum(EvalInputByName(from, "start")), ct = (int)ParseNum(EvalInputByName(from, "count")); st = Math.Min(Math.Max(st, 0), s.Length); ct = Math.Min(Math.Max(ct, 0), s.Length - st); return s.Substring(st, ct); }
            // 乱数 (デバッグ実行は固定シード)。
            case "Random Float": { double lo = ParseNum(EvalInputByName(from, "min")), hi = ParseNum(EvalInputByName(from, "max")); return Fmt(lo + _rng.NextDouble() * (hi - lo)); }
            case "Random Int":   { int lo = (int)ParseNum(EvalInputByName(from, "min")), hi = (int)ParseNum(EvalInputByName(from, "max")); return (hi <= lo ? lo : _rng.Next(lo, hi + 1)).ToString(CultureInfo.InvariantCulture); }
            case "Random Bool":  return _rng.Next(2) == 0 ? "false" : "true";
            // 時間 (デバッグ実行は代表値。codegen は実 dt/時刻)。
            case "Get Delta Time": return "0.016";
            case "Get Time":       return "0";
        }
        return null;
    }
}
