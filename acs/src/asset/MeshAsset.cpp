// SPDX-License-Identifier: Apache-2.0
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

/** FBX の三角形化scratchに許可するindex数。 */
inline constexpr usize kFbxTriangulationScratchIndexCount = 256u;

/** FBXの要素数または範囲が公開mesh形式へ収まらない。 */
inline constexpr u16 kFbxSubCountOutOfRange = 401u;

/** FBX mesh構築用の確保に失敗した。 */
inline constexpr u16 kFbxSubOutOfMemory = 402u;

/** FBX faceの三角形化に失敗した。 */
inline constexpr u16 kFbxSubTriangulationFailed = 403u;

/** FBX sceneを全経路で解放する所有scope。 */
class CFbxSceneScope final {
public:
    /** 解放対象sceneを所有する。 */
    explicit CFbxSceneScope(ufbx_scene* scene) noexcept : m_Scene(scene) {}

    /** 所有sceneを解放する。 */
    ~CFbxSceneScope() noexcept
    {
        if (m_Scene != nullptr) ::ufbx_free_scene(m_Scene);
    }

    /** コピーを禁止する。 */
    CFbxSceneScope(const CFbxSceneScope&) = delete;

    /** コピー代入を禁止する。 */
    CFbxSceneScope& operator=(const CFbxSceneScope&) = delete;

    /** 所有sceneを返す。 */
    ufbx_scene* Get() const noexcept { return m_Scene; }

private:
    /** 解放対象のufbx scene。 */
    ufbx_scene* m_Scene = nullptr;
};

/** FBX scene全体を構築するための検査済み要素数。 */
struct FFbxBuildCounts {
    /** 出力するsubmesh数。 */
    usize mesh_count = 0u;

    /** 出力する頂点数とindex数。 */
    usize element_count = 0u;
};

/** 乗算結果が指定上限へ収まるかを返す。 */
constexpr bool FbxProductFits(usize left, usize right, usize limit) noexcept
{
    return left == 0u || right <= limit / left;
}

/** 加算結果が指定上限へ収まるかを返す。 */
constexpr bool FbxSumFits(usize left, usize right, usize limit) noexcept
{
    return left <= limit && right <= limit - left;
}

/**
 * 指定上限内で二つの要素数を乗算する。
 * @param left 左辺。
 * @param right 右辺。
 * @param limit 許可する積の上限。
 * @param output 成功時の積。失敗時は変更しない。
 * @return 積がusizeとlimitへ収まる場合はtrue。
 */
bool TryMultiplyFbxCount(usize left, usize right, usize limit, usize& output) noexcept
{
    if (!FbxProductFits(left, right, limit)) return false;
    output = left * right;
    return true;
}

/**
 * 指定上限内で二つの要素数を加算する。
 * @param left 左辺。
 * @param right 右辺。
 * @param limit 許可する和の上限。
 * @param output 成功時の和。失敗時は変更しない。
 * @return 和がusizeとlimitへ収まる場合はtrue。
 */
bool TryAddFbxCount(usize left, usize right, usize limit, usize& output) noexcept
{
    if (!FbxSumFits(left, right, limit)) return false;
    output = left + right;
    return true;
}

/** u32公開形式の乗算境界。 */
inline constexpr usize kMaximumFbxElementCount = static_cast<usize>(~u32(0));

static_assert(FbxProductFits(kMaximumFbxElementCount / 3u, 3u, kMaximumFbxElementCount));
static_assert(!FbxProductFits(kMaximumFbxElementCount / 3u + 1u, 3u, kMaximumFbxElementCount));
static_assert(FbxSumFits(kMaximumFbxElementCount - 1u, 1u, kMaximumFbxElementCount));
static_assert(!FbxSumFits(kMaximumFbxElementCount, 1u, kMaximumFbxElementCount));

/**
 * FBX sceneの全出力数とface範囲を、確保や三角形化より前に検査する。
 * @param scene ufbxでparse済みのscene。
 * @param output 成功時に検査済み要素数を受け取る。失敗時は変更しない。
 * @return 全meshがu32公開形式と固定scratchへ収まる場合はtrue。
 */
bool TryCalculateFbxBuildCounts(const ufbx_scene& scene, FFbxBuildCounts& output) noexcept
{
    if (scene.meshes.count > kMaximumFbxElementCount) return false;
    if (scene.meshes.count != 0u && scene.meshes.data == nullptr) return false;

    /** 全検査が終わるまで呼び出し元へ渡さない要素数。 */
    FFbxBuildCounts staged_counts{};
    staged_counts.mesh_count = scene.meshes.count;
    for (usize mesh_index = 0u; mesh_index < scene.meshes.count; ++mesh_index) {
        /** 現在検査するufbx mesh。 */
        const ufbx_mesh* mesh = scene.meshes.data[mesh_index];
        if (mesh == nullptr || mesh->faces.count != mesh->num_faces) return false;
        if (mesh->faces.count != 0u && mesh->faces.data == nullptr) return false;
        /** 現在meshが公開配列へ追加する要素数。 */
        usize mesh_element_count = 0u;
        if (!TryMultiplyFbxCount(mesh->num_triangles, 3u, kMaximumFbxElementCount, mesh_element_count)) return false;

        /** 現在meshを追加した後の全要素数。 */
        usize next_element_count = 0u;
        if (!TryAddFbxCount(staged_counts.element_count, mesh_element_count, kMaximumFbxElementCount, next_element_count)) return false;

        /** face列が主張する三角形数。 */
        usize counted_triangles = 0u;
        for (usize face_index = 0u; face_index < mesh->faces.count; ++face_index) {
            /** 現在検査するface。 */
            const ufbx_face face = mesh->faces.data[face_index];

            /** ufbx mesh内でのface先頭index。 */
            const usize face_begin = static_cast<usize>(face.index_begin);
            if (face_begin > mesh->num_indices) return false;
            if (static_cast<usize>(face.num_indices) > mesh->num_indices - face_begin) return false;
            if (face.num_indices < 3u) continue;

            /** 現在faceが必要とする三角形数。 */
            const usize face_triangle_count = static_cast<usize>(face.num_indices) - 2u;

            /** 現在faceを含む三角形数。 */
            usize next_triangle_count = 0u;
            if (!TryAddFbxCount(counted_triangles, face_triangle_count, mesh->num_triangles, next_triangle_count)) return false;
            counted_triangles = next_triangle_count;

            if (!FbxProductFits(face_triangle_count, 3u, kFbxTriangulationScratchIndexCount)) return false;
        }
        if (counted_triangles != mesh->num_triangles) return false;
        staged_counts.element_count = next_element_count;
    }

    output = staged_counts;
    return true;
}

/**
 * 事前確保で既に進んだrevisionを差し引き、旧loaderのmutable accessor一回分を再現する。
 * @param mesh revisionを進める構築途中mesh。
 * @param absorbed_mutation_count 事前確保の三accessorが既に再現した残り回数。
 */
void RecordLegacyFbxGeometryMutation(AMeshAsset& mesh, usize& absorbed_mutation_count) noexcept
{
    if (absorbed_mutation_count != 0u) {
        --absorbed_mutation_count;
        return;
    }
    mesh.MarkGeometryDirty();
}

/**
 * 構築済み AMeshAsset に ID と Ready 状態を設定し TSharedPtr<Asset> として返す。
 *
 * @param id アセットに割り当てる ID。
 * @param m 構築済みの AMeshAsset (所有権がムーブされる)。
 * @return Ready 状態にした Asset への共有ポインタ。
 */
TSharedPtr<AAsset> WrapMesh(FAssetId id, TSharedPtr<AMeshAsset>&& m) noexcept {
    m->SetId(id);
    m->SetState(EAssetState::Ready);
    return TSharedPtr<AAsset>(Move(m));
}

/**
 * cgltf accessor から FVec3 を読み出し、setter 経由で各頂点へ書き込む。
 *
 * @param a 読み出し元の cgltf accessor (nullptr なら何もしない)。
 * @param dst 書き込み先の頂点配列先頭。
 * @param n 処理する頂点数。
 * @param stride 未使用 (互換用に保持)。
 * @param setter 読み出した FVec3 を MeshVertex の該当フィールドへ書く関数。
 */
void ReadAttributeVec3(const cgltf_accessor* a, FMeshVertex* dst, usize n,
                       usize stride, void (*setter)(FMeshVertex&, FVec3)) noexcept {
    if (!a) return;
    f32 buf[3];
    for (usize i = 0; i < n; ++i) {
        if (::cgltf_accessor_read_float(a, i, buf, 3)) {
            setter(dst[i], FVec3(buf[0], buf[1], buf[2]));
        }
    }
    (void)stride;
}

/**
 * cgltf accessor から UV (FVec2) を読み出し各頂点の u/v へ書き込む。
 *
 * @param a 読み出し元の cgltf accessor (nullptr なら何もしない)。
 * @param dst 書き込み先の頂点配列先頭。
 * @param n 処理する頂点数。
 */
void ReadAttributeUV(const cgltf_accessor* a, FMeshVertex* dst, usize n) noexcept {
    if (!a) return;
    f32 buf[2];
    for (usize i = 0; i < n; ++i) {
        if (::cgltf_accessor_read_float(a, i, buf, 2)) {
            dst[i].u = buf[0];
            dst[i].v = buf[1];
        }
    }
}

/**
 * cgltf のシーンデータを 1 つの AMeshAsset へ flatten して構築する。
 *
 * @details 全メッシュ・全プリミティブを走査し、POSITION/NORMAL/TEXCOORD_0 を頂点へ、
 * インデックス (無ければ連番) をサブメッシュとして 1 つのアセットにまとめる。
 * @param data パース済みの cgltf データ (nullptr やメッシュ無しなら空メッシュを返す)。
 * @return 構築した AMeshAsset。
 */
TSharedPtr<AMeshAsset> BuildFromCgltf(cgltf_data* data) noexcept {
    auto mesh = MakeShared<AMeshAsset>();
    if (!data || data->meshes_count == 0) return mesh;

    // 全プリミティブを 1 つの AMeshAsset に flatten（v1）
    u32 vertex_offset = 0;
    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& m = data->meshes[mi];
        for (cgltf_size pi = 0; pi < m.primitives_count; ++pi) {
            const cgltf_primitive& p = m.primitives[pi];
            if (!p.attributes_count) continue;

            // 頂点数を最初の attribute から取得
            const usize vcount = p.attributes[0].data->count;
            const usize start = mesh->Vertices().Num();
            mesh->Vertices().SetNum(start + vcount);
            FMeshVertex* dst = mesh->Vertices().GetData() + start;

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
                    [](FMeshVertex& v, FVec3 x) { v.position = x; });
            if (nrm)
                ReadAttributeVec3(nrm, dst, vcount, 0,
                    [](FMeshVertex& v, FVec3 x) { v.normal = x; });
            if (uv0) ReadAttributeUV(uv0, dst, vcount);

            // インデックス取得（無ければトライアングルストリップ的に連番生成）
            FSubMesh sub{};
            sub.first_index = static_cast<u32>(mesh->Indices().Num());
            if (p.indices) {
                cgltf_accessor* idx = p.indices;
                for (cgltf_size ii = 0; ii < idx->count; ++ii) {
                    cgltf_uint v = 0;
                    ::cgltf_accessor_read_uint(idx, ii, &v, 1);
                    mesh->Indices().Add(static_cast<u32>(v) + vertex_offset);
                }
                sub.index_count = static_cast<u32>(idx->count);
            } else {
                for (u32 ii = 0; ii < vcount; ++ii) mesh->Indices().Add(ii + vertex_offset);
                sub.index_count = static_cast<u32>(vcount);
            }
            mesh->SubMeshes().Add(sub);
            vertex_offset += static_cast<u32>(vcount);
        }
    }
    return mesh;
}

} // namespace

/** glTF (.gltf) をパースして AMeshAsset を構築する (埋め込みバッファのみ対応)。 */
TResult<TSharedPtr<AAsset>> CGltfAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    cgltf_options opts{};
    cgltf_data* data = nullptr;
    if (::cgltf_parse(&opts, bytes.GetData(), bytes.Num(), &data) != cgltf_result_success || !data)
        return ACS_ERR(Asset, 300, "cgltf_parse failed (.gltf)");
    // .gltf は別ファイルにバイナリを持つことがあるので、メモリ単体ロードは
    // 簡易対応として「埋め込みバッファのみ」サポート。
    if (::cgltf_load_buffers(&opts, data, nullptr) != cgltf_result_success) {
        ::cgltf_free(data);
        return ACS_ERR(Asset, 301, "cgltf_load_buffers failed (external bin not supported in v1)");
    }
    TSharedPtr<AMeshAsset> mesh = BuildFromCgltf(data);
    ::cgltf_free(data);
    return TResult<TSharedPtr<AAsset>>(OkInit, WrapMesh(id, Move(mesh)));
}

/** GLB (.glb) をパースして AMeshAsset を構築する。 */
TResult<TSharedPtr<AAsset>> CGlbAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    cgltf_options opts{};
    opts.type = cgltf_file_type_glb;
    cgltf_data* data = nullptr;
    if (::cgltf_parse(&opts, bytes.GetData(), bytes.Num(), &data) != cgltf_result_success || !data)
        return ACS_ERR(Asset, 310, "cgltf_parse failed (.glb)");
    if (::cgltf_load_buffers(&opts, data, nullptr) != cgltf_result_success) {
        ::cgltf_free(data);
        return ACS_ERR(Asset, 311, "cgltf_load_buffers failed");
    }
    TSharedPtr<AMeshAsset> mesh = BuildFromCgltf(data);
    ::cgltf_free(data);
    return TResult<TSharedPtr<AAsset>>(OkInit, WrapMesh(id, Move(mesh)));
}

/** OBJ (.obj) を自前パーサで読み込み AMeshAsset を構築する (v/vn/vt/f のみ対応、マテリアル無視)。 */
TResult<TSharedPtr<AAsset>> CObjAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    auto mesh = MakeShared<AMeshAsset>();
    TArray<FVec3> positions;
    TArray<FVec3> normals;
    struct FUv { f32 u, v; };
    TArray<FUv> uvs;

    // strtod/strtol は NUL 終端まで数字を読むため、bytes が NUL 終端でないと
    // バッファ末尾を越えて OOB read する。NUL 終端コピーを作ってから解析する。
    TArray<char> text;
    text.SetNum(bytes.Num() + 1);
    for (usize i = 0; i < bytes.Num(); ++i) text[i] = static_cast<char>(bytes[i]);
    text[bytes.Num()] = '\0';
    const char* p = text.GetData();
    const char* end = p + bytes.Num();

    auto skip_ws = [&](const char*& s) {
        while (s < end && (*s == ' ' || *s == '\t')) ++s;
    };
    auto next_line = [&](const char*& s) {
        while (s < end && *s != '\n') ++s;
        if (s < end) ++s;
    };
    auto parse_f32 = [&](const char*& s) -> f32 {
        char* endp = nullptr;
        const f32 v = static_cast<f32>(::strtod(s, &endp));
        s = endp;
        return v;
    };
    auto parse_index = [&](const char*& s) -> i32 {
        char* endp = nullptr;
        const long v = ::strtol(s, &endp, 10);
        s = endp;
        return static_cast<i32>(v);
    };

    while (p < end) {
        skip_ws(p);
        if (p + 2 >= end) { next_line(p); continue; }
        // 行頭でトークン判定
        if (p[0] == 'v' && p[1] == ' ') {
            p += 2;
            const f32 x = parse_f32(p);
            const f32 y = parse_f32(p);
            const f32 z = parse_f32(p);
            positions.Add(FVec3(x, y, z));
        } else if (p[0] == 'v' && p[1] == 'n') {
            p += 2;
            const f32 x = parse_f32(p);
            const f32 y = parse_f32(p);
            const f32 z = parse_f32(p);
            normals.Add(FVec3(x, y, z));
        } else if (p[0] == 'v' && p[1] == 't') {
            p += 2;
            const f32 u = parse_f32(p);
            const f32 v = parse_f32(p);
            uvs.Add({u, v});
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
                    if (p < end && *p != '/') ti[n] = parse_index(p);
                    if (p < end && *p == '/') {
                        ++p;
                        ni[n] = parse_index(p);
                    }
                }
                ++n;
            }
            // 三角形化（クアッドは 0-1-2, 0-2-3）
            auto add_vertex = [&](int k) {
                FMeshVertex mv{};
                if (vi[k] > 0 && static_cast<usize>(vi[k]) <= positions.Num())
                    mv.position = positions[vi[k] - 1];
                if (ni[k] > 0 && static_cast<usize>(ni[k]) <= normals.Num())
                    mv.normal = normals[ni[k] - 1];
                if (ti[k] > 0 && static_cast<usize>(ti[k]) <= uvs.Num()) {
                    mv.u = uvs[ti[k] - 1].u;
                    mv.v = uvs[ti[k] - 1].v;
                }
                const u32 idx = static_cast<u32>(mesh->Vertices().Num());
                mesh->Vertices().Add(mv);
                mesh->Indices().Add(idx);
            };
            if (n >= 3) {
                add_vertex(0); add_vertex(1); add_vertex(2);
                if (n == 4) { add_vertex(0); add_vertex(2); add_vertex(3); }
            }
        }
        next_line(p);
    }

    FSubMesh sub{};
    sub.first_index = 0;
    sub.index_count = static_cast<u32>(mesh->Indices().Num());
    mesh->SubMeshes().Add(sub);
    return TResult<TSharedPtr<AAsset>>(OkInit, WrapMesh(id, Move(mesh)));
}

/** FBX (.fbx) を ufbx で読み込み、三角形化して AMeshAsset を構築する。 */
TResult<TSharedPtr<AAsset>> CFbxAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    ufbx_load_opts opts{};
    ufbx_error err{};
    CFbxSceneScope scene_scope(::ufbx_load_memory(bytes.GetData(), bytes.Num(), &opts, &err));
    ufbx_scene* const scene = scene_scope.Get();
    if (scene == nullptr) return ACS_ERR(Asset, 400, "ufbx_load_memory failed");

    /** 全確保より前に検査するscene全体の要素数。 */
    FFbxBuildCounts counts{};
    if (!TryCalculateFbxBuildCounts(*scene, counts)) return ACS_ERR(Asset, kFbxSubCountOutOfRange, "FBX mesh count or face range is invalid");

    /** 全構築が終わるまで結果へ公開しないmesh。 */
    TSharedPtr<AMeshAsset> staged_mesh = MakeShared<AMeshAsset>();
    if (!staged_mesh) return ACS_ERR(Memory, kFbxSubOutOfMemory, "FBX mesh allocation failed");

    // 旧loaderはmeshが無い場合にmutable accessorを呼ばず、revision 1を維持する。
    if (counts.mesh_count == 0u) return TResult<TSharedPtr<AAsset>>(OkInit, WrapMesh(id, Move(staged_mesh)));

    /** 現行loaderの三角形化結果列を保持する一時配列。 */
    TArray<u32> source_indices;
    if (!source_indices.TrySetNum(counts.element_count)) return ACS_ERR(Memory, kFbxSubOutOfMemory, "FBX source index allocation failed");

    /** 一度だけ取得して事前確保する頂点配列。 */
    TArray<FMeshVertex>& staged_vertices = staged_mesh->Vertices();

    /** 一度だけ取得して事前確保するindex配列。 */
    TArray<u32>& staged_indices = staged_mesh->Indices();

    /** 一度だけ取得して事前確保するsubmesh配列。 */
    TArray<FSubMesh>& staged_submeshes = staged_mesh->SubMeshes();
    if (!staged_vertices.TrySetNum(counts.element_count) || !staged_indices.TrySetNum(counts.element_count) || !staged_submeshes.TrySetNum(counts.mesh_count)) return ACS_ERR(Memory, kFbxSubOutOfMemory, "FBX mesh buffer allocation failed");

    /** 三配列の事前取得で既に進んだ旧mutable accessor相当回数。 */
    usize absorbed_mutation_count = 3u;

    /** 現在meshの出力先頭位置。 */
    usize mesh_element_begin = 0u;
    for (usize mesh_index = 0u; mesh_index < scene->meshes.count; ++mesh_index) {
        /** 現在構築するufbx mesh。 */
        const ufbx_mesh* mesh = scene->meshes.data[mesh_index];

        /** 現在meshが公開配列へ書く要素数。 */
        usize mesh_element_count = 0u;
        if (!TryMultiplyFbxCount(mesh->num_triangles, 3u, kMaximumFbxElementCount, mesh_element_count)) return ACS_ERR(Asset, kFbxSubCountOutOfRange, "FBX mesh count changed after preflight");

        /** 現在meshの出力終端位置。 */
        usize mesh_element_end = 0u;
        if (!TryAddFbxCount(mesh_element_begin, mesh_element_count, counts.element_count, mesh_element_end)) return ACS_ERR(Asset, kFbxSubCountOutOfRange, "FBX mesh range changed after preflight");

        // 旧loaderがsubmesh先頭取得時にIndices()を呼んだ一回分。
        RecordLegacyFbxGeometryMutation(*staged_mesh, absorbed_mutation_count);

        /** legacy loaderが三角形化結果を書いていた次の位置。 */
        usize source_write = mesh_element_begin;
        for (usize face_index = 0u; face_index < mesh->faces.count; ++face_index) {
            /** 現在三角形化するface。 */
            const ufbx_face face = mesh->faces.data[face_index];
            if (face.num_indices < 3u) continue;

            /** 既存loaderと同じ固定長のface三角形化scratch。 */
            u32 local_indices[kFbxTriangulationScratchIndexCount]{};

            /** ufbx内部panicを戻り値へ変換する診断値。 */
            ufbx_panic panic{};

            /** ufbxが生成した三角形数。 */
            const u32 added = ::ufbx_catch_triangulate_face(&panic, local_indices, kFbxTriangulationScratchIndexCount, mesh, face);
            if (panic.did_panic) return ACS_ERR(Asset, kFbxSubTriangulationFailed, "FBX face triangulation failed");
            if (static_cast<usize>(added) != static_cast<usize>(face.num_indices) - 2u) return ACS_ERR(Asset, kFbxSubTriangulationFailed, "FBX face triangle count changed after preflight");
            if (source_write > mesh_element_end || static_cast<usize>(added) > mesh_element_end - source_write) return ACS_ERR(Asset, kFbxSubCountOutOfRange, "FBX triangle count exceeds preflight count");

            // ufbxは3倍のindexを書いてtriangle数を返す。現行loaderは戻り値個だけ
            // 採用していたため、そのobservable列はこの安全化waveでは維持する。
            for (usize triangle = 0u; triangle < static_cast<usize>(added); ++triangle) source_indices[source_write++] = local_indices[triangle];
        }

        /** 現在meshのsubmesh範囲。 */
        FSubMesh& submesh = staged_submeshes[mesh_index];
        submesh.first_index = static_cast<u32>(mesh_element_begin);
        submesh.index_count = static_cast<u32>(mesh_element_count);
        for (usize output_index = mesh_element_begin; output_index < mesh_element_end; ++output_index) {
            /** ufbx属性列から読むsource index。 */
            const u32 source_index = source_indices[output_index];
            if (static_cast<usize>(source_index) >= mesh->num_indices) return ACS_ERR(Asset, kFbxSubCountOutOfRange, "FBX source index exceeds mesh range");

            /** source indexに対応する位置。 */
            const ufbx_vec3 position = ::ufbx_get_vertex_vec3(&mesh->vertex_position, source_index);

            /** source indexに対応する法線。 */
            const ufbx_vec3 normal = mesh->vertex_normal.exists ? ::ufbx_get_vertex_vec3(&mesh->vertex_normal, source_index) : ufbx_vec3{0, 0, 0};

            /** source indexに対応するUV。 */
            const ufbx_vec2 uv = mesh->vertex_uv.exists ? ::ufbx_get_vertex_vec2(&mesh->vertex_uv, source_index) : ufbx_vec2{0, 0};

            FMeshVertex& vertex = staged_vertices[output_index];
            vertex.position = FVec3(static_cast<f32>(position.x), static_cast<f32>(position.y), static_cast<f32>(position.z));
            vertex.normal = FVec3(static_cast<f32>(normal.x), static_cast<f32>(normal.y), static_cast<f32>(normal.z));
            vertex.u = static_cast<f32>(uv.x);
            vertex.v = static_cast<f32>(uv.y);
            staged_indices[output_index] = static_cast<u32>(output_index);

            // 旧loaderがVertices().Num()/Vertices().Add()/Indices().Add()で進めた三回分。
            RecordLegacyFbxGeometryMutation(*staged_mesh, absorbed_mutation_count);
            RecordLegacyFbxGeometryMutation(*staged_mesh, absorbed_mutation_count);
            RecordLegacyFbxGeometryMutation(*staged_mesh, absorbed_mutation_count);
        }

        // 旧loaderがIndices().Num()/SubMeshes().Add()/Vertices().Num()で進めた三回分。
        RecordLegacyFbxGeometryMutation(*staged_mesh, absorbed_mutation_count);
        RecordLegacyFbxGeometryMutation(*staged_mesh, absorbed_mutation_count);
        RecordLegacyFbxGeometryMutation(*staged_mesh, absorbed_mutation_count);
        mesh_element_begin = mesh_element_end;
    }

    if (mesh_element_begin != counts.element_count || absorbed_mutation_count != 0u) return ACS_ERR(Asset, kFbxSubCountOutOfRange, "FBX mesh totals changed after preflight");

    return TResult<TSharedPtr<AAsset>>(OkInit, WrapMesh(id, Move(staged_mesh)));
}

} // namespace acs
