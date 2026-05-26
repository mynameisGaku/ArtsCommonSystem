// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム — DialogueLocalizer 実装
//
// 責務:
//   ・行 / 選択肢メタ (key のみ) の蓄積
//   ・StartFromLine で LocalizationDirector::Get を 2 回 (speaker / text) 叩いて
//     callback に流すだけのシンプルな bridge
//
// 状態は持たない (進行状態は DialogueSystem 側で管理)。
// 線形検索コストは典型シナリオ N < 数百で十分小さく、THashMap 化は不要。
//
// 設計上の留意:
//   ・LocalizationDirector::Get は key == nullptr 時に空文字 "" を返す契約。
//     未翻訳時は key 自身を返すので、cb に nullptr が渡ることは原理的に無い。
//     ただし _localizer == nullptr のときは LocalizationDirector を呼べないので、
//     その場合に「key 自身 (または空文字)」を渡す挙動をここで自前で再現する。
//   ・speaker_id が nullptr (ナレーション行) のときは空文字 "" を speaker に渡す。
//     line_key が nullptr (空行) のときも空文字 "" を text に渡す。
//     どちらも callback 側で null チェック不要にする契約。
#include "gameframework/DialogueLocalizer.h"

#include "gameframework/LocalizationDirector.h"

namespace acs::game {

namespace {

// LocalizationDirector が未接続のときに「key 自身 or 空文字」を返す helper。
// LocalizationDirector::Get の契約 (key==nullptr→"" / 未翻訳→key) と同じ挙動を
// 自前で再現することで、_localizer の有無に関わらず callback に渡る値の
// nullptr-safe 性を保証する。
const char* ResolveWithoutLocalizer(const char* key) noexcept {
    if (key == nullptr) return "";
    return key;
}

// _localizer 有無を一元化して key→text 解決。
// 戻り値は常に非 nullptr (空文字最小)。
const char* ResolveKey(const LocalizationDirector* loc, const char* key) noexcept {
    if (loc == nullptr) {
        return ResolveWithoutLocalizer(key);
    }
    if (key == nullptr) return "";
    // LocalizationDirector::Get は未翻訳時に key 自身、key==nullptr で "" を返す。
    // const_cast 不要 (Get は const メンバ)。
    return loc->Get(key);
}

} // namespace

void DialogueLocalizer::RegisterLine(const LocalizedDialogueLine& line) noexcept {
    // speaker_id / line_key が nullptr でも蓄積は許す (= ナレーション行 / 空行を
    // 表現可能)。StartFromLine 側で nullptr→"" 変換するので落ちない。
    _lines.PushBack(line);
}

void DialogueLocalizer::RegisterChoice(u32 at_line_index,
                                       const LocalizedDialogueChoice* choices, u32 count) noexcept {
    // DialogueSystem::AddChoices と同契約: 不正引数は no-op、上書き禁止。
    if (choices == nullptr || count == 0u) return;
    if (at_line_index >= static_cast<u32>(_lines.Size())) return;

    // 同 line への重複登録は無視 (= 最初の登録のみ有効)
    for (usize i = 0; i < _choices_at.Size(); ++i) {
        if (_choices_at[i].line_index == at_line_index) return;
    }

    ChoicesAt rec;
    rec.line_index   = at_line_index;
    rec.choice_start = static_cast<u32>(_all_choices.Size());
    rec.choice_count = count;
    for (u32 i = 0; i < count; ++i) {
        // choices[i] を値コピーで保持 (const char* メンバの寿命は呼び出し側保証、
        // 文字列バッファそのものは複製しない)
        _all_choices.PushBack(choices[i]);
    }
    _choices_at.PushBack(rec);
}

void DialogueLocalizer::SetLocalizer(LocalizationDirector* loc) noexcept {
    // nullptr で detach (= 以降 ResolveKey は key 自身 or 空文字を返す)。
    // 寿命は呼び出し側保証 (LocalizationDirector は通常セッション寿命なので
    // dangling になるのは Director 側のリセット忘れの場合のみ)。
    _localizer = loc;
}

void DialogueLocalizer::StartFromLine(u32 line_index, BindCallback cb, void* user) noexcept {
    // cb が無ければ呼ぶ相手が居ないので何もしない (= 副作用なし契約)。
    if (cb == nullptr) return;
    // 範囲外 index は no-op (= DialogueChoice::next_line_index で UINT32_MAX 等が
    // 渡されて終了扱いになるケースをここで吸収)。
    if (line_index >= static_cast<u32>(_lines.Size())) return;

    const LocalizedDialogueLine& line = _lines[line_index];

    const char* speaker = ResolveKey(_localizer, line.speaker_id);
    const char* text    = ResolveKey(_localizer, line.line_key);

    // ResolveKey の契約上、speaker / text は常に非 nullptr。
    // cb 側で nullptr チェック不要。
    cb(user, speaker, text, line.type_speed_cps);
}

u32 DialogueLocalizer::LineCount() const noexcept {
    return static_cast<u32>(_lines.Size());
}

void DialogueLocalizer::Clear() noexcept {
    // 蓄積データを全破棄。Localizer 参照は維持する
    // (= 同 Localizer で別シナリオを再構築する想定が大半)。
    _lines.Clear();
    _choices_at.Clear();
    _all_choices.Clear();
}

} // namespace acs::game
