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
//   ・Stub は副作用ゼロ。`Init()` は provider を記録し m_Initialized を立てるが、
//     IsAvailable() は false のまま (SDK 接続が無いことを示す)。
//   ・JoinChannel / SetLocalMute 等の操作系は全て
//     `ACS_ERR(Generic, kSubVoiceNotImplemented, ...)` を返し、上位ロジックで
//     サイレントに無視するかログ出力するかを呼び出し側に委ねる。
//   ・`ParticipantCount` は TResult を返さない設計のため、Stub では単に 0 を返す
//     (未初期化 / 未 join とも区別しない、UI 側で 0 → "誰もいない" 表示を想定)。
//   ・`GetVoiceStub()` は Meyer's singleton。C++11 以降の規格でスレッド初回構築は
//     保証されるため追加同期は不要。

#include "gameframework/VoiceChat.h"
#include "memory/SystemAllocator.h"

#include "foundation/Error.h"
#include "foundation/Move.h"
#include "memory/Memory.h"   // MemCopy / MemSet

namespace acs::game {

/** DefaultAllocator を使うループバック backend を構築する。 */
FVoiceChatLoopbackBackend::FVoiceChatLoopbackBackend() noexcept : FVoiceChatLoopbackBackend(DefaultAllocator())
{
}

/** 指定 allocator を全チャンネルの可変長状態へ固定して構築する。 */
FVoiceChatLoopbackBackend::FVoiceChatLoopbackBackend(FAllocator& allocator) noexcept
    : m_Allocator(&allocator),
      m_Channels{FChannel(allocator), FChannel(allocator), FChannel(allocator), FChannel(allocator)}
{
}

/** provider を記録し m_Initialized を立てる (実 SDK 接続はしない)。 */
TResult<void> FVoiceChatBackendStub::Init(EVoiceProvider p) noexcept {
    // provider 選択は記録するが、Stub は SDK 接続を行わないので IsAvailable は
    // false のまま。多重 Init は明示的に許容 (テスト容易性のため)。
    m_Provider = p;
    m_Initialized = true;
    return Ok();
}

/** 状態を完全に初期値へ戻す (Init() 前に呼んでも安全)。 */
void FVoiceChatBackendStub::Shutdown() noexcept {
    // Init() 前に呼ばれても安全。状態は完全に初期値に戻す。
    m_Provider = EVoiceProvider::None;
    m_Initialized = false;
}

/** チャンネル参加 (未初期化なら NotInitialized、それ以外は NotImplemented)。 */
TResult<void> FVoiceChatBackendStub::JoinChannel(EVoiceChannel ch, const char* channel_id) noexcept {
    (void)ch;
    (void)channel_id;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatBackendStub::JoinChannel called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "FVoiceChatBackendStub: JoinChannel is not implemented (link real voice SDK)");
}

/** チャンネル離脱 (未初期化なら NotInitialized、それ以外は NotImplemented)。 */
TResult<void> FVoiceChatBackendStub::LeaveChannel(EVoiceChannel ch) noexcept {
    (void)ch;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatBackendStub::LeaveChannel called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "FVoiceChatBackendStub: LeaveChannel is not implemented (link real voice SDK)");
}

/** ローカルミュート (未初期化なら NotInitialized、それ以外は NotImplemented)。 */
TResult<void> FVoiceChatBackendStub::SetLocalMute(bool muted) noexcept {
    (void)muted;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatBackendStub::SetLocalMute called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "FVoiceChatBackendStub: SetLocalMute is not implemented (link real voice SDK)");
}

/** 参加者ミュート (未初期化なら NotInitialized、それ以外は NotImplemented)。 */
TResult<void> FVoiceChatBackendStub::SetParticipantMute(const char* user_id, bool muted) noexcept {
    (void)user_id;
    (void)muted;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatBackendStub::SetParticipantMute called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "FVoiceChatBackendStub: SetParticipantMute is not implemented (link real voice SDK)");
}

/** 参加者音量設定 (未初期化なら NotInitialized、それ以外は NotImplemented)。 */
TResult<void> FVoiceChatBackendStub::SetParticipantVolume(const char* user_id, f32 volume) noexcept {
    (void)user_id;
    (void)volume;
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatBackendStub::SetParticipantVolume called before Init()");
    }
    return ACS_ERR(Generic, kSubVoiceNotImplemented,
                   "FVoiceChatBackendStub: SetParticipantVolume is not implemented (link real voice SDK)");
}

/** Stub では誰も join していない扱いで常に 0 を返す。 */
u32 FVoiceChatBackendStub::ParticipantCount(EVoiceChannel ch) noexcept {
    (void)ch;
    // Stub は誰も join していない扱い。UI 側は 0 をそのまま「参加者なし」表示に
    // 反映できる。TResult を返さない設計なので未初期化との区別は意図的に省略。
    return 0;
}

/** 参加者取得 (未初期化なら NotInitialized、それ以外は NotImplemented)。 */
TResult<VoiceParticipant> FVoiceChatBackendStub::GetParticipant(EVoiceChannel ch, u32 index) noexcept {
    (void)ch;
    (void)index;
    if (!m_Initialized) {
        return TResult<VoiceParticipant>(
            ACS_ERR(Generic, kSubVoiceNotInitialized,
                    "FVoiceChatBackendStub::GetParticipant called before Init()"));
    }
    return TResult<VoiceParticipant>(
        ACS_ERR(Generic, kSubVoiceNotImplemented,
                "FVoiceChatBackendStub: GetParticipant is not implemented (link real voice SDK)"));
}

/** Stub は event pump を持たないので何もしない。 */
void FVoiceChatBackendStub::Tick(f32 dt) noexcept {
    (void)dt;  // Stub は event pump を持たないので何もしない
}

/** Stub backend の Meyer's singleton を返す。 */
IVoiceChatBackend& GetVoiceStub() noexcept {
    // C++11 以降、関数スコープ static の初期化は thread-safe。
    static FVoiceChatBackendStub m_Instance;
    return m_Instance;
}

/** int16 PCM を magic+seq+count 付き framing に encode する (header+payload)。 */
u32 FVoiceChatLoopbackBackend::EncodeFrame(const i16* pcm, u32 sample_count, u32 sequence,
                                           u8* out, u32 out_capacity) noexcept {
    if (out == nullptr) return 0;
    if (sample_count > kVoiceMaxFrameSamples) return 0;
    if (sample_count != 0 && pcm == nullptr) return 0;

    const u32 payload_bytes = sample_count * static_cast<u32>(sizeof(i16));
    const u32 total = static_cast<u32>(sizeof(VoiceFrameHeader)) + payload_bytes;
    if (out_capacity < total) return 0;

    VoiceFrameHeader hdr;
    hdr.magic        = kVoiceFrameMagic;
    hdr.sequence     = sequence;
    hdr.sample_count = sample_count;
    hdr.reserved     = 0;

    // header はトリビアルコピー可能な POD なのでバイト列として安全に転写できる。
    MemCopy(out, &hdr, sizeof(VoiceFrameHeader));
    if (payload_bytes != 0) {
        MemCopy(out + sizeof(VoiceFrameHeader), pcm, payload_bytes);
    }
    return total;
}

/** framed バイト列を検証して int16 PCM を復元する (破損は FrameCorrupt)。 */
TResult<u32> FVoiceChatLoopbackBackend::DecodeFrame(const u8* in, u32 in_size,
                                                    i16* out, u32 out_capacity) noexcept {
    if (in == nullptr || in_size < sizeof(VoiceFrameHeader)) {
        return TResult<u32>(ACS_ERR(Generic, kSubVoiceFrameCorrupt,
                                    "DecodeFrame: input smaller than header"));
    }

    VoiceFrameHeader hdr;
    MemCopy(&hdr, in, sizeof(VoiceFrameHeader));
    if (hdr.magic != kVoiceFrameMagic) {
        return TResult<u32>(ACS_ERR(Generic, kSubVoiceFrameCorrupt,
                                    "DecodeFrame: bad magic"));
    }
    if (hdr.sample_count > kVoiceMaxFrameSamples) {
        return TResult<u32>(ACS_ERR(Generic, kSubVoiceFrameCorrupt,
                                    "DecodeFrame: sample_count exceeds max"));
    }

    const u32 payload_bytes = hdr.sample_count * static_cast<u32>(sizeof(i16));
    if (in_size < sizeof(VoiceFrameHeader) + payload_bytes) {
        return TResult<u32>(ACS_ERR(Generic, kSubVoiceFrameCorrupt,
                                    "DecodeFrame: truncated payload"));
    }
    if (out == nullptr || out_capacity < hdr.sample_count) {
        return TResult<u32>(ACS_ERR(Generic, kSubVoiceBadArgument,
                                    "DecodeFrame: output capacity too small"));
    }

    if (payload_bytes != 0) {
        MemCopy(out, in + sizeof(VoiceFrameHeader), payload_bytes);
    }
    return Ok<u32>(hdr.sample_count);
}

/** ch 内で user_id を線形検索する (見つからなければ size を返す)。 */
u32 FVoiceChatLoopbackBackend::FindParticipant(const FChannel& c, const char* user_id) const noexcept {
    if (user_id == nullptr) return static_cast<u32>(c.participants.Size());
    const FStringView want(user_id);
    for (usize i = 0; i < c.participants.Size(); ++i) {
        if (c.participants[i].user_id.View() == want) return static_cast<u32>(i);
    }
    return static_cast<u32>(c.participants.Size());
}

/** 1 フレームを encode して participant の rx_frames 末尾に append する。 */
void FVoiceChatLoopbackBackend::EnqueueFrame(FLoopParticipant& p, const i16* pcm, u32 sample_count) noexcept {
    // この参加者の受信キュー末尾に 1 frame を append する。sequence は送信元
    // (= ローカル) ではなく「キュー所有者から見た到着順」として next_seq を使う。
    u8 staging[sizeof(VoiceFrameHeader) + kVoiceMaxFrameSamples * sizeof(i16)];
    const u32 n = EncodeFrame(pcm, sample_count, p.next_seq,
                              staging, static_cast<u32>(sizeof(staging)));
    if (n == 0) return;  // 引数不正 (上位で弾いている想定だが防御的に)
    ++p.next_seq;

    const usize old = p.rx_frames.Size();
    p.rx_frames.Resize(old + n);
    MemCopy(p.rx_frames.Data() + old, staging, n);
}

/** provider を記録し全チャンネルの状態をリセットする。 */
TResult<void> FVoiceChatLoopbackBackend::Init(EVoiceProvider p) noexcept {
    m_Provider     = p;
    m_Initialized  = true;
    m_LocalMuted   = false;
    m_LastLocalRms = 0.0f;
    m_LastLocalPeak= 0.0f;
    // チャンネルは多重 Init でも状態を引きずらないようリセットする。
    for (u32 i = 0; i < 4; ++i) {
        m_Channels[i].joined      = false;
        m_Channels[i].pump_target = 0;
        m_Channels[i].channel_id.ReleaseStorage();
        m_Channels[i].participants.ReleaseStorage();
    }
    return Ok();
}

/** 状態を初期値へ戻し全チャンネルをクリアする。 */
void FVoiceChatLoopbackBackend::Shutdown() noexcept {
    m_Provider     = EVoiceProvider::None;
    m_Initialized  = false;
    m_LocalMuted   = false;
    m_LastLocalRms = 0.0f;
    m_LastLocalPeak= 0.0f;
    for (u32 i = 0; i < 4; ++i) {
        m_Channels[i].joined      = false;
        m_Channels[i].pump_target = 0;
        m_Channels[i].channel_id.ReleaseStorage();
        m_Channels[i].participants.ReleaseStorage();
    }
}

/** joined フラグを立て channel_id を保持する。 */
TResult<void> FVoiceChatLoopbackBackend::JoinChannel(EVoiceChannel ch, const char* channel_id) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatLoopbackBackend::JoinChannel called before Init()");
    }
    FChannel& c = Chan(ch);
    c.joined = true;
    // move 代入で一時 FString の DefaultAllocator を取り込まないよう、既存 allocator
    // のまま内容だけを更新する。
    c.channel_id.Clear();
    c.channel_id.Append(channel_id ? channel_id : "");
    return Ok();
}

/** joined を下ろし参加者・キュー・channel_id を破棄する。 */
TResult<void> FVoiceChatLoopbackBackend::LeaveChannel(EVoiceChannel ch) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatLoopbackBackend::LeaveChannel called before Init()");
    }
    FChannel& c = Chan(ch);
    c.joined      = false;
    c.pump_target = 0;
    c.channel_id.Clear();
    c.participants.Clear();
    return Ok();
}

/** ローカルミュートフラグを設定する (送信抑止)。 */
TResult<void> FVoiceChatLoopbackBackend::SetLocalMute(bool muted) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatLoopbackBackend::SetLocalMute called before Init()");
    }
    m_LocalMuted = muted;
    return Ok();
}

/** 指定 user を全チャンネル横断で mute する。 */
TResult<void> FVoiceChatLoopbackBackend::SetParticipantMute(const char* user_id, bool muted) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatLoopbackBackend::SetParticipantMute called before Init()");
    }
    if (user_id == nullptr) {
        return ACS_ERR(Generic, kSubVoiceBadArgument,
                       "FVoiceChatLoopbackBackend::SetParticipantMute: null user_id");
    }
    // 全チャンネル横断で一致 user を mute する (同 user が複数 ch に居る場合に対応)。
    bool found = false;
    for (u32 i = 0; i < 4; ++i) {
        FChannel& c = m_Channels[i];
        const u32 idx = FindParticipant(c, user_id);
        if (idx < c.participants.Size()) {
            c.participants[idx].muted_local = muted;
            found = true;
        }
    }
    if (!found) {
        return ACS_ERR(Generic, kSubVoiceUnknownUser,
                       "FVoiceChatLoopbackBackend::SetParticipantMute: unknown user_id");
    }
    return Ok();
}

/** 指定 user の音量を全チャンネル横断で 0.0〜2.0 に clamp して設定する。 */
TResult<void> FVoiceChatLoopbackBackend::SetParticipantVolume(const char* user_id, f32 volume) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatLoopbackBackend::SetParticipantVolume called before Init()");
    }
    if (user_id == nullptr) {
        return ACS_ERR(Generic, kSubVoiceBadArgument,
                       "FVoiceChatLoopbackBackend::SetParticipantVolume: null user_id");
    }
    // 0.0〜2.0 に clamp (1.0 超で増幅も許容、mix 時に最終 clamp されるため安全)。
    f32 v = volume;
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;

    bool found = false;
    for (u32 i = 0; i < 4; ++i) {
        FChannel& c = m_Channels[i];
        const u32 idx = FindParticipant(c, user_id);
        if (idx < c.participants.Size()) {
            c.participants[idx].volume = v;
            found = true;
        }
    }
    if (!found) {
        return ACS_ERR(Generic, kSubVoiceUnknownUser,
                       "FVoiceChatLoopbackBackend::SetParticipantVolume: unknown user_id");
    }
    return Ok();
}

/** join 済みチャンネルの参加者数を返す (未 join は 0)。 */
u32 FVoiceChatLoopbackBackend::ParticipantCount(EVoiceChannel ch) noexcept {
    if (!m_Initialized) return 0;
    const FChannel& c = Chan(ch);
    if (!c.joined) return 0;
    return static_cast<u32>(c.participants.Size());
}

/** index 番目の参加者を VoiceParticipant に詰めて返す。 */
TResult<VoiceParticipant> FVoiceChatLoopbackBackend::GetParticipant(EVoiceChannel ch, u32 index) noexcept {
    if (!m_Initialized) {
        return TResult<VoiceParticipant>(
            ACS_ERR(Generic, kSubVoiceNotInitialized,
                    "FVoiceChatLoopbackBackend::GetParticipant called before Init()"));
    }
    FChannel& c = Chan(ch);
    if (!c.joined) {
        return TResult<VoiceParticipant>(
            ACS_ERR(Generic, kSubVoiceNotJoined,
                    "FVoiceChatLoopbackBackend::GetParticipant: channel not joined"));
    }
    if (index >= c.participants.Size()) {
        return TResult<VoiceParticipant>(
            ACS_ERR(Generic, kSubVoiceBadArgument,
                    "FVoiceChatLoopbackBackend::GetParticipant: index out of range"));
    }
    const FLoopParticipant& p = c.participants[index];
    VoiceParticipant out;
    out.user_id        = p.user_id.Data();        // 寿命 = 次 Tick / 構造変更まで (FString 所有領域)
    out.display_name   = p.display_name.Data();
    out.is_muted_local = p.muted_local;
    // 受信キューに未消費フレームがあれば「発言中」とみなし、最後の RMS を level に。
    out.is_speaking    = !p.rx_frames.IsEmpty();
    out.audio_level    = (index == 0) ? m_LastLocalRms : (p.rx_frames.IsEmpty() ? 0.0f : 1.0f);
    return TResult<VoiceParticipant>(OkInit, out);
}

/** ループバックは event pump を持たないので何もしない。 */
void FVoiceChatLoopbackBackend::Tick(f32 dt) noexcept {
    // ループバックは event pump を持たないが、I/F 契約 (毎フレーム呼ぶ) は守る。
    // 将来 jitter buffer の経時 drain 等を入れる余地として dt を受ける。
    (void)dt;
}

/** 参加者を ch に追加する (既存 user は表示名のみ更新する冪等扱い)。 */
TResult<void> FVoiceChatLoopbackBackend::AddParticipant(EVoiceChannel ch, const char* user_id,
                                                        const char* display_name) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatLoopbackBackend::AddParticipant called before Init()");
    }
    if (user_id == nullptr || user_id[0] == '\0') {
        return ACS_ERR(Generic, kSubVoiceBadArgument,
                       "FVoiceChatLoopbackBackend::AddParticipant: empty user_id");
    }
    FChannel& c = Chan(ch);
    if (!c.joined) {
        return ACS_ERR(Generic, kSubVoiceNotJoined,
                       "FVoiceChatLoopbackBackend::AddParticipant: channel not joined");
    }
    // 冪等: 既存 user は表示名のみ更新して成功扱い。
    const u32 idx = FindParticipant(c, user_id);
    if (idx < c.participants.Size()) {
        c.participants[idx].display_name.Clear();
        c.participants[idx].display_name.Append(display_name ? display_name : "");
        return Ok();
    }
    FLoopParticipant p(*m_Allocator);
    p.user_id.Append(user_id);
    p.display_name.Append(display_name ? display_name : "");
    p.volume       = 1.0f;
    p.muted_local  = false;
    p.next_seq     = 0;
    c.participants.PushBack(Move(p));
    return Ok();
}

/** 参加者を swap-remove で取り除き pump_target を補正する。 */
TResult<void> FVoiceChatLoopbackBackend::RemoveParticipant(EVoiceChannel ch, const char* user_id) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatLoopbackBackend::RemoveParticipant called before Init()");
    }
    if (user_id == nullptr) {
        return ACS_ERR(Generic, kSubVoiceBadArgument,
                       "FVoiceChatLoopbackBackend::RemoveParticipant: null user_id");
    }
    FChannel& c = Chan(ch);
    const u32 idx = FindParticipant(c, user_id);
    if (idx >= c.participants.Size()) {
        return ACS_ERR(Generic, kSubVoiceUnknownUser,
                       "FVoiceChatLoopbackBackend::RemoveParticipant: unknown user_id");
    }
    // 順序維持の必要が薄いので swap-remove。pump_target が末尾を指していたら
    // 補正する (swap で末尾要素が idx に移動するため)。
    const u32 last = static_cast<u32>(c.participants.Size()) - 1;
    c.participants.RemoveAtSwap(idx);
    if (c.pump_target == last) c.pump_target = idx;
    if (c.pump_target >= c.participants.Size()) c.pump_target = 0;
    return Ok();
}

/** pump 対象を指定 user の index に切り替える。 */
TResult<void> FVoiceChatLoopbackBackend::SetPumpTarget(EVoiceChannel ch, const char* user_id) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatLoopbackBackend::SetPumpTarget called before Init()");
    }
    FChannel& c = Chan(ch);
    const u32 idx = FindParticipant(c, user_id);
    if (idx >= c.participants.Size()) {
        return ACS_ERR(Generic, kSubVoiceUnknownUser,
                       "FVoiceChatLoopbackBackend::SetPumpTarget: unknown user_id");
    }
    c.pump_target = idx;
    return Ok();
}

/** rx_frames を header 単位に辿り、溜まっているフレーム数を数える。 */
u32 FVoiceChatLoopbackBackend::PendingFrameCount(EVoiceChannel ch, const char* user_id) noexcept {
    if (!m_Initialized) return 0;
    FChannel& c = Chan(ch);
    const u32 idx = FindParticipant(c, user_id);
    if (idx >= c.participants.Size()) return 0;
    // rx_frames は frame の連結。header を辿って frame 数を数える。
    const FLoopParticipant& p = c.participants[idx];
    const u8* base = p.rx_frames.Data();
    const usize total = p.rx_frames.Size();
    usize off = 0;
    u32 count = 0;
    while (off + sizeof(VoiceFrameHeader) <= total) {
        VoiceFrameHeader hdr;
        MemCopy(&hdr, base + off, sizeof(VoiceFrameHeader));
        if (hdr.magic != kVoiceFrameMagic) break;
        const usize frame = sizeof(VoiceFrameHeader)
                          + static_cast<usize>(hdr.sample_count) * sizeof(i16);
        if (off + frame > total) break;
        off += frame;
        ++count;
    }
    return count;
}

/** RMS/peak を算出し、ミュートでなければ自分以外の全参加者へ enqueue する。 */
TResult<void> FVoiceChatLoopbackBackend::PushLocalFrame(EVoiceChannel ch, const i16* pcm,
                                                        u32 sample_count) noexcept {
    if (!m_Initialized) {
        return ACS_ERR(Generic, kSubVoiceNotInitialized,
                       "FVoiceChatLoopbackBackend::PushLocalFrame called before Init()");
    }
    if (sample_count > kVoiceMaxFrameSamples) {
        return ACS_ERR(Generic, kSubVoiceBadArgument,
                       "FVoiceChatLoopbackBackend::PushLocalFrame: frame too large");
    }
    if (sample_count != 0 && pcm == nullptr) {
        return ACS_ERR(Generic, kSubVoiceBadArgument,
                       "FVoiceChatLoopbackBackend::PushLocalFrame: null pcm");
    }
    FChannel& c = Chan(ch);
    if (!c.joined) {
        return ACS_ERR(Generic, kSubVoiceNotJoined,
                       "FVoiceChatLoopbackBackend::PushLocalFrame: channel not joined");
    }

    // 実データから RMS / peak を算出 (VAD / メーター用)。ミュート中でも計測はする
    // (capture は続くという Vivox 流儀)。i16 を [-1,1] に正規化。
    f64 sumsq = 0.0;
    i32 peak  = 0;
    for (u32 i = 0; i < sample_count; ++i) {
        const i32 s = pcm[i];
        sumsq += static_cast<f64>(s) * static_cast<f64>(s);
        const i32 a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    if (sample_count > 0) {
        const f64 mean = sumsq / static_cast<f64>(sample_count);
        // sqrt を STL 無しで: f64 の Newton 法 2 回でも良いが、ここは <cmath> 非依存の
        // 単純な反復平方根を使う (決定的)。
        f64 r = mean;
        if (r > 0.0) {
            f64 x = r;
            for (int it = 0; it < 24; ++it) x = 0.5 * (x + r / x);
            r = x;
        } else {
            r = 0.0;
        }
        m_LastLocalRms  = static_cast<f32>(r / 32768.0);
        m_LastLocalPeak = static_cast<f32>(static_cast<f64>(peak) / 32768.0);
        if (m_LastLocalRms  > 1.0f) m_LastLocalRms  = 1.0f;
        if (m_LastLocalPeak > 1.0f) m_LastLocalPeak = 1.0f;
    } else {
        m_LastLocalRms  = 0.0f;
        m_LastLocalPeak = 0.0f;
    }

    // ローカルミュート中は route しない (送信停止)。計測だけ済ませて返す。
    if (m_LocalMuted) return Ok();

    // 自分 (index 0) 以外の全参加者の受信キューへ enqueue。これが loopback の本体。
    for (usize i = 1; i < c.participants.Size(); ++i) {
        EnqueueFrame(c.participants[i], pcm, sample_count);
    }
    return Ok();
}

/** float [-1,1] を対称量子化で int16 化し PushLocalFrame に合流する。 */
TResult<void> FVoiceChatLoopbackBackend::PushLocalFrameF32(EVoiceChannel ch, const f32* pcm,
                                                           u32 sample_count) noexcept {
    if (sample_count > kVoiceMaxFrameSamples) {
        return ACS_ERR(Generic, kSubVoiceBadArgument,
                       "FVoiceChatLoopbackBackend::PushLocalFrameF32: frame too large");
    }
    if (sample_count != 0 && pcm == nullptr) {
        return ACS_ERR(Generic, kSubVoiceBadArgument,
                       "FVoiceChatLoopbackBackend::PushLocalFrameF32: null pcm");
    }
    // float [-1,1] → int16 量子化してから int16 経路に合流。
    i16 staging[kVoiceMaxFrameSamples];
    for (u32 i = 0; i < sample_count; ++i) {
        f32 v = pcm[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        // 32767 倍で対称量子化 (-32768 は使わず歪みを避ける)。
        i32 q = static_cast<i32>(v * 32767.0f + (v >= 0.0f ? 0.5f : -0.5f));
        if (q >  32767) q =  32767;
        if (q < -32768) q = -32768;
        staging[i] = static_cast<i16>(q);
    }
    return PushLocalFrame(ch, staging, sample_count);
}

/** pump 対象の受信キューを decode・gain して N-way mix し out へ clamp 書き込む。 */
u32 FVoiceChatLoopbackBackend::PumpMixedOutput(EVoiceChannel ch, i16* out, u32 out_capacity) noexcept {
    if (!m_Initialized || out == nullptr || out_capacity == 0) return 0;
    FChannel& c = Chan(ch);
    if (!c.joined) return 0;
    if (c.pump_target >= c.participants.Size()) return 0;

    FLoopParticipant& target = c.participants[c.pump_target];
    const usize total = target.rx_frames.Size();
    if (total == 0) return 0;

    // mix アキュムレータ。out_capacity を超える分のフレームは consume せず残す。
    // i32 で総和し最後に i16 へ clamp する。
    // 単一参加者の受信キューには「複数送信元のフレームが時系列に連結」されており、
    // 各フレームは sample 位置 0 から重なる前提 (= 同一時刻の音をミックス) で扱う。
    // よって全フレームを sample index ごとに sum する。
    //
    // out バッファをまず 0 クリアし、フレームを 1 つずつ decode して
    //   out[i] += sample * target_gain
    // を i32 シャドウバッファ上で行い、最後に clamp して書き戻す。
    i32 accum[kVoiceMaxFrameSamples];
    for (u32 i = 0; i < out_capacity && i < kVoiceMaxFrameSamples; ++i) accum[i] = 0;
    const u32 cap = out_capacity < kVoiceMaxFrameSamples ? out_capacity : kVoiceMaxFrameSamples;

    u32 mixed_samples = 0;          // 実際に音を書いた最大サンプル index+1
    const f32 gain = target.muted_local ? 0.0f : target.volume;

    const u8* base = target.rx_frames.Data();
    usize off = 0;
    usize consumed = 0;             // 完全に mix し終えたバイト数 (キューから除去する量)
    i16 decoded[kVoiceMaxFrameSamples];

    while (off + sizeof(VoiceFrameHeader) <= total) {
        VoiceFrameHeader hdr;
        MemCopy(&hdr, base + off, sizeof(VoiceFrameHeader));
        if (hdr.magic != kVoiceFrameMagic) break;  // 破損したら以降を捨てる
        const usize frame_bytes = sizeof(VoiceFrameHeader)
                                + static_cast<usize>(hdr.sample_count) * sizeof(i16);
        if (off + frame_bytes > total) break;       // 途中までしか無い (理論上起きない)

        TResult<u32> dec = DecodeFrame(base + off, static_cast<u32>(frame_bytes),
                                       decoded, kVoiceMaxFrameSamples);
        if (dec.IsErr()) { break; }
        const u32 n = dec.Value();
        const u32 lim = n < cap ? n : cap;
        for (u32 i = 0; i < lim; ++i) {
            accum[i] += static_cast<i32>(static_cast<f32>(decoded[i]) * gain);
        }
        if (lim > mixed_samples) mixed_samples = lim;

        off      += frame_bytes;
        consumed  = off;
    }

    // clamp して out へ書き戻す。
    for (u32 i = 0; i < mixed_samples; ++i) {
        i32 s = accum[i];
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        out[i] = static_cast<i16>(s);
    }

    // 消費したフレームをキューから取り除く (残りを前方へ詰める)。
    if (consumed > 0) {
        const usize remain = total - consumed;
        if (remain > 0) {
            MemMove(target.rx_frames.Data(), base + consumed, remain);
        }
        target.rx_frames.Resize(remain);
    }

    return mixed_samples;
}

/** ループバック backend の Meyer's singleton を返す。 */
FVoiceChatLoopbackBackend& GetVoiceLoopback() noexcept {
    struct VoiceLoopbackState {
        VoiceLoopbackState() noexcept : backend(allocator)
        {
        }

        // backend を先に破棄してから allocator を破棄する宣言順にする。
        FSystemAllocator allocator;
        FVoiceChatLoopbackBackend backend;
    };
    static VoiceLoopbackState s_state;
    return s_state.backend;
}

} // namespace acs::game
