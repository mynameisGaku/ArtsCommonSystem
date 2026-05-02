// メッシュアセット実装（cgltf / 自前 OBJ / ufbx）
#include "asset/MeshAsset.h"
#include "memory/Memory.h"
#include "foundation/Move.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <ufbx.h>

#include <cstdlib>
#include <cstring>

namespace acs {

namespace {

// 共通: MeshAsset を Rc<Asset> として返すヘルパ
Rc<Asset> WrapMesh(AssetId id, Rc<MeshAsset>&& m) noexcept {
    m->SetId(id);
    m->SetState(AssetState::Ready);
    return Rc<Asset>(Move(m));
}

// cgltf の attribute から Vec3 配列を読み出す
void ReadAttributeVec3(const cgltf_accessor* a, MeshVertex* dst, usize n,
                       usize stride, void (*setter)(MeshVertex&, Vec3)) noexcept {
    if (!a) return;
    f32 buf[3];
    for (usize i = 0; i < n; ++i) {
        if (::cgltf_accessor_read_float(a, i, buf, 3)) {
            setter(dst[i], Vec3(buf[0], buf[1], buf[2]));
        }
    }
    (void)stride;
}

// cgltf の attribute から UV (Vec2) を読み出す
void ReadAttributeUV(const cgltf_accessor* a, MeshVertex* dst, usize n) noexcept {
    if (!a) return;
    f32 buf[2];
    for (usize i = 0; i < n; ++i) {
        if (::cgltf_accessor_read_float(a, i, buf, 2)) {
            dst[i].u = buf[0];
            dst[i].v = buf[1];
        }
    }
}

// cgltf の data から MeshAsset へ詰める共通処理
Rc<MeshAsset> BuildFromCgltf(cgltf_data* data) noexcept {
    auto mesh = MakeRc<MeshAsset>();
    if (!data || data->meshes_count == 0) return mesh;

    // 全プリミティブを 1 つの MeshAsset に flatten（v1）
    u32 vertex_offset = 0;
    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& m = data->meshes[mi];
        for (cgltf_size pi = 0; pi < m.primitives_count; ++pi) {
            const cgltf_primitive& p = m.primitives[pi];
            if (!p.attributes_count) continue;

            // 頂点数を最初の attribute から取得
            usize vcount = p.attributes[0].data->count;
            usize start = mesh->Vertices().Size();
            mesh->Vertices().Resize(start + vcount);
            MeshVertex* dst = mesh->Vertices().Data() + start;

            // POSITION / NORMAL / TEXCOORD_0 を取得
            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* nrm = nullptr;
            const cgltf_accessor* uv0 = nullptr;
            for (cgltf_size ai = 0; ai < p.attributes_count; ++ai) {
                const cgltf_attribute& a = p.attributes[ai];
                if (a.type == cgltf_attribute_type_position) pos = a.data;
                else if (a.type == cgltf_attribute_type_normal) nrm = a.data;
                else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uv0 = a.data;
            }
            if (pos)
                ReadAttributeVec3(pos, dst, vcount, 0,
                    [](MeshVertex& v, Vec3 x) { v.position = x; });
            if (nrm)
                ReadAttributeVec3(nrm, dst, vcount, 0,
                    [](MeshVertex& v, Vec3 x) { v.normal = x; });
            if (uv0) ReadAttributeUV(uv0, dst, vcount);

            // インデックス取得（無ければトライアングルストリップ的に連番生成）
            SubMesh sub{};
            sub.first_index = static_cast<u32>(mesh->Indices().Size());
            if (p.indices) {
                cgltf_accessor* idx = p.indices;
                for (cgltf_size ii = 0; ii < idx->count; ++ii) {
                    cgltf_uint v = 0;
                    ::cgltf_accessor_read_uint(idx, ii, &v, 1);
                    mesh->Indices().PushBack(static_cast<u32>(v) + vertex_offset);
                }
                sub.index_count = static_cast<u32>(idx->count);
            } else {
                for (u32 ii = 0; ii < vcount; ++ii) mesh->Indices().PushBack(ii + vertex_offset);
                sub.index_count = static_cast<u32>(vcount);
            }
            mesh->SubMeshes().PushBack(sub);
            vertex_offset += static_cast<u32>(vcount);
        }
    }
    return mesh;
}

} // namespace

// ---- glTF / GLB (cgltf) -------------------------------------------------
Result<Rc<Asset>> GltfAssetLoader::LoadFromBytes(AssetId id, const Array<byte>& bytes) noexcept {
    cgltf_options opts{};
    cgltf_data* data = nullptr;
    if (::cgltf_parse(&opts, bytes.Data(), bytes.Size(), &data) != cgltf_result_success || !data)
        return ACS_ERR(Asset, 300, "cgltf_parse failed (.gltf)");
    // .gltf は別ファイルにバイナリを持つことがあるので、メモリ単体ロードは
    // 簡易対応として「埋め込みバッファのみ」サポート。
    if (::cgltf_load_buffers(&opts, data, nullptr) != cgltf_result_success) {
        ::cgltf_free(data);
        return ACS_ERR(Asset, 301, "cgltf_load_buffers failed (external bin not supported in v1)");
    }
    Rc<MeshAsset> mesh = BuildFromCgltf(data);
    ::cgltf_free(data);
    return Result<Rc<Asset>>(OkInit, WrapMesh(id, Move(mesh)));
}

Result<Rc<Asset>> GlbAssetLoader::LoadFromBytes(AssetId id, const Array<byte>& bytes) noexcept {
    cgltf_options opts{};
    opts.type = cgltf_file_type_glb;
    cgltf_data* data = nullptr;
    if (::cgltf_parse(&opts, bytes.Data(), bytes.Size(), &data) != cgltf_result_success || !data)
        return ACS_ERR(Asset, 310, "cgltf_parse failed (.glb)");
    if (::cgltf_load_buffers(&opts, data, nullptr) != cgltf_result_success) {
        ::cgltf_free(data);
        return ACS_ERR(Asset, 311, "cgltf_load_buffers failed");
    }
    Rc<MeshAsset> mesh = BuildFromCgltf(data);
    ::cgltf_free(data);
    return Result<Rc<Asset>>(OkInit, WrapMesh(id, Move(mesh)));
}

// ---- OBJ (自前パーサ、最小機能) -----------------------------------------
// v / vn / vt / f だけサポート。マテリアルは無視。
Result<Rc<Asset>> ObjAssetLoader::LoadFromBytes(AssetId id, const Array<byte>& bytes) noexcept {
    auto mesh = MakeRc<MeshAsset>();
    Array<Vec3> positions;
    Array<Vec3> normals;
    struct UV { f32 u, v; };
    Array<UV> uvs;

    const char* p = reinterpret_cast<const char*>(bytes.Data());
    const char* end = p + bytes.Size();

    auto skip_ws = [&](const char*& s) {
        while (s < end && (*s == ' ' || *s == '\t')) ++s;
    };
    auto next_line = [&](const char*& s) {
        while (s < end && *s != '\n') ++s;
        if (s < end) ++s;
    };
    auto parse_f32 = [&](const char*& s) -> f32 {
        char* endp = nullptr;
        f32 v = static_cast<f32>(::strtod(s, &endp));
        s = endp;
        return v;
    };
    auto parse_index = [&](const char*& s) -> i32 {
        char* endp = nullptr;
        long v = ::strtol(s, &endp, 10);
        s = endp;
        return static_cast<i32>(v);
    };

    while (p < end) {
        skip_ws(p);
        if (p + 2 >= end) { next_line(p); continue; }
        // 行頭でトークン判定
        if (p[0] == 'v' && p[1] == ' ') {
            p += 2;
            f32 x = parse_f32(p);
            f32 y = parse_f32(p);
            f32 z = parse_f32(p);
            positions.PushBack(Vec3(x, y, z));
        } else if (p[0] == 'v' && p[1] == 'n') {
            p += 2;
            f32 x = parse_f32(p);
            f32 y = parse_f32(p);
            f32 z = parse_f32(p);
            normals.PushBack(Vec3(x, y, z));
        } else if (p[0] == 'v' && p[1] == 't') {
            p += 2;
            f32 u = parse_f32(p);
            f32 v = parse_f32(p);
            uvs.PushBack({u, v});
        } else if (p[0] == 'f' && p[1] == ' ') {
            p += 2;
            // f は 3 〜 4 頂点。v/vt/vn の 1-based 添字
            i32 vi[4] = {0,0,0,0}, ti[4] = {0,0,0,0}, ni[4] = {0,0,0,0};
            int n = 0;
            while (p < end && *p != '\n' && n < 4) {
                skip_ws(p);
                if (p >= end || *p == '\n') break;
                vi[n] = parse_index(p);
                if (p < end && *p == '/') {
                    ++p;
                    if (*p != '/') ti[n] = parse_index(p);
                    if (p < end && *p == '/') {
                        ++p;
                        ni[n] = parse_index(p);
                    }
                }
                ++n;
            }
            // 三角形化（クアッドは 0-1-2, 0-2-3）
            auto add_vertex = [&](int k) {
                MeshVertex mv{};
                if (vi[k] > 0 && static_cast<usize>(vi[k]) <= positions.Size())
                    mv.position = positions[vi[k] - 1];
                if (ni[k] > 0 && static_cast<usize>(ni[k]) <= normals.Size())
                    mv.normal = normals[ni[k] - 1];
                if (ti[k] > 0 && static_cast<usize>(ti[k]) <= uvs.Size()) {
                    mv.u = uvs[ti[k] - 1].u;
                    mv.v = uvs[ti[k] - 1].v;
                }
                u32 idx = static_cast<u32>(mesh->Vertices().Size());
                mesh->Vertices().PushBack(mv);
                mesh->Indices().PushBack(idx);
            };
            if (n >= 3) {
                add_vertex(0); add_vertex(1); add_vertex(2);
                if (n == 4) { add_vertex(0); add_vertex(2); add_vertex(3); }
            }
        }
        next_line(p);
    }

    SubMesh sub{};
    sub.first_index = 0;
    sub.index_count = static_cast<u32>(mesh->Indices().Size());
    mesh->SubMeshes().PushBack(sub);
    return Result<Rc<Asset>>(OkInit, WrapMesh(id, Move(mesh)));
}

// ---- FBX (ufbx) ---------------------------------------------------------
Result<Rc<Asset>> FbxAssetLoader::LoadFromBytes(AssetId id, const Array<byte>& bytes) noexcept {
    ufbx_load_opts opts{};
    ufbx_error err{};
    ufbx_scene* scene = ::ufbx_load_memory(bytes.Data(), bytes.Size(), &opts, &err);
    if (!scene) return ACS_ERR(Asset, 400, "ufbx_load_memory failed");

    auto mesh = MakeRc<MeshAsset>();
    u32 vertex_offset = 0;
    for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
        ufbx_mesh* fm = scene->meshes.data[mi];
        // 三角形化（ufbx の標準ヘルパ）
        usize tri_count = fm->num_triangles;
        Array<u32> tri_indices;
        tri_indices.Resize(tri_count * 3);
        usize triangle_idx = 0;
        for (size_t fi = 0; fi < fm->num_faces; ++fi) {
            ufbx_face face = fm->faces.data[fi];
            // 1 face = 多角形 → 三角形へ分割
            uint32_t local_indices[256];
            uint32_t added = ::ufbx_triangulate_face(local_indices, 256, fm, face);
            for (uint32_t t = 0; t < added; ++t) {
                tri_indices[triangle_idx++] = local_indices[t];
            }
        }
        // 頂点は ufbx_get_vertex_* で取得
        SubMesh sub{};
        sub.first_index = static_cast<u32>(mesh->Indices().Size());
        for (u32 i = 0; i < tri_indices.Size(); ++i) {
            uint32_t fi = tri_indices[i];
            ufbx_vec3 p = ::ufbx_get_vertex_vec3(&fm->vertex_position, fi);
            ufbx_vec3 n = fm->vertex_normal.exists ? ::ufbx_get_vertex_vec3(&fm->vertex_normal, fi) : ufbx_vec3{0,0,0};
            ufbx_vec2 t = fm->vertex_uv.exists ? ::ufbx_get_vertex_vec2(&fm->vertex_uv, fi) : ufbx_vec2{0,0};
            MeshVertex v{};
            v.position = Vec3(static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z));
            v.normal   = Vec3(static_cast<f32>(n.x), static_cast<f32>(n.y), static_cast<f32>(n.z));
            v.u        = static_cast<f32>(t.x);
            v.v        = static_cast<f32>(t.y);
            u32 idx = static_cast<u32>(mesh->Vertices().Size());
            mesh->Vertices().PushBack(v);
            mesh->Indices().PushBack(idx);
        }
        sub.index_count = static_cast<u32>(mesh->Indices().Size()) - sub.first_index;
        mesh->SubMeshes().PushBack(sub);
        vertex_offset = static_cast<u32>(mesh->Vertices().Size());
    }
    ::ufbx_free_scene(scene);
    return Result<Rc<Asset>>(OkInit, WrapMesh(id, Move(mesh)));
}

} // namespace acs
