// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar T — VoiceChat 実装 (Stub のみ)
//
// 本 .cpp では `IVoiceChatBackend` の I/F 自体は提供せず、Steam Voice / EOS Voice
// / Vivox / Discord / OpusSelf 各 SDK 未統合ビルドでも常に使える
// `VoiceChatBackendStub` のみを実装する。実 SDK を使う backend は SDK ヘッダ /
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

TResult<void> VoiceChatBackendStub::Init(EVoiceProvider p) noexcept {
    // provider 選択は記録するが、Stub は SDK 接続を行わないので IsAvailable は
    // false のまま。多重 Init は明示的に許容 (テスト容易性のため)。
    _provider = p;
    _initialized = true;
    return Ok();
}

void VoiceChatBackendStub::Shutdown() noexcept {
    // Init() 前に呼ばれても安全。状態は完全に初期値に戻す。
    _provider = EVoiceProvider::None;
    _initialized = false;
}

// ---- Stub: チャンネル参加 / 離脱 -----------------------------------------

TResult<void> VoiceChatBackendStub::JoinChannel(EVoiceChannel ch, const char* channel_id) noexcept {
    (void)ch;
    (void)channel_id;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "VoiceChatBackendStub::JoinChannel called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "VoiceChatBackendStub: JoinChannel is not implemented (link real voice SDK)");
}

TResult<void> VoiceChatBackendStub::LeaveChannel(EVoiceChannel ch) noexcept {
    (void)ch;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "VoiceChatBackendStub::LeaveChannel called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "VoiceChatBackendStub: LeaveChannel is not implemented (link real voice SDK)");
}

// ---- Stub: ミュート / 音量 -----------------------------------------------

TResult<void> VoiceChatBackendStub::SetLocalMute(bool muted) noexcept {
    (void)muted;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "VoiceChatBackendStub::SetLocalMute called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "VoiceChatBackendStub: SetLocalMute is not implemented (link real voice SDK)");
}

TResult<void> VoiceChatBackendStub::SetParticipantMute(const char* user_id, bool muted) noexcept {
    (void)user_id;
    (void)muted;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "VoiceChatBackendStub::SetParticipantMute called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "VoiceChatBackendStub: SetParticipantMute is not implemented (link real voice SDK)");
}

TResult<void> VoiceChatBackendStub::SetParticipantVolume(const char* user_id, f32 volume) noexcept {
    (void)user_id;
    (void)volume;
    if (!_initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "VoiceChatBackendStub::SetParticipantVolume called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "VoiceChatBackendStub: SetParticipantVolume is not implemented (link real voice SDK)");
}

// ---- Stub: 参加者取得 ----------------------------------------------------

u32 VoiceChatBackendStub::ParticipantCount(EVoiceChannel ch) noexcept {
    (void)ch;
    // Stub は誰も join していない扱い。UI 側は 0 をそのまま「参加者なし」表示に
    // 反映できる。TResult を返さない設計なので未初期化との区別は意図的に省略。
    return 0;
}

TResult<VoiceParticipant> VoiceChatBackendStub::GetParticipant(EVoiceChannel ch, u32 index) noexcept {
    (void)ch;
    (void)index;
    if (!_initialized) {
        return TResult<VoiceParticipant>(
            ACS_ERR(Generic, kSubVoiceNotInitialized,
                    "VoiceChatBackendStub::GetParticipant called before Init()"));
    }
    return TResult<VoiceParticipant>(
        ACS_ERR(Generic, kSubVoiceNotImplemented,
                "VoiceChatBackendStub: GetParticipant is not implemented (link real voice SDK)"));
}

// ---- Stub: Tick ----------------------------------------------------------

void VoiceChatBackendStub::Tick(f32 dt) noexcept {
    (void)dt;  // Stub は event pump を持たないので何もしない
}

// ---- static singleton ---------------------------------------------------

IVoiceChatBackend& GetVoiceStub() noexcept {
    // C++11 以降、関数スコープ static の初期化は thread-safe。
    static VoiceChatBackendStub _instance;
    return _instance;
}

} // namespace acs::game
