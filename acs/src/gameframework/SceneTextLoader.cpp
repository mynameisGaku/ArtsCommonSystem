// SPDX-License-Identifier: Apache-2.0
#include "gameframework/SceneTextLoader.h"

#include "gameframework/Node2D.h"
#include "gameframework/Component2D.h"
#include "gameframework/ComponentFactory.h"   // CreateComponentByName
#include "gameframework/ReflectApply.h"        // ApplyFieldValue
#include "gameframework/Reflect.h"             // FTypeRegistry / FTypeDesc
#include "gameframework/PrimitiveRenderer2D.h" // FPrimitiveRenderer2D (builtin の typed setter)
#include "gameframework/PolygonRenderer2D.h"   // FPolygonRenderer2D (POLY 行の描画)
#include "gameframework/Sprite2DComponent.h"   // FSprite2DComponent (SPRT 行の描画)
#include "gameframework/Material2D.h"           // FMaterial2D (MAT 行 = 使用マテリアル)
#include "asset/ImageAsset.h"                  // ImageAssetLoader (画像デコード)
#include "render/RenderAssets.h"               // UploadTexture (GPU テクスチャ化)
#include "render/IRhiTexture.h"
#include "container/Array.h"
#include "foundation/Move.h"

#include <cstdio>
#include <cstring>

namespace acs::game {

namespace {

/** ノード行 1 件分の解析結果 (親解決と visual 設定のため保持)。 */
struct LoadedNode {
    int      id     = 0;
    int      parent = -1;
    FNode2D* node   = nullptr;
    FVec4    color{ 0.6f, 0.7f, 0.9f, 1.0f };
    f32      base   = 48.0f;
    // 剛体 (FRigidBody2D)。COMP の論理 slot 順を comp_count で数え、rigid_slot で CPROP を拾う。
    int      comp_count = 0;        // この node の COMP 行数 (= 論理 slot)
    int      rigid_slot = -1;       // FRigidBody2D の論理 slot (-1=無し)
    f32      rb[6]      = { 1.0f, 0.1f, 0.5f, 1.0f, 0.05f, 0.1f };  // bodyType,rest,fric,mass,linD,angD
    FVec2    poly[kMaxPolyVerts]{}; // POLY 行のローカル頂点 (物理の polygon 用)
    u32      polyCount  = 0u;
};

/** id からノードのインデックスを引く (線形)。 */
int FindLoadedIndex(const TArray<LoadedNode>& nodes, int id) noexcept {
    for (u32 i = 0; i < nodes.Size(); ++i)
        if (nodes[i].id == id) return static_cast<int>(i);
    return -1;
}

/** comp が FPrimitiveRenderer2D ならそのポインタを返す (反射名で判定)。 */
FPrimitiveRenderer2D* AsPrimitive(FComponent2D* c) noexcept {
    if (c == nullptr) return nullptr;
    const char* n = c->ReflectName();
    return (n != nullptr && std::strcmp(n, "FPrimitiveRenderer2D") == 0)
               ? static_cast<FPrimitiveRenderer2D*>(c) : nullptr;
}

} // namespace

FSceneBounds LoadAcsceneText(const char* text, FNode2D& root,
                             TArray<FSpriteRequest>* out_sprites,
                             TArray<FRigidBodyRequest>* out_bodies,
                             TArray<FMaterialTexRequest>* out_mat_tex) noexcept {
    FSceneBounds bounds;
    if (text == nullptr) return bounds;

    const char* p = text;
    char line[2048];
    auto read_line = [&](char* out, int outsz) -> bool {
        if (*p == '\0') return false;
        int k = 0;
        while (*p != '\0' && *p != '\n') { if (k < outsz - 1) out[k++] = *p; ++p; }
        out[k] = '\0';
        if (*p == '\n') ++p;
        return true;
    };

    if (!read_line(line, sizeof(line)) || std::strncmp(line, "ACSCENE", 7) != 0) return bounds;
    if (!read_line(line, sizeof(line))) return bounds;
    int count = 0;
    std::sscanf(line, "%d", &count);

    TArray<LoadedNode> nodes;

    // --- ノード行: root 直下に平坦生成 (親付けは後で) ---
    for (int i = 0; i < count; ++i) {
        if (!read_line(line, sizeof(line))) break;
        LoadedNode ln;
        float x = 0, y = 0, rot = 0, sx = 1, sy = 1, base = 48, r = 0.6f, g = 0.7f, b = 0.9f, a = 1;
        int consumed = 0;
        const int got = std::sscanf(line, "%d %d %f %f %f %f %f %f %f %f %f %f %n",
            &ln.id, &ln.parent, &x, &y, &rot, &sx, &sy, &base, &r, &g, &b, &a, &consumed);
        if (got < 12) continue;
        FNode2D& child = root.AddChild(MakeUnique<FNode2D>());
        child.Local() = FTransform2D{ FVec2{ x, y }, rot, FVec2{ sx, sy } };
        ln.node = &child; ln.base = base; ln.color = FVec4{ r, g, b, a };
        nodes.PushBack(ln);
    }

    // --- COMP / CPROP を取り込む (COMP→CPROP の順に並ぶ) ---
    while (read_line(line, sizeof(line))) {
        if (std::strncmp(line, "COMP ", 5) == 0) {           // COMP <id> <type>
            int nid = 0; char type[128] = {};
            if (std::sscanf(line, "COMP %d %127s", &nid, type) >= 2) {
                const int idx = FindLoadedIndex(nodes, nid);
                if (idx >= 0) {
                    LoadedNode& ln = nodes[static_cast<u32>(idx)];
                    if (std::strcmp(type, "FRigidBody2D") == 0) {
                        ln.rigid_slot = ln.comp_count;   // default-ctor 不可 → attach せず物理 request で扱う
                    } else {
                        TUniquePtr<FComponent2D> c = CreateComponentByName(type);
                        if (c) ln.node->AttachComponent(Move(c));
                    }
                    ++ln.comp_count;                     // 論理 slot は «attach 有無に関わらず» 進める
                }
            }
            continue;
        }
        if (std::strncmp(line, "CPROP ", 6) == 0) {          // CPROP <id> <slot> <prop> <x y z w>
            int nid = 0; unsigned slot = 0, prop = 0;
            float v0 = 0, v1 = 0, v2 = 0, v3 = 0;
            if (std::sscanf(line, "CPROP %d %u %u %f %f %f %f", &nid, &slot, &prop, &v0, &v1, &v2, &v3) >= 4) {
                const int idx = FindLoadedIndex(nodes, nid);
                if (idx >= 0) {
                    LoadedNode& ln = nodes[static_cast<u32>(idx)];
                    if (ln.rigid_slot == static_cast<int>(slot)) {       // FRigidBody2D の prop → 物理 request へ
                        if (prop < 6) ln.rb[prop] = v0;
                    } else {
                        // FRigidBody2D を attach していない分 slot がずれるので補正 (それより後ろは -1)。
                        const u32 actual = (ln.rigid_slot >= 0 && static_cast<int>(slot) > ln.rigid_slot)
                                               ? slot - 1u : slot;
                        FComponent2D* c = ln.node->ComponentAt(actual);
                        if (c != nullptr) {
                            const f32 v[4] = { v0, v1, v2, v3 };
                            if (FPrimitiveRenderer2D* pr = AsPrimitive(c)) {
                                if (prop == 0) pr->SetShape(static_cast<FPrimitiveRenderer2D::EShape>(static_cast<int>(v0)));
                            } else {
                                const FTypeDesc* d = FTypeRegistry::Get().FindByName(c->ReflectName());
                                if (d != nullptr && d->fields != nullptr && prop < d->field_count)
                                    ApplyFieldValue(static_cast<void*>(c), d->fields[prop], v);
                            }
                        }
                    }
                }
            }
            continue;
        }
        if (std::strncmp(line, "POLY ", 5) == 0) {           // POLY <id> <count> <x0 y0 x1 y1 ...>
            int nid = 0, cnt = 0, consumed = 0;
            if (std::sscanf(line, "POLY %d %d %n", &nid, &cnt, &consumed) >= 2 && cnt >= 3) {
                const int idx = FindLoadedIndex(nodes, nid);
                if (idx >= 0) {
                    if (cnt > static_cast<int>(FPolygonRenderer2D::kMaxVerts)) cnt = FPolygonRenderer2D::kMaxVerts;
                    FVec2 verts[FPolygonRenderer2D::kMaxVerts];
                    const char* q = line + consumed;
                    int parsed = 0;
                    for (; parsed < cnt; ++parsed) {
                        int c2 = 0;
                        if (std::sscanf(q, "%f %f %n", &verts[parsed].x, &verts[parsed].y, &c2) < 2) break;
                        q += c2;
                    }
                    if (parsed >= 3) {
                        LoadedNode& ln = nodes[static_cast<u32>(idx)];
                        FPolygonRenderer2D& poly = ln.node->AddComponent<FPolygonRenderer2D>();
                        poly.SetVerts(verts, static_cast<u32>(parsed));
                        poly.SetColor(ln.color);
                        const u32 pc = (static_cast<u32>(parsed) < kMaxPolyVerts) ? static_cast<u32>(parsed) : kMaxPolyVerts;
                        for (u32 k = 0; k < pc; ++k) ln.poly[k] = verts[k];   // 物理 polygon 用に保持
                        ln.polyCount = pc;
                    }
                }
            }
            continue;
        }
        if (out_sprites != nullptr && std::strncmp(line, "SPRT ", 5) == 0) {   // SPRT <id> <path...>
            int nid = 0, consumed = 0;
            if (std::sscanf(line, "SPRT %d %n", &nid, &consumed) >= 1) {
                const int idx = FindLoadedIndex(nodes, nid);
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                if (idx >= 0 && *path != '\0') {
                    FSpriteRequest req;
                    req.node = nodes[static_cast<u32>(idx)].node;
                    req.size = FVec2{ nodes[static_cast<u32>(idx)].base, nodes[static_cast<u32>(idx)].base };
                    int k = 0; for (; path[k] != '\0' && k < 259; ++k) req.path[k] = path[k];
                    req.path[k] = '\0';
                    out_sprites->PushBack(req);
                }
            }
            continue;
        }
        if (std::strncmp(line, "MAT ", 4) == 0) {            // MAT <id> <path...> (使用マテリアル)
            int nid = 0, consumed = 0;
            if (std::sscanf(line, "MAT %d %n", &nid, &consumed) >= 1) {
                const int idx = FindLoadedIndex(nodes, nid);
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                if (idx >= 0 && *path != '\0' && nodes[static_cast<u32>(idx)].node != nullptr) {
                    FMaterial2D mat;
                    if (LoadAcsmatFile(path, mat)) {
                        FNode2D* mn = nodes[static_cast<u32>(idx)].node;
                        mn->SetMaterial(mat);                          // マテリアルを node へ
                        // PBR の法線マップは後段で GPU 化するため要求に積む。
                        if (out_mat_tex != nullptr && mat.kind == EMaterialKind::Lit
                            && mat.pbr.normalPath[0] != '\0') {
                            FMaterialTexRequest req;
                            req.node = mn;
                            int k = 0; for (; mat.pbr.normalPath[k] != '\0' && k < 259; ++k)
                                req.normalPath[k] = mat.pbr.normalPath[k];
                            req.normalPath[k] = '\0';
                            out_mat_tex->PushBack(req);
                        }
                    }
                }
            }
            continue;
        }
        // NFLG / RPLY / SEL は現状スキップ (best-effort)。
    }

    // --- 親付け fixup (順序非依存。Reparent は Local 保持) ---
    bool reparented = false;
    for (u32 i = 0; i < nodes.Size(); ++i) {
        if (nodes[i].parent < 0) continue;
        const int pj = FindLoadedIndex(nodes, nodes[i].parent);
        if (pj >= 0 && nodes[i].node->Parent() != nodes[static_cast<u32>(pj)].node) {
            nodes[i].node->Reparent(*nodes[static_cast<u32>(pj)].node);
            reparented = true;
        }
    }
    if (reparented) root.ResolveStructuralChanges();

    // --- FPrimitiveRenderer2D の見た目をノードの color/base で設定 (editor の描画と一致) ---
    // shape が Box/Circle/Triangle (0-2) なら base サイズで描画。Polygon (>=3) は FPolygonRenderer2D
    // が実形状を描くので、ここの箱は size 0 で隠す (二重描画/巨大な箱を防ぐ)。
    for (u32 i = 0; i < nodes.Size(); ++i) {
        FNode2D* n = nodes[i].node;
        for (u32 c = 0; c < n->ComponentCount(); ++c) {
            if (FPrimitiveRenderer2D* pr = AsPrimitive(n->ComponentAt(c))) {
                pr->SetColor(nodes[i].color);
                const bool is_poly = static_cast<int>(pr->Shape()) > 2;
                pr->SetSize(is_poly ? FVec2{ 0.0f, 0.0f } : FVec2{ nodes[i].base, nodes[i].base });
            }
        }
    }

    // --- 剛体 (FRigidBody2D) request を組み立てる (shape は FPrimitiveRenderer2D から) ---
    if (out_bodies != nullptr) {
        for (u32 i = 0; i < nodes.Size(); ++i) {
            if (nodes[i].rigid_slot < 0) continue;
            FRigidBodyRequest req;
            req.node        = nodes[i].node;
            req.bodyType    = static_cast<int>(nodes[i].rb[0]);
            req.restitution = nodes[i].rb[1];
            req.friction    = nodes[i].rb[2];
            req.mass        = nodes[i].rb[3];
            req.linDamp     = nodes[i].rb[4];
            req.angDamp     = nodes[i].rb[5];
            req.base        = nodes[i].base;
            req.shape       = 0;   // 既定 box
            for (u32 c = 0; c < nodes[i].node->ComponentCount(); ++c)
                if (FPrimitiveRenderer2D* pr = AsPrimitive(nodes[i].node->ComponentAt(c)))
                    req.shape = static_cast<int>(pr->Shape());
            req.polyCount = nodes[i].polyCount;
            for (u32 k = 0; k < nodes[i].polyCount; ++k) req.poly[k] = nodes[i].poly[k];
            out_bodies->PushBack(req);
        }
    }

    // --- world 境界 (カメラフレーミング用) ---
    for (u32 i = 0; i < nodes.Size(); ++i) {
        const FVec2 wp = nodes[i].node->World().position;
        const f32 half = nodes[i].base * 0.5f;
        const FVec2 lo{ wp.x - half, wp.y - half }, hi{ wp.x + half, wp.y + half };
        if (!bounds.valid) { bounds.min = lo; bounds.max = hi; bounds.valid = true; }
        else {
            if (lo.x < bounds.min.x) bounds.min.x = lo.x;
            if (lo.y < bounds.min.y) bounds.min.y = lo.y;
            if (hi.x > bounds.max.x) bounds.max.x = hi.x;
            if (hi.y > bounds.max.y) bounds.max.y = hi.y;
        }
    }
    return bounds;
}

FSceneBounds LoadAcsceneFile(const char* path, FNode2D& root,
                             TArray<FSpriteRequest>* out_sprites,
                             TArray<FRigidBodyRequest>* out_bodies,
                             TArray<FMaterialTexRequest>* out_mat_tex) noexcept {
    FSceneBounds bounds;
    if (path == nullptr) return bounds;
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return bounds;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return bounds; }
    TArray<char> buf;
    buf.Resize(static_cast<u32>(sz) + 1);
    const usize rd = std::fread(buf.Data(), 1, static_cast<usize>(sz), f);
    std::fclose(f);
    buf[static_cast<u32>(rd)] = '\0';
    return LoadAcsceneText(buf.Data(), root, out_sprites, out_bodies, out_mat_tex);
}

void BuildSceneRigidBodies(FRigidWorld2D& world, const TArray<FRigidBodyRequest>& reqs,
                           TArray<FNode2D*>& out_nodes, TArray<u32>& out_bodies) noexcept {
    for (u32 i = 0; i < reqs.Size(); ++i) {
        const FRigidBodyRequest& req = reqs[i];
        if (req.node == nullptr) continue;
        const FTransform2D w = req.node->World();
        const f32 sx = (w.scale.x != 0.0f) ? w.scale.x : 1.0f;
        const f32 sy = (w.scale.y != 0.0f) ? w.scale.y : 1.0f;
        const FVec2 pos{ w.position.x, w.position.y };
        const bool dynamic = (req.bodyType == 1);
        const f32 mass = (req.mass > 0.001f) ? req.mass : 1.0f;

        u32 bi;
        if (req.shape == 3 && req.polyCount >= 3u) {                    // ポリゴン
            FVec2 lv[kMaxPolyVerts];
            for (u32 k = 0; k < req.polyCount; ++k) lv[k] = FVec2{ req.poly[k].x * sx, req.poly[k].y * sy };
            bi = world.AddPolygon(pos, lv, req.polyCount, dynamic ? mass : 0.0f, req.restitution, req.friction);
        } else if (req.shape == 1) {                                   // 円
            const f32 radius = req.base * 0.5f * ((sx > sy) ? sx : sy);
            bi = world.AddCircle(pos, radius, dynamic ? mass : 0.0f, req.restitution, req.friction);
        } else {                                                       // 箱 (OBB)
            const FVec2 half{ req.base * 0.5f * sx, req.base * 0.5f * sy };
            bi = dynamic ? world.AddDynamicAabb(pos, half, mass, req.restitution, req.friction)
                         : world.AddStaticAabb(pos, half, req.restitution, req.friction);
        }
        world.SetAngle(bi, w.rotation);
        if (dynamic) {
            world.SetDamping(bi, req.linDamp, req.angDamp);
            out_nodes.PushBack(req.node);
            out_bodies.PushBack(bi);
        }
    }
}

void StepSceneRigidBodies(FRigidWorld2D& world, const TArray<FNode2D*>& nodes,
                          const TArray<u32>& bodies, f32 dt, FVec2 gravity) noexcept {
    if (dt > 0.05f) dt = 0.05f;
    world.Step(dt, gravity);
    const u32 n = (nodes.Size() < bodies.Size()) ? nodes.Size() : bodies.Size();
    for (u32 i = 0; i < n; ++i) {
        if (nodes[i] == nullptr) continue;
        nodes[i]->Local().position = world.Position(bodies[i]);
        nodes[i]->Local().rotation = world.Angle(bodies[i]);
    }
}

void LoadSceneSprites(IRhiDevice& device, const TArray<FSpriteRequest>& reqs,
                      TArray<TUniquePtr<IRhiTexture>>& out_textures) noexcept {
    for (u32 i = 0; i < reqs.Size(); ++i) {
        const FSpriteRequest& req = reqs[i];
        if (req.node == nullptr || req.path[0] == '\0') continue;

        std::FILE* f = std::fopen(req.path, "rb");   // 非 ASCII パスは未対応
        if (f == nullptr) continue;
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz <= 0) { std::fclose(f); continue; }
        TArray<byte> bytes;
        bytes.Resize(static_cast<u32>(sz));
        const usize rd = std::fread(bytes.Data(), 1, static_cast<usize>(sz), f);
        std::fclose(f);
        if (rd != static_cast<usize>(sz)) continue;

        ImageAssetLoader loader;
        auto decoded = loader.LoadFromBytes(kInvalidAssetId, bytes);
        if (decoded.IsErr()) continue;
        auto asset = decoded.Value();                 // TSharedPtr を保持
        const FImageAsset* img = static_cast<const FImageAsset*>(asset.Get());
        if (img == nullptr) continue;
        auto tex = UploadTexture(device, *img);
        if (tex.IsErr()) continue;

        FSprite2DComponent& sp = req.node->AddComponent<FSprite2DComponent>(req.size);
        sp.SetTexture(tex.Value().Get());              // raw ptr を bind (所有は out_textures)
        out_textures.PushBack(Move(tex.Value()));
    }
}

void LoadSceneMaterialTextures(IRhiDevice& device, const TArray<FMaterialTexRequest>& reqs,
                               TArray<TUniquePtr<IRhiTexture>>& out_textures) noexcept {
    for (u32 i = 0; i < reqs.Size(); ++i) {
        const FMaterialTexRequest& req = reqs[i];
        if (req.node == nullptr || req.normalPath[0] == '\0') continue;

        std::FILE* f = std::fopen(req.normalPath, "rb");   // 非 ASCII パスは未対応
        if (f == nullptr) continue;
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz <= 0) { std::fclose(f); continue; }
        TArray<byte> bytes;
        bytes.Resize(static_cast<u32>(sz));
        const usize rd = std::fread(bytes.Data(), 1, static_cast<usize>(sz), f);
        std::fclose(f);
        if (rd != static_cast<usize>(sz)) continue;

        ImageAssetLoader loader;
        auto decoded = loader.LoadFromBytes(kInvalidAssetId, bytes);
        if (decoded.IsErr()) continue;
        auto asset = decoded.Value();
        const FImageAsset* img = static_cast<const FImageAsset*>(asset.Get());
        if (img == nullptr) continue;
        auto tex = UploadTexture(device, *img);
        if (tex.IsErr()) continue;

        req.node->SetMaterialNormalTex(tex.Value().Get());   // 非所有 ptr を bind (所有は out_textures)
        out_textures.PushBack(Move(tex.Value()));
    }
}

} // namespace acs::game
