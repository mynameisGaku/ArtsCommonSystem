// SPDX-License-Identifier: Apache-2.0
// ASkinnedMeshAsset + CAnimationPlayer 実装
#include "asset/SkinnedMesh.h"
#include "math/Math.h"
#include "foundation/Move.h"

#include <cmath>

#include <ufbx.h>

namespace acs {

namespace {

/**
 * ローカル TRS から行列を合成する (row-major: scale → rotation → translation)。
 *
 * @param t 平行移動。
 * @param r 回転。
 * @param s スケール。
 * @return 合成したローカル変換行列。
 */
FMat4 ComposeTRS(FVec3 t, FQuat r, FVec3 s) noexcept {
    return FMat4::Scale(s) * ToMatrix(r) * FMat4::Translation(t);
}

/**
 * アニメーションチャネルから時刻 t の TRS を補間サンプリングする。
 *
 * @details キーが空なら単位 TRS、端の外なら端のキー値、それ以外は隣接 2 キーを
 * 線形補間 (回転は slerp) する。キー探索は線形 (チャネルあたりのキー数が少ない前提)。
 * @param ch サンプリングするアニメーションチャネル。
 * @param t サンプリングする時刻 (秒)。
 * @param out_t 補間後の平行移動を受け取る。
 * @param out_r 補間後の回転を受け取る。
 * @param out_s 補間後のスケールを受け取る。
 */
void SampleChannel(const FAnimationChannel& ch, f32 t,
                   FVec3& out_t, FQuat& out_r, FVec3& out_s) noexcept {
    const usize n = ch.keys.Num();
    if (n == 0) {
        out_t = FVec3{0, 0, 0};
        out_r = FQuat{};
        out_s = FVec3{1, 1, 1};
        return;
    }
    if (n == 1 || t <= ch.keys[0].time) {
        const FAnimationKey& k = ch.keys[0];
        out_t = k.translation; out_r = k.rotation; out_s = k.scale;
        return;
    }
    if (t >= ch.keys[n - 1].time) {
        const FAnimationKey& k = ch.keys[n - 1];
        out_t = k.translation; out_r = k.rotation; out_s = k.scale;
        return;
    }
    // 二分探索ではなく線形（チャネルあたりキー数は通常少ない）
    usize i = 0;
    while (i + 1 < n && ch.keys[i + 1].time < t) ++i;
    const FAnimationKey& a = ch.keys[i];
    const FAnimationKey& b = ch.keys[i + 1];
    const f32 dt = b.time - a.time;
    const f32 alpha = dt > 1e-6f ? (t - a.time) / dt : 0.0f;
    out_t = FVec3{
        a.translation.x + (b.translation.x - a.translation.x) * alpha,
        a.translation.y + (b.translation.y - a.translation.y) * alpha,
        a.translation.z + (b.translation.z - a.translation.z) * alpha,
    };
    out_r = Slerp(a.rotation, b.rotation, alpha);
    out_s = FVec3{
        a.scale.x + (b.scale.x - a.scale.x) * alpha,
        a.scale.y + (b.scale.y - a.scale.y) * alpha,
        a.scale.z + (b.scale.z - a.scale.z) * alpha,
    };
}

} // namespace

/** 各ボーンのバインドワールド行列を親から合成し、その逆行列を inverse_bind に格納する。 */
void ASkinnedMeshAsset::ComputeInverseBindMatrices() noexcept {
    const u32 n = static_cast<u32>(m_Bones.Num());
    if (n == 0) return;

    // 1) 各ボーンのバインド世界行列を親から順に計算する。
    //    TArray<FBone> は親が i より小さい番号で並んでいる前提（前向き列挙可）。
    TArray<FMat4> world_at_bind;
    world_at_bind.SetNum(n);

    for (u32 i = 0; i < n; ++i) {
        const FBone& b = m_Bones[i];
        const FMat4 local = ComposeTRS(b.bind_translation, b.bind_rotation, b.bind_scale);
        // 親 index は子 (i) より小さい前提。不正な glTF で parent>=i や parent>=n の場合、
        // 未計算/範囲外の world_at_bind[parent] を読むと未初期化値読み or OOB になる。
        // parent が 0..i-1 の範囲にあるときだけ親乗算し、それ以外はローカルを採用する。
        if (b.parent >= 0 && static_cast<u32>(b.parent) < i) {
            world_at_bind[i] = local * world_at_bind[b.parent];
        } else {
            world_at_bind[i] = local;
        }
    }

    // 2) 逆行列 = inverse(bind_world)
    for (u32 i = 0; i < n; ++i) {
        m_Bones[i].inverse_bind = Inverse(world_at_bind[i]);
    }
}

/** 指定アニメーションを先頭から再生開始する (mesh 未設定・範囲外は無視)。 */
void CAnimationPlayer::Play(u32 anim_index, bool loop) noexcept {
    if (!m_Mesh) return;
    if (anim_index >= m_Mesh->Animations().Num()) return;
    m_Anim = static_cast<i32>(anim_index);
    m_bLoop = loop;
    m_Time = 0;
    m_Playing = true;
}

/** 再生時刻を dt 進め、ループ時は wrap、非ループ時は終端でクランプして停止する。 */
void CAnimationPlayer::Update(f32 dt) noexcept {
    if (!m_Playing || !m_Mesh || m_Anim < 0) return;
    if (m_Anim >= static_cast<i32>(m_Mesh->Animations().Num())) return;
    const FAnimation& a = m_Mesh->Animations()[m_Anim];

    if (m_bLoop && a.duration > 0.0f && std::isfinite(a.duration)) {
        // 非有限入力では再生時刻と再生状態を変更しない。
        if (!std::isfinite(m_Time) || !std::isfinite(dt)) return;

        /** wrapに使う正の有限duration。 */
        const f64 duration = static_cast<f64>(a.duration);

        /** f32 の加算overflowを避けるため、wrap前の時刻を倍精度で合成する。 */
        const f64 summed_time = static_cast<f64>(m_Time) + static_cast<f64>(dt);

        /** duration内へ一回で折り返した時刻。 */
        f64 wrapped_time = std::fmod(summed_time, duration);
        if (wrapped_time < 0.0) wrapped_time += duration;
        /** f32へ丸めたloop時刻。 */
        f32 narrowed_time = static_cast<f32>(wrapped_time);

        // f32への丸めでduration端に達した値は、旧f32加算と同じく先頭へ戻す。
        if (narrowed_time >= a.duration) narrowed_time = 0.0f;
        // 負の0を公開しない。
        if (narrowed_time == 0.0f) narrowed_time = 0.0f;
        m_Time = narrowed_time;
        return;
    }

    m_Time += dt;
    if (a.duration > 0) {
        if (m_bLoop) {
            // fmod 風（負値ガード付き）
            while (m_Time >= a.duration) m_Time -= a.duration;
            while (m_Time < 0)            m_Time += a.duration;
        } else {
            if (m_Time > a.duration) {
                m_Time = a.duration;
                m_Playing = false;
            }
        }
    }
}

/** 現在時刻の (world * inverse_bind) ボーンパレットを書き込み、書き込んだボーン数を返す。 */
u32 CAnimationPlayer::WritePalette(FMat4* out_palette, u32 max_count) const noexcept {
    if (!m_Mesh || !out_palette) return 0;
    const TArray<FBone>& bones = m_Mesh->Bones();
    const u32 nb = static_cast<u32>(bones.Num());
    const u32 count = nb < max_count ? nb : max_count;
    if (count == 0) return 0;

    // 1) 各ボーンの「アニメーション後ローカル」を求める
    //    アニメ無し（m_Anim == -1）の場合はバインド姿勢の TRS を使う
    static constexpr u32 kStackBones = 256;
    FMat4 local_pose[kStackBones];
    FMat4 world_pose[kStackBones];
    if (count > kStackBones) {
        // ボーン数が多すぎる場合は max_count に丸めて palette に書く
        // パレット計算自体は正しく動く
    }
    const u32 effective = count < kStackBones ? count : kStackBones;

    // 初期値: バインドローカル
    for (u32 i = 0; i < effective; ++i) {
        const FBone& b = bones[i];
        local_pose[i] = ComposeTRS(b.bind_translation, b.bind_rotation, b.bind_scale);
    }

    // アニメーションチャネルで上書き
    if (m_Anim >= 0 && m_Anim < static_cast<i32>(m_Mesh->Animations().Num())) {
        const FAnimation& a = m_Mesh->Animations()[m_Anim];
        for (usize ci = 0; ci < a.channels.Num(); ++ci) {
            const FAnimationChannel& ch = a.channels[ci];
            if (ch.bone_index < 0 || ch.bone_index >= static_cast<i32>(effective)) continue;
            FVec3 t, s; FQuat r;
            SampleChannel(ch, m_Time, t, r, s);
            local_pose[ch.bone_index] = ComposeTRS(t, r, s);
        }
    }

    // 2) ローカル → ワールド（親が小さい index にいる前提）
    for (u32 i = 0; i < effective; ++i) {
        const i32 parent = bones[i].parent;
        if (parent < 0) {
            world_pose[i] = local_pose[i];
        } else if (parent < static_cast<i32>(i)) {
            world_pose[i] = local_pose[i] * world_pose[parent];
        } else {
            // 想定外（親が後ろにある）→ ローカルをそのまま使う
            world_pose[i] = local_pose[i];
        }
    }

    // 3) palette = world * inverse_bind
    for (u32 i = 0; i < effective; ++i) {
        out_palette[i] = bones[i].inverse_bind * world_pose[i];
    }

    // 余り（max_count > kStackBones の場合）は単位行列で埋める
    for (u32 i = effective; i < count; ++i) {
        out_palette[i] = FMat4::Identity();
    }
    return count;
}

// ---- FBX からの取り込み ----------------------------------------------------

namespace {

/** 1 頂点が持てる影響ボーンの数 (FSkinnedVertex の配列長)。 */
constexpr u32 kSkinnedInfluenceCount = 4u;

/** ボーンの上限。FSkinnedVertex の index が u8 なので 256 が上限。 */
constexpr usize kSkinnedMaxBones = 256u;

/** 面を三角形へ割るときの一時領域。 */
constexpr usize kSkinnedTriangulationScratch = 256u;

/** 読み込み失敗の内訳。 */
constexpr u16 kSkinnedFbxSubParseFailed  = 410u;
constexpr u16 kSkinnedFbxSubNoSkin       = 411u;
constexpr u16 kSkinnedFbxSubOutOfMemory  = 412u;
constexpr u16 kSkinnedFbxSubTooManyBones = 413u;
constexpr u16 kSkinnedFbxSubTriangulate  = 414u;

/** ufbx scene を必ず解放する。 */
class CSkinnedFbxScene final {
public:
    explicit CSkinnedFbxScene(ufbx_scene* scene) noexcept : m_Scene(scene) {}
    ~CSkinnedFbxScene() noexcept { if (m_Scene != nullptr) ::ufbx_free_scene(m_Scene); }
    CSkinnedFbxScene(const CSkinnedFbxScene&) = delete;
    CSkinnedFbxScene& operator=(const CSkinnedFbxScene&) = delete;
    ufbx_scene* Get() const noexcept { return m_Scene; }

private:
    ufbx_scene* m_Scene = nullptr;
};

/**
 * ufbx の行列を ACS の行列へ移す。
 *
 * @details
 * ufbx は列ベクトル (v' = M * v) で 4x3、ACS は行ベクトル (v' = v * M) で row-major。
 * 入れ替えないと、回転と平行移動が転置されたまま骨に効いて形が破綻する。
 * ufbx の cols[i] が、そのまま ACS の行 i になる。
 *
 * @param m ufbx の行列。
 * @return ACS の行列。
 */
FMat4 FromUfbxMatrix(const ufbx_matrix& m) noexcept {
    FMat4 out = FMat4::Identity();
    for (u32 i = 0u; i < 4u; ++i) {
        out.m[i][0] = static_cast<f32>(m.cols[i].x);
        out.m[i][1] = static_cast<f32>(m.cols[i].y);
        out.m[i][2] = static_cast<f32>(m.cols[i].z);
        out.m[i][3] = (i == 3u) ? 1.0f : 0.0f;
    }
    return out;
}

/** ufbx のベクトルを移す。 */
FVec3 FromUfbxVec3(const ufbx_vec3& v) noexcept {
    return FVec3{static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)};
}

/** ufbx のクォータニオンを移す。 */
FQuat FromUfbxQuat(const ufbx_quat& q) noexcept {
    FQuat out;
    out.x = static_cast<f32>(q.x);
    out.y = static_cast<f32>(q.y);
    out.z = static_cast<f32>(q.z);
    out.w = static_cast<f32>(q.w);
    return out;
}

/** ufbx の文字列を移す。 */
FString FromUfbxString(const ufbx_string& s) noexcept {
    if (s.data == nullptr || s.length == 0u) return FString();
    return FString(FStringView(s.data, s.length));
}

/** ボーンとして採用したノードと、その並び順。 */
struct FSkinnedBoneTable {
    /** ボーン番号順のノード。 */
    TArray<const ufbx_node*> nodes;

    /**
     * ノードの element_id からボーン番号を引く表。
     *
     * @details element_id は scene 内で一意。ボーンでないノードは -1。
     */
    TArray<i32> index_by_element;
};

/**
 * ノードとその祖先を、親が先に来る順で表へ足す。
 *
 * @details
 * 祖先を落とすと親の回転が失われ、腰から上だけが取り残されたように崩れる。
 * 親を先に入れるのは、CAnimationPlayer が「親は自分より小さい番号」を前提に
 * world を組むため。
 *
 * @param node 足すノード。
 * @param table 追加先。
 * @return 足せたら true。上限超過や確保失敗で false。
 */
bool TryAddBoneChain(const ufbx_node* node, FSkinnedBoneTable& table) noexcept {
    if (node == nullptr || node->is_root) return true;
    if (static_cast<usize>(node->element_id) < table.index_by_element.Num()
        && table.index_by_element[node->element_id] >= 0) return true;

    if (!TryAddBoneChain(node->parent, table)) return false;
    if (table.nodes.Num() >= kSkinnedMaxBones) return false;
    if (!table.nodes.TryAdd(node)) return false;

    table.index_by_element[node->element_id] = static_cast<i32>(table.nodes.Num()) - 1;
    return true;
}

/**
 * 重みの大きい 4 本を採って合計 1 に均す。
 *
 * @details
 * ufbx は既に降順で並べているので先頭から採るだけ。正規化しないと、4 本に切った分の
 * 重みが消えて、その頂点だけ縮んで見える。
 *
 * @param skin 読み元の skin。
 * @param vertex_index メッシュの頂点番号。
 * @param table ボーン表。
 * @param out 書き込み先の頂点。
 */
void FillSkinInfluences(const ufbx_skin_deformer& skin, u32 vertex_index,
                        const FSkinnedBoneTable& table, FSkinnedVertex& out) noexcept {
    for (u32 i = 0u; i < kSkinnedInfluenceCount; ++i) {
        out.bones[i] = 0u;
        out.weights[i] = 0.0f;
    }
    if (static_cast<usize>(vertex_index) >= skin.vertices.count) {
        out.weights[0] = 1.0f;
        return;
    }

    const ufbx_skin_vertex entry = skin.vertices.data[vertex_index];
    f32 total = 0.0f;
    u32 taken = 0u;
    for (u32 i = 0u; i < entry.num_weights && taken < kSkinnedInfluenceCount; ++i) {
        const usize weight_index = static_cast<usize>(entry.weight_begin) + i;
        if (weight_index >= skin.weights.count) break;

        const ufbx_skin_weight weight = skin.weights.data[weight_index];
        if (static_cast<usize>(weight.cluster_index) >= skin.clusters.count) continue;

        const ufbx_skin_cluster* const cluster = skin.clusters.data[weight.cluster_index];
        if (cluster == nullptr || cluster->bone_node == nullptr) continue;
        if (static_cast<usize>(cluster->bone_node->element_id) >= table.index_by_element.Num()) continue;

        const i32 bone = table.index_by_element[cluster->bone_node->element_id];
        if (bone < 0) continue;

        out.bones[taken] = static_cast<u8>(bone);
        out.weights[taken] = static_cast<f32>(weight.weight);
        total += out.weights[taken];
        ++taken;
    }

    if (total > 1e-8f) {
        const f32 inverse = 1.0f / total;
        for (u32 i = 0u; i < taken; ++i) out.weights[i] *= inverse;
    } else {
        // どのボーンにも結ばれていない頂点。0 番へ全部預けて、消えるのを防ぐ。
        out.bones[0] = 0u;
        out.weights[0] = 1.0f;
    }
}

} // namespace

TResult<TSharedPtr<ASkinnedMeshAsset>> LoadSkinnedMeshFromFbxMemory(
    const byte* data, usize size, f32 sample_rate) noexcept {
    if (data == nullptr || size == 0u)
        return ACS_ERR(Asset, kSkinnedFbxSubParseFailed, "empty FBX buffer");

    ufbx_load_opts opts{};
    ufbx_error err{};
    CSkinnedFbxScene scope(::ufbx_load_memory(data, size, &opts, &err));
    ufbx_scene* const scene = scope.Get();
    if (scene == nullptr)
        return ACS_ERR(Asset, kSkinnedFbxSubParseFailed, "ufbx_load_memory failed");

    // 1) スキンの付いたメッシュを 1 つ選ぶ。
    const ufbx_mesh* mesh = nullptr;
    const ufbx_skin_deformer* skin = nullptr;
    for (usize i = 0u; i < scene->meshes.count && mesh == nullptr; ++i) {
        const ufbx_mesh* const candidate = scene->meshes.data[i];
        if (candidate == nullptr || candidate->skin_deformers.count == 0u) continue;
        const ufbx_skin_deformer* const candidate_skin = candidate->skin_deformers.data[0];
        if (candidate_skin == nullptr || candidate_skin->clusters.count == 0u) continue;
        mesh = candidate;
        skin = candidate_skin;
    }
    if (mesh == nullptr || skin == nullptr)
        return ACS_ERR(Asset, kSkinnedFbxSubNoSkin, "no skinned mesh in FBX");

    // 2) ボーン表を作る。クラスタの骨と、そこから根までの祖先を、親が先の順で。
    FSkinnedBoneTable table;
    if (!table.index_by_element.TrySetNum(scene->elements.count))
        return ACS_ERR(Memory, kSkinnedFbxSubOutOfMemory, "bone table allocation failed");
    for (usize i = 0u; i < table.index_by_element.Num(); ++i) table.index_by_element[i] = -1;

    for (usize i = 0u; i < skin->clusters.count; ++i) {
        const ufbx_skin_cluster* const cluster = skin->clusters.data[i];
        if (cluster == nullptr || cluster->bone_node == nullptr) continue;
        if (!TryAddBoneChain(cluster->bone_node, table))
            return ACS_ERR(Asset, kSkinnedFbxSubTooManyBones, "FBX skeleton exceeds 256 bones");
    }
    if (table.nodes.IsEmpty())
        return ACS_ERR(Asset, kSkinnedFbxSubNoSkin, "FBX skin has no usable bones");

    TSharedPtr<ASkinnedMeshAsset> staged = MakeShared<ASkinnedMeshAsset>();
    if (!staged) return ACS_ERR(Memory, kSkinnedFbxSubOutOfMemory, "skinned mesh allocation failed");

    // 3) ボーンのバインド姿勢と逆バインド行列。
    TArray<FBone>& bones = staged->Bones();
    if (!bones.TrySetNum(table.nodes.Num()))
        return ACS_ERR(Memory, kSkinnedFbxSubOutOfMemory, "bone allocation failed");

    for (usize i = 0u; i < table.nodes.Num(); ++i) {
        const ufbx_node* const node = table.nodes[i];
        FBone& bone = bones[i];
        bone.name = FromUfbxString(node->name);
        bone.parent = (node->parent != nullptr && !node->parent->is_root
                       && static_cast<usize>(node->parent->element_id) < table.index_by_element.Num())
            ? table.index_by_element[node->parent->element_id] : -1;
        bone.bind_translation = FromUfbxVec3(node->local_transform.translation);
        bone.bind_rotation    = FromUfbxQuat(node->local_transform.rotation);
        bone.bind_scale       = FromUfbxVec3(node->local_transform.scale);
        // 骨でない祖先は誰も参照しないので単位のまま。
        bone.inverse_bind = FMat4::Identity();
    }
    // 逆バインドは「いま入れた bind TRS の連鎖」から組み直す。
    //
    // 最初は ufbx の geometry_to_bone をそのまま入れていたが、**バインド姿勢で
    // palette が単位行列にならなかった** (単体テストで捕まえた)。cluster が持つ
    // バインドと、node の local_transform が指す姿勢が食い違うファイルがあるため。
    //
    // 食い違ったままだと、頂点が骨と別の場所へ飛んで «真っ黒» や «消える» になる。
    // どちらか一方に揃えるしかないので、**player が world を組む元と同じ TRS** に揃える。
    // 代償として、保存時の姿勢がバインド姿勢と違うファイルでは «その姿勢» が基準になる。
    staged->ComputeInverseBindMatrices();

    // 4) 三角形へ割って頂点を作る。位置・法線・UV は静的メッシュと同じ読み方。
    TArray<FSkinnedVertex>& vertices = staged->Vertices();
    TArray<u32>& indices = staged->Indices();
    const usize element_count = static_cast<usize>(mesh->num_triangles) * 3u;
    if (element_count == 0u)
        return ACS_ERR(Asset, kSkinnedFbxSubTriangulate, "FBX skinned mesh has no triangles");
    if (!vertices.TrySetNum(element_count) || !indices.TrySetNum(element_count))
        return ACS_ERR(Memory, kSkinnedFbxSubOutOfMemory, "skinned vertex allocation failed");

    usize written = 0u;
    for (usize face_index = 0u; face_index < mesh->faces.count; ++face_index) {
        const ufbx_face face = mesh->faces.data[face_index];
        if (face.num_indices < 3u) continue;

        u32 local_indices[kSkinnedTriangulationScratch]{};
        ufbx_panic panic{};
        const u32 added = ::ufbx_catch_triangulate_face(
            &panic, local_indices, kSkinnedTriangulationScratch, mesh, face);
        if (panic.did_panic)
            return ACS_ERR(Asset, kSkinnedFbxSubTriangulate, "FBX face triangulation failed");

        const usize produced = static_cast<usize>(added) * 3u;
        if (produced > element_count - written)
            return ACS_ERR(Asset, kSkinnedFbxSubTriangulate, "FBX triangle count exceeds preflight");

        for (usize i = 0u; i < produced; ++i) {
            const u32 source = local_indices[i];
            if (static_cast<usize>(source) >= mesh->num_indices)
                return ACS_ERR(Asset, kSkinnedFbxSubTriangulate, "FBX source index out of range");

            const ufbx_vec3 position = ::ufbx_get_vertex_vec3(&mesh->vertex_position, source);
            const ufbx_vec3 normal = mesh->vertex_normal.exists
                ? ::ufbx_get_vertex_vec3(&mesh->vertex_normal, source) : ufbx_vec3{0, 1, 0};
            const ufbx_vec2 uv = mesh->vertex_uv.exists
                ? ::ufbx_get_vertex_vec2(&mesh->vertex_uv, source) : ufbx_vec2{0, 0};

            FSkinnedVertex& vertex = vertices[written];
            vertex.position = FromUfbxVec3(position);
            vertex.normal = FromUfbxVec3(normal);
            vertex.u = static_cast<f32>(uv.x);
            vertex.v = static_cast<f32>(uv.y);
            FillSkinInfluences(*skin, mesh->vertex_indices.data[source], table, vertex);

            indices[written] = static_cast<u32>(written);
            ++written;
        }
    }
    if (written != element_count)
        return ACS_ERR(Asset, kSkinnedFbxSubTriangulate, "FBX triangle total changed");

    // 5) アニメーションを一定間隔で焼く。
    const f32 rate = (sample_rate > 0.0f && std::isfinite(sample_rate))
        ? sample_rate : kSkinnedFbxDefaultSampleRate;
    TArray<FAnimation>& animations = staged->Animations();

    for (usize stack_index = 0u; stack_index < scene->anim_stacks.count; ++stack_index) {
        const ufbx_anim_stack* const stack = scene->anim_stacks.data[stack_index];
        if (stack == nullptr || stack->anim == nullptr) continue;

        const f64 begin = stack->time_begin;
        const f64 duration = stack->time_end - begin;
        if (!(duration > 0.0)) continue;

        const usize sample_count =
            static_cast<usize>(duration * static_cast<f64>(rate)) + 1u;

        FAnimation animation;
        animation.name = FromUfbxString(stack->name);
        animation.duration = static_cast<f32>(duration);
        if (!animation.channels.TrySetNum(table.nodes.Num()))
            return ACS_ERR(Memory, kSkinnedFbxSubOutOfMemory, "animation channel allocation failed");

        for (usize bone = 0u; bone < table.nodes.Num(); ++bone) {
            FAnimationChannel& channel = animation.channels[bone];
            channel.bone_index = static_cast<i32>(bone);
            if (!channel.keys.TrySetNum(sample_count))
                return ACS_ERR(Memory, kSkinnedFbxSubOutOfMemory, "animation key allocation failed");

            for (usize sample = 0u; sample < sample_count; ++sample) {
                const f64 offset = static_cast<f64>(sample) / static_cast<f64>(rate);
                const f64 time = (offset < duration) ? offset : duration;
                const ufbx_transform transform =
                    ::ufbx_evaluate_transform(stack->anim, table.nodes[bone], begin + time);

                FAnimationKey& key = channel.keys[sample];
                key.time = static_cast<f32>(time);
                key.translation = FromUfbxVec3(transform.translation);
                key.rotation = FromUfbxQuat(transform.rotation);
                key.scale = FromUfbxVec3(transform.scale);
            }
        }

        if (!animations.TryAdd(Move(animation)))
            return ACS_ERR(Memory, kSkinnedFbxSubOutOfMemory, "animation allocation failed");
    }

    return TResult<TSharedPtr<ASkinnedMeshAsset>>(OkInit, Move(staged));
}

} // namespace acs
