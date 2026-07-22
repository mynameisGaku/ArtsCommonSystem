using System;
using System.Runtime.InteropServices;

namespace AcsEditor;

[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal struct EditorProfilerSnapshot
{
    public uint Version;
    public uint StructSize;
    public uint TimingSource;
    public uint Flags;

    public ulong FrameIndex;
    public ulong DrawCalls;
    public ulong DispatchCalls;
    public ulong Triangles;

    public float Fps;
    public float CpuFrameMs;
    public float CpuSubmitMs;
    public float GpuFrameMs;

    public float OpaqueCpuMs;
    public float AtmosphereCpuMs;
    public float CloudCpuMs;
    public float FogCpuMs;
    public float PostCpuMs;

    public float OpaqueGpuMs;
    public float AtmosphereGpuMs;
    public float CloudGpuMs;
    public float FogGpuMs;
    public float PostGpuMs;

    public uint ViewportWidth;
    public uint ViewportHeight;
    public uint CloudWidth;
    public uint CloudHeight;
    public uint CloudMarchSteps;
    public uint CloudLightSteps;
    public float CloudRenderScale;

    public ulong GpuFrameIndex;
    public float CpuFramePeakMs;
    public float GpuFramePeakMs;
    public uint PeakWindowFrames;
    public uint GpuLatencyFrames;

    public uint GpuQueryWindowCount;
    public uint GpuQueryWindowCapacity;
    public float GpuFrameAverageMs;
    public float OpaqueGpuAverageMs;
    public float AtmosphereGpuAverageMs;
    public float CloudGpuAverageMs;
    public float FogGpuAverageMs;
    public float PostGpuAverageMs;
    public float OpaqueGpuWindowPeakMs;
    public float AtmosphereGpuWindowPeakMs;
    public float CloudGpuWindowPeakMs;
    public float FogGpuWindowPeakMs;
    public float PostGpuWindowPeakMs;
}

internal static class EditorProfilerContract
{
    internal const uint Version = 3;
    internal const uint TimingCpuRecordSubmit = 1;
    internal const uint TimingGpuTimestamp = 2;

    internal const uint FlagView3D = 1u << 0;
    internal const uint FlagClouds = 1u << 1;
    internal const uint FlagFog = 1u << 2;
    internal const uint FlagAerialPerspective = 1u << 3;
    internal const uint FlagGpuTimingsValid = 1u << 4;

    internal static uint SnapshotSize =>
        checked((uint)Marshal.SizeOf<EditorProfilerSnapshot>());
}

/// <summary>
/// acs_editor_abi.dll (C ABI) への P/Invoke バインディング。
/// エンジンの DX12 描画を外部 HWND にホストするブリッジ関数群。
/// export 名の node/node3d は互換維持する C ABI 識別子で、C++ 型名ではないため
/// ANode 統一後も改名しない。
/// </summary>
internal static class EngineInterop
{
    private const string Dll = "acs_editor_abi";

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_version();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_create();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_attach(IntPtr handle, IntPtr hwnd, uint width, uint height);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_startup_status(
        IntPtr handle, out uint completed, out uint total);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_render(IntPtr handle, float dt);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern int acs_editor_profiler_get(
        IntPtr handle,
        ref EditorProfilerSnapshot snapshot,
        uint snapshotSize);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void acs_editor_profiler_reset_peaks(
        IntPtr handle);

    internal static bool TryGetProfilerSnapshot(
        IntPtr handle,
        out EditorProfilerSnapshot snapshot)
    {
        snapshot = new EditorProfilerSnapshot
        {
            Version = EditorProfilerContract.Version,
            StructSize = EditorProfilerContract.SnapshotSize,
        };
        if (handle == IntPtr.Zero)
            return false;

        try
        {
            return acs_editor_profiler_get(
                handle,
                ref snapshot,
                EditorProfilerContract.SnapshotSize) != 0 &&
                   snapshot.Version == EditorProfilerContract.Version &&
                   snapshot.StructSize >= EditorProfilerContract.SnapshotSize;
        }
        catch (EntryPointNotFoundException)
        {
            return false;
        }
        catch (DllNotFoundException)
        {
            return false;
        }
    }

    internal static void ResetProfilerPeaks(IntPtr handle)
    {
        if (handle == IntPtr.Zero)
            return;
        try
        {
            acs_editor_profiler_reset_peaks(handle);
        }
        catch (EntryPointNotFoundException)
        {
        }
        catch (DllNotFoundException)
        {
        }
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_resize(IntPtr handle, uint width, uint height);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_destroy(IntPtr handle);

    /// <summary>MSAA サンプル数 (1=FXAA のみ / 2 / 4 / 8) を設定。次フレームから適用。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_set_msaa(IntPtr handle, int samples);

    // ----- プロジェクト設定 (Project Settings) -----
    // INI テキストの読込/シリアライズは ABI、ファイル I/O は C# 側 (規律どおり)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_settings_load_text(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string iniText);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_settings_serialize(IntPtr handle, [Out] byte[] buf, int cap);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_settings_count(IntPtr handle);
    /// <summary>TSV 1行 "category\tkey\tvalue\ttype\toptions\tbuiltin\tdesc" を返す。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_settings_entry(IntPtr handle, int index, [Out] byte[] buf, int cap);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_settings_set(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string cat, [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_settings_add(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string cat, [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_settings_remove(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string cat, [MarshalAs(UnmanagedType.LPUTF8Str)] string key);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_settings_get_value(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string cat, [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
        [Out] byte[] buf, int cap);
    /// <summary>照明 (太陽+空) の時間帯プリセットを適用 (Noon/Sunset/Overcast/Night)。既知名で 1。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_apply_lighting_preset(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    /// <summary>3D ビューポートのグリッド表示を切替 (清書/スクショ用)。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_set_show_grid3d(IntPtr handle, int on);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_get_show_grid3d(IntPtr handle);
    /// <summary>現在の品質プリセットが要求する影マップ解像度 (0=影オフ)。設定反映の確認用。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_quality_shadow_size(IntPtr handle);
    /// <summary>現在の品質プリセットの bloom 強度 ×100 (0=bloom オフ)。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_quality_bloom_x100(IntPtr handle);

    /// <summary>NUL 終端 UTF-8 バイト列を string にする (Out バッファのデコード用)。</summary>
    public static string Utf8Z(byte[] buf)
    {
        int n = Array.IndexOf(buf, (byte)0);
        if (n < 0) n = buf.Length;
        return System.Text.Encoding.UTF8.GetString(buf, 0, n);
    }

    /// <summary>現在の実効 MSAA サンプル数を返す。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_get_msaa(IntPtr handle);

    // ----- 3D ビューポート (Phase 1) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_set_view3d(IntPtr handle, int on);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_set_ortho3d(IntPtr handle, int on);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_add_polygon3d(IntPtr handle, float[] xy, int count, float r, float g, float b, float a, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_get_ortho3d(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_get_view3d(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_cam3d_reset(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_add_node3d(IntPtr handle, int prim, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    /// <summary>«空ノード» (描画しないグループ用トランスフォーム) を追加。新 id / -1。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_add_empty3d(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_duplicate(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_node3d_copy(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_paste(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_delete_node3d(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_count(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_id_at(IntPtr handle, int index);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_parent(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_reparent3d(IntPtr handle, int child, int parent);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_name(IntPtr handle, int id, [Out] byte[] buf, int cap);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_set_name(IntPtr handle, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_prim(IntPtr handle, int id);
    /// <summary>ノード種別: 0=Cube 1=Sphere 2=Plane 3=Mesh 4=Sprite 5=Polygon (不明 -1)。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_kind(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_node3d_sprite_get(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_set_sprite(IntPtr handle, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    /// <summary>スプライト画像を外し平面プリミティブへ戻す (2D clear_sprite の 3D 版)。成功 1。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_clear_sprite(IntPtr handle, int id);
    // 3D ノードの prefab/blueprint インスタンスリンク (2D node_set/get_prefab_src の 3D 版)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_set_prefab_src(IntPtr handle, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_node3d_get_prefab_src(IntPtr handle, int id);
    /// <summary>3D ノードの prefab/blueprint リンクパス (UTF-8、インスタンスでなければ "")。</summary>
    public static string NodePrefabSrc3D(IntPtr handle, int id) =>
        Marshal.PtrToStringUTF8(acs_editor_node3d_get_prefab_src(handle, id)) ?? "";
    /// <summary>プリミティブ形状を切替 (0=Cube 1=Sphere 2=Plane)。sprite/polygon/mesh は不可。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_set_prim(IntPtr handle, int id, int prim);
    /// <summary>3D スプライトノードの画像パス (UTF-8、スプライトでなければ "")。</summary>
    public static string Node3DSprite(IntPtr handle, int id)
    {
        IntPtr p = acs_editor_node3d_sprite_get(handle, id);
        if (p == IntPtr.Zero) return "";
        int n = 0; while (Marshal.ReadByte(p, n) != 0) n++;
        if (n == 0) return "";
        var b = new byte[n]; Marshal.Copy(p, b, 0, n);
        return System.Text.Encoding.UTF8.GetString(b);
    }
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_get_transform(IntPtr handle, int id, [Out] float[] out9);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_set_transform(IntPtr handle, int id,
        float px, float py, float pz, float rx, float ry, float rz, float sx, float sy, float sz);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_get_color(IntPtr handle, int id, [Out] float[] out4);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_set_color(IntPtr handle, int id, float r, float g, float b, float a);
    // 3D ノードの使用マテリアル (.acsmat パス参照。2D の node_set/get/clear_material を鏡映)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_set_material(IntPtr handle, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_node3d_get_material(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_clear_material(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_selected3d(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_select3d(IntPtr handle, int id);
    // 3D 複数選択 (multi-select)
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_select3d_toggle(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_is_selected(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_selected3d_count(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_selected3d_at(IntPtr handle, int index);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_align3d_selection(IntPtr handle, int mode);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_distribute3d_selection(IntPtr handle, int axis);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_pick3d(IntPtr handle, float sx, float sy);
    /// <summary>
    /// Serializes the legacy .acs3d scene source into <paramref name="buf"/>.
    /// A return value greater than or equal to <paramref name="cap"/> means the buffer was too
    /// small and its contents must not be used.
    /// </summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_scene3d_serialize(IntPtr handle, [Out] byte[] buf, int cap);

    /// <summary>
    /// Returns a complete legacy .acs3d source serialization. The native ABI reports overflow, so this
    /// retries with a larger buffer instead of ever treating a truncated scene as valid.
    /// </summary>
    public static string Scene3DText(IntPtr handle)
    {
        if (handle == IntPtr.Zero)
            throw new ArgumentException("A valid editor handle is required.", nameof(handle));

        const int initialCapacity = 64 * 1024;
        const int maximumCapacity = 256 * 1024 * 1024;
        int capacity = initialCapacity;

        while (true)
        {
            var buffer = new byte[capacity];
            int length = acs_editor_scene3d_serialize(handle, buffer, buffer.Length);
            if (length <= 0)
                throw new InvalidOperationException(
                    "The native editor could not serialize the .acs3d source.");

            if (length < buffer.Length)
                return System.Text.Encoding.UTF8.GetString(buffer, 0, length);

            if (capacity >= maximumCapacity)
                throw new InvalidOperationException(
                    $"The .acs3d source serialization exceeds the {maximumCapacity / (1024 * 1024)} MiB safety limit.");

            long requested = Math.Max((long)capacity * 2L, (long)length + 1L);
            capacity = (int)Math.Min(requested, maximumCapacity);
        }
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_scene3d_load_text(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string text);
    /// <summary>メッシュファイル (.gltf/.glb/.obj/.fbx) を 3D ノードとして読み込む。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_add_mesh3d(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    /// <summary>画像ファイルを z=0 のスプライトとしてlegacy .acs3d payloadへ追加する。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_add_sprite3d(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    // 3D 変形ギズモ (現在のギズモモード move/rotate/scale を軸方向に適用)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_gizmo3d_begin(IntPtr handle, float sx, float sy);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_gizmo3d_drag(IntPtr handle, float sx, float sy);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_gizmo3d_end(IntPtr handle);

    // ----- scene introspection / edit (Hierarchy / Inspector) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_count(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_id_at(IntPtr handle, int index);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_parent(IntPtr handle, int id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_node_name(IntPtr handle, int id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_node_get_transform(IntPtr handle, int id,
        out float x, out float y, out float rot, out float sx, out float sy);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_node_set_transform(IntPtr handle, int id,
        float x, float y, float rot, float sx, float sy);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_select(IntPtr handle, int id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_selected(IntPtr handle);

    // ----- ノード表示プロパティ (色 / base / visible / enabled / sortLayer) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_node_get_color(IntPtr handle, int id,
        out float r, out float g, out float b, out float a);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_node_set_color(IntPtr handle, int id, float r, float g, float b, float a);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern float acs_editor_node_get_base(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_node_set_base(IntPtr handle, int id, float baseSize);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_get_visible(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_node_set_visible(IntPtr handle, int id, int visible);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_get_enabled(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_node_set_enabled(IntPtr handle, int id, int enabled);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_get_sortlayer(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_node_set_sortlayer(IntPtr handle, int id, int layer);

    // スプライト画像 (UTF-8 パス)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_set_sprite(IntPtr handle, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_node_get_sprite(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_clear_sprite(IntPtr handle, int id);

    // Play モード (物理プレビュー)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_play_start(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_play_stop(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_play_set_paused(IntPtr handle, int paused);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_play_step(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_play_state(IntPtr handle);

    // ポリゴン描画ツール。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_poly_begin(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_poly_add_point(IntPtr handle, float sx, float sy);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_poly_finalize(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_poly_cancel(IntPtr handle);
    // Ortho ビューでの 3D ポリゴン描画 (クリックを z=0 へ逆射影)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_poly3d_begin(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_poly3d_add_point(IntPtr handle, float sx, float sy);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_poly3d_finalize(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_poly3d_cancel(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_poly_is_drawing(IntPtr handle);

    /// <summary>ノードのスプライトパス (UTF-8、未設定は "")。</summary>
    public static string NodeSprite(IntPtr handle, int id)
    {
        IntPtr p = acs_editor_node_get_sprite(handle, id);
        if (p == IntPtr.Zero) return "";
        int n = 0; while (Marshal.ReadByte(p, n) != 0) n++;
        if (n == 0) return "";
        var b = new byte[n]; Marshal.Copy(p, b, 0, n);
        return System.Text.Encoding.UTF8.GetString(b);
    }

    // ----- マテリアル (.acsmat = 効果プリセット) -----
    // ノードの使用マテリアル (UTF-8 .acsmat パス)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_set_material(IntPtr handle, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_node_get_material(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_clear_material(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_reload_material(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Path);

    // 効果プリセット列挙 (マテリアルエディタのドロップダウン用)。dropdown index == 効果 enum 値。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_effect_count();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_material_effect_name(int index);

    // .acsmat ファイルの読み書き (マテリアルエディタ)。
    // 効果を切り替えたときに入れる「見栄えのする既定パラメータ」。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_material_default_params(int effect,
        out float strength, out float p0, out float p1, out float p2,
        [Out] float[] color4, out int animated);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_create([MarshalAs(UnmanagedType.LPUTF8Str)] string path, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_load([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        out int effect, out float strength, out float p0, out float p1, out float p2,
        [Out] float[] color4, out int animated, [Out] byte[] nameBuf, int nameCap);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_save([MarshalAs(UnmanagedType.LPUTF8Str)] string path, [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        int effect, float strength, float p0, float p1, float p2,
        float r, float g, float b, float a, int animated);

    // PBR (Lit) マテリアル: 種別 (0=Lit/PBR, 1=Effect) + プロパティ読み書き。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_kind([MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_set_kind([MarshalAs(UnmanagedType.LPUTF8Str)] string path, int kind);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_load_pbr([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        [Out] float[] baseColor4, out float metallic, out float roughness,
        [Out] float[] emissive3, out float emissiveStrength, out float normalStrength, out float ao,
        [Out] byte[] albedoBuf, int albedoCap, [Out] byte[] normalBuf, int normalCap);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_save_pbr([MarshalAs(UnmanagedType.LPUTF8Str)] string path, [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        float br, float bg, float bb, float ba, float metallic, float roughness,
        float er, float eg, float eb, float emissiveStrength, float normalStrength, float ao,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string albedoPath, [MarshalAs(UnmanagedType.LPUTF8Str)] string normalPath);

    // 実シェーダ GPU プレビュー (RT に描いて readback。out は BGRA32 size×size)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_render_preview_pbr(IntPtr handle,
        float br, float bg, float bb, float ba, float metallic, float roughness,
        float er, float eg, float eb, float emStr, float normalStr, float ao,
        [Out] byte[] outRgba, int size);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_render_preview_effect(IntPtr handle,
        int effect, float strength, float p0, float p1, float p2,
        float r, float g, float b, float a, float time, [Out] byte[] outRgba, int size);
    // .acsmat を読み込んで実シェーダで描く統合プレビュー (PBR/Toon/Effect を engine 側で分岐)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_render_preview_material(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, [Out] byte[] outRgba, int size);
    // Editor-only presentation state. quality: 0=1x, 1=2x, 2=4x;
    // model: 0=sphere, 1=cube, 2=plane; background: 0=studio, 1=checker, 2=black.
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_set_material_preview_options(
        IntPtr handle, int quality, int model, int background, float exposure);

    // シェーディングモード + トゥーン項目 (s1/s2/rim/spec は float[3])。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_load_toon([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        out int mode, [Out] float[] s1, out float thr1, [Out] float[] s2, out float thr2,
        [Out] float[] rim, out float rimPower, [Out] float[] spec, out float specThr, out float softness);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_save_toon([MarshalAs(UnmanagedType.LPUTF8Str)] string path, int mode,
        float s1r, float s1g, float s1b, float thr1, float s2r, float s2g, float s2b, float thr2,
        float rimr, float rimg, float rimb, float rimPower,
        float specr, float specg, float specb, float specThr, float softness);

    // Substrate 拡張 (clearcoat/異方/鏡面レベル・tint/シーン/SSS)。sheen/sss は float[3]。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_load_pbr_ext([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        out float clearcoat, out float clearcoatRoughness, out float anisotropy,
        out float specularLevel, out float specularTint,
        out float sheen, out float sheenRoughness, [Out] float[] sheenColor3,
        out float subsurface, [Out] float[] sssColor3);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_save_pbr_ext([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        float clearcoat, float clearcoatRoughness, float anisotropy,
        float specularLevel, float specularTint,
        float sheen, float sheenRoughness, float sheenR, float sheenG, float sheenB,
        float subsurface, float sssR, float sssG, float sssB);

    // Substrate closure DAG. The .acsmat file owns topology and Slab data; the
    // editor sidecar stores presentation-only state such as positions and zoom.
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_substrate_max_nodes();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_substrate_slab_scalar_count();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_substrate_get_header(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        out int enabled, out int root, out int count);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_substrate_get_node(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, int index,
        out int type, out int inputA, out int inputB, out float factor, out uint flags,
        [Out] float[] slab39);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_substrate_save(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        int enabled, int root, int count,
        [In] int[] types, [In] int[] inputsA, [In] int[] inputsB,
        [In] float[] factors, [In] uint[] flags, [In] float[] slabs39);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_substrate_compile(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        out int errorCode, out int errorNode, out uint featureBits,
        out int closureCount, out int complexity, out int bytesPerPixel);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_substrate_compile_arrays(
        int enabled, int root, int count,
        [In] int[] types, [In] int[] inputsA, [In] int[] inputsB,
        [In] float[] factors, [In] uint[] flags, [In] float[] slabs39,
        out int errorCode, out int errorNode, out uint featureBits,
        out int closureCount, out int complexity, out int bytesPerPixel);

    // Typed shader-expression graph used by Substrate Slab scalar inputs.
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_expression_max_nodes();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_expression_texture_slots();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_expression_get_header(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        out int root, out int count);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_expression_get_node(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, int index,
        out int op, out int declaredType, out int textureSlot, out int textureFlags,
        out int componentIndex, out int input0, out int input1, out int input2,
        out uint parameterId, out uint textureAssetIdLow, out uint textureAssetIdHigh,
        [Out] float[] value4);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_expression_get_bindings(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, int slabNodeIndex,
        [Out] int[] roots39);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_expression_get_texture_path(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, int slot,
        [Out] byte[] utf8, int capacity);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_expression_compile_arrays(
        int root, int count,
        [In] int[] ops, [In] int[] declaredTypes,
        [In] int[] textureSlots, [In] int[] textureFlags,
        [In] int[] componentIndices,
        [In] int[] input0, [In] int[] input1, [In] int[] input2,
        [In] uint[] parameterIds,
        [In] uint[] textureAssetIdLows, [In] uint[] textureAssetIdHighs,
        [In] float[] values4,
        out int errorCode, out int errorNode, out int errorInput,
        out int expectedType, out int actualType,
        out int instructionCount, out int constantFoldCount,
        out uint hashLow, out uint hashHigh);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_expression_compile(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        out int linkError, out int linkErrorNode, out int linkErrorScalar,
        out int expressionError, out int expressionErrorNode,
        out int expressionErrorInput, out int expectedType, out int actualType,
        out int instructionCount, out int bindingCount);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_material_substrate_expression_save(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        int enabled, int substrateRoot, int substrateCount,
        [In] int[] substrateTypes, [In] int[] substrateInputsA,
        [In] int[] substrateInputsB, [In] float[] substrateFactors,
        [In] uint[] substrateFlags, [In] float[] substrateSlabs39,
        [In] int[] substrateExpressionRoots39,
        int expressionRoot, int expressionCount,
        [In] int[] expressionOps, [In] int[] expressionDeclaredTypes,
        [In] int[] expressionTextureSlots, [In] int[] expressionTextureFlags,
        [In] int[] expressionComponentIndices,
        [In] int[] expressionInput0, [In] int[] expressionInput1,
        [In] int[] expressionInput2, [In] uint[] expressionParameterIds,
        [In] uint[] expressionTextureAssetIdLows,
        [In] uint[] expressionTextureAssetIdHighs,
        [In] float[] expressionValues4,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string texturePath0,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string texturePath1,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string texturePath2,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string texturePath3);

    /// <summary>3D ノードの使用マテリアルパス (UTF-8、未設定は "")。NodeMaterial(2D) の鏡映。</summary>
    public static string NodeMaterial3D(IntPtr handle, int id)
    {
        IntPtr p = acs_editor_node3d_get_material(handle, id);
        if (p == IntPtr.Zero) return "";
        int m = 0; while (Marshal.ReadByte(p, m) != 0) m++;
        if (m == 0) return "";
        var bb = new byte[m]; Marshal.Copy(p, bb, 0, m);
        return System.Text.Encoding.UTF8.GetString(bb);
    }

    /// <summary>ノードの使用マテリアルパス (UTF-8、未設定は "")。</summary>
    public static string NodeMaterial(IntPtr handle, int id)
    {
        IntPtr p = acs_editor_node_get_material(handle, id);
        if (p == IntPtr.Zero) return "";
        int n = 0; while (Marshal.ReadByte(p, n) != 0) n++;
        if (n == 0) return "";
        var b = new byte[n]; Marshal.Copy(p, b, 0, n);
        return System.Text.Encoding.UTF8.GetString(b);
    }

    /// <summary>効果プリセット名の一覧 (index 0..count-1 == 効果 enum 値)。</summary>
    public static string[] MaterialEffectNames()
    {
        int c = acs_editor_material_effect_count();
        if (c <= 0) return Array.Empty<string>();
        var names = new string[c];
        for (int i = 0; i < c; i++)
            names[i] = Marshal.PtrToStringUTF8(acs_editor_material_effect_name(i)) ?? "?";
        return names;
    }

    // ----- multi-select (選択集合。primary = acs_editor_selected) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_select_toggle(IntPtr handle, int id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_select_all(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_select_none(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_selection_count(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_selection_at(IntPtr handle, int index);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_selection_contains(IntPtr handle, int id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_selection_delete(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_selection_duplicate(IntPtr handle);

    // ----- align / distribute (multi-select) -----
    // align mode: 0=left 1=right 2=top 3=bottom 4=h-center 5=v-center
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_align_selection(IntPtr handle, int mode);

    // distribute axis: 0=horizontal 1=vertical
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_distribute_selection(IntPtr handle, int axis);

    // ----- rubber-band (box) selection -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_select_box(IntPtr handle,
        float x0, float y0, float x1, float y1, int additive);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_set_marquee(IntPtr handle,
        int active, float x0, float y0, float x1, float y1);

    // ----- view camera (pan / zoom) + picking -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_pick(IntPtr handle, float screenX, float screenY);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_camera_pan(IntPtr handle, float dx, float dy);

    // 真のパン (平行移動)。3D 透視で中ドラッグ時に使う (camera_pan は透視で軌道回転)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_camera_move(IntPtr handle, float dx, float dy);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_camera_zoom(IntPtr handle, float factor, float anchorX, float anchorY);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_camera_reset(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_camera_get(IntPtr handle, out float panX, out float panY, out float zoom);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_camera3d_set(
        IntPtr handle, float yaw, float pitch, float distance,
        float targetX, float targetY, float targetZ);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_camera3d_get(
        IntPtr handle, out float yaw, out float pitch, out float distance,
        out float targetX, out float targetY, out float targetZ);

    // ----- transform gizmo (move / rotate / scale) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_gizmo_set_mode(IntPtr handle, int mode);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_gizmo_get_mode(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_gizmo_begin(IntPtr handle, float screenX, float screenY);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_gizmo_update(IntPtr handle, float screenX, float screenY);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_gizmo_end(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_set_snap(IntPtr handle, int enabled, float moveGrid, float rotateDeg, float scaleStep);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_get_snap(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_camera_focus(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_camera_frame_all(IntPtr handle);

    // ----- type registry introspection (エンジンの登録型を列挙) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_type_count();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_type_name_at(int index);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_type_category_at(int index);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_type_instantiable_at(int index);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_type_member_count_at(int index);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_category_label(int category);

    // ----- user-defined types (ゲーム DLL から取り込んだもの) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_user_type_count(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_user_type_name_at(IntPtr handle, int index);

    // リフレクション DLL をロードしてユーザー定義 Component 型を取り込む (取り込み数 / 失敗負値)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_load_game_dll(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path);

    // ----- scene instantiation (authored 値で実コンポーネントを attach → tick) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_instantiate_scene(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_tick_instances(IntPtr handle, float dt);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_clear_instances(IntPtr handle);

    // ----- Preview (DLL ビルド不要のエンジンコンポーネント ライブ実行) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_preview_start(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_preview_stop(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_preview_state(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_instance_count(IntPtr handle);

    // ----- in-process play (ゲーム DLL がユーザーコンポーネントを実行) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_logic_play_start(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string dllPath);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_logic_play_stop(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_logic_play_active(IntPtr handle);

    // Play 中の DLL へキー入力をフィードする (keycode = acs::EKey の整数値, down = 1/0)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_logic_input_key(IntPtr handle, int keycode, int down);

    // Play 中の DLL へマウス入力をフィードする (button: 0=Left,1=Right,2=Middle、down=1/0)。Play 外は no-op。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_logic_input_mouse_button(IntPtr handle, int button, int down);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_logic_input_mouse_move(IntPtr handle, float x, float y);

    // ゲームビュー (Game View タブ): editor chrome を消してゲーム画面だけ描く。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_set_game_view(IntPtr handle, int on);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_is_game_view(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_add_node(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string typeName, int parentId);

    // ----- scene save / load (永続化。ファイル I/O は C# 側が担う) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_scene_serialize(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_scene_load_text(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string text);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_scene_new(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_scene3d_new(IntPtr handle);

    // ----- undo / redo (シーンスナップショット) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_undo(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_redo(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_can_undo(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_can_redo(IntPtr handle);

    // 連続編集 (ドラッグスクラブ) を 1 undo に束ねる。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_begin_continuous(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_end_continuous(IntPtr handle);

    // ----- node operations (rename / delete / reparent) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_rename(IntPtr handle, int id,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_delete(IntPtr handle, int id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_reparent(IntPtr handle, int id, int newParentId);

    // ノードを兄弟として target の前(0)/後(1)へ挿入、または子(2)にする。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_move(IntPtr handle, int id, int targetId, int mode);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_duplicate(IntPtr handle, int id);

    // ----- プレハブ・インスタンスのリンク (.acsprefab パス) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_set_prefab_src(IntPtr handle, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_node_get_prefab_src(IntPtr handle, int id);
    /// <summary>ノードのプレハブリンク (.acsprefab パス、インスタンスでなければ "")。</summary>
    public static string NodePrefabSrc(IntPtr handle, int id) =>
        Marshal.PtrToStringUTF8(acs_editor_node_get_prefab_src(handle, id)) ?? "";

    // ----- copy / paste (subtree。クリップボードは C# 側が保持) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_copy_subtree(IntPtr handle, int id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_paste_subtree(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string text, int parentId);

    /// <summary>id のノードの subtree シリアライズ文字列を取得 (UTF-8)。</summary>
    public static string CopySubtree(IntPtr handle, int id) =>
        Marshal.PtrToStringUTF8(acs_editor_copy_subtree(handle, id)) ?? "";

    // ----- 3D copy / paste (subtree。ACS3D テキスト。Prefab/Blueprint 保存・インスタンス化が使う) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_copy_subtree3d(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_paste_subtree3d(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string text, int parentId);
    /// <summary>3D ノードの subtree を ACS3D テキストへシリアライズ取得 (UTF-8)。</summary>
    public static string CopySubtree3D(IntPtr handle, int id) =>
        Marshal.PtrToStringUTF8(acs_editor_copy_subtree3d(handle, id)) ?? "";

    // ----- ノードコンポーネント (reflection 登録済み AComponent 型のアタッチ記述子) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_add_component(IntPtr handle, int id,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string typeName);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_component_count(IntPtr handle, int id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_node_component_name_at(IntPtr handle, int id, int index);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_remove_component_at(IntPtr handle, int id, int index);

    /// <summary>ノードの index 番目のコンポーネント型名 (UTF-8)。</summary>
    public static string ComponentName(IntPtr handle, int id, int index) =>
        Marshal.PtrToStringUTF8(acs_editor_node_component_name_at(handle, id, index)) ?? "";

    // ----- 3D ノードコンポーネント (2D と同じ。AEditor3DRecordComponent にメタデータとして保持) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_add_component(IntPtr handle, int id,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string typeName);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_component_count(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_node3d_component_name_at(IntPtr handle, int id, int index);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_remove_component_at(IntPtr handle, int id, int index);
    /// <summary>3D ノードの index 番目のコンポーネント型名 (UTF-8)。</summary>
    public static string Component3DName(IntPtr handle, int id, int index) =>
        Marshal.PtrToStringUTF8(acs_editor_node3d_component_name_at(handle, id, index)) ?? "";
    // 3D コンポーネント編集プロパティ値 (2D の node_component_prop_get/set / invoke の 3D 版。
    // スキーマは 2D と共有の acs_editor_component_prop_* / component_method_* を流用)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_component_prop_get(IntPtr handle, int id, int slot, int prop,
        out float x, out float y, out float z, out float w);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_component_prop_set(IntPtr handle, int id, int slot, int prop,
        float x, float y, float z, float w);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_invoke_method(IntPtr handle, int id, int slot,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string method);

    // ----- 3D ノードの visible/enabled (ANode の m_Visible/m_Enabled) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_get_visible(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_node3d_set_visible(IntPtr handle, int id, int visible);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_get_enabled(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_node3d_set_enabled(IntPtr handle, int id, int enabled);

    // ----- コンポーネントプロパティ編集 (リフレクション・スキーマ駆動) -----
    // 型のスキーマ (どの編集フィールドがあるか) は reflection から、インスタンスの値は
    // ノード+slot ごとに ABI が保持する。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_component_prop_count(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string typeName);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_component_prop_name_at(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string typeName, int index);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_component_prop_kind_at(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string typeName, int index);

    /// <summary>編集プロパティのフラグ (bit0=READONLY, bit1=HIDDEN, bit2=TRANSIENT)。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_component_prop_flags_at(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string typeName, int index);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_component_prop_category_at(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string typeName, int index);
    /// <summary>編集プロパティのカテゴリ名 (UPROPERTY(Category=…)、未指定は "")。</summary>
    public static string ComponentPropCategory(string typeName, int index) =>
        Marshal.PtrToStringUTF8(acs_editor_component_prop_category_at(typeName, index)) ?? "";

    // 反射メソッド (ACS_FUNCTION / BlueprintCallable / CallInEditor)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_component_method_count([MarshalAs(UnmanagedType.LPUTF8Str)] string typeName);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_component_method_name_at([MarshalAs(UnmanagedType.LPUTF8Str)] string typeName, int index);
    /// <summary>メソッドフラグ (bit0=BlueprintCallable, bit1=CallInEditor)。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_component_method_flags_at([MarshalAs(UnmanagedType.LPUTF8Str)] string typeName, int index);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_invoke_method(IntPtr handle, int id, int slot, [MarshalAs(UnmanagedType.LPUTF8Str)] string method);
    /// <summary>反射メソッド名 (UTF-8)。</summary>
    public static string ComponentMethodName(string typeName, int index) =>
        Marshal.PtrToStringUTF8(acs_editor_component_method_name_at(typeName, index)) ?? "";

    // グローバル反射メソッド列挙 (Blueprint ノードパレット用)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_method_count();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_method_name_at(int index);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_method_owner_at(int index);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_method_flags_at(int index);
    /// <summary>反射メソッドの引数種別 (0=None,1=F32,2=I32,3=Str)。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_method_argkind_at(int index);
    /// <summary>反射メソッドの戻り値種別 (0=None/void,1=F32,2=I32,3=Str)。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_method_retkind_at(int index);
    /// <summary>反射メソッドを文字列引数付きで実呼出 (引数なしメソッドは arg 無視)。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_invoke_method_arg(IntPtr handle, int id, int slot,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string method, [MarshalAs(UnmanagedType.LPUTF8Str)] string arg);
    /// <summary>反射メソッドを arg 付きで実呼出し、戻り値を文字列で受ける (void は空)。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_invoke_method_ret(IntPtr handle, int id, int slot,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string method, [MarshalAs(UnmanagedType.LPUTF8Str)] string arg,
        byte[] outBuf, int outCap);
    /// <summary>戻り値付き実呼出。成功なら true で out に戻り値文字列 (UTF-8 デコード)。</summary>
    public static bool InvokeMethodRet(IntPtr handle, int id, int slot, string method, string arg, out string ret)
    {
        var buf = new byte[256];
        if (acs_editor_node_invoke_method_ret(handle, id, slot, method, arg, buf, buf.Length) == 0) { ret = ""; return false; }
        int n = Array.IndexOf(buf, (byte)0); if (n < 0) n = buf.Length;
        ret = System.Text.Encoding.UTF8.GetString(buf, 0, n);
        return true;
    }
    /// <summary>i 番目のグローバル反射メソッド名 (UTF-8)。</summary>
    public static string MethodName(int index) =>
        Marshal.PtrToStringUTF8(acs_editor_method_name_at(index)) ?? "";
    /// <summary>i 番目のグローバル反射メソッドの所有型名 (UTF-8)。</summary>
    public static string MethodOwner(int index) =>
        Marshal.PtrToStringUTF8(acs_editor_method_owner_at(index)) ?? "";

    // エンジンログ取り込み (エディタのコンソールへ)。
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_log_poll(out int severity, byte[] buf, int buflen);
    /// <summary>キューのエンジンログを 1 件取り出す。空なら false。message は UTF-8 デコード。</summary>
    public static bool LogPoll(out int severity, out string message)
    {
        var buf = new byte[256];
        if (acs_editor_log_poll(out severity, buf, buf.Length) == 0) { message = ""; return false; }
        int n = Array.IndexOf(buf, (byte)0); if (n < 0) n = buf.Length;
        message = System.Text.Encoding.UTF8.GetString(buf, 0, n);
        return true;
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_component_prop_get(IntPtr handle, int id, int slot, int prop,
        out float x, out float y, out float z, out float w);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node_component_prop_set(IntPtr handle, int id, int slot, int prop,
        float x, float y, float z, float w);

    /// <summary>Component 型名の index 番目の編集プロパティ名 (UTF-8)。</summary>
    public static string ComponentPropName(string typeName, int index) =>
        Marshal.PtrToStringUTF8(acs_editor_component_prop_name_at(typeName, index)) ?? "";

    /// <summary>現在のシーンをテキスト (UTF-8) としてシリアライズ取得。</summary>
    public static string SceneText(IntPtr handle) =>
        Marshal.PtrToStringUTF8(acs_editor_scene_serialize(handle)) ?? "";

    /// <summary>ノード名を UTF-8 文字列として取得。</summary>
    public static string NodeName(IntPtr handle, int id) =>
        Marshal.PtrToStringUTF8(acs_editor_node_name(handle, id)) ?? "";

    /// <summary>index 番目の登録型名 (UTF-8)。</summary>
    public static string TypeName(int index) =>
        Marshal.PtrToStringUTF8(acs_editor_type_name_at(index)) ?? "";

    /// <summary>カテゴリ整数値の人間可読ラベル。</summary>
    public static string CategoryLabel(int category) =>
        Marshal.PtrToStringUTF8(acs_editor_category_label(category)) ?? "Unknown";

    /// <summary>i 番目のユーザー定義型名。</summary>
    public static string UserTypeName(IntPtr handle, int index) =>
        Marshal.PtrToStringUTF8(acs_editor_user_type_name_at(handle, index)) ?? "";

    /// <summary>ABI バージョン文字列 (interop 疎通確認用)。DLL ロード失敗時は例外メッセージを返す。</summary>
    public static string Version()
    {
        try
        {
            IntPtr p = acs_editor_version();
            return Marshal.PtrToStringAnsi(p) ?? "(null)";
        }
        catch (Exception ex)
        {
            return "ABI load failed: " + ex.Message;
        }
    }
}
