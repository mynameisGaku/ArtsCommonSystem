// SPDX-License-Identifier: Apache-2.0
#include "event/MessageBroker.h"
#include "memory/New.h"

#include <limits>

namespace acs {

namespace {

/**
 * 世代番号を一つ進め、最大値へ到達済みなら変更しない。
 * @param generation 最後に割り当てた世代番号。
 */
constexpr bool TryAdvanceGeneration(u32& generation) noexcept {
    if (generation == ~u32(0)) return false;
    ++generation;
    return true;
}

/** 最大世代を一度だけ割り当てた後は登録を永久に拒否することを検査する。 */
constexpr bool GenerationExhaustionIsPermanent() noexcept {
    /** 最大値の一つ前から始める検査用の番号。 */
    u32 generation = ~u32(0) - 1u;
    return TryAdvanceGeneration(generation) && generation == ~u32(0) && !TryAdvanceGeneration(generation) && generation == ~u32(0);
}

static_assert(GenerationExhaustionIsPermanent());

} // namespace

/** 全通路を解放する。 */
FMessageBroker::~FMessageBroker() noexcept {
    Clear();
}

/** 全通路の購読を無効化し、配信中の参照が指す領域は維持する。 */
void FMessageBroker::InvalidateAllChannels() noexcept {
    for (/** 無効にする通路の位置。 */ usize i = 0; i < m_Channels.Size(); ++i) {
        /** 購読を無効化する通路。 */
        FChannel* const channel = m_Channels[i];
        if (!channel) continue;
        for (/** 無効化する購読枠の位置。 */ usize slot_index = 0; slot_index < channel->slots.Size(); ++slot_index) {
            /** 無効化する購読情報。 */
            FSlot& slot = channel->slots[slot_index];
            slot.active = false;
            slot.pending_reuse = false;
            slot.cb = nullptr;
            slot.user = nullptr;
        }
        channel->free_indices.Clear();
        channel->active_count = 0;
    }
}

/** 全通路と保持領域を解放し、保留中の全解除を完了する。 */
void FMessageBroker::ReleaseChannels() noexcept {
    /** 通路の確保と解放に使う既存配列のメモリ管理器。 */
    FAllocator& allocator = *m_Channels.GetAllocator();
    for (/** 解放する通路の位置。 */ usize i = 0; i < m_Channels.Size(); ++i) {
        if (m_Channels[i]) Delete(allocator, m_Channels[i]);
    }
    m_Channels = TArray<FChannel*>{*m_Channels.GetAllocator()};
}

/** 全購読を直ちに無効化し、通路を安全な時点で解放する。 */
void FMessageBroker::Clear() noexcept {
    if (IsClearPending()) return;
    /** 全通路の配信状態を保持する制御領域。 */
    FChannel* const control = GetControlChannel();
    InvalidateAllChannels();
    if (control && control->broker_publish_depth != 0) {
        control->broker_clear_pending = true;
        return;
    }
    ReleaseChannels();
}

/** 制御領域を返し、未作成ならnullptrを返す。 */
FMessageBroker::FChannel* FMessageBroker::GetControlChannel() noexcept {
    return m_Channels.Size() != 0 ? m_Channels[0] : nullptr;
}

/** 配信終了まで全解除を保留しているかを返す。 */
bool FMessageBroker::IsClearPending() const noexcept {
    return m_Channels.Size() != 0 && m_Channels[0] && m_Channels[0]->broker_clear_pending;
}

/**
 * 指定した通路を取得し、必要なら作成する。
 * @param id 取得する通路番号。
 * @param create 存在しない通路を作成するか。
 */
FMessageBroker::FChannel* FMessageBroker::GetChannel(EventTypeId id, bool create) noexcept {
    if (IsClearPending() || !IsValidEventTypeId(id)) return nullptr;
    /** 通路番号と一致する配列位置。 */
    const usize storage_index = static_cast<usize>(id);
    if (storage_index >= m_Channels.Size()) {
        if (!create) return nullptr;
        /** 拡張前の通路数。 */
        const usize old_size = m_Channels.Size();
        if (!m_Channels.TryResize(storage_index + 1u)) return nullptr;
        for (/** 未初期化の通路位置。 */ usize i = old_size; i <= storage_index; ++i) m_Channels[i] = nullptr;
    }
    if (!m_Channels[0] && create) {
        /** 通路0と全通路の配信状態を保持する制御領域。 */
        FChannel* const control = New<FChannel>(*m_Channels.GetAllocator());
        if (!control) return nullptr;
        m_Channels[0] = control;
    }
    if (!m_Channels[storage_index] && create) {
        /** 既存のメモリ管理器で確保した新しい通路。 */
        FChannel* const channel = New<FChannel>(*m_Channels.GetAllocator());
        if (!channel) return nullptr;
        m_Channels[storage_index] = channel;
    }
    return m_Channels[storage_index];
}

/**
 * 再利用待ちの購読枠を空き一覧へ移す。
 * @param channel 整理する通路。
 */
void FMessageBroker::CollectReusableSlots(FChannel& channel) noexcept {
    if (channel.publish_depth != 0) return;
    for (/** 現在調べる購読枠の位置。 */ u32 index = 0; index < channel.slots.Size(); ++index) {
        /** 現在調べる購読情報。 */
        FSlot& slot = channel.slots[index];
        if (!slot.pending_reuse) continue;
        if (!channel.free_indices.TryPushBack(index)) return;
        slot.pending_reuse = false;
    }
}

/**
 * 型を消去した関数を指定した通路へ登録する。
 * @param channel 登録先の通路番号。
 * @param cb 配信時に呼び出す関数。
 * @param user 呼び出す関数へ渡す値。
 */
FSubscriptionHandle FMessageBroker::SubscribeRaw(EventTypeId channel, MessageCallback cb, void* user) noexcept {
    /** 今回の購読へ割り当てる世代番号。 */
    u32 generation = m_GenerationSeed;
    if (!cb || IsClearPending() || !TryAdvanceGeneration(generation)) return kInvalidSubscription;
    /** 登録先の通路。 */
    FChannel* ch = GetChannel(channel, true);
    if (!ch) return kInvalidSubscription;

    CollectReusableSlots(*ch);
    /** 登録先の購読枠の位置。 */
    u32 index = 0;
    if (ch->publish_depth == 0 && ch->free_indices.Size() > 0) {
        index = ch->free_indices[ch->free_indices.Size() - 1];
        ch->free_indices.PopBack();
    } else {
        if (ch->slots.Size() >= static_cast<usize>(std::numeric_limits<u32>::max())) return kInvalidSubscription;
        index = static_cast<u32>(ch->slots.Size());
        if (!ch->slots.TryPushBack(FSlot{})) return kInvalidSubscription;
    }

    /** 登録先の購読情報。 */
    FSlot& slot = ch->slots[index];
    if (slot.id == 0) slot.id = ch->next_id++;
    m_GenerationSeed = generation;
    slot.generation = generation;
    slot.active = true;
    slot.pending_reuse = false;
    slot.cb = cb;
    slot.user = user;
    ++ch->active_count;
    return FSubscriptionHandle{channel, slot.id, slot.generation};
}

/**
 * 指定した購読を解除する。
 * @param handle 解除する購読のハンドル。
 */
bool FMessageBroker::Unsubscribe(FSubscriptionHandle handle) noexcept {
    if (!handle.IsValid()) return false;
    /** 解除元の通路。 */
    FChannel* channel = GetChannel(handle.channel, false);
    if (!channel) return false;

    /** 購読番号から求めた購読枠の位置。 */
    const u32 index = handle.id - 1;
    if (index >= channel->slots.Size()) return false;
    /** 解除候補の購読情報。 */
    FSlot& slot = channel->slots[index];
    if (slot.id != handle.id || slot.generation != handle.generation || !slot.active) return false;

    slot.active = false;
    slot.cb = nullptr;
    slot.user = nullptr;
    --channel->active_count;
    slot.pending_reuse = channel->publish_depth != 0 || !channel->free_indices.TryPushBack(index);
    return true;
}

/**
 * 型を消去した値を指定した通路へ配信する。
 * @param channel 配信先の通路番号。
 * @param payload 配信する値。
 */
void FMessageBroker::PublishRaw(EventTypeId channel, const void* payload) noexcept {
    if (IsClearPending()) return;
    /** 配信先の通路。 */
    FChannel* target = GetChannel(channel, false);
    if (!target) return;
    /** 全通路の配信状態を保持する制御領域。 */
    FChannel* const control = GetControlChannel();
    if (!control) return;

    ++target->publish_depth;
    ++control->broker_publish_depth;
    /** 配信開始時点の購読枠数。 */
    const usize initial_count = target->slots.Size();
    for (/** 現在配信する購読枠の位置。 */ usize i = 0; i < initial_count; ++i) {
        if (control->broker_clear_pending) break;
        /** 現在配信する購読情報。 */
        FSlot& slot = target->slots[i];
        if (!slot.active || !slot.cb) continue;
        /** 今回呼び出す関数。 */
        const MessageCallback callback = slot.cb;
        /** 今回の関数へ渡す値。 */
        void* const user = slot.user;
        callback(payload, user);
    }
    --target->publish_depth;
    --control->broker_publish_depth;
    if (control->broker_clear_pending) {
        if (control->broker_publish_depth == 0) ReleaseChannels();
        return;
    }
    CollectReusableSlots(*target);
}

/**
 * 指定した通路の有効な購読数を返す。
 * @param channel 調べる通路番号。
 */
u32 FMessageBroker::SubscriberCount(EventTypeId channel) const noexcept {
    if (!IsValidEventTypeId(channel)) return 0;
    /** 通路番号と一致する配列位置。 */
    const usize storage_index = static_cast<usize>(channel);
    if (storage_index >= m_Channels.Size() || !m_Channels[storage_index]) return 0;
    return m_Channels[storage_index]->active_count;
}

} // namespace acs
