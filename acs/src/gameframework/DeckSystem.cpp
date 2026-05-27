// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット (Card FGame) — FDeckSystem 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・card_id は const char* per-byte 線形検索 (Inventory / Economy / FEntitlement と同設計)。
//     カード種別 (500〜2000 枚オーダー) は線形で十分。
//   ・deck の「トップ」は内部実装上「末尾」(PushBack / PopBack で O(1) ドロー)。
//     Shuffle 後の順序がランダムなので、末尾/先頭のどちらをトップと呼ぶかは UX に影響しない。
//   ・自動 reshuffle: Draw / DrawN がデッキ空時に discard を shuffle して deck に戻す。
//     これにより Slay the Spire 系のデッキ循環モデルがそのまま実装される。
//   ・PlayCard は「callback → discard」順。callback 側で「コストを払う」「効果を起動」を
//     完了させてから戻る前提 (= 失敗時は callback 側で巻き戻し)。Manager 側は遷移を保証するだけ。
//   ・WARN は Inventory / Economy と同じ `Log.h` 経由。
#include "gameframework/DeckSystem.h"

#include "foundation/Log.h"
#include "gameframework/Random.h"

namespace acs::game {

namespace {

// const char* の per-byte 安全比較。Inventory / Economy / FCharacterCustomizer と同設計。
// どちらかが nullptr なら false。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// 「id 未発見」を表す哨兵値 (Inventory / Economy と同設計)。
constexpr u32 kNotFound = ~static_cast<u32>(0);

} // namespace

// =============================================================================
// 内部ユーティリティ
// =============================================================================

u32 FDeckSystem::FindCardSlot(const char* card_id) const noexcept {
    if (card_id == nullptr) return kNotFound;
    const usize n = _cards.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_cards[i].id, card_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

// =============================================================================
// カード定義
// =============================================================================

void FDeckSystem::RegisterCard(const FCardDef& def) noexcept {
    // defensive: id == nullptr は意味を持たないので静かに弾く。
    if (def.id == nullptr) return;

    // 同 id の 2 重登録は no-op (アセット二重ロード保護)。
    if (FindCardSlot(def.id) != kNotFound) {
        ACS_LOG_WARN("FDeckSystem: duplicate card registration ignored ('%s')", def.id);
        return;
    }

    _cards.PushBack(def);
}

const FCardDef* FDeckSystem::FindCardDef(const char* card_id) const noexcept {
    const u32 slot = FindCardSlot(card_id);
    if (slot == kNotFound) return nullptr;
    return &_cards[slot];
}

// =============================================================================
// デッキ構築
// =============================================================================

void FDeckSystem::AddToDeck(const char* card_id, u32 count) noexcept {
    if (card_id == nullptr || count == 0) return;

    const u32 def_idx = FindCardSlot(card_id);
    if (def_idx == kNotFound) {
        // 未登録カードはデッキに入れない (アセットエラーを早期に拾う)。
        ACS_LOG_WARN("FDeckSystem: AddToDeck for unregistered card ('%s') ignored", card_id);
        return;
    }

    // ItemDef::id を直接参照 (= リテラル参照、非所有)。Inventory と同じパターン。
    const char* canon_id = _cards[def_idx].id;
    for (u32 i = 0; i < count; ++i) {
        _deck.PushBack(canon_id);
    }
}

void FDeckSystem::ClearDeck() noexcept {
    _deck.Clear();
}

// =============================================================================
// ゾーンサイズ照会
// =============================================================================

u32 FDeckSystem::DeckSize()    const noexcept { return static_cast<u32>(_deck.Size());    }
u32 FDeckSystem::HandSize()    const noexcept { return static_cast<u32>(_hand.Size());    }
u32 FDeckSystem::DiscardSize() const noexcept { return static_cast<u32>(_discard.Size()); }
u32 FDeckSystem::ExileSize()   const noexcept { return static_cast<u32>(_exile.Size());   }

// =============================================================================
// シャッフル
// =============================================================================

void FDeckSystem::Shuffle(u32 seed) noexcept {
    // FRandom::Shuffle は Fisher-Yates (O(n))。0/1 枚のときは即帰る。
    if (_deck.Size() < 2) return;

    if (seed == 0u) {
        // 真にランダム = プロセス共有 Global を使う (時刻ベース seed)。
        FRandom::Global().Shuffle(_deck);
    } else {
        // 決定論再現 = ローカル instance を作って使う (Global を汚さない)。
        FRandom r(static_cast<u64>(seed));
        r.Shuffle(_deck);
    }
}

// =============================================================================
// ドロー
// =============================================================================

bool FDeckSystem::Draw() noexcept {
    // デッキ空時は discard を shuffle して reuse。
    if (_deck.IsEmpty()) {
        if (_discard.IsEmpty()) return false;  // 両方とも空 → 引けない

        // discard → deck へ全部移動 (PushBack で順序は元のまま、直後の Shuffle で乱す)。
        const usize n = _discard.Size();
        _deck.Reserve(n);
        for (usize i = 0; i < n; ++i) {
            _deck.PushBack(_discard[i]);
        }
        _discard.Clear();

        // reshuffle。seed=0 = Global (時刻ベース)。
        if (_deck.Size() >= 2) {
            FRandom::Global().Shuffle(_deck);
        }
    }

    // ここまで来れば _deck.Size() >= 1 が保証される (上で空 chain を処理済)。
    // 「トップ = 末尾」約束に従って PopBack で O(1) ドロー。
    const char* top = _deck.Back();
    _deck.PopBack();
    _hand.PushBack(top);
    return true;
}

bool FDeckSystem::DrawN(u32 n) noexcept {
    if (n == 0u) return true;
    // 「引けただけ引く」設計: 途中で false になっても戻り値だけ false にして抜ける。
    for (u32 i = 0; i < n; ++i) {
        if (!Draw()) return false;
    }
    return true;
}

// =============================================================================
// ゾーン操作
// =============================================================================

bool FDeckSystem::DiscardFromHand(u32 hand_index) noexcept {
    if (hand_index >= static_cast<u32>(_hand.Size())) return false;
    const char* card = _hand[hand_index];

    // hand から削除 (順序保持のため前方シフト)。手札数は数枚〜十数枚オーダーで
    // O(n) シフトが UI 上自然 (= 引いた順序を維持する)。
    const usize n = _hand.Size();
    for (usize i = static_cast<usize>(hand_index); i + 1u < n; ++i) {
        _hand[i] = _hand[i + 1u];
    }
    _hand.PopBack();

    _discard.PushBack(card);
    return true;
}

bool FDeckSystem::ExileFromHand(u32 hand_index) noexcept {
    if (hand_index >= static_cast<u32>(_hand.Size())) return false;
    const char* card = _hand[hand_index];

    const usize n = _hand.Size();
    for (usize i = static_cast<usize>(hand_index); i + 1u < n; ++i) {
        _hand[i] = _hand[i + 1u];
    }
    _hand.PopBack();

    _exile.PushBack(card);
    return true;
}

bool FDeckSystem::DiscardAllHand() noexcept {
    // 手札空でも true (= no-op として成功扱い、ターン終了処理を統一して書きやすくする)。
    const usize n = _hand.Size();
    if (n == 0u) return true;

    _discard.Reserve(_discard.Size() + n);
    for (usize i = 0; i < n; ++i) {
        _discard.PushBack(_hand[i]);
    }
    _hand.Clear();
    return true;
}

bool FDeckSystem::PlayCard(u32 hand_index) noexcept {
    if (hand_index >= static_cast<u32>(_hand.Size())) return false;

    // callback 発火 (登録されていれば)。hand_index は callback 呼び出し時点の値を渡す
    // (= callback 内で「どこから出した」が分かるようにする)。
    const char* card = _hand[hand_index];
    if (_on_play != nullptr) {
        _on_play(_on_play_user, card, hand_index);
    }

    // callback 後に hand → discard へ移動。
    // 注意: callback 内で FDeckSystem を再帰的に触られた場合の防御は持たない
    // (= 呼出側の責務)。Inventory の ChangeCallback と同じ。
    const usize n = _hand.Size();
    // callback 内で hand が変動している可能性があるので、再度範囲チェック。
    if (hand_index >= static_cast<u32>(n)) return false;
    // card が callback 内で別物に差し替わっている可能性 (リサイズ等) は通常起きないが、
    // 念のため hand[hand_index] を読み直して discard へ送る。
    const char* card_now = _hand[hand_index];
    for (usize i = static_cast<usize>(hand_index); i + 1u < n; ++i) {
        _hand[i] = _hand[i + 1u];
    }
    _hand.PopBack();
    _discard.PushBack(card_now);
    return true;
}

// =============================================================================
// 手札照会
// =============================================================================

const char* FDeckSystem::HandCardAt(u32 index) const noexcept {
    if (index >= static_cast<u32>(_hand.Size())) return nullptr;
    return _hand[index];
}

// =============================================================================
// コールバック
// =============================================================================

void FDeckSystem::SetOnPlayCallback(PlayCallback cb, void* user) noexcept {
    // nullptr で detach は明示的に許可。
    _on_play      = cb;
    _on_play_user = user;
}

// =============================================================================
// 全リセット
// =============================================================================

void FDeckSystem::ClearAll() noexcept {
    _cards.Clear();
    _deck.Clear();
    _hand.Clear();
    _discard.Clear();
    _exile.Clear();
    _on_play      = nullptr;
    _on_play_user = nullptr;
}

} // namespace acs::game
