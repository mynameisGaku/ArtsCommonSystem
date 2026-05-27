// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar T — FVoiceChat 実装 (Stub のみ)
//
// 本 .cpp では `IVoiceChatBackend` の I/F 自体は提供せず、Steam Voice / EOS Voice
// / Vivox / Discord / OpusSelf 各 SDK 未統合ビルドでも常に使える
// `FVoiceChatBackendStub` のみを実装する。実 SDK を使う backend は SDK ヘッダ /
// ライブラリ依存を含むため、別モジュール (将来の `acs_voice_vivox` 等) で独立に
// 実装し、本ファイルには持ち込まない。
//
// 設計のポイント:
//   ・Stub は副作用ゼロ。`Init()` は provider を記録し _initialized を立てるが、
//     IsAvailable() は false のまま (SDK 接続が無いことを示す)。
//   ・JoinChannel / SetLocalMute 等の操作系は全て
//     `ACS_ERR(Generic, kSubVoiceNotImplemented, ...)` を返し、上位ロジックで
//     サイレントに無視するかログ出力するかを呼び出し側に委ねる。
//   ・`ParticipantCount` は TResult を返さない設計のため、Stub では単に 0 を返す
//     (未初期化 / 未 join とも区別しない、UI 側で 0 → "誰もいない" 表示を想定)。
//   ・`GetVoiceStub()` は Meyer's singleton。C++11 以降の規格でスレッド初回構築は
//     保証されるため追加同期は不要。

#include "gameframework/VoiceChat.h"

#include "foundation/Error.h"

namespace acs::game {

// ---- Stub: Init / Shutdown ------------------------------------------------

TResult<void> FVoiceChatBackendStub::Init(EVoiceProvider p) noexcept {
    // provider 選択は記録するが、Stub は SDK 接続を行わないので IsAvailable は
    // false のまま。多重 Init は明示的に許容 (テスト容易性のため)。
    _provider = p;
    _initialized = true;
    return Ok();
}

void FVoiceChatBackendStub::Shutdown() noexcept {
    // Init() 前に呼ばれても安全。状態は完全に初期値に戻す。
    _provider = EVoiceProvider::None;
    _initialized = false;
}

// ---- Stub: チャンネル参加 / 離脱 -----------------------------------------

TResult<void> FVoiceChatBackendStub::JoinChannel(EVoiceChannel ch, const char* channel_id) noexcept {
    (void)ch;
    (void)channel_id;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatBackendStub::JoinChannel called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "FVoiceChatBackendStub: JoinChannel is not implemented (link real voice SDK)");
}

TResult<void> FVoiceChatBackendStub::LeaveChannel(EVoiceChannel ch) noexcept {
    (void)ch;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatBackendStub::LeaveChannel called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "FVoiceChatBackendStub: LeaveChannel is not implemented (link real voice SDK)");
}

// ---- Stub: ミュート / 音量 -----------------------------------------------

TResult<void> FVoiceChatBackendStub::SetLocalMute(bool muted) noexcept {
    (void)muted;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatBackendStub::SetLocalMute called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "FVoiceChatBackendStub: SetLocalMute is not implemented (link real voice SDK)");
}

TResult<void> FVoiceChatBackendStub::SetParticipantMute(const char* user_id, bool muted) noexcept {
    (void)user_id;
    (void)muted;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatBackendStub::SetParticipantMute called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "FVoiceChatBackendStub: SetParticipantMute is not implemented (link real voice SDK)");
}

TResult<void> FVoiceChatBackendStub::SetParticipantVolume(const char* user_id, f32 volume) noexcept {
    (void)user_id;
    (void)volume;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatBackendStub::SetParticipantVolume called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "FVoiceChatBackendStub: SetParticipantVolume is not implemented (link real voice SDK)");
}

// ---- Stub: 参加者取得 ----------------------------------------------------

u32 FVoiceChatBackendStub::ParticipantCount(EVoiceChannel ch) noexcept {
    (void)ch;
    // Stub は誰も join していない扱い。UI 側は 0 をそのまま「参加者なし」表示に
    // 反映できる。TResult を返さない設計なので未初期化との区別は意図的に省略。
    return 0;
}

TResult<VoiceParticipant> FVoiceChatBackendStub::GetParticipant(EVoiceChannel ch, u32 index) noexcept {
    (void)ch;
    (void)index;
    if (!_initialized) {
        return TResult<VoiceParticipant>(
            ACS_ERR(Generic, kSubVoiceNotInitialized,
                    "FVoiceChatBackendStub::GetParticipant called before Init()"));
    }
    return TResult<VoiceParticipant>(
        ACS_ERR(Generic, kSubVoiceNotImplemented,
                "FVoiceChatBackendStub: GetParticipant is not implemented (link real voice SDK)"));
}

// ---- Stub: Tick ----------------------------------------------------------

void FVoiceChatBackendStub::Tick(f32 dt) noexcept {
    (void)dt;  // Stub は event pump を持たないので何もしない
}

// ---- static singleton ---------------------------------------------------

IVoiceChatBackend& GetVoiceStub() noexcept {
    // C++11 以降、関数スコープ static の初期化は thread-safe。
    static FVoiceChatBackendStub _instance;
    return _instance;
}

} // namespace acs::game
