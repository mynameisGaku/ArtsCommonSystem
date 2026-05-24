// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS AssetPack — AES-256-GCM 暗号化 + PBKDF2 鍵導出 (Windows CNG / BCrypt)
// -----------------------------------------------------------------------------
// `.acpak` v2 で各ファイルエントリを暗号化するための薄いラッパ。AcpakReader /
// AcpakWriter から呼ばれる。実装は Windows CNG (BCrypt) を使い、サードパーティ
// 依存は OS 同梱 `Bcrypt.lib` 1 つのみ。
//
// 仕様 (AssetPack.md §4 に準拠):
//   ・暗号: AES-256-GCM (AEAD)。x86-64 全 CPU で AES-NI が CNG により自動利用。
//   ・key:   256bit 固定 (AcpakKey)。各 .acpak は 1 鍵で全エントリを暗号化。
//   ・nonce: 96bit (12 バイト) per-entry。BCryptGenRandom で CSPRNG 取得、
//            AcpakFileEntry::cipher_nonce に保存。
//   ・tag:   128bit (16 バイト) per-entry。Encrypt が出力、Decrypt が検証。
//            AcpakFileEntry::cipher_tag に保存。
//   ・KDF:   PBKDF2-HMAC-SHA256, 10000 iterations (BCryptDeriveKeyPBKDF2)。
//
// 設計上の注記:
//   ・<windows.h> / <bcrypt.h> は .cpp 局所。本ヘッダはエンジン他層から
//     include されるので Win32 ヘッダを露出させない。
//   ・全 API noexcept、Result<T, ErrorCode>。AES-GCM 認証タグ失敗は
//     ACS_ERR(Asset, kAcpakSubBadCrc) として返す (= 改竄 / 破損の単一窓口)。
//   ・STL 不使用、<string> 不使用、内部スタック / ハンドルだけで完結する。
//   ・Decrypt は in-place 可能 (plaintext == ciphertext が OK)。CNG 自体の
//     BCryptDecrypt が同一バッファを許容する。Encrypt も同じく in-place 可。
// =============================================================================
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::assetpack {

// ---- 鍵 (256bit) ----------------------------------------------------------
// 32 バイトの POD。`AssetPack.md` の `ArchiveKey` と同じレイアウト。
// AcpakWriter / AcpakReader は事前に DeriveKey() で作成した本鍵を受け取り、
// 内部で BCryptGenerateSymmetricKey に渡す。
struct AcpakKey {
    u8 bytes[32];
};

// ---- AES-GCM nonce / tag のバイト長 (定数化して呼び出し側の誤りを減らす) -
inline constexpr u32 kAcpakNonceSize = 12;   // 96bit (GCM 推奨幅)
inline constexpr u32 kAcpakTagSize   = 16;   // 128bit (GCM 認証タグ)

// ---- ErrorCode subcode (AssetPack の 1300 番台に追加) --------------------
// 既存 subcode は AcpakFormat.h で予約済 (1301-1313)。crypto / lz4 用は
// 1314-1320 で予約する。
inline constexpr u16 kAcpakSubCryptoInit   = 1314; // BCryptOpenAlgorithmProvider 失敗
inline constexpr u16 kAcpakSubCryptoKey    = 1315; // BCryptGenerateSymmetricKey 失敗
inline constexpr u16 kAcpakSubCryptoOp     = 1316; // BCryptEncrypt / BCryptDecrypt 失敗
inline constexpr u16 kAcpakSubCryptoTag    = 1317; // 認証タグ検証失敗 (改竄)
inline constexpr u16 kAcpakSubCryptoRand   = 1318; // BCryptGenRandom 失敗
inline constexpr u16 kAcpakSubCryptoKdf    = 1319; // BCryptDeriveKeyPBKDF2 失敗

// ---- AcpakCrypto (static 関数群、インスタンス化しない) -------------------
// 全 API が静的関数。状態は持たず、呼び出し毎に CNG ハンドルを開いて閉じる
// (.acpak のロードは典型的に起動時 1 度 + ストリーミング読み出しなので、
// ハンドルキャッシュは Phase 2 では行わない。Phase 3+ で AcpakReader に
// per-instance ハンドルを持たせる余地は残す)。
class AcpakCrypto {
public:
    AcpakCrypto() = delete;

    // パスワード + salt から 32 バイトの AES-256 鍵を導出する。
    //   ・PBKDF2-HMAC-SHA256, 10000 iterations
    //   ・password / salt のポインタは関数戻り後に参照されない (即時消費)。
    //   ・password / salt は 0 バイトでも構わない (BCryptDeriveKeyPBKDF2 が許容)。
    // 主なエラー:
    //   ・ACS_ERR_OS(Asset, kAcpakSubCryptoInit, ...) — algorithm provider 取得失敗
    //   ・ACS_ERR_OS(Asset, kAcpakSubCryptoKdf,  ...) — KDF 計算失敗
    static Result<AcpakKey> DeriveKey(const u8* password,
                                      u32       password_len,
                                      const u8* salt,
                                      u32       salt_len) noexcept;

    // plaintext (size バイト) を暗号化し、ciphertext (= size バイト) と
    // 認証タグ (tag_out 16 バイト) を書き出す。
    //   ・nonce は呼び出し側で per-entry に一意な 12 バイトを渡すこと
    //     (GenerateRandomNonce で生成可)。同一 (key, nonce) は GCM を破滅させる
    //     ため、絶対に再利用しない。
    //   ・plaintext == ciphertext (in-place) は許可される。
    //   ・size == 0 のときも tag_out は計算される (= GMAC として動作)。
    // 主なエラー:
    //   ・ACS_ERR_OS(Asset, kAcpakSubCryptoInit, ...) — provider / key 失敗
    //   ・ACS_ERR_OS(Asset, kAcpakSubCryptoOp,   ...) — BCryptEncrypt 失敗
    static Result<void> Encrypt(const AcpakKey& key,
                                const u8        nonce[kAcpakNonceSize],
                                const u8*       plaintext,
                                u64             size,
                                u8*             ciphertext,
                                u8              tag_out[kAcpakTagSize]) noexcept;

    // ciphertext (size バイト) + tag (16 バイト) を復号して plaintext (size
    // バイト) を書き出す。認証タグ検証に失敗するとデータは破棄される (= 一部
    // でも変更されていれば全て弾く)。
    //   ・plaintext == ciphertext (in-place) は許可される。
    //   ・size == 0 のときも tag 検証だけ行う (= GMAC として動作)。
    // 主なエラー:
    //   ・ACS_ERR_OS(Asset, kAcpakSubCryptoInit, ...) — provider / key 失敗
    //   ・ACS_ERR(Asset, kAcpakSubCryptoTag, ...)    — タグ検証失敗 (改竄 or 破損)
    //   ・ACS_ERR_OS(Asset, kAcpakSubCryptoOp, ...)  — その他 BCryptDecrypt 失敗
    static Result<void> Decrypt(const AcpakKey& key,
                                const u8        nonce[kAcpakNonceSize],
                                const u8        tag[kAcpakTagSize],
                                const u8*       ciphertext,
                                u64             size,
                                u8*             plaintext) noexcept;

    // CSPRNG で 12 バイトのランダム nonce を生成する。
    //   ・BCryptGenRandom + BCRYPT_USE_SYSTEM_PREFERRED_RNG で OS 標準乱数源。
    //   ・失敗時 (= OS 異常) は MemSet(0) でフォールバック。AcpakWriter は
    //     致命的でなく単に「弱い nonce」になるが、production code として
    //     正常パスのみ通る前提。Phase 3 で Result<void> 化する余地あり。
    static void GenerateRandomNonce(u8 nonce_out[kAcpakNonceSize]) noexcept;
};

} // namespace acs::assetpack
