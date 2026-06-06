// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS AssetPack — FAcpakCrypto (AES-256-GCM / PBKDF2 / nonce)
// ラウンドトリップ・改竄検出・鍵導出の決定性を検証する (Windows CNG, ヘッドレス可)。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "assetpack/AcpakCrypto.h"

using namespace acs;
using namespace acs::assetpack;

namespace {

/** テスト用の決定的な 32 バイト鍵 (本番は DeriveKey / CSPRNG)。 */
FAcpakKey MakeKey(u8 seed) noexcept {
    FAcpakKey k{};
    for (u32 i = 0; i < 32; ++i) k.bytes[i] = static_cast<u8>(seed + i);
    return k;
}

/** n バイトのバイト列が一致するか。 */
bool BytesEq(const u8* a, const u8* b, u64 n) noexcept {
    for (u64 i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
}

} // namespace

// ---- 暗号化 → 復号 の往復で平文が復元される -------------------------------
ACS_TEST(AcpakCrypto, EncryptDecryptRoundTrip) {
    const FAcpakKey key = MakeKey(1);
    const u8 nonce[kAcpakNonceSize] = {1,2,3,4,5,6,7,8,9,10,11,12};
    const u8 plain[] = {'A','C','S',' ','s','e','c','r','e','t','!',0,9,8,7};
    const u64 n = sizeof(plain);
    u8 cipher[sizeof(plain)] = {};
    u8 tag[kAcpakTagSize] = {};

    EXPECT_TRUE(FAcpakCrypto::Encrypt(key, nonce, plain, n, cipher, tag).IsOk());
    EXPECT_FALSE(BytesEq(cipher, plain, n));   // 暗号文 ≠ 平文

    u8 out[sizeof(plain)] = {};
    EXPECT_TRUE(FAcpakCrypto::Decrypt(key, nonce, tag, cipher, n, out).IsOk());
    EXPECT_TRUE(BytesEq(out, plain, n));       // 平文を復元
}

// ---- 改竄 (暗号文 / タグ / 鍵) は必ずタグ検証で弾く -----------------------
ACS_TEST(AcpakCrypto, TamperDetected) {
    const FAcpakKey key = MakeKey(2);
    const u8 nonce[kAcpakNonceSize] = {9,9,9,9,9,9,9,9,9,9,9,9};
    const u8 plain[] = {1,2,3,4,5,6,7,8};
    const u64 n = sizeof(plain);
    u8 cipher[sizeof(plain)] = {};
    u8 tag[kAcpakTagSize] = {};
    EXPECT_TRUE(FAcpakCrypto::Encrypt(key, nonce, plain, n, cipher, tag).IsOk());

    u8 out[sizeof(plain)] = {};

    // 暗号文 1 バイト改竄 → 失敗
    u8 bad_cipher[sizeof(plain)];
    for (u64 i = 0; i < n; ++i) bad_cipher[i] = cipher[i];
    bad_cipher[0] ^= 0xFF;
    EXPECT_TRUE(FAcpakCrypto::Decrypt(key, nonce, tag, bad_cipher, n, out).IsErr());

    // タグ改竄 → 失敗
    u8 bad_tag[kAcpakTagSize];
    for (u32 i = 0; i < kAcpakTagSize; ++i) bad_tag[i] = tag[i];
    bad_tag[0] ^= 0xFF;
    EXPECT_TRUE(FAcpakCrypto::Decrypt(key, nonce, bad_tag, cipher, n, out).IsErr());

    // 鍵違い → 失敗
    EXPECT_TRUE(FAcpakCrypto::Decrypt(MakeKey(99), nonce, tag, cipher, n, out).IsErr());
}

// ---- PBKDF2 鍵導出は決定的、salt が違えば別鍵 -----------------------------
ACS_TEST(AcpakCrypto, DeriveKeyDeterministic) {
    const u8 pw[]   = {'h','u','n','t','e','r','2'};
    const u8 salt[] = {'s','a','l','t'};
    auto k1 = FAcpakCrypto::DeriveKey(pw, sizeof(pw), salt, sizeof(salt));
    auto k2 = FAcpakCrypto::DeriveKey(pw, sizeof(pw), salt, sizeof(salt));
    EXPECT_TRUE(k1.IsOk());
    EXPECT_TRUE(k2.IsOk());
    EXPECT_TRUE(BytesEq(k1.Value().bytes, k2.Value().bytes, 32));   // 同入力 → 同鍵

    const u8 salt2[] = {'p','e','p','p','e','r'};
    auto k3 = FAcpakCrypto::DeriveKey(pw, sizeof(pw), salt2, sizeof(salt2));
    EXPECT_TRUE(k3.IsOk());
    EXPECT_FALSE(BytesEq(k1.Value().bytes, k3.Value().bytes, 32));  // salt 違い → 別鍵
}

// ---- CSPRNG nonce は毎回異なる (再利用ゼロ) -------------------------------
ACS_TEST(AcpakCrypto, RandomNonceUnique) {
    u8 a[kAcpakNonceSize] = {};
    u8 b[kAcpakNonceSize] = {};
    EXPECT_TRUE(FAcpakCrypto::GenerateRandomNonce(a).IsOk());
    EXPECT_TRUE(FAcpakCrypto::GenerateRandomNonce(b).IsOk());
    EXPECT_FALSE(BytesEq(a, b, kAcpakNonceSize));  // 衝突確率 2^-96 → 実質ゼロ
}
