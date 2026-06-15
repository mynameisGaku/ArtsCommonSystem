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
#include "container/Array.h"
#include "gameframework/Reflect.h"   // FTypeId / AcsTypeHash / ACS_RCAT

namespace acs::game {

/** メソッド指定子フラグ(UFUNCTION 相当)。 */
enum EMethodFlags : u32 {
    METHOD_NONE           = 0u,
    METHOD_BP_CALLABLE    = 1u << 0,   /**< Blueprint/スクリプトから呼べる。 */
    METHOD_CALL_IN_EDITOR = 1u << 1,   /**< エディタのボタンから呼べる。 */
};

/** 反射された 1 メソッド(所有型 + 名前 + 呼び出しサンク + フラグ)。引数なし void 限定。 */
struct FReflectMethod {
    FTypeId      owner;                 ///< 所有型の ID(AcsTypeHash)。
    const char*  name;                  ///< メソッド名。
    void       (*invoke)(void* self);   ///< self を渡して呼ぶサンク(非捕捉ラムダ)。
    u32          flags;                 ///< EMethodFlags の OR。
};

/** 反射メソッドのグローバル登録簿(ACS_REGISTER_METHOD が登録)。 */
class FMethodRegistry {
public:
    /** 単一インスタンス(初回呼び出しで遅延構築)。 */
    static FMethodRegistry& Get() noexcept {
        static FMethodRegistry s_instance;
        return s_instance;
    }

    /** 登録する(同 owner+name が既にあれば無視)。 */
    void Register(const FReflectMethod& m) noexcept {
        for (u32 i = 0; i < m_Methods.Size(); ++i)
            if (m_Methods[i].owner == m.owner && StrEq(m_Methods[i].name, m.name)) return;
        m_Methods.PushBack(m);
    }

    /** 全登録数。 */
    u32 Count() const noexcept { return static_cast<u32>(m_Methods.Size()); }

    /** i 番目。 */
    const FReflectMethod& At(u32 i) const noexcept { return m_Methods[i]; }

    /** owner 型の n 番目のメソッド(無ければ nullptr)。 */
    const FReflectMethod* AtOfOwner(FTypeId owner, u32 nth) const noexcept {
        u32 seen = 0;
        for (u32 i = 0; i < m_Methods.Size(); ++i) {
            if (m_Methods[i].owner != owner) continue;
            if (seen == nth) return &m_Methods[i];
            ++seen;
        }
        return nullptr;
    }

    /** owner 型のメソッド数。 */
    u32 CountOfOwner(FTypeId owner) const noexcept {
        u32 c = 0;
        for (u32 i = 0; i < m_Methods.Size(); ++i) if (m_Methods[i].owner == owner) ++c;
        return c;
    }

    /** owner + 名前で引く(無ければ nullptr)。 */
    const FReflectMethod* Find(FTypeId owner, const char* name) const noexcept {
        for (u32 i = 0; i < m_Methods.Size(); ++i)
            if (m_Methods[i].owner == owner && StrEq(m_Methods[i].name, name)) return &m_Methods[i];
        return nullptr;
    }

private:
    FMethodRegistry() noexcept = default;
    static bool StrEq(const char* a, const char* b) noexcept {
        if (a == nullptr || b == nullptr) return a == b;
        while (*a != '\0' && *a == *b) { ++a; ++b; }
        return *a == *b;
    }
    TArray<FReflectMethod> m_Methods;
};

/** ACS_REGISTER_METHOD が生成する自動登録ヘルパ。 */
struct FMethodAutoRegister {
    explicit FMethodAutoRegister(const FReflectMethod& m) noexcept { FMethodRegistry::Get().Register(m); }
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
    const FReflectMethod* m = FMethodRegistry::Get().Find(owner, name);
    if (m == nullptr || m->invoke == nullptr || obj == nullptr) return false;
    m->invoke(obj);
    return true;
}

} // namespace acs::game

/** 引数なし void メソッド method を self に対して呼ぶサンク(非捕捉ラムダ → 関数ポインタ)。 */
#define ACS_RMETHOD_THUNK(Type, method)                                              \
    [](void* self) noexcept { static_cast<Type*>(self)->method(); }

/** 型 Type の引数なし void メソッド method を flags 付きで登録する(.cpp に 1 回)。
 *  ACS_REGISTER と同じく namespace acs::game を開く(Type は外側ルックアップで
 *  acs::game 内型もグローバルなユーザー型も解決される)。 */
#define ACS_REGISTER_METHOD(Type, method, flags)                                     \
    namespace acs::game { namespace {                                                 \
        const ::acs::game::FMethodAutoRegister ACS_RCAT(s_acs_rm_, __LINE__) {        \
            ::acs::game::FReflectMethod{ ::acs::game::AcsTypeHash(#Type), #method,    \
                ACS_RMETHOD_THUNK(Type, method), static_cast<::acs::u32>(flags) } };  \
    } }
