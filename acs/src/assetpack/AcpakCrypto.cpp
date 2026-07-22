// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS AssetPack — FAcpakCrypto 実装 (Windows CNG / BCrypt)
// -----------------------------------------------------------------------------
// Win32 BCrypt API ラッパ。各 API 呼び出しでアルゴリズムプロバイダを開いて
// 閉じるシンプルな実装。キャッシュ無し (起動時 + ストリーミングが
// 主用途で、ハンドル生成コストは I/O に比べて誤差)。
//
// 暗号化フロー:
//   BCryptOpenAlgorithmProvider(BCRYPT_AES_ALGORITHM)
//     → BCryptSetProperty(BCRYPT_CHAINING_MODE, BCRYPT_CHAIN_MODE_GCM)
//     → BCryptGenerateSymmetricKey(key.bytes, 32)
//     → BCryptEncrypt( ..., BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO { nonce, tag } )
//     → BCryptDestroyKey / BCryptCloseAlgorithmProvider
//
// 復号フロー:
//   同じ手順 + BCryptDecrypt が STATUS_AUTH_TAG_MISMATCH を返したら
//   kAcpakSubCryptoTag に変換 (= 改竄 / 破損として呼び出し側に通知)。
//
// 注意:
//   ・BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO 構造体は ZeroMemory + cbSize 必須。
//   ・GCM の cbAuthData == 0 で AAD なし。AAD (path_hash 等) を渡すと
//     cut-and-paste 攻撃対策になるが、シンプル化のため AAD なし版を提供する。
//   ・BCryptEncrypt の cbInput == 0 のケースは GCM の GMAC 動作 (タグ計算のみ)。
//     これは Win10 以降で正しく動作する。Win7 は ACS のターゲット外。
// =============================================================================
#include "assetpack/AcpakCrypto.h"

#include "assetpack/AcpakFormat.h"   // kAcpakSub* (1301-1313)
#include "foundation/Platform.h"     // <windows.h>
#include "memory/Memory.h"           // MemSet / MemCopy

#include <bcrypt.h>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_AUTH_TAG_MISMATCH
#define STATUS_AUTH_TAG_MISMATCH        ((NTSTATUS)0xC000A002L)
#endif

namespace acs::assetpack {

namespace {

/**
 * AES-GCM 用に algorithm provider を開き chain mode を設定する。
 *
 * @details 成功時 *hAlg にハンドルを返し、失敗時は内部で閉じて nullptr にする。
 * @param hAlg provider ハンドルの出力先。
 * @return CNG の NTSTATUS (STATUS_SUCCESS で成功)。
 */
NTSTATUS OpenAesGcmProvider(BCRYPT_ALG_HANDLE* hAlg) noexcept {
    *hAlg = nullptr;
    NTSTATUS s = ::BCryptOpenAlgorithmProvider(hAlg,
                                               BCRYPT_AES_ALGORITHM,
                                               nullptr, 0);
    if (s != STATUS_SUCCESS) return s;

    s = ::BCryptSetProperty(*hAlg,
                            BCRYPT_CHAINING_MODE,
                            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                            sizeof(BCRYPT_CHAIN_MODE_GCM),
                            0);
    if (s != STATUS_SUCCESS) {
        ::BCryptCloseAlgorithmProvider(*hAlg, 0);
        *hAlg = nullptr;
    }
    return s;
}

/**
 * algorithm provider から AES-256 symmetric key ハンドルを作る。
 *
 * @details 鍵バイトは内部で CNG にコピーされるため、戻り直後に呼び出し側で
 * SecureZero しても問題ない。
 * @param hAlg AES-GCM provider ハンドル。
 * @param key 256bit の AES 鍵。
 * @param hKey 生成した key ハンドルの出力先。
 * @return CNG の NTSTATUS (STATUS_SUCCESS で成功)。
 */
NTSTATUS CreateAesKey(BCRYPT_ALG_HANDLE  hAlg,
                      const FAcpakKey&    key,
                      BCRYPT_KEY_HANDLE* hKey) noexcept {
    *hKey = nullptr;
    return ::BCryptGenerateSymmetricKey(hAlg,
                                        hKey,
                                        nullptr, 0,
                                        const_cast<PUCHAR>(key.bytes),
                                        static_cast<ULONG>(sizeof(key.bytes)),
                                        0);
}

} // namespace

TResult<FAcpakKey> FAcpakCrypto::DeriveKey(const u8* password,
                                        u32       password_len,
                                        const u8* salt,
                                        u32       salt_len) noexcept {
    // password / salt nullptr は空バイト列扱い (CNG は許容する)。
    if (password == nullptr) password_len = 0;
    if (salt     == nullptr) salt_len     = 0;

    FAcpakKey out{};

    // ---- SHA-256 algorithm provider を HMAC モードで開く ------------------
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS s = ::BCryptOpenAlgorithmProvider(&hAlg,
                                               BCRYPT_SHA256_ALGORITHM,
                                               nullptr,
                                               BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (s != STATUS_SUCCESS) {
        return ACS_ERR_OS(Asset, kAcpakSubCryptoInit,
                          "FAcpakCrypto::DeriveKey: BCryptOpenAlgorithmProvider failed",
                          static_cast<u32>(s));
    }

    // ---- PBKDF2: password + salt + 10000 iter → 32 バイト ---------------
    constexpr ULONGLONG kIterations = 10000ULL;
    s = ::BCryptDeriveKeyPBKDF2(hAlg,
                                const_cast<PUCHAR>(password),
                                static_cast<ULONG>(password_len),
                                const_cast<PUCHAR>(salt),
                                static_cast<ULONG>(salt_len),
                                kIterations,
                                out.bytes,
                                static_cast<ULONG>(sizeof(out.bytes)),
                                0);
    ::BCryptCloseAlgorithmProvider(hAlg, 0);

    if (s != STATUS_SUCCESS) {
        // 鍵領域は失敗時にゴミが残るので念のため 0 クリア (defensive)。
        MemSet(out.bytes, 0, sizeof(out.bytes));
        return ACS_ERR_OS(Asset, kAcpakSubCryptoKdf,
                          "FAcpakCrypto::DeriveKey: BCryptDeriveKeyPBKDF2 failed",
                          static_cast<u32>(s));
    }
    return TResult<FAcpakKey>(OkInit, out);
}

TResult<void> FAcpakCrypto::Encrypt(const FAcpakKey& key,
                                  const u8        nonce[kAcpakNonceSize],
                                  const u8*       plaintext,
                                  u64             size,
                                  u8*             ciphertext,
                                  u8              tag_out[kAcpakTagSize]) noexcept {
    // size == 0 は GMAC 専用パス (タグだけ計算)。plaintext は nullptr 可。
    if (size > 0 && (plaintext == nullptr || ciphertext == nullptr)) {
        return ACS_ERR(Asset, kAcpakSubCryptoOp,
                       "FAcpakCrypto::Encrypt: null buffer with non-zero size");
    }
    if (nonce == nullptr || tag_out == nullptr) {
        return ACS_ERR(Asset, kAcpakSubCryptoOp,
                       "FAcpakCrypto::Encrypt: nonce/tag pointer is null");
    }
    // BCryptEncrypt の cbInput は ULONG (32bit)。single-chunk とし、
    // >4GiB の単一エントリは未対応 (実用上 .acpak のエントリは数 MB 程度)。
    if (size > 0xFFFFFFFFu) {
        return ACS_ERR(Asset, kAcpakSubCryptoOp,
                       "FAcpakCrypto::Encrypt: size > 4GiB not supported");
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS s = OpenAesGcmProvider(&hAlg);
    if (s != STATUS_SUCCESS) {
        return ACS_ERR_OS(Asset, kAcpakSubCryptoInit,
                          "FAcpakCrypto::Encrypt: provider init failed",
                          static_cast<u32>(s));
    }

    BCRYPT_KEY_HANDLE hKey = nullptr;
    s = CreateAesKey(hAlg, key, &hKey);
    if (s != STATUS_SUCCESS) {
        ::BCryptCloseAlgorithmProvider(hAlg, 0);
        return ACS_ERR_OS(Asset, kAcpakSubCryptoKey,
                          "FAcpakCrypto::Encrypt: BCryptGenerateSymmetricKey failed",
                          static_cast<u32>(s));
    }

    // ---- AUTHENTICATED_CIPHER_MODE_INFO の組み立て ------------------------
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce      = const_cast<PUCHAR>(nonce);
    info.cbNonce      = static_cast<ULONG>(kAcpakNonceSize);
    info.pbTag        = tag_out;
    info.cbTag        = static_cast<ULONG>(kAcpakTagSize);
    // AAD なし、chain block 状態も不要 (single-chunk)。
    info.pbAuthData   = nullptr;
    info.cbAuthData   = 0;
    info.pbMacContext = nullptr;
    info.cbMacContext = 0;
    info.cbAAD        = 0;
    info.cbData       = 0;
    info.dwFlags      = 0;

    ULONG written = 0;
    s = ::BCryptEncrypt(hKey,
                        const_cast<PUCHAR>(plaintext),
                        static_cast<ULONG>(size),
                        &info,
                        nullptr, 0,                            // GCM は IV 未使用
                        ciphertext,
                        static_cast<ULONG>(size),
                        &written,
                        0);

    ::BCryptDestroyKey(hKey);
    ::BCryptCloseAlgorithmProvider(hAlg, 0);

    if (s != STATUS_SUCCESS) {
        return ACS_ERR_OS(Asset, kAcpakSubCryptoOp,
                          "FAcpakCrypto::Encrypt: BCryptEncrypt failed",
                          static_cast<u32>(s));
    }
    if (written != static_cast<ULONG>(size)) {
        // GCM では入力 == 出力 サイズが原則。これが崩れているなら CNG が
        // 想定外モードで動いている (バグ)。
        return ACS_ERR(Asset, kAcpakSubCryptoOp,
                       "FAcpakCrypto::Encrypt: unexpected output size");
    }
    return Ok();
}

TResult<void> FAcpakCrypto::Decrypt(const FAcpakKey& key,
                                  const u8        nonce[kAcpakNonceSize],
                                  const u8        tag[kAcpakTagSize],
                                  const u8*       ciphertext,
                                  u64             size,
                                  u8*             plaintext) noexcept {
    if (size > 0 && (ciphertext == nullptr || plaintext == nullptr)) {
        return ACS_ERR(Asset, kAcpakSubCryptoOp,
                       "FAcpakCrypto::Decrypt: null buffer with non-zero size");
    }
    if (nonce == nullptr || tag == nullptr) {
        return ACS_ERR(Asset, kAcpakSubCryptoOp,
                       "FAcpakCrypto::Decrypt: nonce/tag pointer is null");
    }
    if (size > 0xFFFFFFFFu) {
        return ACS_ERR(Asset, kAcpakSubCryptoOp,
                       "FAcpakCrypto::Decrypt: size > 4GiB not supported");
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS s = OpenAesGcmProvider(&hAlg);
    if (s != STATUS_SUCCESS) {
        return ACS_ERR_OS(Asset, kAcpakSubCryptoInit,
                          "FAcpakCrypto::Decrypt: provider init failed",
                          static_cast<u32>(s));
    }

    BCRYPT_KEY_HANDLE hKey = nullptr;
    s = CreateAesKey(hAlg, key, &hKey);
    if (s != STATUS_SUCCESS) {
        ::BCryptCloseAlgorithmProvider(hAlg, 0);
        return ACS_ERR_OS(Asset, kAcpakSubCryptoKey,
                          "FAcpakCrypto::Decrypt: BCryptGenerateSymmetricKey failed",
                          static_cast<u32>(s));
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce      = const_cast<PUCHAR>(nonce);
    info.cbNonce      = static_cast<ULONG>(kAcpakNonceSize);
    info.pbTag        = const_cast<PUCHAR>(tag);
    info.cbTag        = static_cast<ULONG>(kAcpakTagSize);
    info.pbAuthData   = nullptr;
    info.cbAuthData   = 0;
    info.pbMacContext = nullptr;
    info.cbMacContext = 0;
    info.cbAAD        = 0;
    info.cbData       = 0;
    info.dwFlags      = 0;

    ULONG written = 0;
    s = ::BCryptDecrypt(hKey,
                        const_cast<PUCHAR>(ciphertext),
                        static_cast<ULONG>(size),
                        &info,
                        nullptr, 0,
                        plaintext,
                        static_cast<ULONG>(size),
                        &written,
                        0);

    ::BCryptDestroyKey(hKey);
    ::BCryptCloseAlgorithmProvider(hAlg, 0);

    if (s == STATUS_AUTH_TAG_MISMATCH) {
        // 改竄 or 破損。CNG は失敗時に plaintext を未定義状態に書き換える
        // 可能性があるので 0 クリア (呼び出し側の安全のため)。
        if (size > 0 && plaintext != nullptr) {
            MemSet(plaintext, 0, static_cast<usize>(size));
        }
        return ACS_ERR(Asset, kAcpakSubCryptoTag,
                       "FAcpakCrypto::Decrypt: GCM tag verification failed");
    }
    if (s != STATUS_SUCCESS) {
        return ACS_ERR_OS(Asset, kAcpakSubCryptoOp,
                          "FAcpakCrypto::Decrypt: BCryptDecrypt failed",
                          static_cast<u32>(s));
    }
    if (written != static_cast<ULONG>(size)) {
        return ACS_ERR(Asset, kAcpakSubCryptoOp,
                       "FAcpakCrypto::Decrypt: unexpected output size");
    }
    return Ok();
}

// セキュリティ上の不変条件: AES-GCM では (key, nonce) の組が一度でも再利用される
// と認証鍵 (H) が漏洩し、任意改竄が可能になる致命的破綻を起こす。したがって
// CSPRNG が失敗した場合に「ゼロ / 予測可能な nonce」で処理を続行してはならない。
// ここでは TResult<void> を返し、RNG 失敗を必ずエラーとして伝播させる。
TResult<void> FAcpakCrypto::GenerateRandomNonce(u8 nonce_out[kAcpakNonceSize]) noexcept {
    if (nonce_out == nullptr) {
        return ACS_ERR(Asset, kAcpakSubCryptoRand,
                       "FAcpakCrypto::GenerateRandomNonce: nonce_out pointer is null");
    }

    const NTSTATUS s = ::BCryptGenRandom(nullptr,
                                   nonce_out,
                                   static_cast<ULONG>(kAcpakNonceSize),
                                   BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (s != STATUS_SUCCESS) {
        // CSPRNG 失敗。ゼロ/予測可能 nonce での暗号化を絶対に避けるため、出力を
        // 0 クリアしたうえでエラーを返し、呼び出し側に暗号化を中止させる。
        // (0 クリアは「失敗後に呼び出し側が誤って使っても弱鍵で暗号化しない」
        //  防御だが、本実装では呼び出し側が必ずエラーを伝播するので未使用になる。)
        MemSet(nonce_out, 0, kAcpakNonceSize);
        return ACS_ERR_OS(Asset, kAcpakSubCryptoRand,
                          "FAcpakCrypto::GenerateRandomNonce: BCryptGenRandom failed",
                          static_cast<u32>(s));
    }
    return Ok();
}

} // namespace acs::assetpack
