using System;
using System.Globalization;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace AcsEditor;

/// <summary>
/// .acsmat の Substrate closure DAG、Slab物性、トゥーンおよびポストプロセスを編集する。
/// closure topology と物性値は .acsmat が正本、ノード位置・ズーム等の表示状態だけを
/// .graph.json に分離する。旧 metallic/roughness アセットは読み込み時に物理 Slab graph
/// としてプレビューし、明示的な保存時にのみ Substrate 正本へ変換する。
/// </summary>
public partial class MaterialEditorWindow : Window
{
    private readonly Func<IntPtr> _engineProvider;
    private string _path;
    private bool _loading = true;  // XAML パース中 (非0 Minimum のスライダ強制で発火するハンドラ) を抑止
    private bool _syncing;     // スライダ ↔ テキストの相互更新ループ防止
    private int  _kind;        // 0 = Lit/PBR, 1 = Effect
    private int  _shadingMode; // 0 = PBR, 1 = Toon (Lit 内のサブモード)
    private string _albedoPath = "";
    private string _normalPath = "";
    private readonly float[] _toonSpec = { 1, 1, 1 };  // ハイライト色 (UI 無し: ロード値を保持)
    private readonly DispatcherTimer _previewTimer = new();
    private readonly MaterialPreviewPipeline _previewPipeline = new();
    private bool _closeInProgress;
    private bool _lifetimeEnded;
    private bool _previewPipelineDisposed;
    private bool _previewTimerSubscribed;
    private bool _assetPathAvailable = true;
    private bool _assetPathMutationSuspended;

    public MaterialEditorWindow(IntPtr engine, string acsmatPath)
        : this(() => engine, acsmatPath)
    {
    }

    internal MaterialEditorWindow(EngineViewport viewport, string acsmatPath)
        : this(() => viewport.Engine, acsmatPath)
    {
    }

    private MaterialEditorWindow(Func<IntPtr> engineProvider, string acsmatPath)
    {
        InitializeComponent();
        _engineProvider = engineProvider;
        _path = acsmatPath;
        FileLabel.Text = Path.GetFileName(acsmatPath);
        _previewTimer.Interval = TimeSpan.FromMilliseconds(180);
        _previewTimer.Tick += OnPreviewTimerTick;
        _previewTimerSubscribed = true;

        foreach (string n in EngineInterop.MaterialEffectNames())
            EffectBox.Items.Add(n);

        Load();
        InitializeGraphEditor();
    }

    internal string? CurrentAssetPath => _assetPathAvailable ? _path : null;

    private bool CanAccessAssetPath =>
        _assetPathAvailable && !_assetPathMutationSuspended && !_lifetimeEnded;

    internal bool SuspendForAssetPathMutation()
    {
        if (!CanAccessAssetPath || !IsEnabled) return false;
        _assetPathMutationSuspended = true;
        _previewTimer.Stop();
        _previewPipeline.CancelPending();
        IsEnabled = false;
        return true;
    }

    internal void ResumeAfterAssetPathMutation()
    {
        if (!_assetPathMutationSuspended) return;
        _assetPathMutationSuspended = false;
        if (!_assetPathAvailable || _lifetimeEnded) return;
        IsEnabled = true;
        RefreshPreview();
    }

    internal void DetachFromAssetPath(string status)
    {
        // Invalidate the path first. Even if a WPF lifetime callback fails below, every
        // save/reload path remains gated and cannot recreate the old asset or sidecar.
        _assetPathAvailable = false;
        _assetPathMutationSuspended = false;
        try { _previewTimer.Stop(); }
        catch { }
        try { _previewPipeline.CancelPending(); }
        catch { }
        try { IsEnabled = false; }
        catch { }
        try { StatusText.Text = status; }
        catch { }
        if (_lifetimeEnded) return;
        try { Close(); }
        catch { }
    }

    internal bool ApplyAssetPathsChanged(AssetPathsChangedEventArgs change)
    {
        ArgumentNullException.ThrowIfNull(change);
        if (!_assetPathAvailable || _lifetimeEnded) return false;
        if (change.TryRemapPath(_path, out string remappedPath))
        {
            _path = remappedPath;
            FileLabel.Text = Path.GetFileName(remappedPath);
            Title = "ACS Material Editor - " + Path.GetFileName(remappedPath);
            return true;
        }
        if (!change.IsDeletedPath(_path)) return false;

        DetachFromAssetPath("Asset deleted - editor detached");
        return true;
    }

    private IntPtr LiveEngine
    {
        get
        {
            if (_closeInProgress || !CanAccessAssetPath || !IsLoaded) return IntPtr.Zero;
            try { return _engineProvider(); }
            catch { return IntPtr.Zero; }
        }
    }

    private void ReloadMaterialInViewport()
    {
        if (!CanAccessAssetPath) return;
        IntPtr engine = LiveEngine;
        if (engine != IntPtr.Zero)
            EngineInterop.acs_editor_reload_material(engine, _path);
    }

    private async void OnPreviewTimerTick(object? sender, EventArgs e)
    {
        _previewTimer.Stop();
        if (!_closeInProgress && CanAccessAssetPath && IsLoaded)
            await RenderPreviewNowAsync();
    }

    private void OnMaterialEditorLoaded(object sender, RoutedEventArgs e)
    {
        if (!CanAccessAssetPath) return;
        _closeInProgress = false;
        RefreshPreview();
    }

    private void BeginCloseAttempt()
    {
        _closeInProgress = true;
        _previewTimer.Stop();
        _previewPipeline.CancelPending();
    }

    private void CancelCloseAttempt()
    {
        if (_lifetimeEnded) return;
        _closeInProgress = false;
        RefreshPreview();
    }

    private void OnMaterialEditorClosed(object? sender, EventArgs e) =>
        EndMaterialEditorLifetime();

    private void OnMaterialEditorUnloaded(object sender, RoutedEventArgs e) =>
        EndMaterialEditorLifetime();

    private void EndMaterialEditorLifetime()
    {
        _closeInProgress = true;
        _lifetimeEnded = true;
        _previewTimer.Stop();
        if (!_previewPipelineDisposed)
        {
            _previewPipeline.Dispose();
            _previewPipelineDisposed = true;
        }
        if (_previewTimerSubscribed)
        {
            _previewTimer.Tick -= OnPreviewTimerTick;
            _previewTimerSubscribed = false;
        }
    }

    // ファイルから読み込んで UI に反映 (両種別ぶん読む)。
    private void Load()
    {
        _loading = true;
        _kind = EngineInterop.acs_editor_material_kind(_path);

        // --- 効果ブロック ---
        var color = new float[4] { 0, 0, 0, 1 };
        var nameBuf = new byte[64];
        int ok = EngineInterop.acs_editor_material_load(_path,
            out int effect, out float strength, out float p0, out float p1, out float p2,
            color, out int animated, nameBuf, nameBuf.Length);
        if (ok == 0) { _loading = false; return; }
        int z = Array.IndexOf(nameBuf, (byte)0); if (z < 0) z = nameBuf.Length;
        string name = System.Text.Encoding.UTF8.GetString(nameBuf, 0, z);
        NameBox.Text = string.IsNullOrEmpty(name) ? Path.GetFileNameWithoutExtension(_path) : name;
        EffectBox.SelectedIndex = (effect >= 0 && effect < EffectBox.Items.Count) ? effect : 0;
        Strength.Text = F(strength);
        P0.Text = F(p0); P1.Text = F(p1); P2.Text = F(p2);
        ColR.Text = F(color[0]); ColG.Text = F(color[1]); ColB.Text = F(color[2]); ColA.Text = F(color[3]);
        AnimatedBox.IsChecked = animated != 0;

        // --- PBR ブロック ---
        var baseCol = new float[4] { 1, 1, 1, 1 };
        var emis = new float[3] { 0, 0, 0 };
        var albedoBuf = new byte[260];
        var normalBuf = new byte[260];
        EngineInterop.acs_editor_material_load_pbr(_path, baseCol, out float metallic, out float roughness,
            emis, out float emStr, out float normalStr, out float ao,
            albedoBuf, albedoBuf.Length, normalBuf, normalBuf.Length);
        BcR.Text = F(baseCol[0]); BcG.Text = F(baseCol[1]); BcB.Text = F(baseCol[2]); BcA.Text = F(baseCol[3]);
        Metallic.Text = F(metallic); Roughness.Text = F(roughness);
        NormalStr.Text = F(normalStr); Ao.Text = F(ao);
        EmR.Text = F(emis[0]); EmG.Text = F(emis[1]); EmB.Text = F(emis[2]); EmStr.Text = F(emStr);
        _albedoPath = Cstr(albedoBuf); _normalPath = Cstr(normalBuf);
        AlbedoLabel.Text = TexLabel(_albedoPath); NormalLabel.Text = TexLabel(_normalPath);

        // --- トゥーン (シェーディングモード + UTS 風項目) ---
        var s1 = new float[3]; var s2 = new float[3]; var rim = new float[3]; var spec = new float[3];
        EngineInterop.acs_editor_material_load_toon(_path, out int sMode, s1, out float t1, s2, out float t2,
            rim, out float rimPow, spec, out float specTh, out float soft);
        _shadingMode = sMode;
        _toonSpec[0] = spec[0]; _toonSpec[1] = spec[1]; _toonSpec[2] = spec[2];
        S1R.Text = F(s1[0]); S1G.Text = F(s1[1]); S1B.Text = F(s1[2]); S1Thr.Text = F(t1);
        S2R.Text = F(s2[0]); S2G.Text = F(s2[1]); S2B.Text = F(s2[2]); S2Thr.Text = F(t2);
        RimR.Text = F(rim[0]); RimG.Text = F(rim[1]); RimB.Text = F(rim[2]); RimPow.Text = F(rimPow);
        SpecThr.Text = F(specTh); Soft.Text = F(soft);
        ShadeBox.SelectedIndex = _shadingMode;
        ApplyShadeMode();

        // --- Substrate 拡張 (clearcoat/異方/鏡面/シーン/SSS) ---
        var shc = new float[3]; var ssc = new float[3];
        EngineInterop.acs_editor_material_load_pbr_ext(_path,
            out float cc, out float ccR, out float aniso, out float specLvl, out float specTint,
            out float shn, out float shnR, shc, out float sss, ssc);
        ClrCoat.Text = F(cc); CoatRough.Text = F(ccR); Aniso.Text = F(aniso);
        SpecLvl.Text = F(specLvl); SpecTint.Text = F(specTint);
        Sheen.Text = F(shn); SheenRough.Text = F(shnR);
        ShR.Text = F(shc[0]); ShG.Text = F(shc[1]); ShB.Text = F(shc[2]);
        Subsurf.Text = F(sss); SsR.Text = F(ssc[0]); SsG.Text = F(ssc[1]); SsB.Text = F(ssc[2]);

        KindBox.SelectedIndex = _kind;
        ShowPanelForKind();
        UpdateLabels();
        SyncSliderFromText();
        SyncPbrSlidersFromText();
        SyncToonSlidersFromText();
        SyncExtSlidersFromText();
        UpdateSwatch();
        UpdatePbrSwatches();
        UpdateToonSwatches();
        UpdateExtSwatches();
        RefreshPreview();
        _loading = false;
    }

    private static string Cstr(byte[] b)
    {
        int z = Array.IndexOf(b, (byte)0); if (z < 0) z = b.Length;
        return z == 0 ? "" : System.Text.Encoding.UTF8.GetString(b, 0, z);
    }
    private static string TexLabel(string path) => string.IsNullOrEmpty(path) ? "(なし)" : Path.GetFileName(path);

    private void ShowPanelForKind()
    {
        bool showLegacySurface = _substrateEnabled == 0 || _shadingMode == 1;
        SurfaceModePanel.Visibility = _kind == 0 ? Visibility.Visible : Visibility.Collapsed;
        PbrPanel.Visibility    = _kind == 0 && showLegacySurface && _selectedNode < 0
            ? Visibility.Visible : Visibility.Collapsed;
        EffectPanel.Visibility = _kind == 1 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnKindChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;
        _kind = KindBox.SelectedIndex < 0 ? 0 : KindBox.SelectedIndex;
        ShowPanelForKind();
        if (_kind == 1) SelectGraphNode(-1);
        else if (_shadingMode == 0) SelectGraphNode(_substrateRoot);
        EngineInterop.acs_editor_material_set_kind(_path, _kind);
        ReloadMaterialInViewport();
        StatusText.Text = "保存 ✓";
        RefreshPreview();
    }

    private static string F(float v) => v.ToString("0.###", CultureInfo.InvariantCulture);
    private static float P(string s, float fb = 0f) =>
        float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out float v) && float.IsFinite(v) ? v : fb;

    // ===== PBR =====
    private void SyncPbrSlidersFromText()
    {
        _syncing = true;
        SetSlider(MetallicSlider, Metallic.Text);
        SetSlider(RoughnessSlider, Roughness.Text);
        SetSlider(NormalStrSlider, NormalStr.Text);
        SetSlider(AoSlider, Ao.Text);
        SetSlider(EmStrSlider, EmStr.Text);
        _syncing = false;
    }
    private static void SetSlider(Slider s, string text)
    {
        double raw = P(text);
        if (raw > s.Maximum) s.Maximum = raw;   // キャップ超は範囲を広げる (データ保持)
        s.Value = Math.Clamp(raw, s.Minimum, s.Maximum);
    }

    private void UpdatePbrSwatches()
    {
        static byte B(float v) => (byte)Math.Clamp(v * 255f, 0f, 255f);
        BaseSwatch.Background = new SolidColorBrush(Color.FromRgb(B(P(BcR.Text)), B(P(BcG.Text)), B(P(BcB.Text))));
        EmSwatch.Background   = new SolidColorBrush(Color.FromRgb(B(P(EmR.Text)), B(P(EmG.Text)), B(P(EmB.Text))));
    }

    private void OnPbrChanged(object sender, TextChangedEventArgs e)
    {
        if (_loading) return;
        if (!_syncing) SyncPbrSlidersFromText();
        UpdatePbrSwatches();
        SavePbrAndReload();
    }

    private void OnPbrSlider(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_loading || _syncing) return;
        _syncing = true;
        if (sender == MetallicSlider)      Metallic.Text  = F((float)e.NewValue);
        else if (sender == RoughnessSlider) Roughness.Text = F((float)e.NewValue);
        else if (sender == NormalStrSlider) NormalStr.Text = F((float)e.NewValue);
        else if (sender == AoSlider)        Ao.Text        = F((float)e.NewValue);
        else if (sender == EmStrSlider)     EmStr.Text     = F((float)e.NewValue);
        _syncing = false;
    }

    private void OnBrowseAlbedo(object sender, RoutedEventArgs e) => BrowseTexture(ref _albedoPath, AlbedoLabel);
    private void OnBrowseNormal(object sender, RoutedEventArgs e) => BrowseTexture(ref _normalPath, NormalLabel);
    private void OnClearAlbedo(object sender, RoutedEventArgs e) { _albedoPath = ""; AlbedoLabel.Text = "(なし)"; SavePbrAndReload(); }
    private void OnClearNormal(object sender, RoutedEventArgs e) { _normalPath = ""; NormalLabel.Text = "(なし)"; SavePbrAndReload(); }

    private void BrowseTexture(ref string target, TextBlock label)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "画像を選択",
            Filter = "画像 (*.png;*.jpg;*.jpeg;*.bmp;*.tga)|*.png;*.jpg;*.jpeg;*.bmp;*.tga|すべて (*.*)|*.*",
        };
        if (dlg.ShowDialog(this) != true) return;
        target = dlg.FileName;
        label.Text = TexLabel(target);
        SavePbrAndReload();
    }

    private void SavePbrAndReload()
    {
        if (_loading || !CanAccessAssetPath) return;
        string name = (NameBox.Text ?? "").Trim();
        if (name.Length == 0) name = Path.GetFileNameWithoutExtension(_path);
        int ok = EngineInterop.acs_editor_material_save_pbr(_path, name,
            P(BcR.Text, 1f), P(BcG.Text, 1f), P(BcB.Text, 1f), P(BcA.Text, 1f),
            P(Metallic.Text), P(Roughness.Text),
            P(EmR.Text), P(EmG.Text), P(EmB.Text), P(EmStr.Text), P(NormalStr.Text, 1f), P(Ao.Text, 1f),
            _albedoPath, _normalPath);
        if (ok != 0)
        {
            ReloadMaterialInViewport();
            StatusText.Text = "保存 ✓";
        }
        else StatusText.Text = "保存失敗";
        RefreshPreview();
    }

    // ===== トゥーン (UTS 風 / Lit 内のサブモード) =====
    // トゥーン/Substrate パネルの排他表示と PBR ヒント文を現在のシェーディングモードに合わせる。
    private void ApplyShadeMode()
    {
        bool toon = _shadingMode == 1;
        ToonSubPanel.Visibility   = toon ? Visibility.Visible : Visibility.Collapsed;
        SubstratePanel.Visibility = toon ? Visibility.Collapsed : Visibility.Visible;
        PbrHint.Text = toon
            ? "トゥーン (セル/アニメ調)。陰影をしきい値でバンド化し、1影/2影色・リム光・ハイライト閾を指定。UTS 風。"
            : "PBR (metallic-roughness) + Substrate 拡張 (クリアコート/異方性/鏡面/シーン/SSS)。2D ライトで陰影付け、球サンプル。";
    }

    private void SyncToonSlidersFromText()
    {
        _syncing = true;
        SetSlider(S1ThrSlider, S1Thr.Text);
        SetSlider(S2ThrSlider, S2Thr.Text);
        SetSlider(RimPowSlider, RimPow.Text);
        SetSlider(SpecThrSlider, SpecThr.Text);
        SetSlider(SoftSlider, Soft.Text);
        _syncing = false;
    }

    private void UpdateToonSwatches()
    {
        static byte B(float v) => (byte)Math.Clamp(v * 255f, 0f, 255f);
        S1Swatch.Background  = new SolidColorBrush(Color.FromRgb(B(P(S1R.Text)), B(P(S1G.Text)), B(P(S1B.Text))));
        S2Swatch.Background  = new SolidColorBrush(Color.FromRgb(B(P(S2R.Text)), B(P(S2G.Text)), B(P(S2B.Text))));
        RimSwatch.Background = new SolidColorBrush(Color.FromRgb(B(P(RimR.Text)), B(P(RimG.Text)), B(P(RimB.Text))));
    }

    private void OnShadeChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;
        _shadingMode = ShadeBox.SelectedIndex < 0 ? 0 : ShadeBox.SelectedIndex;
        ApplyShadeMode();
        SelectGraphNode(_shadingMode == 1 ? -1 : _substrateRoot);
        ShowPanelForKind();
        SaveToonAndReload();
    }

    private void OnToonChanged(object sender, TextChangedEventArgs e)
    {
        if (_loading) return;
        if (!_syncing) SyncToonSlidersFromText();
        UpdateToonSwatches();
        SaveToonAndReload();
    }

    private void OnToonSlider(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_loading || _syncing) return;
        _syncing = true;
        if (sender == S1ThrSlider)        S1Thr.Text   = F((float)e.NewValue);
        else if (sender == S2ThrSlider)   S2Thr.Text   = F((float)e.NewValue);
        else if (sender == RimPowSlider)  RimPow.Text  = F((float)e.NewValue);
        else if (sender == SpecThrSlider) SpecThr.Text = F((float)e.NewValue);
        else if (sender == SoftSlider)    Soft.Text    = F((float)e.NewValue);
        _syncing = false;
    }

    private void SaveToonAndReload()
    {
        if (_loading || !CanAccessAssetPath) return;
        int ok = EngineInterop.acs_editor_material_save_toon(_path, _shadingMode,
            P(S1R.Text), P(S1G.Text), P(S1B.Text), P(S1Thr.Text, 0.5f),
            P(S2R.Text), P(S2G.Text), P(S2B.Text), P(S2Thr.Text, 0.18f),
            P(RimR.Text), P(RimG.Text), P(RimB.Text), P(RimPow.Text, 3f),
            _toonSpec[0], _toonSpec[1], _toonSpec[2], P(SpecThr.Text, 0.9f), P(Soft.Text, 0.04f));
        if (ok != 0)
        {
            ReloadMaterialInViewport();
            StatusText.Text = "保存 ✓";
        }
        else StatusText.Text = "保存失敗";
        RefreshPreview();
    }

    // ===== Substrate 拡張ロブ (PBR モードのみ) =====
    private void SyncExtSlidersFromText()
    {
        _syncing = true;
        SetSlider(ClrCoatSlider, ClrCoat.Text);
        SetSlider(CoatRoughSlider, CoatRough.Text);
        SetSlider(AnisoSlider, Aniso.Text);
        SetSlider(SpecLvlSlider, SpecLvl.Text);
        SetSlider(SpecTintSlider, SpecTint.Text);
        SetSlider(SheenSlider, Sheen.Text);
        SetSlider(SheenRoughSlider, SheenRough.Text);
        SetSlider(SubsurfSlider, Subsurf.Text);
        _syncing = false;
    }

    private void UpdateExtSwatches()
    {
        static byte B(float v) => (byte)Math.Clamp(v * 255f, 0f, 255f);
        ShSwatch.Background = new SolidColorBrush(Color.FromRgb(B(P(ShR.Text)), B(P(ShG.Text)), B(P(ShB.Text))));
        SsSwatch.Background = new SolidColorBrush(Color.FromRgb(B(P(SsR.Text)), B(P(SsG.Text)), B(P(SsB.Text))));
    }

    private void OnExtChanged(object sender, TextChangedEventArgs e)
    {
        if (_loading) return;
        if (!_syncing) SyncExtSlidersFromText();
        UpdateExtSwatches();
        SaveExtAndReload();
    }

    private void OnExtSlider(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_loading || _syncing) return;
        _syncing = true;
        if (sender == ClrCoatSlider)         ClrCoat.Text    = F((float)e.NewValue);
        else if (sender == CoatRoughSlider)  CoatRough.Text  = F((float)e.NewValue);
        else if (sender == AnisoSlider)      Aniso.Text      = F((float)e.NewValue);
        else if (sender == SpecLvlSlider)    SpecLvl.Text    = F((float)e.NewValue);
        else if (sender == SpecTintSlider)   SpecTint.Text   = F((float)e.NewValue);
        else if (sender == SheenSlider)      Sheen.Text      = F((float)e.NewValue);
        else if (sender == SheenRoughSlider) SheenRough.Text = F((float)e.NewValue);
        else if (sender == SubsurfSlider)    Subsurf.Text    = F((float)e.NewValue);
        _syncing = false;
    }

    private void SaveExtAndReload()
    {
        if (_loading || !CanAccessAssetPath) return;
        int ok = EngineInterop.acs_editor_material_save_pbr_ext(_path,
            P(ClrCoat.Text), P(CoatRough.Text, 0.1f), P(Aniso.Text),
            P(SpecLvl.Text, 0.5f), P(SpecTint.Text),
            P(Sheen.Text), P(SheenRough.Text, 0.3f), P(ShR.Text, 1f), P(ShG.Text, 1f), P(ShB.Text, 1f),
            P(Subsurf.Text), P(SsR.Text, 1f), P(SsG.Text, 0.3f), P(SsB.Text, 0.2f));
        if (ok != 0)
        {
            ReloadMaterialInViewport();
            StatusText.Text = "保存 ✓";
        }
        else StatusText.Text = "保存失敗";
        RefreshPreview();
    }

    // ===== 効果プリセット (既存) =====
    private void ApplyEffectDefaults()
    {
        int effect = EffectBox.SelectedIndex < 0 ? 0 : EffectBox.SelectedIndex;
        var color = new float[4] { 1, 1, 1, 1 };
        EngineInterop.acs_editor_material_default_params(effect,
            out float strength, out float p0, out float p1, out float p2, color, out int animated);
        _loading = true;
        Strength.Text = F(strength);
        P0.Text = F(p0); P1.Text = F(p1); P2.Text = F(p2);
        ColR.Text = F(color[0]); ColG.Text = F(color[1]); ColB.Text = F(color[2]); ColA.Text = F(color[3]);
        AnimatedBox.IsChecked = animated != 0;
        _loading = false;
        SyncSliderFromText();
        UpdateSwatch();
    }

    private void SyncSliderFromText()
    {
        _syncing = true;
        double raw = P(Strength.Text);
        if (raw > StrengthSlider.Maximum) StrengthSlider.Maximum = raw;
        StrengthSlider.Value = Math.Clamp(raw, StrengthSlider.Minimum, StrengthSlider.Maximum);
        _syncing = false;
    }

    private void UpdateLabels()
    {
        string fx = (EffectBox.SelectedItem as string) ?? "None";
        LblStrength.Text = "Strength"; LblP0.Text = "P0"; LblP1.Text = "P1"; LblP2.Text = "P2"; LblColor.Text = "Color";
        Visibility vStrength = Visibility.Visible, vP0 = Visibility.Visible, vP1 = Visibility.Visible,
                   vP2 = Visibility.Collapsed, vColor = Visibility.Visible;
        double sliderMax = 1.0;
        string hint;
        switch (fx)
        {
            case "None": vStrength = vP0 = vP1 = vColor = Visibility.Collapsed;
                hint = "効果なし (通常描画)。プリセットを選ぶと見た目が変わります。"; break;
            case "Grayscale": LblStrength.Text = "度合い"; vP0 = vP1 = vColor = Visibility.Collapsed;
                hint = "色を灰色へ。度合い 0=元の色 / 1=完全グレースケール。"; break;
            case "Tint": LblStrength.Text = "度合い"; LblColor.Text = "染め色"; vP0 = vP1 = Visibility.Collapsed;
                hint = "染め色を乗算。度合い 0=元の色 / 1=完全に染める。"; break;
            case "Vignette": LblStrength.Text = "強さ"; LblP0.Text = "開始半径"; LblP1.Text = "終了半径"; LblColor.Text = "縁色";
                hint = "周辺減光。開始半径(0..1)から終了半径(0..1)へ縁色へ。強さ=濃さ。"; break;
            case "Wave": LblStrength.Text = "振幅"; LblP0.Text = "周波数"; LblP1.Text = "速度"; vColor = Visibility.Collapsed;
                sliderMax = 0.15; hint = "波打ち歪み。振幅=揺れ幅 / 周波数=波の数 / 速度=流れ。アニメON推奨。"; break;
            case "Pixelate": LblP0.Text = "セル数X"; LblP1.Text = "セル数Y"; vStrength = vColor = Visibility.Collapsed;
                hint = "モザイク。セル数が小さいほど粗い。例: 24 × 24。"; break;
            case "HueShift": LblStrength.Text = "角度°"; LblP0.Text = "回転速度°/s"; vP1 = vColor = Visibility.Collapsed;
                sliderMax = 360.0; hint = "色相回転。角度=固定回転 / 回転速度=毎秒の回転 (アニメON で虹色に)。"; break;
            case "Brightness": LblStrength.Text = "明るさ"; LblP0.Text = "コントラスト"; vP1 = vColor = Visibility.Collapsed;
                sliderMax = 2.0; hint = "明るさ/コントラスト。明るさ 1=元 / コントラスト 1=元 (>1 で強調)。"; break;
            case "Invert": LblStrength.Text = "度合い"; vP0 = vP1 = vColor = Visibility.Collapsed;
                hint = "色反転。度合い 0=元の色 / 1=完全反転 (ネガ)。"; break;
            case "Sepia": LblStrength.Text = "度合い"; vP0 = vP1 = vColor = Visibility.Collapsed;
                hint = "セピア調 (古写真風)。度合い 0=元の色 / 1=完全セピア。"; break;
            case "Posterize": LblStrength.Text = "度合い"; LblP0.Text = "階調数"; vP1 = vColor = Visibility.Collapsed;
                hint = "階調化 (ポスター調)。階調数が少ないほどベタ塗りに。例: 4。"; break;
            case "Scanline": LblStrength.Text = "濃さ"; LblP0.Text = "線の本数"; vP1 = vColor = Visibility.Collapsed;
                hint = "走査線 (CRT/レトロ風)。濃さ=暗くする度合い / 線の本数。"; break;
            case "Chromatic": LblStrength.Text = "度合い"; LblP0.Text = "ずれ量"; vP1 = vColor = Visibility.Collapsed;
                hint = "色収差 (RGBずれ)。ずれ量=UVずらし量。スプライト(画像)で効果的。"; break;
            default: hint = ""; break;
        }
        ParamPanel.Children[0].SetCurrentValue(VisibilityProperty, vStrength);
        RowP0.Visibility = vP0; RowP1.Visibility = vP1; RowP2.Visibility = vP2; RowColor.Visibility = vColor;
        StrengthSlider.Maximum = sliderMax;
        StrengthSlider.SmallChange = sliderMax / 100.0;
        StrengthSlider.LargeChange = sliderMax / 10.0;
        AnimatedBox.Visibility = (fx is "Wave" or "HueShift") ? Visibility.Visible : Visibility.Collapsed;
        HintText.Text = hint;
    }

    private void UpdateSwatch()
    {
        static byte B(float v) => (byte)Math.Clamp(v * 255f, 0f, 255f);
        ColorSwatch.Background = new SolidColorBrush(Color.FromRgb(B(P(ColR.Text)), B(P(ColG.Text)), B(P(ColB.Text))));
    }

    private void RefreshPreview()
    {
        if (_loading || _closeInProgress || !CanAccessAssetPath || !IsLoaded) return;
        // Invalidate/cancel before debounce elapses. Otherwise an older job could
        // complete during the idle window and briefly display stale parameters.
        _previewPipeline.CancelPending();
        _previewTimer.Stop();
        _previewTimer.Start();
    }

    /// <summary>
    /// Bypasses the interactive debounce for deterministic visual-regression capture.
    /// Production UI updates continue to use <see cref="RefreshPreview"/>.
    /// </summary>
    internal void RenderPreviewImmediatelyForTest()
    {
        _previewTimer.Stop();
        if (!_closeInProgress && CanAccessAssetPath && IsLoaded)
        {
            MaterialPreviewGenerationResult result =
                _previewPipeline.GenerateLatestAsync(CapturePreviewRequest())
                    .GetAwaiter().GetResult();
            ApplyPreviewResult(result);
        }
    }

    private async Task RenderPreviewNowAsync()
    {
        if (_closeInProgress || !CanAccessAssetPath || !IsLoaded) return;
        MaterialPreviewRequest request = CapturePreviewRequest();
        UpdatePreviewModeText(request);
        PreviewStateText.Text = "Preview: generating…";

        MaterialPreviewGenerationResult result =
            await _previewPipeline.GenerateLatestAsync(request);
        ApplyPreviewResult(result);
    }

    private MaterialPreviewRequest CapturePreviewRequest()
    {
        int quality = PreviewQualityBox.SelectedIndex < 0
            ? 1 : PreviewQualityBox.SelectedIndex;
        int previewSize = quality switch { 0 => 192, 2 => 384, _ => 256 };
        // The live CPU renderer currently supports one mesh/background path.
        // The disabled selectors reserve the native GPU preview contract only.
        const int model = 0;
        const int background = 0;

        if (_kind == 0)
        {
            var baseColor = new[]
            {
                P(BcR.Text, 1f),
                P(BcG.Text, 1f),
                P(BcB.Text, 1f),
                P(BcA.Text, 1f),
            };
            var emissive = new[] { P(EmR.Text), P(EmG.Text), P(EmB.Text) };
            return MaterialPreviewRequest.CreatePbr(
                model,
                background,
                quality,
                previewSize,
                baseColor,
                P(Metallic.Text),
                P(Roughness.Text),
                emissive,
                P(EmStr.Text),
                P(NormalStr.Text, 1f),
                P(Ao.Text, 1f));
        }

        int effect = EffectBox.SelectedIndex < 0 ? 0 : EffectBox.SelectedIndex;
        var color = new[]
        {
            P(ColR.Text),
            P(ColG.Text),
            P(ColB.Text),
            P(ColA.Text, 1f),
        };
        return MaterialPreviewRequest.CreateEffect(
            model,
            background,
            quality,
            previewSize,
            effect,
            P(Strength.Text, 1f),
            P(P0.Text),
            P(P1.Text),
            P(P2.Text),
            color,
            AnimatedBox.IsChecked == true);
    }

    private void ApplyPreviewResult(MaterialPreviewGenerationResult result)
    {
        if (_closeInProgress ||
            !CanAccessAssetPath ||
            !IsLoaded ||
            !_previewPipeline.IsCurrent(result))
        {
            return;
        }

        UpdatePreviewModeText(result.Request);
        if (result.Error is not null)
        {
            // Preserve the last-good image. Preview failure must not blank the editor
            // or escape an async-void dispatcher event.
            PreviewStateText.Text = "Preview: failed · last good retained";
            return;
        }
        if (result.Image is null) return;

        PreviewImage.Source = result.Image;
        string source = result.CacheHit
            ? "cache hit"
            : result.Coalesced ? "shared job" : "generated";
        PreviewStateText.Text =
            $"Preview: {result.Duration.TotalMilliseconds:0.0} ms · {source}";
    }

    private void UpdatePreviewModeText(MaterialPreviewRequest? request = null)
    {
        if (PreviewModeText == null) return;
        int quality = request?.Quality ??
            (PreviewQualityBox.SelectedIndex < 0 ? 1 : PreviewQualityBox.SelectedIndex);
        int previewSize = request?.PixelSize ??
            (quality switch { 0 => 192, 2 => 384, _ => 256 });
        PreviewModeText.Text =
            $"CPU preview · async · {previewSize}px";
    }

    private void OnPreviewOptionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;
        UpdatePreviewModeText();
        RefreshPreview();
    }

    private void OnEffectChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;
        UpdateLabels();
        ApplyEffectDefaults();
        SaveAndReload();
    }

    private void OnParamChanged(object sender, TextChangedEventArgs e)
    {
        if (_loading) return;
        if (!_syncing) SyncSliderFromText();
        UpdateSwatch();
        SaveAndReload();
    }

    private void OnStrengthSlider(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_loading || _syncing) return;
        _syncing = true;
        Strength.Text = F((float)e.NewValue);
        _syncing = false;
    }

    private void OnParamToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        SaveAndReload();
    }

    private void SaveAndReload()
    {
        if (_loading || !CanAccessAssetPath) return;
        int effect = EffectBox.SelectedIndex < 0 ? 0 : EffectBox.SelectedIndex;
        string name = (NameBox.Text ?? "").Trim();
        if (name.Length == 0) name = Path.GetFileNameWithoutExtension(_path);
        int ok = EngineInterop.acs_editor_material_save(_path, name, effect,
            P(Strength.Text, 1f), P(P0.Text), P(P1.Text), P(P2.Text),
            P(ColR.Text), P(ColG.Text), P(ColB.Text), P(ColA.Text, 1f),
            (AnimatedBox.IsChecked == true) ? 1 : 0);
        if (ok != 0)
        {
            ReloadMaterialInViewport();
            StatusText.Text = "保存 ✓";
        }
        else StatusText.Text = "保存失敗";
        RefreshPreview();
    }

    private void OnClose(object sender, RoutedEventArgs e) => Close();
}
