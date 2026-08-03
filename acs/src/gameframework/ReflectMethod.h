// SPDX-License-Identifier: Apache-2.0
// GameFramework — 関数リフレクション (ACS_FUNCTION / BlueprintCallable の土台)
//
// 「型に属する引数なし void メソッド」を名前 + 呼び出しサンク(関数ポインタ)として
// 登録する軽量レジストリ。UE の UFUNCTION(BlueprintCallable / CallInEditor) に相当する
// «名前で呼べる関数» を、reflection 経由でエディタのボタンや将来のスクリプト/Blueprint
// グラフから起動できるようにする土台。
//
//   // クラス側 (引数なし void メソッド):
//   void Ping() noexcept { ACS_LOG_INFO("ping"); }
//   // 登録 (.cpp で 1 回):
//   ACS_REGISTER_METHOD(FMyComp, Ping, ::acs::game::METHOD_BP_CALLABLE | ::acs::game::METHOD_CALL_IN_EDITOR)
//   // 呼び出し:
//   InvokeMethodByName(AcsTypeHash("FMyComp"), instance, "Ping");
//
// 設計: フィールド(FReflectField)とは別レジストリにして FTypeDesc / ACS_REGISTER を
// 触らない(追加のみ)。引数あり / 戻り値ありは将来拡張(Blueprint グラフ実装時)。
#pragma once

#include "foundation/Types.h"
#include "memory/SystemAllocator.h"
#include "container/Array.h"
#include "gameframework/Reflect.h"   // FTypeId / AcsTypeHash / ACS_RCAT

#include <cstdlib>   // atof / atoi (文字列引数のパース)
#include <cstdio>    // snprintf (戻り値の文字列化)

namespace acs::game {

/** メソッド指定子フラグ(UFUNCTION 相当)。 */
enum EMethodFlags : u32 {
    METHOD_NONE           = 0u,
    METHOD_BP_CALLABLE    = 1u << 0,   /**< Blueprint/スクリプトから呼べる。 */
    METHOD_CALL_IN_EDITOR = 1u << 1,   /**< エディタのボタンから呼べる。 */
};

/** メソッド引数の種別(単一引数のみ対応。文字列で受けて内部でパースする)。 */
enum EMethodArgKind : u32 {
    METHOD_ARG_NONE = 0u,   /**< 引数なし void。 */
    METHOD_ARG_F32  = 1u,   /**< f32 1 個。 */
    METHOD_ARG_I32  = 2u,   /**< i32 1 個。 */
    METHOD_ARG_STR  = 3u,   /**< const char* 1 個。 */
};

/** 反射された 1 メソッド(所有型 + 名前 + 呼び出しサンク群 + フラグ)。引数なし/単一引数/戻り値あり。 */
struct FReflectMethod {
    FTypeId      owner;                              ///< 所有型の ID(AcsTypeHash)。
    const char*  name;                               ///< メソッド名。
    void       (*invoke)(void* self);                ///< 引数なし void サンク。
    void       (*invokeArg)(void* self, const char* arg);  ///< 単一引数 void サンク(文字列をパース)。
    void       (*invokeRet)(void* self, const char* arg, char* out, int outcap);  ///< 戻り値ありサンク(結果を文字列化)。
    u32          argKind;                            ///< EMethodArgKind(引数の型)。
    u32          retKind;                            ///< EMethodArgKind(戻り値の型。None=void)。
    u32          flags;                              ///< EMethodFlags の OR。
};

struct CMethodAutoRegister;

/** 反射メソッドのグローバル登録簿(ACS_REGISTER_METHOD が登録)。 */
class CMethodRegistry {
public:
    /** 単一インスタンス(初回呼び出しで遅延構築)。 */
    static CMethodRegistry& Get() noexcept {
        static CMethodRegistry s_instance;
        return s_instance;
    }

    /** 登録する。同じ owner+name の異なる実装は登録元として併存させる。 */
    void Register(const FReflectMethod& m) noexcept {
        RegisterSource(m, nullptr);
    }

    /**
     * 手動登録したメソッドを解除する。
     *
     * @return 一致する手動登録を 1 件解除できたら true。
     */
    bool Unregister(const FReflectMethod& m) noexcept
    {
        return UnregisterSource(m, nullptr);
    }

    /** 全登録数。 */
    u32 Count() const noexcept
    {
        return static_cast<u32>(m_Entries.Num());
    }

    /** i 番目。 */
    const FReflectMethod& At(u32 i) const noexcept
    {
        return m_Entries[i].sources[0].method;
    }

    /** owner 型の n 番目のメソッド(無ければ nullptr)。 */
    const FReflectMethod* AtOfOwner(FTypeId owner, u32 nth) const noexcept {
        u32 seen = 0;
        for (u32 i = 0; i < m_Entries.Num(); ++i) {
            const FReflectMethod& method = m_Entries[i].sources[0].method;
            if (method.owner != owner) continue;
            if (seen == nth) return &method;
            ++seen;
        }
        return nullptr;
    }

    /** owner 型のメソッド数。 */
    u32 CountOfOwner(FTypeId owner) const noexcept {
        u32 c = 0;
        for (u32 i = 0; i < m_Entries.Num(); ++i) {
            if (m_Entries[i].sources[0].method.owner == owner) ++c;
        }
        return c;
    }

    /** owner + 名前で引く(無ければ nullptr)。 */
    const FReflectMethod* Find(FTypeId owner, const char* name) const noexcept {
        for (u32 i = 0; i < m_Entries.Num(); ++i) {
            const FReflectMethod& method = m_Entries[i].sources[0].method;
            if (method.owner == owner && StrEq(method.name, name)) return &method;
        }
        return nullptr;
    }

private:
    friend struct CMethodAutoRegister;

    /** 同じ owner+name へ同時登録できる module/source 数。 */
    static constexpr u32 kMaxSourcesPerMethod = 8;

    struct FMethodSource {
        FReflectMethod method{};
        const void* token = nullptr;
    };

    struct FMethodEntry {
        FMethodSource sources[kMaxSourcesPerMethod]{};
        u8 source_count = 0;
    };

    /** process lifetime の登録簿が一時的な DefaultAllocator を捕捉しないようにする。 */
    CMethodRegistry() noexcept : m_Entries(m_Allocator)
    {
    }

    static bool StrEq(const char* a, const char* b) noexcept {
        if (a == nullptr || b == nullptr) return a == b;
        while (*a != '\0' && *a == *b) { ++a; ++b; }
        return *a == *b;
    }

    static bool SameImplementation(const FReflectMethod& a, const FReflectMethod& b) noexcept
    {
        return a.owner == b.owner && StrEq(a.name, b.name) && a.invoke == b.invoke && a.invokeArg == b.invokeArg &&
               a.invokeRet == b.invokeRet && a.argKind == b.argKind && a.retKind == b.retKind && a.flags == b.flags;
    }

    void RegisterSource(const FReflectMethod& method, const void* token) noexcept
    {
        for (u32 entry_index = 0; entry_index < m_Entries.Num(); ++entry_index) {
            FMethodEntry& entry = m_Entries[entry_index];
            const FReflectMethod& active = entry.sources[0].method;
            if (active.owner != method.owner || !StrEq(active.name, method.name)) continue;

            for (u32 source = 0; source < entry.source_count; ++source) {
                if (token != nullptr && entry.sources[source].token == token) return;
                if (token == nullptr && entry.sources[source].token == nullptr &&
                    SameImplementation(entry.sources[source].method, method))
                    return;
            }
            if (entry.source_count >= kMaxSourcesPerMethod) return;
            entry.sources[entry.source_count++] = FMethodSource{method, token};
            return;
        }

        FMethodEntry entry{};
        entry.sources[0] = FMethodSource{method, token};
        entry.source_count = 1;
        m_Entries.Add(entry);
    }

    bool UnregisterSource(const FReflectMethod& method, const void* token) noexcept
    {
        for (u32 entry_index = 0; entry_index < m_Entries.Num(); ++entry_index) {
            FMethodEntry& entry = m_Entries[entry_index];
            const FReflectMethod& active = entry.sources[0].method;
            if (active.owner != method.owner || !StrEq(active.name, method.name)) continue;

            u32 source_index = entry.source_count;
            for (u32 source = 0; source < entry.source_count; ++source) {
                const bool token_matches = token != nullptr
                                               ? entry.sources[source].token == token
                                               : entry.sources[source].token == nullptr &&
                                                     SameImplementation(entry.sources[source].method, method);
                if (token_matches) {
                    source_index = source;
                    break;
                }
            }
            if (source_index == entry.source_count) return false;

            for (u32 source = source_index; source + 1u < entry.source_count; ++source) {
                entry.sources[source] = entry.sources[source + 1u];
            }
            entry.sources[entry.source_count - 1u] = FMethodSource{};
            --entry.source_count;
            if (entry.source_count != 0) return true;

            for (u32 remaining = entry_index; remaining + 1u < m_Entries.Num(); ++remaining) {
                m_Entries[remaining] = m_Entries[remaining + 1u];
            }
            m_Entries.Pop();
            return true;
        }
        return false;
    }

    /** 登録簿より後に破棄される、登録簿専用の process-lifetime allocator。 */
    CSystemAllocator m_Allocator;

    /** owner+name ごとに登録元を束ねた method 一覧。 */
    TArray<FMethodEntry> m_Entries;
};

/** ACS_REGISTER_METHOD が生成する自動登録ヘルパ。 */
struct CMethodAutoRegister {
    explicit CMethodAutoRegister(const FReflectMethod& method) noexcept : m_Method(method)
    {
        CMethodRegistry::Get().RegisterSource(m_Method, this);
    }

    ~CMethodAutoRegister() noexcept
    {
        (void)CMethodRegistry::Get().UnregisterSource(m_Method, this);
    }

    CMethodAutoRegister(const CMethodAutoRegister&) = delete;
    CMethodAutoRegister& operator=(const CMethodAutoRegister&) = delete;

private:
    FReflectMethod m_Method{};
};

/**
 * 名前で型のメソッドを呼ぶ(引数なし void)。
 *
 * @param owner 所有型の ID(AcsTypeHash)。
 * @param obj   呼び出し対象インスタンス先頭。
 * @param name  メソッド名。
 * @return 見つかって呼べたら true。
 */
inline bool InvokeMethodByName(FTypeId owner, void* obj, const char* name) noexcept {
    const FReflectMethod* m = CMethodRegistry::Get().Find(owner, name);
    if (m == nullptr || m->invoke == nullptr || obj == nullptr) return false;
    m->invoke(obj);
    return true;
}

/**
 * 名前で型のメソッドを «文字列引数» 付きで呼ぶ。引数ありメソッドは invokeArg で
 * arg をパースして起動し、引数なしメソッドは arg を無視して invoke を起動する。
 *
 * @param owner 所有型の ID。
 * @param obj   呼び出し対象インスタンス先頭。
 * @param name  メソッド名。
 * @param arg   文字列引数(引数なしメソッドでは無視。null 可)。
 * @return 見つかって呼べたら true。
 */
inline bool InvokeMethodByNameArg(FTypeId owner, void* obj, const char* name, const char* arg) noexcept {
    const FReflectMethod* m = CMethodRegistry::Get().Find(owner, name);
    if (m == nullptr || obj == nullptr) return false;
    if (m->argKind != METHOD_ARG_NONE && m->invokeArg != nullptr) { m->invokeArg(obj, arg != nullptr ? arg : ""); return true; }
    if (m->invoke != nullptr) { m->invoke(obj); return true; }
    return false;
}

/**
 * 名前で型のメソッドを arg 付きで呼び、戻り値を文字列化して out へ書く。
 *
 * @details 戻り値ありメソッドは invokeRet で結果を out に書き、引数あり/なしメソッドは
 *          従来通り起動して out を空にする。out は呼び出し前に空文字へ初期化される。
 * @param owner  所有型の ID。
 * @param obj    対象インスタンス先頭。
 * @param name   メソッド名。
 * @param arg    文字列引数(null 可)。
 * @param out    戻り値文字列の書き込み先(null 可)。
 * @param outcap out の容量。
 * @return 呼べたら true。
 */
inline bool InvokeMethodByNameRet(FTypeId owner, void* obj, const char* name,
                                  const char* arg, char* out, int outcap) noexcept {
    if (out != nullptr && outcap > 0) out[0] = '\0';
    const FReflectMethod* m = CMethodRegistry::Get().Find(owner, name);
    if (m == nullptr || obj == nullptr) return false;
    if (m->retKind != METHOD_ARG_NONE && m->invokeRet != nullptr) { m->invokeRet(obj, arg != nullptr ? arg : "", out, outcap); return true; }
    if (m->argKind != METHOD_ARG_NONE && m->invokeArg != nullptr) { m->invokeArg(obj, arg != nullptr ? arg : ""); return true; }
    if (m->invoke != nullptr) { m->invoke(obj); return true; }
    return false;
}

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FMethodAutoRegister = CMethodAutoRegister;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FMethodRegistry = CMethodRegistry;

} // namespace acs::game

/** 引数なし void メソッド method を self に対して呼ぶサンク(非捕捉ラムダ → 関数ポインタ)。 */
#define ACS_RMETHOD_THUNK(Type, method)                                              \
    [](void* self) noexcept { static_cast<Type*>(self)->method(); }

/** 単一引数サンク(文字列 a をパースして method へ渡す)。 */
#define ACS_RMETHOD_THUNK_F32(Type, method)                                          \
    [](void* self, const char* a) noexcept { static_cast<Type*>(self)->method(static_cast<::acs::f32>(::atof(a))); }
#define ACS_RMETHOD_THUNK_I32(Type, method)                                          \
    [](void* self, const char* a) noexcept { static_cast<Type*>(self)->method(static_cast<::acs::i32>(::atoi(a))); }
#define ACS_RMETHOD_THUNK_STR(Type, method)                                          \
    [](void* self, const char* a) noexcept { static_cast<Type*>(self)->method(a); }

/** 戻り値ありサンク(引数なし method() を呼び結果を out へ文字列化)。 */
#define ACS_RMETHOD_THUNK_RET_F32(Type, method)                                      \
    [](void* self, const char*, char* out, int cap) noexcept {                       \
        ::snprintf(out, static_cast<size_t>(cap), "%.6g",                            \
                   static_cast<double>(static_cast<Type*>(self)->method())); }
#define ACS_RMETHOD_THUNK_RET_I32(Type, method)                                      \
    [](void* self, const char*, char* out, int cap) noexcept {                       \
        ::snprintf(out, static_cast<size_t>(cap), "%d",                              \
                   static_cast<int>(static_cast<Type*>(self)->method())); }
#define ACS_RMETHOD_THUNK_RET_STR(Type, method)                                      \
    [](void* self, const char*, char* out, int cap) noexcept {                       \
        const char* r = static_cast<Type*>(self)->method();                          \
        int i = 0; for (; r != nullptr && r[i] != '\0' && i < cap - 1; ++i) out[i] = r[i]; \
        if (cap > 0) out[i] = '\0'; }

/** 型 Type の引数なし void メソッド method を flags 付きで登録する(.cpp に 1 回)。
 *  ACS_REGISTER と同じく namespace acs::game を開く(Type は外側ルックアップで
 *  acs::game 内型もグローバルなユーザー型も解決される)。 */
#define ACS_REGISTER_METHOD(Type, method, flags)                                     \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rm_, __LINE__) {        \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                ACS_RMETHOD_THUNK(Type, method), nullptr, nullptr,                   \
                ::acs::game::METHOD_ARG_NONE, ::acs::game::METHOD_ARG_NONE,           \
                static_cast<::acs::u32>(flags) } };                                   \
    } }

/** 単一引数(f32 / i32 / const char*)メソッドを登録する。argKind に応じてサンクを選ぶ。 */
#define ACS_REGISTER_METHOD_F32(Type, method, flags)                                 \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rma_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, ACS_RMETHOD_THUNK_F32(Type, method), nullptr,               \
                ::acs::game::METHOD_ARG_F32, ::acs::game::METHOD_ARG_NONE,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }
#define ACS_REGISTER_METHOD_I32(Type, method, flags)                                 \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rma_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, ACS_RMETHOD_THUNK_I32(Type, method), nullptr,               \
                ::acs::game::METHOD_ARG_I32, ::acs::game::METHOD_ARG_NONE,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }
#define ACS_REGISTER_METHOD_STR(Type, method, flags)                                 \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rma_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, ACS_RMETHOD_THUNK_STR(Type, method), nullptr,               \
                ::acs::game::METHOD_ARG_STR, ::acs::game::METHOD_ARG_NONE,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }

/** 戻り値あり(引数なし)メソッドを登録する。retKind に結果型を入れ invokeRet を持つ。 */
#define ACS_REGISTER_METHOD_RET_F32(Type, method, flags)                             \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rmr_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, nullptr, ACS_RMETHOD_THUNK_RET_F32(Type, method),           \
                ::acs::game::METHOD_ARG_NONE, ::acs::game::METHOD_ARG_F32,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }
#define ACS_REGISTER_METHOD_RET_I32(Type, method, flags)                             \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rmr_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, nullptr, ACS_RMETHOD_THUNK_RET_I32(Type, method),           \
                ::acs::game::METHOD_ARG_NONE, ::acs::game::METHOD_ARG_I32,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }
#define ACS_REGISTER_METHOD_RET_STR(Type, method, flags)                             \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rmr_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, nullptr, ACS_RMETHOD_THUNK_RET_STR(Type, method),           \
                ::acs::game::METHOD_ARG_NONE, ::acs::game::METHOD_ARG_STR,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }
