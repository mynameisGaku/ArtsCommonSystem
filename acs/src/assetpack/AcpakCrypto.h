// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS FAssetPack — AES-256-GCM 暗号化 + PBKDF2 鍵導出 (Windows CNG / BCrypt)
// -----------------------------------------------------------------------------
// `.acpak` v2 で各ファイルエントリを暗号化するための薄いラッパ。FAcpakReader /
// FAcpakWriter から呼ばれる。実装は Windows CNG (BCrypt) を使い、サードパーティ
// 依存は OS 同梱 `Bcrypt.lib` 1 つのみ。
//
// 仕様 (FAssetPack.md §4 に準拠):
//   ・暗号: AES-256-GCM (AEAD)。x86-64 全 CPU で AES-NI が CNG により自動利用。
//   ・key:   256bit 固定 (FAcpakKey)。各 .acpak は 1 鍵で全エントリを暗号化。
//   ・nonce: 96bit (12 バイト) per-entry。BCryptGenRandom で CSPRNG 取得、
//            FAcpakFileEntry::cipher_nonce に保存。
//   ・tag:   128bit (16 バイト) per-entry。Encrypt が出力、Decrypt が検証。
//            FAcpakFileEntry::cipher_tag に保存。
//   ・KDF:   PBKDF2-HMAC-SHA256, 10000 iterations (BCryptDeriveKeyPBKDF2)。
//
// 設計上の注記:
//   ・<windows.h> / <bcrypt.h> は .cpp 局所。本ヘッダはエンジン他層から
//     include されるので Win32 ヘッダを露出させない。
//   ・全 API noexcept、TResult<T, FErrorCode>。AES-GCM 認証タグ失敗は
//     ACS_ERR(Asset, kAcpakSubBadCrc) として返す (= 改竄 / 破損の単一窓口)。
//   ・STL 不使用、<string> 不使用、内部スタック / ハンドルだけで完結する。
//   ・Decrypt は in-place 可能 (plaintext == ciphertext が OK)。CNG 自体の
//     BCryptDecrypt が同一バッファを許容する。Encrypt も同じく in-place 可。
// =============================================================================
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::assetpack {

/**
 * AES-256 鍵 (256bit) を保持する POD。
 *
 * @details
 * `FAssetPack.md` の `ArchiveKey` と同じレイアウト。FAcpakWriter /
 * FAcpakReader は事前に DeriveKey() で作成した本鍵を受け取り、内部で
 * BCryptGenerateSymmetricKey に渡す。
 */
struct FAcpakKey {
    /** 鍵バイト列 (32 バイト = 256bit)。 */
    u8 bytes[32];
};

/** AES-GCM nonce のバイト長 (= 12、96bit、GCM 推奨幅)。 */
inline constexpr u32 kAcpakNonceSize = 12;

/** AES-GCM 認証タグのバイト長 (= 16、128bit)。 */
inline constexpr u32 kAcpakTagSize   = 16;

// crypto / lz4 用 subcode は AcpakFormat.h で予約済の 1301-1313 に続く
// 1314 番台を使う (lz4 は 1320 番台)。

/** BCryptOpenAlgorithmProvider 失敗。 */
inline constexpr u16 kAcpakSubCryptoInit   = 1314;

/** BCryptGenerateSymmetricKey 失敗。 */
inline constexpr u16 kAcpakSubCryptoKey    = 1315;

/** BCryptEncrypt / BCryptDecrypt 失敗。 */
inline constexpr u16 kAcpakSubCryptoOp     = 1316;

/** AES-GCM 認証タグ検証失敗 (改竄 or 破損)。 */
inline constexpr u16 kAcpakSubCryptoTag    = 1317;

/** BCryptGenRandom 失敗 (nonce 生成不能)。 */
inline constexpr u16 kAcpakSubCryptoRand   = 1318;

/** BCryptDeriveKeyPBKDF2 失敗。 */
inline constexpr u16 kAcpakSubCryptoKdf    = 1319;

/**
 * AES-256-GCM 暗号化 + PBKDF2 鍵導出を提供する static 関数群。
 *
 * @details
 * 全 API が静的関数で状態を持たず、呼び出し毎に CNG ハンドルを開いて閉じる。
 * .acpak のロードは典型的に起動時 1 度 + ストリーミング読み出しなので、
 * ハンドルキャッシュは行わない。インスタンス化はできない (ctor delete)。
 */
class FAcpakCrypto {
public:
    /** インスタンス化禁止 (全 API は static)。 */
    FAcpakCrypto() = delete;

    /**
     * パスワード + salt から 32 バイトの AES-256 鍵を導出する。
     *
     * @details
     * PBKDF2-HMAC-SHA256, 10000 iterations。password / salt のポインタは
     * 即時消費され、関数戻り後に参照されない。どちらも 0 バイトでも構わない
     * (BCryptDeriveKeyPBKDF2 が許容)。失敗時は kAcpakSubCryptoInit (provider
     * 取得失敗) / kAcpakSubCryptoKdf (KDF 計算失敗)。
     * @param password パスワードバイト列 (nullptr なら空扱い)。
     * @param password_len password のバイト数。
     * @param salt salt バイト列 (nullptr なら空扱い)。
     * @param salt_len salt のバイト数。
     * @return 導出した 32 バイト鍵、失敗ならエラー。
     */
    static TResult<FAcpakKey> DeriveKey(const u8* password,
                                      u32       password_len,
                                      const u8* salt,
                                      u32       salt_len) noexcept;

    /**
     * plaintext を AES-256-GCM で暗号化し、ciphertext と認証タグを書き出す。
     *
     * @details
     * nonce は呼び出し側で per-entry に一意な 12 バイトを渡すこと
     * (GenerateRandomNonce で生成可)。同一 (key, nonce) は GCM を破滅させるため
     * 絶対に再利用しない。plaintext == ciphertext (in-place) は許可される。
     * size == 0 のときも tag_out は計算される (= GMAC として動作)。失敗時は
     * kAcpakSubCryptoInit (provider/key 失敗) / kAcpakSubCryptoOp
     * (BCryptEncrypt 失敗)。
     * @param key 暗号化に使う AES-256 鍵。
     * @param nonce per-entry に一意な 12 バイトの nonce。
     * @param plaintext 暗号化する平文 (size バイト)。
     * @param size 平文のバイト数。
     * @param ciphertext 暗号文の出力先 (size バイト)。
     * @param tag_out 認証タグの出力先 (16 バイト)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    static TResult<void> Encrypt(const FAcpakKey& key,
                                const u8        nonce[kAcpakNonceSize],
                                const u8*       plaintext,
                                u64             size,
                                u8*             ciphertext,
                                u8              tag_out[kAcpakTagSize]) noexcept;

    /**
     * ciphertext + tag を AES-256-GCM で復号し、plaintext を書き出す。
     *
     * @details
     * 認証タグ検証に失敗するとデータは破棄される (一部でも改竄されていれば全て
     * 弾く)。plaintext == ciphertext (in-place) は許可される。size == 0 のときも
     * tag 検証だけ行う (= GMAC として動作)。失敗時は kAcpakSubCryptoInit
     * (provider/key 失敗) / kAcpakSubCryptoTag (タグ検証失敗 = 改竄 or 破損) /
     * kAcpakSubCryptoOp (その他 BCryptDecrypt 失敗)。
     * @param key 復号に使う AES-256 鍵。
     * @param nonce 暗号化時に使った 12 バイトの nonce。
     * @param tag 検証する 16 バイトの認証タグ。
     * @param ciphertext 復号する暗号文 (size バイト)。
     * @param size 暗号文のバイト数。
     * @param plaintext 平文の出力先 (size バイト)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    static TResult<void> Decrypt(const FAcpakKey& key,
                                const u8        nonce[kAcpakNonceSize],
                                const u8        tag[kAcpakTagSize],
                                const u8*       ciphertext,
                                u64             size,
                                u8*             plaintext) noexcept;

    /**
     * CSPRNG で 12 バイトのランダム nonce を生成する。
     *
     * @details
     * BCryptGenRandom + BCRYPT_USE_SYSTEM_PREFERRED_RNG で OS 標準乱数源を使う。
     * RNG 失敗時は決して成功を返さない: AES-GCM は (key, nonce) 再利用で認証鍵が
     * 漏洩する致命的破綻を起こすため、ゼロ/予測可能な nonce で暗号化を続行しては
     * ならない。失敗時は nonce_out を 0 クリアしたうえで
     * kAcpakSubCryptoRand を返すので、呼び出し側は必ずこのエラーを伝播して
     * 暗号化を中止すること。
     * @param nonce_out 生成した 12 バイト nonce の出力先。
     * @return 成功なら空の TResult、BCryptGenRandom 失敗ならエラー。
     */
    static TResult<void> GenerateRandomNonce(u8 nonce_out[kAcpakNonceSize]) noexcept;
};

} // namespace acs::assetpack
