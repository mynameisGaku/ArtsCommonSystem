// SPDX-License-Identifier: Apache-2.0
// ContentModeratorStub のテキスト/画像モデレーションテスト
//
// テキスト照合は «検閲バイパスされないこと» が本質なので、素の表記だけでなく
// 回避変種 (大小文字混在 / leet 置換 / 記号・空白挿入) と、正規化バッファ切り捨てを
// 突いた長文パディングバイパスの回帰テストを重点的に置く。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/ContentModerator.h"
#include "container/Array.h"

using namespace acs;
using namespace acs::game;

namespace {

/** text の判定 verdict を返す (エラーは Allow 扱いにせずテスト失敗させたいので EXPECT 済み前提)。 */
EModerationVerdict VerdictOf(const char* text) noexcept
{
    CContentModeratorStub stub;
    const auto r = stub.ModerateText("test_user", text);
    if (r.IsErr()) return EModerationVerdict::Allow;
    return r.Value().verdict;
}

} // namespace

ACS_TEST(ContentModerator, AllowsBenignTextAndRejectsPlainProfanity)
{
    // 無害な文は Allow。
    EXPECT_TRUE(VerdictOf("hello world, nice to meet you") == EModerationVerdict::Allow);
    EXPECT_TRUE(VerdictOf("") == EModerationVerdict::Allow);
    EXPECT_TRUE(VerdictOf(nullptr) == EModerationVerdict::Allow);

    // 素の NG ワードは Block (大小文字非感受)。
    EXPECT_TRUE(VerdictOf("what the fuck") == EModerationVerdict::Block);
    EXPECT_TRUE(VerdictOf("What The FUCK") == EModerationVerdict::Block);
}

ACS_TEST(ContentModerator, CatchesLeetAndSeparatorVariants)
{
    // leet 置換 (5→s, 1→i) と記号/空白挿入の変種を正規化経路で捕捉する。
    EXPECT_TRUE(VerdictOf("sh1t happens") == EModerationVerdict::Block);
    EXPECT_TRUE(VerdictOf("f u c k") == EModerationVerdict::Block);
    EXPECT_TRUE(VerdictOf("f.u.c.k") == EModerationVerdict::Block);
    EXPECT_TRUE(VerdictOf("b1tch") == EModerationVerdict::Block);

    // 正規化で偽陽性が出ない (区切り除去で偶然つながる無害語のケアは辞書側の責務だが、
    // 代表的な無害文が素通りすることは確認しておく)。
    EXPECT_TRUE(VerdictOf("classic assessment") == EModerationVerdict::Allow);
}

ACS_TEST(ContentModerator, HardcodedBlockIsNotBypassableByLongPadding)
{
    // 回帰テスト: 旧実装は入力を 512 文字の固定バッファへ正規化してから照合していた
    // ため、英数字 512 文字を超えた位置に leet/spaced 変種を置くと «正規化経路が
    // 切り捨てられて» 素通りした (生経路は変種を捕捉できない)。ストリーミング照合に
    // 変えた現実装では、位置によらず捕捉できることを検証する。
    TArray<char> long_text;
    for (u32 i = 0; i < 600u; ++i) long_text.Add('a');   // 正規化 600 文字分のパディング
    const char* variant = "c.h.i.l.d p.o.r.n";                 // ハードコード辞書の spaced 変種
    for (const char* p = variant; *p != '\0'; ++p) long_text.Add(*p);
    long_text.Add('\0');

    CContentModeratorStub stub;
    const auto r = stub.ModerateText("test_user", long_text.GetData());
    EXPECT_TRUE(r.IsOk());
    if (r.IsOk()) {
        EXPECT_TRUE(r.Value().verdict == EModerationVerdict::Block);
        EXPECT_TRUE(r.Value().rating == EContentRating::SexualMinor_HardcodedBlock);
    }

    // 一般 NG ワードも同様に長文パディングで素通りしない。
    TArray<char> long_text2;
    for (u32 i = 0; i < 600u; ++i) long_text2.Add('x');
    const char* variant2 = "k y s";
    for (const char* p = variant2; *p != '\0'; ++p) long_text2.Add(*p);
    long_text2.Add('\0');
    EXPECT_TRUE(VerdictOf(long_text2.GetData()) == EModerationVerdict::Block);
}

ACS_TEST(ContentModerator, UserNameUsesSameDictionary)
{
    CContentModeratorStub stub;
    const auto bad = stub.ModerateUserName("xX_sh1t_Xx");
    EXPECT_TRUE(bad.IsOk());
    if (bad.IsOk()) EXPECT_TRUE(bad.Value().verdict == EModerationVerdict::Block);

    const auto ok = stub.ModerateUserName("PlayerOne");
    EXPECT_TRUE(ok.IsOk());
    if (ok.IsOk()) EXPECT_TRUE(ok.Value().verdict == EModerationVerdict::Allow);
}

ACS_TEST(ContentModerator, ImageRgba8SkinRatioThresholds)
{
    CContentModeratorStub stub;

    // 全ピクセル肌色 (R>95,G>40,B>20, R>G>B, spread>15) → ratio 1.0 → Block/Explicit。
    constexpr u32 kW = 8, kH = 8;
    u8 skin[kW * kH * 4];
    for (u32 i = 0; i < kW * kH; ++i) {
        skin[i * 4 + 0] = 200; skin[i * 4 + 1] = 140;
        skin[i * 4 + 2] = 110; skin[i * 4 + 3] = 255;
    }
    const auto blocked = stub.ModerateImageRgba8(skin, kW, kH);
    EXPECT_TRUE(blocked.IsOk());
    if (blocked.IsOk()) {
        EXPECT_TRUE(blocked.Value().verdict == EModerationVerdict::Block);
        EXPECT_TRUE(blocked.Value().rating == EContentRating::Explicit);
    }

    // 全ピクセル青 → ratio 0 → Allow/Safe。
    u8 blue[kW * kH * 4];
    for (u32 i = 0; i < kW * kH; ++i) {
        blue[i * 4 + 0] = 20; blue[i * 4 + 1] = 40;
        blue[i * 4 + 2] = 200; blue[i * 4 + 3] = 255;
    }
    const auto allowed = stub.ModerateImageRgba8(blue, kW, kH);
    EXPECT_TRUE(allowed.IsOk());
    if (allowed.IsOk()) EXPECT_TRUE(allowed.Value().verdict == EModerationVerdict::Allow);

    // 透明ピクセルは母数から除外される: 肌色 1px + 透明 63px → ratio 1.0 → Block。
    u8 mostly_transparent[kW * kH * 4] = {};
    mostly_transparent[0] = 200; mostly_transparent[1] = 140;
    mostly_transparent[2] = 110; mostly_transparent[3] = 255;
    const auto sparse = stub.ModerateImageRgba8(mostly_transparent, kW, kH);
    EXPECT_TRUE(sparse.IsOk());
    if (sparse.IsOk()) EXPECT_TRUE(sparse.Value().verdict == EModerationVerdict::Block);

    // 不正引数は BadArgument エラー。
    EXPECT_TRUE(stub.ModerateImageRgba8(nullptr, kW, kH).IsErr());
    EXPECT_TRUE(stub.ModerateImageRgba8(skin, 0, kH).IsErr());
}
