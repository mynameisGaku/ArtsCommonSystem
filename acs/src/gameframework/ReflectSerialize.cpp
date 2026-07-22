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
    bool Skip(u32 n) noexcept {
        if (!ok || n > size - cur) { ok = false; return false; }
        cur += n;
        return true;
    }
    u8  U8 () noexcept { u8  v = 0u; Bytes(&v, 1u); return v; }
    u32 U32() noexcept { u32 v = 0u; Bytes(&v, 4u); return v; }
    u32 Remaining() const noexcept { return ok ? size - cur : 0u; }
};

/** 直列化対象のフィールドか (実体メモリを持つ値型のみ。名前のみ反射 / 文字列ポインタは除外)。 */
bool IsSerializable(const FReflectField& f) noexcept {
    return f.size > 0u && f.size <= kReflectSerializeMaxFieldValueBytes
        && f.kind != EFieldKind::String && (f.flags & FIELD_TRANSIENT) == 0u;
}

bool TryFieldNameLength(const char* name, u32& out_length) noexcept {
    if (name == nullptr) return false;
    for (u32 length = 0u; length <= kReflectSerializeMaxFieldNameBytes; ++length) {
        if (name[length] == '\0') {
            if (length == 0u) return false;
            out_length = length;
            return true;
        }
    }
    return false;
}

bool ValidateDescriptor(const FTypeDesc* d) noexcept {
    if (d == nullptr || d->field_count > kReflectSerializeMaxFieldCount) return false;
    if (d->field_count > 0u && d->fields == nullptr) return false;

    for (u32 i = 0u; i < d->field_count; ++i) {
        const FReflectField& f = d->fields[i];
        if (!IsSerializable(f)) continue;
        u32 name_length = 0u;
        if (!TryFieldNameLength(f.name, name_length)) return false;
        if (f.offset > d->size || f.size > d->size - f.offset) return false;
    }
    return true;
}

FReflectDeserializeResult DeserializeFailure(EReflectSerializeError error, u32 bytes_read,
                                             FTypeId type_id = 0u) noexcept {
    return FReflectDeserializeResult{error, 0u, bytes_read, type_id};
}

FReflectSerializeResult SerializeFailure(EReflectSerializeError error,
                                         u32 required_bytes = 0u,
                                         u32 field_count = 0u) noexcept {
    return FReflectSerializeResult{error, 0u, required_bytes, field_count};
}

bool CheckedAdd(u32& value, u32 increment) noexcept {
    constexpr u32 kU32Max = ~u32{0};
    if (increment > kU32Max - value) return false;
    value += increment;
    return true;
}

} // namespace

const char* ReflectSerializeErrorName(EReflectSerializeError error) noexcept {
    switch (error) {
    case EReflectSerializeError::None:               return "none";
    case EReflectSerializeError::NullDescriptor:     return "null_descriptor";
    case EReflectSerializeError::NullObject:         return "null_object";
    case EReflectSerializeError::NullData:           return "null_data";
    case EReflectSerializeError::TruncatedData:      return "truncated_data";
    case EReflectSerializeError::InvalidMagic:       return "invalid_magic";
    case EReflectSerializeError::TypeMismatch:       return "type_mismatch";
    case EReflectSerializeError::FieldLimitExceeded: return "field_limit_exceeded";
    case EReflectSerializeError::InvalidFieldRecord: return "invalid_field_record";
    case EReflectSerializeError::InvalidMetadata:    return "invalid_metadata";
    case EReflectSerializeError::NullOutput:         return "null_output";
    case EReflectSerializeError::BufferTooSmall:     return "buffer_too_small";
    case EReflectSerializeError::SerializedSizeOverflow: return "serialized_size_overflow";
    }
    return "unknown";
}

FReflectSerializeResult TrySerializeReflected(
    const FTypeDesc* d, const void* obj, u8* buf, u32 cap) noexcept {
    if (d == nullptr) return SerializeFailure(EReflectSerializeError::NullDescriptor);
    if (obj == nullptr) return SerializeFailure(EReflectSerializeError::NullObject);
    if (!ValidateDescriptor(d)) return SerializeFailure(EReflectSerializeError::InvalidMetadata);

    u32 required_bytes = 12u;
    u32 fcount = 0u;
    for (u32 i = 0u; i < d->field_count; ++i) {
        const FReflectField& f = d->fields[i];
        if (!IsSerializable(f)) continue;
        u32 name_length = 0u;
        (void)TryFieldNameLength(f.name, name_length); // ValidateDescriptor 済み。
        if (!CheckedAdd(required_bytes, 3u + name_length) ||
            !CheckedAdd(required_bytes, f.size)) {
            return SerializeFailure(EReflectSerializeError::SerializedSizeOverflow,
                                    0u, fcount);
        }
        ++fcount;
    }

    if (buf == nullptr) {
        return SerializeFailure(cap == 0u ? EReflectSerializeError::BufferTooSmall
                                         : EReflectSerializeError::NullOutput,
                                required_bytes, fcount);
    }
    if (cap < required_bytes) {
        return SerializeFailure(EReflectSerializeError::BufferTooSmall,
                                required_bytes, fcount);
    }

    FWriter w(buf, cap);
    w.U32(kReflectSerializeMagic);
    w.U32(d->id);
    w.U32(fcount);

    const u8* base = static_cast<const u8*>(obj);
    for (u32 i = 0u; i < d->field_count; ++i) {
        const FReflectField& f = d->fields[i];
        if (!IsSerializable(f)) continue;
        u32 nlen = 0u;
        (void)TryFieldNameLength(f.name, nlen); // ValidateDescriptor 済み。
        const u8  nl   = static_cast<u8>(nlen);
        w.U8(nl);
        w.Bytes(f.name, nl);
        w.U8(static_cast<u8>(f.kind));
        w.U8(static_cast<u8>(f.size));
        w.Bytes(base + f.offset, f.size);
    }
    if (!w.ok || w.cur != required_bytes) {
        return SerializeFailure(EReflectSerializeError::SerializedSizeOverflow,
                                required_bytes, fcount);
    }
    return FReflectSerializeResult{
        EReflectSerializeError::None, w.cur, required_bytes, fcount
    };
}

u32 SerializeReflected(const FTypeDesc* d, const void* obj, u8* buf, u32 cap) noexcept {
    const FReflectSerializeResult result = TrySerializeReflected(d, obj, buf, cap);
    return result.Succeeded() ? result.BytesWritten : 0u;
}

u32 SerializeByName(const char* type_name, const void* obj, u8* buf, u32 cap) noexcept {
    if (type_name == nullptr) return 0u;
    AcsRegisterEngineTypes();
    const FTypeDesc* d = FTypeRegistry::Get().FindByName(type_name);
    return SerializeReflected(d, obj, buf, cap);
}

FReflectDeserializeResult TryDeserializeReflected(
    const FTypeDesc* d, void* obj, const u8* data, u32 size) noexcept {
    if (d == nullptr) return DeserializeFailure(EReflectSerializeError::NullDescriptor, 0u);
    if (obj == nullptr) return DeserializeFailure(EReflectSerializeError::NullObject, 0u);
    if (data == nullptr) return DeserializeFailure(EReflectSerializeError::NullData, 0u);
    if (!ValidateDescriptor(d))
        return DeserializeFailure(EReflectSerializeError::InvalidMetadata, 0u);
    if (size < 12u) return DeserializeFailure(EReflectSerializeError::TruncatedData, 0u);

    // 第1パス: 全レコードを検証する。この段階では obj に一切書き込まない。
    FReader r(data, size);
    if (r.U32() != kReflectSerializeMagic)
        return DeserializeFailure(EReflectSerializeError::InvalidMagic, r.cur);
    const FTypeId serialized_type_id = r.U32();
    if (serialized_type_id != d->id)
        return DeserializeFailure(EReflectSerializeError::TypeMismatch, r.cur, serialized_type_id);
    const u32 fcount = r.U32();
    if (!r.ok)
        return DeserializeFailure(EReflectSerializeError::TruncatedData, r.cur, serialized_type_id);
    if (fcount > kReflectSerializeMaxFieldCount)
        return DeserializeFailure(EReflectSerializeError::FieldLimitExceeded, r.cur, serialized_type_id);
    // 最小レコードは name_len/name/kind/size/value の 5 bytes。
    if (fcount > r.Remaining() / 5u)
        return DeserializeFailure(EReflectSerializeError::TruncatedData, r.cur, serialized_type_id);

    for (u32 i = 0u; i < fcount; ++i) {
        const u8 name_length = r.U8();
        if (!r.ok)
            return DeserializeFailure(EReflectSerializeError::TruncatedData, r.cur, serialized_type_id);
        if (name_length == 0u)
            return DeserializeFailure(EReflectSerializeError::InvalidFieldRecord, r.cur, serialized_type_id);
        if (!r.Skip(name_length))
            return DeserializeFailure(EReflectSerializeError::TruncatedData, r.cur, serialized_type_id);
        (void)r.U8(); // kind は未知値でも forward-compatible な未適用フィールドとして許す。
        const u8 value_size = r.U8();
        if (!r.ok)
            return DeserializeFailure(EReflectSerializeError::TruncatedData, r.cur, serialized_type_id);
        if (value_size == 0u || value_size > kReflectSerializeMaxFieldValueBytes)
            return DeserializeFailure(EReflectSerializeError::InvalidFieldRecord, r.cur, serialized_type_id);
        if (!r.Skip(value_size))
            return DeserializeFailure(EReflectSerializeError::TruncatedData, r.cur, serialized_type_id);
    }
    const u32 bytes_read = r.cur;

    // 第2パス: 第1パス完走後だけ既知フィールドへ適用する。
    FReader apply_reader(data, bytes_read);
    (void)apply_reader.U32();
    (void)apply_reader.U32();
    (void)apply_reader.U32();
    u8* base = static_cast<u8*>(obj);
    char name[256];
    u32 applied = 0u;

    for (u32 i = 0u; i < fcount; ++i) {
        const u8 nl = apply_reader.U8();
        (void)apply_reader.Bytes(name, nl);
        name[nl] = '\0';
        const u8 kind = apply_reader.U8();
        const u8 sz   = apply_reader.U8();
        const u8* valptr = data + apply_reader.cur;
        (void)apply_reader.Skip(sz);

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
    return FReflectDeserializeResult{
        EReflectSerializeError::None, applied, bytes_read, serialized_type_id
    };
}

u32 DeserializeReflected(const FTypeDesc* d, void* obj, const u8* data, u32 size) noexcept {
    const FReflectDeserializeResult result = TryDeserializeReflected(d, obj, data, size);
    return result.Succeeded() ? result.FieldsApplied : 0u;
}

void* CreateFromBytes(const u8* data, u32 size, FTypeId* out_type_id) noexcept {
    if (out_type_id != nullptr) *out_type_id = 0u;
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

    const FReflectDeserializeResult result = TryDeserializeReflected(d, obj, data, size);
    if (!result.Succeeded()) {
        reg.Destroy(tid, obj);
        return nullptr;
    }
    if (out_type_id != nullptr) *out_type_id = tid;
    return obj;
}

} // namespace acs::game
