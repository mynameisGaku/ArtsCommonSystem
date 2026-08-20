// SPDX-License-Identifier: Apache-2.0
#include "asset/MeshPrimitive.h"

#include "foundation/Move.h"
#include "math/Math.h"

#include <cmath>

namespace acs::Primitive {

namespace {

/** 構築途中のメッシュと、一度だけ取得した可変配列をまとめる。 */
struct FMeshBuild {
    /** 全データの確保と書き込みが完了するまで公開しないメッシュ。 */
    TSharedPtr<AMeshAsset> asset;

    /** 構築中の頂点配列。 */
    TArray<FMeshVertex>* vertices = nullptr;

    /** 構築中のインデックス配列。 */
    TArray<u32>* indices = nullptr;

    /** 構築中のサブメッシュ配列。 */
    TArray<FSubMesh>* submeshes = nullptr;
};

/**
 * 二つの要素数をオーバーフローさせずに加算する。
 * @param left 左辺。
 * @param right 右辺。
 * @param output 成功時の和。失敗時は変更しない。
 * @return 和を usize で表せる場合は true。
 */
bool TryAddCount(usize left, usize right, usize& output) noexcept
{
    if (right > (~usize(0)) - left) return false;
    output = left + right;
    return true;
}

/**
 * 二つの要素数をオーバーフローさせずに乗算する。
 * @param left 左辺。
 * @param right 右辺。
 * @param output 成功時の積。失敗時は変更しない。
 * @return 積を usize で表せる場合は true。
 */
bool TryMultiplyCount(usize left, usize right, usize& output) noexcept
{
    if (left != 0u && right > (~usize(0)) / left) return false;
    output = left * right;
    return true;
}

/**
 * 球メッシュの頂点数とインデックス数を安全に算出する。
 * @param segments 補正済みの経度分割数。
 * @param rings 補正済みの緯度分割数。
 * @param vertex_count 成功時の頂点数。失敗時は変更しない。
 * @param index_count 成功時のインデックス数。失敗時は変更しない。
 * @return 両方の個数を u32 と usize に表せる場合は true。
 */
bool TryCalculateSphereCounts(u32 segments, u32 rings, usize& vertex_count, usize& index_count) noexcept
{
    /** 一行あたりの頂点数。 */
    usize vertex_stride = 0u;
    if (!TryAddCount(static_cast<usize>(segments), 1u, vertex_stride)) return false;

    /** 頂点行数。 */
    usize vertex_rows = 0u;
    if (!TryAddCount(static_cast<usize>(rings), 1u, vertex_rows)) return false;

    /** 全頂点数の一時値。 */
    usize staged_vertex_count = 0u;
    if (!TryMultiplyCount(vertex_stride, vertex_rows, staged_vertex_count)) return false;

    /** 全四角形数の一時値。 */
    usize quad_count = 0u;
    if (!TryMultiplyCount(static_cast<usize>(segments), static_cast<usize>(rings), quad_count)) return false;

    /** 各四角形を二つの三角形へ分割したインデックス数。 */
    usize staged_index_count = 0u;
    if (!TryMultiplyCount(quad_count, 6u, staged_index_count)) return false;

    /** 公開インデックス形式が表せる最大個数。 */
    constexpr usize maximum_u32_count = static_cast<usize>(~u32(0));
    if (staged_vertex_count > maximum_u32_count || staged_index_count > maximum_u32_count) return false;

    vertex_count = staged_vertex_count;
    index_count = staged_index_count;
    return true;
}

/**
 * 生成に必要な三配列をローカルメッシュへ一括確保する。
 * @param vertex_count 必要な頂点数。
 * @param index_count 必要なインデックス数。
 * @param build 成功時に構築領域を受け取るローカル状態。失敗時は変更しない。
 * @return メッシュ本体と三配列をすべて確保できた場合は true。
 */
bool TryCreateMeshBuffers(usize vertex_count, usize index_count, FMeshBuild& build) noexcept
{
    /** 全確保が終わるまで呼び出し元へ渡さないメッシュ。 */
    TSharedPtr<AMeshAsset> staged_asset = MakeShared<AMeshAsset>();
    if (!staged_asset) return false;

    /** 一度だけ取得して形状revisionを従来どおり進める頂点配列。 */
    TArray<FMeshVertex>& staged_vertices = staged_asset->Vertices();

    /** 一度だけ取得して形状revisionを従来どおり進めるインデックス配列。 */
    TArray<u32>& staged_indices = staged_asset->Indices();

    /** 一度だけ取得して形状revisionを従来どおり進めるサブメッシュ配列。 */
    TArray<FSubMesh>& staged_submeshes = staged_asset->SubMeshes();
    if (!staged_vertices.TrySetNum(vertex_count) || !staged_indices.TrySetNum(index_count) || !staged_submeshes.TrySetNum(1u)) return false;

    build.vertices = &staged_vertices;
    build.indices = &staged_indices;
    build.submeshes = &staged_submeshes;
    build.asset = Move(staged_asset);
    return true;
}

/**
 * 確保済み頂点を指定位置へ書き込む。
 * @param vertices 書き込み先配列。
 * @param index 書き込み先の頂点番号。
 * @param position 頂点位置。
 * @param normal 頂点法線。
 * @param u UV の U 値。
 * @param v UV の V 値。
 */
void SetVertex(TArray<FMeshVertex>& vertices, usize index, FVec3 position, FVec3 normal, f32 u, f32 v) noexcept
{
    FMeshVertex& vertex = vertices[index];
    vertex.position = position;
    vertex.normal = normal;
    vertex.u = u;
    vertex.v = v;
}

/**
 * 単一サブメッシュを全インデックス範囲へ設定する。
 * @param build 構築中のメッシュ。
 * @param index_count 確保済みインデックス数。
 */
void SetWholeMeshRange(FMeshBuild& build, usize index_count) noexcept
{
    FSubMesh& submesh = (*build.submeshes)[0];
    submesh.first_index = 0u;
    submesh.index_count = static_cast<u32>(index_count);
}

} // namespace

bool TryMakeCube(f32 size, TSharedPtr<AMeshAsset>& output) noexcept
{
    if (!std::isfinite(size)) return false;

    /** 生成完了まで output から隔離する構築状態。 */
    FMeshBuild build;
    if (!TryCreateMeshBuffers(24u, 36u, build)) return false;

    /** 立方体の中心から各面までの距離。 */
    const f32 half_size = size * 0.5f;

    /** 一面の四頂点と外向き法線。 */
    struct FFace {
        FVec3 a;
        FVec3 b;
        FVec3 c;
        FVec3 d;
        FVec3 normal;
    };

    /** 互換 API と同じ面、頂点、UV の順序。 */
    const FFace faces[6] = {
        {{-half_size, -half_size, -half_size}, { half_size, -half_size, -half_size}, { half_size,  half_size, -half_size}, {-half_size,  half_size, -half_size}, { 0.0f,  0.0f, -1.0f}},
        {{ half_size, -half_size,  half_size}, {-half_size, -half_size,  half_size}, {-half_size,  half_size,  half_size}, { half_size,  half_size,  half_size}, { 0.0f,  0.0f,  1.0f}},
        {{-half_size, -half_size,  half_size}, {-half_size, -half_size, -half_size}, {-half_size,  half_size, -half_size}, {-half_size,  half_size,  half_size}, {-1.0f,  0.0f,  0.0f}},
        {{ half_size, -half_size, -half_size}, { half_size, -half_size,  half_size}, { half_size,  half_size,  half_size}, { half_size,  half_size, -half_size}, { 1.0f,  0.0f,  0.0f}},
        {{-half_size,  half_size, -half_size}, { half_size,  half_size, -half_size}, { half_size,  half_size,  half_size}, {-half_size,  half_size,  half_size}, { 0.0f,  1.0f,  0.0f}},
        {{-half_size, -half_size,  half_size}, { half_size, -half_size,  half_size}, { half_size, -half_size, -half_size}, {-half_size, -half_size, -half_size}, { 0.0f, -1.0f,  0.0f}},
    };

    for (usize face_index = 0u; face_index < 6u; ++face_index) {
        /** 現在面の先頭頂点番号。 */
        const usize vertex_base = face_index * 4u;
        SetVertex(*build.vertices, vertex_base + 0u, faces[face_index].a, faces[face_index].normal, 0.0f, 1.0f);
        SetVertex(*build.vertices, vertex_base + 1u, faces[face_index].b, faces[face_index].normal, 1.0f, 1.0f);
        SetVertex(*build.vertices, vertex_base + 2u, faces[face_index].c, faces[face_index].normal, 1.0f, 0.0f);
        SetVertex(*build.vertices, vertex_base + 3u, faces[face_index].d, faces[face_index].normal, 0.0f, 0.0f);

        /** 現在面の先頭インデックス番号。 */
        const usize index_base = face_index * 6u;
        const u32 first_vertex = static_cast<u32>(vertex_base);
        (*build.indices)[index_base + 0u] = first_vertex + 0u;
        (*build.indices)[index_base + 1u] = first_vertex + 1u;
        (*build.indices)[index_base + 2u] = first_vertex + 2u;
        (*build.indices)[index_base + 3u] = first_vertex + 0u;
        (*build.indices)[index_base + 4u] = first_vertex + 2u;
        (*build.indices)[index_base + 5u] = first_vertex + 3u;
    }

    SetWholeMeshRange(build, 36u);
    output = Move(build.asset);
    return true;
}

bool TryMakeSphere(f32 radius, u32 segments, u32 rings, TSharedPtr<AMeshAsset>& output) noexcept
{
    if (!std::isfinite(radius)) return false;
    if (segments < 3u) segments = 3u;
    if (rings < 2u) rings = 2u;

    /** 検査済みの頂点数。 */
    usize vertex_count = 0u;

    /** 検査済みのインデックス数。 */
    usize index_count = 0u;
    if (!TryCalculateSphereCounts(segments, rings, vertex_count, index_count)) return false;

    /** 生成完了まで output から隔離する構築状態。 */
    FMeshBuild build;
    if (!TryCreateMeshBuffers(vertex_count, index_count, build)) return false;

    /** 一行あたりの頂点数。 */
    const usize vertex_stride = static_cast<usize>(segments) + 1u;
    for (u32 ring = 0u; ring <= rings; ++ring) {
        /** 緯度方向の正規化座標。 */
        const f32 v = static_cast<f32>(ring) / static_cast<f32>(rings);

        /** 北極からの角度。 */
        const f32 phi = v * kPi;

        /** 同一緯度で共有する正弦。 */
        const f32 sin_phi = Sin(phi);

        /** 同一緯度で共有する余弦。 */
        const f32 cos_phi = Cos(phi);
        for (u32 segment = 0u; segment <= segments; ++segment) {
            /** 経度方向の正規化座標。 */
            const f32 u = static_cast<f32>(segment) / static_cast<f32>(segments);

            /** 経度方向の角度。 */
            const f32 theta = u * kPi * 2.0f;

            /** 球面上の外向き法線。 */
            const FVec3 normal{sin_phi * Cos(theta), cos_phi, sin_phi * Sin(theta)};

            /** 半径を反映した頂点位置。 */
            const FVec3 position{normal.x * radius, normal.y * radius, normal.z * radius};

            /** 行優先で並べる出力頂点番号。 */
            const usize vertex_index = static_cast<usize>(ring) * vertex_stride + static_cast<usize>(segment);
            SetVertex(*build.vertices, vertex_index, position, normal, u, v);
        }
    }

    /** 次に書くインデックス位置。 */
    usize output_index = 0u;
    for (u32 ring = 0u; ring < rings; ++ring) {
        for (u32 segment = 0u; segment < segments; ++segment) {
            /** 現在四角形の左上頂点。 */
            const usize top_left = static_cast<usize>(ring) * vertex_stride + static_cast<usize>(segment);

            /** 互換 API と同じ時計回り順序の四頂点。 */
            const u32 i0 = static_cast<u32>(top_left);
            const u32 i1 = i0 + 1u;
            const u32 i2 = static_cast<u32>(top_left + vertex_stride);
            const u32 i3 = i2 + 1u;
            (*build.indices)[output_index++] = i0;
            (*build.indices)[output_index++] = i2;
            (*build.indices)[output_index++] = i1;
            (*build.indices)[output_index++] = i1;
            (*build.indices)[output_index++] = i2;
            (*build.indices)[output_index++] = i3;
        }
    }

    SetWholeMeshRange(build, index_count);
    output = Move(build.asset);
    return true;
}

bool TryMakePlane(f32 width, f32 depth, TSharedPtr<AMeshAsset>& output) noexcept
{
    if (!std::isfinite(width) || !std::isfinite(depth)) return false;

    /** 生成完了まで output から隔離する構築状態。 */
    FMeshBuild build;
    if (!TryCreateMeshBuffers(4u, 6u, build)) return false;

    /** X 方向の半分の幅。 */
    const f32 half_width = width * 0.5f;

    /** Z 方向の半分の奥行き。 */
    const f32 half_depth = depth * 0.5f;

    /** 平面全体で共有する上向き法線。 */
    const FVec3 up{0.0f, 1.0f, 0.0f};
    SetVertex(*build.vertices, 0u, {-half_width, 0.0f, -half_depth}, up, 0.0f, 1.0f);
    SetVertex(*build.vertices, 1u, { half_width, 0.0f, -half_depth}, up, 1.0f, 1.0f);
    SetVertex(*build.vertices, 2u, { half_width, 0.0f,  half_depth}, up, 1.0f, 0.0f);
    SetVertex(*build.vertices, 3u, {-half_width, 0.0f,  half_depth}, up, 0.0f, 0.0f);

    /** 互換 API と同じ二三角形の頂点順序。 */
    constexpr u32 indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    for (usize index = 0u; index < 6u; ++index) (*build.indices)[index] = indices[index];

    SetWholeMeshRange(build, 6u);
    output = Move(build.asset);
    return true;
}

bool TryMakePolygonXY(const FVec2* points, u32 point_count, TSharedPtr<AMeshAsset>& output) noexcept
{
    if (points == nullptr || point_count < 3u) return false;

    /** 先頭点を共有するfanのtriangle数。 */
    const usize triangle_count = static_cast<usize>(point_count) - 2u;

    /** triangleごとに三つ必要なindex数。 */
    usize index_count = 0u;
    if (!TryMultiplyCount(triangle_count, 3u, index_count) || index_count > static_cast<usize>(~u32(0))) return false;
    for (u32 index = 0u; index < point_count; ++index) {
        if (!std::isfinite(points[index].x) || !std::isfinite(points[index].y)) return false;
    }

    FMeshBuild build;
    if (!TryCreateMeshBuffers(static_cast<usize>(point_count), index_count, build)) return false;
    for (u32 index = 0u; index < point_count; ++index)
        SetVertex(*build.vertices, index, FVec3{points[index].x, points[index].y, 0.0f}, FVec3{0.0f, 0.0f, 1.0f}, 0.0f, 0.0f);
    for (u32 triangle = 0u; triangle + 2u < point_count; ++triangle) {
        const usize base = static_cast<usize>(triangle) * 3u;
        (*build.indices)[base + 0u] = 0u;
        (*build.indices)[base + 1u] = triangle + 1u;
        (*build.indices)[base + 2u] = triangle + 2u;
    }
    SetWholeMeshRange(build, index_count);
    output = Move(build.asset);
    return true;
}

TSharedPtr<AMeshAsset> MakeCube(f32 size) noexcept
{
    TSharedPtr<AMeshAsset> output;
    (void)TryMakeCube(size, output);
    return output;
}

TSharedPtr<AMeshAsset> MakeSphere(f32 radius, u32 segments, u32 rings) noexcept
{
    TSharedPtr<AMeshAsset> output;
    (void)TryMakeSphere(radius, segments, rings, output);
    return output;
}

TSharedPtr<AMeshAsset> MakePlane(f32 width, f32 depth) noexcept
{
    TSharedPtr<AMeshAsset> output;
    (void)TryMakePlane(width, depth, output);
    return output;
}

} // namespace acs::Primitive
