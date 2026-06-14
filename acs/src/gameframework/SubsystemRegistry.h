// SPDX-License-Identifier: Apache-2.0
// GameFramework — FSubsystemRegistry (サブシステム型のファクトリ登録簿)
//
// ACS_REGISTER_SUBSYSTEM(T, scope) で「この型をこのスコープで自動生成する」ことを
// 静的初期化時に登録する。スコープ開始時に FSubsystemCollection がこの登録簿を走査して
// 該当スコープのサブシステムを «全て» 生成する(UE の自動インスタンス化と同じ思想)。
//
// ヘッダオンリー: 登録簿は inline 関数内 static で 1 インスタンス。マクロは無名名前空間に
// 静的オブジェクトを置き、その ctor で登録する(TU ごとに 1 回)。
#pragma once

#include "foundation/Types.h"
#include "memory/UniquePtr.h"
#include "container/Array.h"
#include "gameframework/Subsystem.h"

namespace acs::game {

/** サブシステム 1 体を生成するファクトリ関数(非捕捉ラムダ → 関数ポインタ)。 */
using FSubsystemCreateFn = TUniquePtr<FSubsystem> (*)();

/** 1 つのサブシステム型の登録情報(種別 ID・スコープ・名前・生成関数)。 */
struct FSubsystemFactory {
    const void*        kind   = nullptr;                    ///< SubsystemKindOf<T>()。
    ESubsystemScope    scope  = ESubsystemScope::World;     ///< 生成スコープ。
    const char*        name   = "";                         ///< 型名(デバッグ用)。
    FSubsystemCreateFn create = nullptr;                    ///< 1 体生成する関数。
};

/**
 * サブシステム型のファクトリ登録簿(グローバル単一)。ACS_REGISTER_SUBSYSTEM が登録する。
 */
class FSubsystemRegistry {
public:
    /** 単一インスタンスを返す(初回呼び出しで遅延構築 = 静的初期化順に非依存)。 */
    static FSubsystemRegistry& Get() noexcept {
        static FSubsystemRegistry s_instance;
        return s_instance;
    }

    /** ファクトリを登録する(同じ kind が既にあれば無視 = 二重登録防止)。 */
    void Register(const FSubsystemFactory& f) noexcept {
        for (u32 i = 0; i < m_Factories.Size(); ++i) {
            if (m_Factories[i].kind == f.kind) return;
        }
        m_Factories.PushBack(f);
    }

    /** 登録数。 */
    u32 Count() const noexcept { return static_cast<u32>(m_Factories.Size()); }

    /** i 番目の登録情報。 */
    const FSubsystemFactory& At(u32 i) const noexcept { return m_Factories[i]; }

private:
    FSubsystemRegistry() noexcept = default;
    TArray<FSubsystemFactory> m_Factories;
};

} // namespace acs::game

/**
 * サブシステム型 T を スコープ SCOPE で自動生成するよう登録する(静的初期化時)。
 * 翻訳単位の末尾(クラス定義後)に 1 回書く。
 */
#define ACS_REGISTER_SUBSYSTEM(T, SCOPE)                                              \
    namespace {                                                                       \
        struct AcsSubsysReg_##T {                                                     \
            AcsSubsysReg_##T() noexcept {                                             \
                ::acs::game::FSubsystemRegistry::Get().Register(                      \
                    ::acs::game::FSubsystemFactory{                                   \
                        ::acs::game::SubsystemKindOf<T>(), (SCOPE), #T,               \
                        []() noexcept -> ::acs::TUniquePtr<::acs::game::FSubsystem> { \
                            return ::acs::MakeUnique<T>();                            \
                        } });                                                         \
            }                                                                         \
        } g_AcsSubsysReg_##T;                                                         \
    }
