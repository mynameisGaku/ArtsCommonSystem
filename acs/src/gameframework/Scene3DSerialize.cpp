// SPDX-License-Identifier: Apache-2.0
// GameFramework — 3D シーンのテキストシリアライズ 実装
#include "gameframework/Scene3DSerialize.h"
#include "gameframework/Scene3D.h"
#include "gameframework/Node3D.h"
#include "gameframework/MeshComponent3D.h"
#include "container/Array.h"
#include "container/StringView.h"

#include <cstdio>
#include <cstring>

namespace acs::game {

namespace {

/** const ノードから FMeshComponent3D を探す (ComponentAt 経由、非テンプレートで const 可)。 */
const FMeshComponent3D* FindMesh(const FNode3D& n) noexcept {
    const void* k = Component3DKindOf<FMeshComponent3D>();
    for (u32 i = 0; i < n.ComponentCount(); ++i) {
        const FComponent3D* c = n.ComponentAt(i);
        if (c != nullptr && c->Kind() == k) return static_cast<const FMeshComponent3D*>(c);
    }
    return nullptr;
}

/** FStringView を null 終端のローカル char 配列へコピーする (空なら fallback)。 */
void CopyName(FStringView v, char* dst, u32 dst_cap, const char* fallback) noexcept {
    u32 ln = 0;
    for (; ln < v.Size() && ln + 1u < dst_cap; ++ln) dst[ln] = v[ln];
    dst[ln] = '\0';
    if (dst[0] == '\0') {
        u32 i = 0;
        for (; fallback[i] != '\0' && i + 1u < dst_cap; ++i) dst[i] = fallback[i];
        dst[i] = '\0';
    }
}

/** 1 ノードを書いて serial id を割り当て、子を pre-order で再帰する。cur / next を進める。 */
void WriteNode(const FNode3D& n, int parentId, int& next, char* out, u32& cur, u32 cap) noexcept {
    const int myId = next++;
    const FTransform3D& t = n.Local();
    const FVec3 e = t.EulerDeg();
    const FMeshComponent3D* mesh = FindMesh(n);
    const int prim = (mesh != nullptr) ? static_cast<int>(mesh->Primitive()) : -1;
    const FVec4 col = (mesh != nullptr) ? mesh->Color() : FVec4{ 1, 1, 1, 1 };

    char nm[128];
    CopyName(n.Name(), nm, sizeof(nm), "Node");

    if (cur < cap) {
        const int w = std::snprintf(out + cur, static_cast<size_t>(cap - cur),
            "N3D %d %d %d %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %s\n",
            myId, parentId, prim,
            static_cast<double>(t.position.x), static_cast<double>(t.position.y), static_cast<double>(t.position.z),
            static_cast<double>(e.x), static_cast<double>(e.y), static_cast<double>(e.z),
            static_cast<double>(t.scale.x), static_cast<double>(t.scale.y), static_cast<double>(t.scale.z),
            static_cast<double>(col.x), static_cast<double>(col.y), static_cast<double>(col.z), static_cast<double>(col.w),
            nm);
        if (w > 0 && static_cast<u32>(w) < cap - cur) cur += static_cast<u32>(w);
        else { out[cap - 1] = '\0'; return; }   // overflow: 打ち切り
    }

    if (mesh != nullptr && mesh->Primitive() == EMeshPrimitive3D::Mesh
        && mesh->MeshPath().Size() > 0 && cur < cap) {
        char mp[300];
        CopyName(mesh->MeshPath(), mp, sizeof(mp), "");
        const int w2 = std::snprintf(out + cur, static_cast<size_t>(cap - cur), "MSH3D %d %s\n", myId, mp);
        if (w2 > 0 && static_cast<u32>(w2) < cap - cur) cur += static_cast<u32>(w2);
        else { out[cap - 1] = '\0'; return; }
    }

    for (u32 i = 0; i < n.ChildCount(); ++i) {
        if (const FNode3D* c = n.Child(i)) WriteNode(*c, myId, next, out, cur, cap);
    }
}

} // namespace

u32 SaveScene3DText(const FScene3D& scene, char* out, u32 cap) noexcept {
    if (out == nullptr || cap == 0) return 0;
    out[0] = '\0';
    u32 cur = 0;
    int next = 0;
    WriteNode(scene.Root(), -1, next, out, cur, cap);
    const u32 end = (cur < cap) ? cur : (cap - 1);
    out[end] = '\0';
    return end;
}

bool LoadScene3DText(FScene3D& scene, const char* text) noexcept {
    if (text == nullptr) return false;
    scene.Clear();

    TArray<FNode3D*> id_map;   // serial id → node
    const char* p = text;
    char line[600];

    while (*p != '\0') {
        u32 n = 0;
        while (*p != '\0' && *p != '\n') { if (n + 1u < sizeof(line)) line[n++] = *p; ++p; }
        line[n] = '\0';
        if (*p == '\n') ++p;

        if (std::strncmp(line, "N3D ", 4) == 0) {
            int id = -1, parent = -1, prim = -1;
            FVec3 pos{ 0, 0, 0 }, rot{ 0, 0, 0 }, scl{ 1, 1, 1 };
            FVec4 col{ 1, 1, 1, 1 };
            char nm[128] = {};
            const int got = std::sscanf(line,
                "N3D %d %d %d %f %f %f %f %f %f %f %f %f %f %f %f %f %127[^\n]",
                &id, &parent, &prim, &pos.x, &pos.y, &pos.z, &rot.x, &rot.y, &rot.z,
                &scl.x, &scl.y, &scl.z, &col.x, &col.y, &col.z, &col.w, nm);
            if (got < 16 || id < 0) continue;
            const bool has_name = (got >= 17);

            FNode3D* node = nullptr;
            if (parent < 0) {
                node = &scene.Root();
                node->SetName(FStringView(has_name ? nm : "Root"));
            } else {
                FNode3D* par = (static_cast<u32>(parent) < id_map.Size()) ? id_map[static_cast<u32>(parent)] : nullptr;
                if (par == nullptr) par = &scene.Root();   // 壊れた parent 参照は root 下へ
                node = &scene.Spawn(FStringView(has_name ? nm : "Node"), par);
            }
            node->Local().position = pos;
            node->Local().SetEulerDeg(rot);
            node->Local().scale = scl;
            if (prim >= 0) {
                FMeshComponent3D& mc = node->GetOrAddComponent<FMeshComponent3D>();
                mc.SetPrimitive(static_cast<EMeshPrimitive3D>(prim));
                mc.SetColor(col);
            }
            while (id_map.Size() <= static_cast<u32>(id)) id_map.PushBack(nullptr);
            id_map[static_cast<u32>(id)] = node;

        } else if (std::strncmp(line, "MSH3D ", 6) == 0) {
            int id = -1; char mp[300] = {};
            if (std::sscanf(line, "MSH3D %d %299[^\n]", &id, mp) >= 2 && id >= 0
                && static_cast<u32>(id) < id_map.Size() && id_map[static_cast<u32>(id)] != nullptr) {
                FMeshComponent3D& mc = id_map[static_cast<u32>(id)]->GetOrAddComponent<FMeshComponent3D>();
                mc.SetMeshPath(FStringView(mp));
            }
        }
    }
    return true;
}

} // namespace acs::game
