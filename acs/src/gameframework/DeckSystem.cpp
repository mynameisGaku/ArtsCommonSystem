// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット (Card CGame) — CDeckSystem 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・card_id は const char* per-byte 線形検索 (Inventory / Economy / FEntitlement と同設計)。
//     カード種別 (500〜2000 枚オーダー) は線形で十分。
//   ・deck の「トップ」は内部実装上「末尾」(Add / Pop で O(1) ドロー)。
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

/**
 * const char* の per-byte 安全比較。
 *
 * @details どちらかが nullptr なら false。両者を終端まで突き合わせて完全一致を判定する。
 * @param a 比較する文字列 A。
 * @param b 比較する文字列 B。
 * @return 内容が完全一致すれば true。
 */
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/** 「id 未発見」を表す哨兵値 (全 bit 1)。 */
constexpr u32 kNotFound = ~static_cast<u32>(0);

} // namespace

/** card_id を m_Cards から線形検索し、見つかった index を返す (未検出は kNotFound)。 */
u32 CDeckSystem::FindCardSlot(const char* card_id) const noexcept {
    if (card_id == nullptr) return kNotFound;
    const usize n = m_Cards.Num();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Cards[i].id, card_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

/** id 重複を弾いてカード定義を m_Cards に追加する。 */
void CDeckSystem::RegisterCard(const FCardDef& def) noexcept {
    // defensive: id == nullptr は意味を持たないので静かに弾く。
    if (def.id == nullptr) return;

    // 同 id の 2 重登録は no-op (アセット二重ロード保護)。
    if (FindCardSlot(def.id) != kNotFound) {
        ACS_LOG_WARN("CDeckSystem: duplicate card registration ignored ('%s')", def.id);
        return;
    }

    m_Cards.Add(def);
}

/** card_id に対応する FCardDef へのポインタを返す (未登録は nullptr)。 */
const FCardDef* CDeckSystem::FindCardDef(const char* card_id) const noexcept {
    const u32 slot = FindCardSlot(card_id);
    if (slot == kNotFound) return nullptr;
    return &m_Cards[slot];
}

/** 登録済み card_id の正規ポインタを deck 末尾に count 枚 push する。 */
void CDeckSystem::AddToDeck(const char* card_id, u32 count) noexcept {
    if (card_id == nullptr || count == 0) return;

    const u32 def_idx = FindCardSlot(card_id);
    if (def_idx == kNotFound) {
        // 未登録カードはデッキに入れない (アセットエラーを早期に拾う)。
        ACS_LOG_WARN("CDeckSystem: AddToDeck for unregistered card ('%s') ignored", card_id);
        return;
    }

    // ItemDef::id を直接参照 (= リテラル参照、非所有)。Inventory と同じパターン。
    const char* canon_id = m_Cards[def_idx].id;
    for (u32 i = 0; i < count; ++i) {
        m_Deck.Add(canon_id);
    }
}

/** 山札のみクリアする。 */
void CDeckSystem::ClearDeck() noexcept {
    m_Deck.Reset();
}

/** 山札の枚数を返す。 */
u32 CDeckSystem::DeckSize()    const noexcept { return static_cast<u32>(m_Deck.Num());    }

/** 手札の枚数を返す。 */
u32 CDeckSystem::HandSize()    const noexcept { return static_cast<u32>(m_Hand.Num());    }

/** 捨札の枚数を返す。 */
u32 CDeckSystem::DiscardSize() const noexcept { return static_cast<u32>(m_Discard.Num()); }

/** 除外ゾーンの枚数を返す。 */
u32 CDeckSystem::ExileSize()   const noexcept { return static_cast<u32>(m_Exile.Num());   }

/** seed に応じて Global / ローカル FRandom を選んでデッキを Fisher-Yates する。 */
void CDeckSystem::Shuffle(u32 seed) noexcept {
    // FRandom::Shuffle は Fisher-Yates (O(n))。0/1 枚のときは即帰る。
    if (m_Deck.Num() < 2) return;

    if (seed == 0u) {
        // 真にランダム = プロセス共有 Global を使う (時刻ベース seed)。
        FRandom::Global().Shuffle(m_Deck);
    } else {
        // 決定論再現 = ローカル instance を作って使う (Global を汚さない)。
        FRandom r(static_cast<u64>(seed));
        r.Shuffle(m_Deck);
    }
}

/** デッキ末尾を 1 枚引いて手札に追加する (空時は discard を reshuffle)。 */
bool CDeckSystem::Draw() noexcept {
    // デッキ空時は discard を shuffle して reuse。
    if (m_Deck.IsEmpty()) {
        if (m_Discard.IsEmpty()) return false;  // 両方とも空 → 引けない

        // discard → deck へ全部移動 (Add で順序は元のまま、直後の Shuffle で乱す)。
        const usize n = m_Discard.Num();
        m_Deck.Reserve(n);
        for (usize i = 0; i < n; ++i) {
            m_Deck.Add(m_Discard[i]);
        }
        m_Discard.Reset();

        // reshuffle。seed=0 = Global (時刻ベース)。
        if (m_Deck.Num() >= 2) {
            FRandom::Global().Shuffle(m_Deck);
        }
    }

    // ここまで来れば m_Deck.Size() >= 1 が保証される (上で空 chain を処理済)。
    // 「トップ = 末尾」約束に従って Pop で O(1) ドロー。
    const char* top = m_Deck.Last();
    m_Deck.Pop();
    m_Hand.Add(top);
    return true;
}

/** Draw を n 回繰り返す (途中で引けなくなったら false で抜ける)。 */
bool CDeckSystem::DrawN(u32 n) noexcept {
    if (n == 0u) return true;
    // 「引けただけ引く」設計: 途中で false になっても戻り値だけ false にして抜ける。
    for (u32 i = 0; i < n; ++i) {
        if (!Draw()) return false;
    }
    return true;
}

/** 手札 index のカードを前方シフトで取り出し discard へ移す。 */
bool CDeckSystem::DiscardFromHand(u32 hand_index) noexcept {
    if (hand_index >= static_cast<u32>(m_Hand.Num())) return false;
    const char* card = m_Hand[hand_index];

    // hand から削除 (順序保持のため前方シフト)。手札数は数枚〜十数枚オーダーで
    // O(n) シフトが UI 上自然 (= 引いた順序を維持する)。
    const usize n = m_Hand.Num();
    for (usize i = static_cast<usize>(hand_index); i + 1u < n; ++i) {
        m_Hand[i] = m_Hand[i + 1u];
    }
    m_Hand.Pop();

    m_Discard.Add(card);
    return true;
}

/** 手札 index のカードを前方シフトで取り出し exile へ移す。 */
bool CDeckSystem::ExileFromHand(u32 hand_index) noexcept {
    if (hand_index >= static_cast<u32>(m_Hand.Num())) return false;
    const char* card = m_Hand[hand_index];

    const usize n = m_Hand.Num();
    for (usize i = static_cast<usize>(hand_index); i + 1u < n; ++i) {
        m_Hand[i] = m_Hand[i + 1u];
    }
    m_Hand.Pop();

    m_Exile.Add(card);
    return true;
}

/** 手札を全部 discard へ移し、手札を空にする。 */
bool CDeckSystem::DiscardAllHand() noexcept {
    // 手札空でも true (= no-op として成功扱い、ターン終了処理を統一して書きやすくする)。
    const usize n = m_Hand.Num();
    if (n == 0u) return true;

    m_Discard.Reserve(m_Discard.Num() + n);
    for (usize i = 0; i < n; ++i) {
        m_Discard.Add(m_Hand[i]);
    }
    m_Hand.Reset();
    return true;
}

/** PlayCallback を発火させてから手札 index のカードを discard へ移す。 */
bool CDeckSystem::PlayCard(u32 hand_index) noexcept {
    if (hand_index >= static_cast<u32>(m_Hand.Num())) return false;

    // callback 発火 (登録されていれば)。hand_index は callback 呼び出し時点の値を渡す
    // (= callback 内で「どこから出した」が分かるようにする)。
    const char* card = m_Hand[hand_index];
    if (m_OnPlay != nullptr) {
        m_OnPlay(m_OnPlayUser, card, hand_index);
    }

    // callback 後に hand → discard へ移動。
    // 注意: callback 内で CDeckSystem を再帰的に触られた場合の防御は持たない
    // (= 呼出側の責務)。Inventory の ChangeCallback と同じ。
    const usize n = m_Hand.Num();
    // callback 内で hand が変動している可能性があるので、再度範囲チェック。
    if (hand_index >= static_cast<u32>(n)) return false;
    // card が callback 内で別物に差し替わっている可能性 (リサイズ等) は通常起きないが、
    // 念のため hand[hand_index] を読み直して discard へ送る。
    const char* card_now = m_Hand[hand_index];
    for (usize i = static_cast<usize>(hand_index); i + 1u < n; ++i) {
        m_Hand[i] = m_Hand[i + 1u];
    }
    m_Hand.Pop();
    m_Discard.Add(card_now);
    return true;
}

/** 手札 index のカード id を返す (範囲外は nullptr)。 */
const char* CDeckSystem::HandCardAt(u32 index) const noexcept {
    if (index >= static_cast<u32>(m_Hand.Num())) return nullptr;
    return m_Hand[index];
}

/** プレイ callback と user コンテキストを設定する (nullptr で detach)。 */
void CDeckSystem::SetOnPlayCallback(PlayCallback cb, void* user) noexcept {
    // nullptr で detach は明示的に許可。
    m_OnPlay      = cb;
    m_OnPlayUser = user;
}

/** カード定義・全ゾーン・callback をすべてクリアする。 */
void CDeckSystem::ClearAll() noexcept {
    m_Cards.Reset();
    m_Deck.Reset();
    m_Hand.Reset();
    m_Discard.Reset();
    m_Exile.Reset();
    m_OnPlay      = nullptr;
    m_OnPlayUser = nullptr;
}

} // namespace acs::game
