using System;
using System.Runtime.InteropServices;

namespace AcsEditor;

/// <summary>
/// acs_editor_abi.dll (C ABI) への P/Invoke バインディング。
/// エンジンの DX12 描画を外部 HWND にホストするブリッジ関数群。
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
    public static extern void acs_editor_render(IntPtr handle, float dt);

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
    public static extern int acs_editor_get_view3d(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_cam3d_reset(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_add_node3d(IntPtr handle, int prim, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_delete_node3d(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_count(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_id_at(IntPtr handle, int index);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_name(IntPtr handle, int id, [Out] byte[] buf, int cap);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_prim(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_get_transform(IntPtr handle, int id, [Out] float[] out9);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_set_transform(IntPtr handle, int id,
        float px, float py, float pz, float rx, float ry, float rz, float sx, float sy, float sz);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_get_color(IntPtr handle, int id, [Out] float[] out4);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_node3d_set_color(IntPtr handle, int id, float r, float g, float b, float a);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_selected3d(IntPtr handle);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_select3d(IntPtr handle, int id);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_pick3d(IntPtr handle, float sx, float sy);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_scene3d_serialize(IntPtr handle, [Out] byte[] buf, int cap);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_scene3d_load_text(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string text);
    /// <summary>メッシュファイル (.gltf/.glb/.obj/.fbx) を 3D ノードとして読み込む。</summary>
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_add_mesh3d(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

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

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_camera_zoom(IntPtr handle, float factor, float anchorX, float anchorY);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_camera_reset(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void acs_editor_camera_get(IntPtr handle, out float panX, out float panY, out float zoom);

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

    // ----- copy / paste (subtree。クリップボードは C# 側が保持) -----
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr acs_editor_copy_subtree(IntPtr handle, int id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int acs_editor_paste_subtree(IntPtr handle,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string text, int parentId);

    /// <summary>id のノードの subtree シリアライズ文字列を取得 (UTF-8)。</summary>
    public static string CopySubtree(IntPtr handle, int id) =>
        Marshal.PtrToStringUTF8(acs_editor_copy_subtree(handle, id)) ?? "";

    // ----- node components (reflection-registered Component 型のアタッチ記述子) -----
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

    // ----- component property editing (リフレクション・スキーマ駆動のプロパティ編集) -----
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
