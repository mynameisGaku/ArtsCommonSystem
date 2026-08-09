// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "memory/SystemAllocator.h"
#include "container/Array.h"
#include "gameframework/Reflect.h"   // FTypeId / AcsTypeHash / ACS_RCAT

#include <cstdlib>   // atof / atoi (文字列引数のパース)
#include <cstdio>    // snprintf (戻り値の文字列化)

namespace acs::game {

/** 反射メソッドをエディタやBlueprintへ公開する指定子。 */
enum EMethodFlags : u32 {
    METHOD_NONE           = 0u,
    METHOD_BP_CALLABLE    = 1u << 0,   /**< Blueprint グラフへ公開する。 */
    METHOD_CALL_IN_EDITOR = 1u << 1,   /**< エディタのボタンへ公開する。 */
};

/** 反射メソッドの単一引数または戻り値の種別。文字列入力を対応型へ変換する。 */
enum EMethodArgKind : u32 {
    METHOD_ARG_NONE = 0u,   /**< 引数なし、または戻り値なし。 */
    METHOD_ARG_F32  = 1u,   /**< f32 の単一値。 */
    METHOD_ARG_I32  = 2u,   /**< i32 の単一値。 */
    METHOD_ARG_STR  = 3u,   /**< const char* の単一値。 */
};

/** owner、名前、呼出しサンク、引数・戻り値種別、公開指定子を保持する登録情報。未対応経路のサンクは nullptr。 */
struct FReflectMethod {
    FTypeId      owner;                              ///< 登録対象型の ID(AcsTypeHash)。
    const char*  name;                               ///< 登録対象メソッドの名前。
    void       (*invoke)(void* self);                ///< 引数なし void の呼出し先。未対応なら nullptr。
    void       (*invokeArg)(void* self, const char* arg);  ///< 単一引数 void の呼出し先。文字列を変換する。
    void       (*invokeRet)(void* self, const char* arg, char* out, int outcap);  ///< 戻り値の呼出し先。結果を out へ書く。
    u32          argKind;                            ///< invokeArg が受ける型。
    u32          retKind;                            ///< invokeRet が返す型。None は戻り値なし。
    u32          flags;                              ///< 公開先を選ぶ EMethodFlags の組合せ。
};

struct CMethodAutoRegister;

/** プロセス寿命で owner+name ごとに最大8件の登録元を管理する。先頭の有効な登録元を公開し、上限超過は登録しない。 */
class CMethodRegistry {
public:
    /** 単一インスタンス(初回呼び出しで遅延構築)。 */
    static CMethodRegistry& Get() noexcept {
        static CMethodRegistry s_instance;
        return s_instance;
    }

    /** owner+name の登録元を追加する。同一記述は追加せず、9件目以降は既存登録を保つ。 */
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

    /** owner+name 単位の登録メソッド数を返す。 */
    u32 Count() const noexcept
    {
        return static_cast<u32>(m_Entries.Num());
    }

    /** i 番目の登録を返す。i は Count() 未満でなければならない。 */
    const FReflectMethod& At(u32 i) const noexcept
    {
        return m_Entries[i].sources[0].method;
    }

    /** owner の n 番目の登録を返す。該当しない場合は nullptr。 */
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

    /** owner の登録メソッド数を返す。 */
    u32 CountOfOwner(FTypeId owner) const noexcept {
        u32 c = 0;
        for (u32 i = 0; i < m_Entries.Num(); ++i) {
            if (m_Entries[i].sources[0].method.owner == owner) ++c;
        }
        return c;
    }

    /** owner+name の登録を返す。該当しない場合は nullptr。 */
    const FReflectMethod* Find(FTypeId owner, const char* name) const noexcept {
        for (u32 i = 0; i < m_Entries.Num(); ++i) {
            const FReflectMethod& method = m_Entries[i].sources[0].method;
            if (method.owner == owner && StrEq(method.name, name)) return &method;
        }
        return nullptr;
    }

private:
    friend struct CMethodAutoRegister;

    /** 同じ owner+name に保持できる登録元の最大数。 */
    static constexpr u32 kMaxSourcesPerMethod = 8;

    struct FMethodSource {
        FReflectMethod method{};                     ///< owner+name に対応する実登録情報。
        const void* token = nullptr;                ///< 自動登録元を識別する所有トークン。
    };

    struct FMethodEntry {
        FMethodSource sources[kMaxSourcesPerMethod]{}; ///< owner+name の登録元を保持する配列。
        u8 source_count = 0;                            ///< 現在有効な登録元の件数。
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

/** ACS_REGISTER_METHOD 系マクロが共有する自動登録型。構築時に登録し、破棄時に同じ登録元を解除する。 */
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
 * 名前で型の引数なし void メソッドを呼ぶ。
 *
 * @param owner 所有型の ID(AcsTypeHash)。
 * @param obj   呼び出し対象インスタンス先頭。
 * @param name  メソッド名。
 * @return owner、obj、name、invoke が有効で呼び出せた場合は true。見つからない場合は false。
 */
inline bool InvokeMethodByName(FTypeId owner, void* obj, const char* name) noexcept {
    const FReflectMethod* m = CMethodRegistry::Get().Find(owner, name);
    if (m == nullptr || m->invoke == nullptr || obj == nullptr) return false;
    m->invoke(obj);
    return true;
}

/**
 * 名前で型のメソッドを文字列引数付きで呼ぶ。引数ありメソッドは invokeArg で
 * arg をパースして起動し、引数なしメソッドは arg を無視して invoke を起動する。
 *
 * @param owner 所有型の ID。
 * @param obj   呼び出し対象インスタンス先頭。
 * @param name  メソッド名。
 * @param arg   文字列引数。null は空文字列として渡す。
 * @return owner、obj、name、対応サンクが有効で呼び出せた場合は true。見つからない場合は false。
 */
inline bool InvokeMethodByNameArg(FTypeId owner, void* obj, const char* name, const char* arg) noexcept {
    const FReflectMethod* m = CMethodRegistry::Get().Find(owner, name);
    if (m == nullptr || obj == nullptr) return false;
    if (m->argKind != METHOD_ARG_NONE && m->invokeArg != nullptr) { m->invokeArg(obj, arg != nullptr ? arg : ""); return true; }
    if (m->invoke != nullptr) { m->invoke(obj); return true; }
    return false;
}

/**
 * 名前で型のメソッドを arg 付きで呼び、戻り値を文字列化して out へ渡す。
 *
 * @details 戻り値ありメソッドは invokeRet で結果を out に書く。戻り値サンクを使う呼出し元は
 *          out 非null、outcap 1以上を満たす。void fallback では out が null、outcap が0でも呼び出せる。
 *          out は有効容量がある場合に呼び出し前に空文字へ初期化される。
 * @param owner  所有型の ID。
 * @param obj    対象インスタンス先頭。
 * @param name   メソッド名。
 * @param arg    文字列引数。null は空文字列として渡す。
 * @param out    戻り値文字列の書き込み先。戻り値サンクを使う場合は非null。
 * @param outcap out の容量。戻り値サンクを使う場合は1以上。
 * @return 登録、対象インスタンス、対応サンクが揃って呼び出せた場合は true。
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

/** 旧名を使うコード向けの互換別名。 */
using FMethodAutoRegister = CMethodAutoRegister;

/** 旧名を使うコード向けの互換別名。 */
using FMethodRegistry = CMethodRegistry;

} // namespace acs::game

/** 引数なし void メソッドを self に対して呼ぶサンク。 */
#define ACS_RMETHOD_THUNK(Type, method)                                              \
    [](void* self) noexcept { static_cast<Type*>(self)->method(); }

/** f32 単一引数サンク。文字列 a を f32 へ変換して method へ渡す。 */
#define ACS_RMETHOD_THUNK_F32(Type, method)                                          \
    [](void* self, const char* a) noexcept { static_cast<Type*>(self)->method(static_cast<::acs::f32>(::atof(a))); }
/** i32 単一引数サンク。文字列 a を i32 へ変換して method へ渡す。 */
#define ACS_RMETHOD_THUNK_I32(Type, method)                                          \
    [](void* self, const char* a) noexcept { static_cast<Type*>(self)->method(static_cast<::acs::i32>(::atoi(a))); }
/** const char* 単一引数サンク。文字列 a を method へ渡す。 */
#define ACS_RMETHOD_THUNK_STR(Type, method)                                          \
    [](void* self, const char* a) noexcept { static_cast<Type*>(self)->method(a); }

/** f32 戻り値サンク。out は非null、cap は1以上でなければならない。 */
#define ACS_RMETHOD_THUNK_RET_F32(Type, method)                                      \
    [](void* self, const char*, char* out, int cap) noexcept {                       \
        ::snprintf(out, static_cast<size_t>(cap), "%.6g",                            \
                   static_cast<double>(static_cast<Type*>(self)->method())); }
/** i32 戻り値サンク。out は非null、cap は1以上でなければならない。 */
#define ACS_RMETHOD_THUNK_RET_I32(Type, method)                                      \
    [](void* self, const char*, char* out, int cap) noexcept {                       \
        ::snprintf(out, static_cast<size_t>(cap), "%d",                              \
                   static_cast<int>(static_cast<Type*>(self)->method())); }
/** const char* 戻り値サンク。out は非null、cap は1以上でなければならない。 */
#define ACS_RMETHOD_THUNK_RET_STR(Type, method)                                      \
    [](void* self, const char*, char* out, int cap) noexcept {                       \
        const char* r = static_cast<Type*>(self)->method();                          \
        int i = 0; for (; r != nullptr && r[i] != '\0' && i < cap - 1; ++i) out[i] = r[i]; \
        if (cap > 0) out[i] = '\0'; }

/** 型 Type の引数なし void メソッドを flags 付きで登録する。owner+name の8件超過は登録しない。
 *  namespace acs::game を開き、Type は内側と外側の名前を解決する。 */
#define ACS_REGISTER_METHOD(Type, method, flags)                                     \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rm_, __LINE__) {        \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                ACS_RMETHOD_THUNK(Type, method), nullptr, nullptr,                   \
                ::acs::game::METHOD_ARG_NONE, ::acs::game::METHOD_ARG_NONE,           \
                static_cast<::acs::u32>(flags) } };                                   \
    } }

/** f32 単一引数メソッドを登録し、文字列引数を f32 へ変換する。 */
#define ACS_REGISTER_METHOD_F32(Type, method, flags)                                 \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rma_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, ACS_RMETHOD_THUNK_F32(Type, method), nullptr,               \
                ::acs::game::METHOD_ARG_F32, ::acs::game::METHOD_ARG_NONE,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }
/** i32 単一引数メソッドを登録し、文字列引数を i32 へ変換する。 */
#define ACS_REGISTER_METHOD_I32(Type, method, flags)                                 \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rma_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, ACS_RMETHOD_THUNK_I32(Type, method), nullptr,               \
                ::acs::game::METHOD_ARG_I32, ::acs::game::METHOD_ARG_NONE,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }
/** const char* 単一引数メソッドを登録し、文字列引数をそのまま渡す。 */
#define ACS_REGISTER_METHOD_STR(Type, method, flags)                                 \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rma_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, ACS_RMETHOD_THUNK_STR(Type, method), nullptr,               \
                ::acs::game::METHOD_ARG_STR, ::acs::game::METHOD_ARG_NONE,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }

/** f32 戻り値メソッドを登録し、結果を out へ文字列化する。 */
#define ACS_REGISTER_METHOD_RET_F32(Type, method, flags)                             \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rmr_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, nullptr, ACS_RMETHOD_THUNK_RET_F32(Type, method),           \
                ::acs::game::METHOD_ARG_NONE, ::acs::game::METHOD_ARG_F32,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }
/** i32 戻り値メソッドを登録し、結果を out へ文字列化する。 */
#define ACS_REGISTER_METHOD_RET_I32(Type, method, flags)                             \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rmr_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, nullptr, ACS_RMETHOD_THUNK_RET_I32(Type, method),           \
                ::acs::game::METHOD_ARG_NONE, ::acs::game::METHOD_ARG_I32,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }
/** const char* 戻り値メソッドを登録し、結果を out へコピーする。 */
#define ACS_REGISTER_METHOD_RET_STR(Type, method, flags)                             \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::CMethodAutoRegister ACS_RCAT(s_acs_rmr_, __LINE__) {       \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                nullptr, nullptr, ACS_RMETHOD_THUNK_RET_STR(Type, method),           \
                ::acs::game::METHOD_ARG_NONE, ::acs::game::METHOD_ARG_STR,            \
                static_cast<::acs::u32>(flags) } };                                   \
    } }
