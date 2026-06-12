// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — ReflectSerialize 実装。
// FReflectField を走査して固定バッファへ往復する。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/ReflectSerialize.h"
#include "gameframework/ReflectCatalog.h"   // AcsRegisterEngineTypes (型登録の確定)

#include <cstring>   // std::memcpy / std::strlen / std::strcmp

namespace acs::game {

namespace {

/** 固定バッファへの追記ライタ (cap 超過で ok=false、以降は no-op)。 */
struct FWriter {
    u8* buf; u32 cap; u32 cur; bool ok;
    FWriter(u8* b, u32 c) noexcept : buf(b), cap(c), cur(0u), ok(true) {}
    void Bytes(const void* p, u32 n) noexcept {
        // cur<=cap は常に成立 (成功時のみ前進) → cap-cur は非負。減算形で u32 オーバーフローを避ける。
        if (!ok || n > cap - cur) { ok = false; return; }
        std::memcpy(buf + cur, p, n);
        cur += n;
    }
    void U8 (u8  v) noexcept { Bytes(&v, 1u); }
    void U32(u32 v) noexcept { Bytes(&v, 4u); }
};

/** 固定バッファからの読み出しリーダ (範囲外で ok=false)。 */
struct FReader {
    const u8* data; u32 size; u32 cur; bool ok;
    FReader(const u8* d, u32 s) noexcept : data(d), size(s), cur(0u), ok(true) {}
    bool Bytes(void* out, u32 n) noexcept {
        // cur<=size は常に成立 → size-cur は非負。減算形で u32 オーバーフローを避ける (save data は不信)。
        if (!ok || n > size - cur) { ok = false; return false; }
        std::memcpy(out, data + cur, n);
        cur += n;
        return true;
    }
    u8  U8 () noexcept { u8  v = 0u; Bytes(&v, 1u); return v; }
    u32 U32() noexcept { u32 v = 0u; Bytes(&v, 4u); return v; }
};

/** 直列化対象のフィールドか (実体メモリを持つ値型のみ。名前のみ反射 / 文字列ポインタは除外)。 */
bool IsSerializable(const FReflectField& f) noexcept {
    return f.size > 0u && f.size <= 16u && f.kind != EFieldKind::FString;
}

} // namespace

u32 SerializeReflected(const FTypeDesc* d, const void* obj, u8* buf, u32 cap) noexcept {
    if (d == nullptr || obj == nullptr || buf == nullptr) return 0u;

    u32 fcount = 0u;
    for (u32 i = 0u; i < d->field_count; ++i)
        if (IsSerializable(d->fields[i])) ++fcount;

    FWriter w(buf, cap);
    w.U32(kReflectSerializeMagic);
    w.U32(d->id);
    w.U32(fcount);

    const u8* base = static_cast<const u8*>(obj);
    for (u32 i = 0u; i < d->field_count; ++i) {
        const FReflectField& f = d->fields[i];
        if (!IsSerializable(f)) continue;
        const u32 nlen = (f.name != nullptr) ? static_cast<u32>(std::strlen(f.name)) : 0u;
        const u8  nl   = (nlen > 255u) ? 255u : static_cast<u8>(nlen);
        w.U8(nl);
        w.Bytes(f.name, nl);
        w.U8(static_cast<u8>(f.kind));
        w.U8(static_cast<u8>(f.size));
        w.Bytes(base + f.offset, f.size);
    }
    return w.ok ? w.cur : 0u;
}

u32 SerializeByName(const char* type_name, const void* obj, u8* buf, u32 cap) noexcept {
    if (type_name == nullptr) return 0u;
    AcsRegisterEngineTypes();
    const FTypeDesc* d = FTypeRegistry::Get().FindByName(type_name);
    return SerializeReflected(d, obj, buf, cap);
}

u32 DeserializeReflected(const FTypeDesc* d, void* obj, const u8* data, u32 size) noexcept {
    if (d == nullptr || obj == nullptr || data == nullptr) return 0u;

    FReader r(data, size);
    if (r.U32() != kReflectSerializeMagic) return 0u;
    if (r.U32() != d->id)                  return 0u;   // 型不一致
    const u32 fcount = r.U32();
    if (!r.ok) return 0u;

    u8*  base = static_cast<u8*>(obj);
    char name[256];
    u32  applied = 0u;

    for (u32 i = 0u; i < fcount && r.ok; ++i) {
        const u8 nl = r.U8();
        if (!r.Bytes(name, nl)) break;
        name[nl] = '\0';
        const u8 kind = r.U8();
        const u8 sz   = r.U8();
        if (!r.ok || sz > r.size - r.cur) { r.ok = false; break; }   // 減算形 (オーバーフロー安全)
        const u8* valptr = data + r.cur;
        r.cur += sz;

        // 名前一致 (かつ kind/size 一致) のフィールドへ書き戻す。
        for (u32 fi = 0u; fi < d->field_count; ++fi) {
            const FReflectField& f = d->fields[fi];
            if (f.name != nullptr && IsSerializable(f)
                && static_cast<u8>(f.kind) == kind && static_cast<u8>(f.size) == sz
                && std::strcmp(f.name, name) == 0) {
                std::memcpy(base + f.offset, valptr, sz);
                ++applied;
                break;
            }
        }
    }
    return applied;
}

void* CreateFromBytes(const u8* data, u32 size, FTypeId* out_type_id) noexcept {
    if (data == nullptr || size < 12u) return nullptr;   // 最低でも magic+type_id+count
    u32 magic = 0u, tid = 0u;
    std::memcpy(&magic, data,     4u);
    if (magic != kReflectSerializeMagic) return nullptr;
    std::memcpy(&tid,   data + 4u, 4u);

    AcsRegisterEngineTypes();
    FTypeRegistry& reg = FTypeRegistry::Get();
    const FTypeDesc* d = reg.FindById(tid);
    if (d == nullptr) return nullptr;
    void* obj = reg.CreateById(tid);
    if (obj == nullptr) return nullptr;             // 抽象型 / factory なしは生成不可

    DeserializeReflected(d, obj, data, size);
    if (out_type_id != nullptr) *out_type_id = tid;
    return obj;
}

} // namespace acs::game
