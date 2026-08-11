// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// 前方宣言: 実体は `gameframework/ParticleEffectSystem.h`。.cpp 側で完全型を include する。
struct FParticleEmitterDef;

namespace fxedit {

/** `.fxedit` の checked parse/load/save が返す安定したエラー種別。 */
enum class EFxeditSerializeError : u8 {
    /** エラーなし。 */
    None = 0,
    /** 必須引数が null。 */
    NullArgument,
    /** 指定パスが上限を超過。 */
    PathTooLong,
    /** 入力全体が上限を超過。 */
    InputTooLarge,
    /** 入力に埋め込み NUL を検出。 */
    EmbeddedNul,
    /** 行数が上限を超過。 */
    TooManyLines,
    /** 1 行の長さが上限を超過。 */
    LineTooLong,
    /** ファイル識別子が不正。 */
    BadMagic,
    /** 形式バージョンが未対応。 */
    UnsupportedVersion,
    /** emitter 数の宣言が不足。 */
    MissingEmitterCount,
    /** emitter 数の宣言が重複。 */
    DuplicateEmitterCount,
    /** emitter 数が上限を超過。 */
    TooManyEmitters,
    /** 出力領域が不足。 */
    BufferTooSmall,
    /** 構文が不正。 */
    InvalidSyntax,
    /** emitter index が不正。 */
    InvalidEmitterIndex,
    /** 同じ property key が重複。 */
    DuplicateKey,
    /** property 値の表現が不正。 */
    InvalidValue,
    /** property 値が許容範囲を超過。 */
    ValueOutOfRange,
    /** emitter 名の長さが上限を超過。 */
    NameTooLong,
    /** emitter 名が不正。 */
    InvalidName,
    /** curve 数が上限を超過。 */
    TooManyCurves,
    /** keyframe 数が上限を超過。 */
    TooManyKeyframes,
    /** 必要なメモリを確保できない。 */
    AllocationFailure,
    /** 対象ファイルが存在しない。 */
    FileNotFound,
    /** 対象ファイルを開けない。 */
    FileOpenFailed,
    /** ファイルサイズを取得できない。 */
    FileSizeFailed,
    /** 読み取り中にファイルが変更された。 */
    FileChanged,
    /** ファイル読み取りに失敗。 */
    FileReadFailed,
    /** ファイル書き込みに失敗。 */
    FileWriteFailed,
    /** ファイル内容の同期に失敗。 */
    FileFlushFailed,
    /** ファイルを正常に閉じられない。 */
    FileCloseFailed,
    /** 一時ファイルからの置換に失敗。 */
    AtomicReplaceFailed,
};

/** `.fxedit` の checked operation 結果。 */
struct FFxeditSerializeResult {
    /** serialize 処理が返したエラー。 */
    EFxeditSerializeError error = EFxeditSerializeError::None;
    /** 構文エラーを検出した行。該当しない場合は 0。 */
    u32 line = 0u;
    /** 正常に処理できた emitter 数。 */
    u32 emitter_count = 0u;
    /** 正常に処理できたバイト数。 */
    u64 bytes_processed = 0u;
    /** OS が返したエラーコード。該当しない場合は 0。 */
    u32 os_error = 0u;

    bool Succeeded() const noexcept { return error == EFxeditSerializeError::None; }
    static const char* ErrorName(EFxeditSerializeError error) noexcept;
};

/**
 * `.fxedit` テキストファイルの save/load を担う state-less ユーティリティ。
 *
 * @details
 * すべてのメンバは static。state を持たない utility class なので、コンストラクタ・
 * コピー・ムーブを禁止しておく (誤って実体化されるのを防ぐ)。emitter 群を人間可読 /
 * 差分を確認しやすいテキスト (`ACS_FXEDIT` v1) で書き出し・復元する。
 */
class CFxeditSerializer {
public:
    /** 構築禁止 (state を持たない static ユーティリティのため)。 */
    CFxeditSerializer()                                   = delete;

    /** 破棄禁止 (実体化しないため)。 */
    ~CFxeditSerializer()                                  = delete;

    /** コピー禁止。 */
    CFxeditSerializer(const CFxeditSerializer&)            = delete;

    /** コピー代入も禁止。 */
    CFxeditSerializer& operator=(const CFxeditSerializer&) = delete;

    /** ムーブ禁止。 */
    CFxeditSerializer(CFxeditSerializer&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CFxeditSerializer& operator=(CFxeditSerializer&&)      = delete;

    /** 先頭行に必ず付ける magic 文字列。 */
    static constexpr const char* kMagic            = "ACS_FXEDIT";

    /** 現在のフォーマットバージョン。 */
    static constexpr u32         kCurrentVersion   = 1u;

    /**
     * emitter 1 個分のテキスト出力に必要な最大バイト数 (sprintf 用上限見積)。
     *
     * @details
     * 約 12 key * (prefix "E999 " 6B + name 32B + value 64B + newline) ≒ 1.5 KB。
     * 安全側で 2048B にしておく。
     */
    static constexpr usize       kMaxBytesPerEmitter = 2048;

    /** 1 行のテキストパース時に許容する最大長 (超過はエラー)。 */
    static constexpr usize       kMaxLineLength      = 512;

    /**
     * 1 emitter の name に許容する最大バイト数 (NUL 終端含まず)。
     *
     * @details ParticleEditor 側の UI 想定で 31 文字 (+NUL = 32B) としておく。
     */
    static constexpr usize       kMaxEmitterName     = 31;

    /** checked loader が受理する `.fxedit` 全体の最大 byte 数。 */
    static constexpr usize       kMaxInputBytes      = 8u * 1024u * 1024u;

    /** 1 ファイルに保持できる emitter の安全上限。 */
    static constexpr u32         kMaxEmitterCount    = 1024u;

    /** コメント・空行を含む入力行数の安全上限。 */
    static constexpr u32         kMaxLineCount       = 65536u;

    /** 将来予約の curve 数上限（emitter ごと）。 */
    static constexpr u32         kMaxCurvesPerEmitter = 16u;

    /** 将来予約の keyframe 数上限（curve ごと）。 */
    static constexpr u32         kMaxKeyframesPerCurve = 256u;

    /** Win32 wide path の終端 NUL を除く最大文字数。 */
    static constexpr usize       kMaxPathChars       = 1023u;

    /**
     * save/load のエラー subcode (FErrorCode の EErrCategory::IO で使う)。
     *
     * @details TSaveSlot (1-99) と衝突しないよう、fxedit は 700-799 番を使う。
     */
    enum ESubCode : u16 {
        /** エラーなし。 */
        kSub_OK                = 0,

        /** file_path / defs / names が nullptr。 */
        kSub_NullArgs          = 700,

        /** count > max_emitters。 */
        kSub_TooManyEmitters   = 701,

        /** name buffer 不足 or 出力 buffer 不足。 */
        kSub_BufferOverflow    = 702,

        /** 先頭が "ACS_FXEDIT" でない。 */
        kSub_BadMagic          = 703,

        /** version が未対応。 */
        kSub_BadVersion        = 704,

        /** 行の構文不正。 */
        kSub_BadFormat         = 705,

        /** 読み込み対象が存在しない。 */
        kSub_FileNotFound      = 706,

        /** 下位 CFileSystem からのエラー。 */
        kSub_IOFailure         = 707,

        /** checked parser の schema/value 検証失敗。 */
        kSub_ValidationFailed  = 708,

        /** allocation failure。 */
        kSub_AllocationFailure = 709,

        /** atomic replace に失敗。 */
        kSub_AtomicReplace     = 710,
    };

    /**
     * 長さ付き `.fxedit` text を全検証後に出力へ commit する。
     *
     * @details text は NUL 終端不要。失敗時は out_defs と out_name_buffer を変更しない。
     */
    static FFxeditSerializeResult TryParseText(
        const char* text,
        usize text_size,
        FParticleEmitterDef* out_defs,
        char* out_name_buffer,
        usize name_buffer_capacity,
        u32 max_emitters) noexcept;

    /**
     * `.fxedit` を上限付き完全 read し、TryParseText で復元する。
     *
     * @details サイズ変化、embedded NUL、OOM、close failure を成功扱いしない。
     */
    static FFxeditSerializeResult TryLoad(
        const wchar_t* file_path,
        FParticleEmitterDef* out_defs,
        char* out_name_buffer,
        usize name_buffer_capacity,
        u32 max_emitters) noexcept;

    /**
     * emitter 群を事前検証し、一意 temp へ durable write 後に atomic replace する。
     *
     * @details 失敗時は既存の保存先を変更せず、作成した temp を削除する。
     */
    static FFxeditSerializeResult TrySave(
        const wchar_t* file_path,
        const FParticleEmitterDef* defs,
        const char* const* names,
        u32 count) noexcept;

    /**
     * emitter 群を `.fxedit` テキストへ書き出す。
     *
     * @details count が 0 のときはヘッダだけ書き出す。checked atomic save を経由する。
     * @param file_path 保存先 (Win32 wide path)。
     * @param defs FParticleEmitterDef 配列 (count 個)。count>0 のとき nullptr 不可。
     * @param names emitter 名 (C 文字列) の配列。個別要素 nullptr は "" 扱い。
     *              count>0 で names ポインタ自体が nullptr の場合は kSub_NullArgs。
     * @param count emitter 個数 (0 も valid)。
     * @return 成功なら空の TResult、null 引数 / バッファ溢れ / I/O 失敗ならエラー。
     */
    static TResult<void, FErrorCode> Save(const wchar_t*             file_path,
                                        const FParticleEmitterDef*  defs,
                                        const char* const*         names,
                                        u32                        count) noexcept;

    /**
     * `.fxedit` テキストから emitter 群を読み出す。
     *
     * @param file_path 読み込み元 (Win32 wide path)。
     * @param out_defs 復元する FParticleEmitterDef 配列 (max_emitters 個分の領域)。
     * @param out_name_buffer 名前バッファ。各 emitter の C string を `name0\0name1\0...`
     *                        形式で連結配置する。
     * @param name_buffer_capacity out_name_buffer のバイト数。
     * @param max_emitters out_defs の要素数 (= 受け入れ可能な最大 emitter 数)。
     * @return 成功なら実際にロードした emitter 数 (<= max_emitters)、null 引数 /
     *         magic 不一致 / version 不一致 / 容量超過 / I/O 失敗ならエラー。
     */
    static TResult<u32, FErrorCode> Load(const wchar_t*       file_path,
                                       FParticleEmitterDef*  out_defs,
                                       char*                out_name_buffer,
                                       u32                  name_buffer_capacity,
                                       u32                  max_emitters) noexcept;

    /**
     * 先頭の非空行から "ACS_FXEDIT <v>" を検査して version を返す。
     *
     * @details text は NUL 終端でなくても OK (text_len で長さを与える)。
     * @param text パース対象のテキスト先頭。
     * @param text_len text の長さ (バイト)。
     * @return 解析できた version。magic 不一致 / version 解析失敗なら 0。
     */
    static u32 ParseHeaderVersion(const char* text, u32 text_len) noexcept;

    /**
     * 1 行の "<key> <v0> [<v1> [<v2> [<v3>]]]" を構文解析する。
     *
     * @details
     * line は NUL 終端を期待する (ReadAllText が末尾 NUL 付きで読むため)。out_key は
     * line 内の key 開始ポインタを指す (終端 NUL 化はしないので長さは呼び出し側が
     * 「最初の空白文字まで」で判定する)。出現しない値スロットには 0.0 が入る。
     * @param line パース対象の 1 行 (NUL 終端)。
     * @param out_key key 文字列の開始ポインタ (line 内)。
     * @param out_v0 1 番目の数値スロット。
     * @param out_v1 2 番目の数値スロット。
     * @param out_v2 3 番目の数値スロット。
     * @param out_v3 4 番目の数値スロット。
     * @return 構文が正しければ true、空行 / コメント行なら false。
     */
    static bool ParseLine(const char*  line,
                          const char*& out_key,
                          f32&         out_v0,
                          f32&         out_v1,
                          f32&         out_v2,
                          f32&         out_v3) noexcept;

    /**
     * ' ' / '\t' / '\r' / '\n' を読み飛ばす。
     *
     * @details 末端 NUL は跨がない (NUL に当たったらそこで止まる)。
     * @param p 走査開始ポインタ (nullptr 可)。
     * @return 最初の非空白文字 (または NUL) を指すポインタ。p が nullptr なら nullptr。
     */
    static const char* SkipWhitespace(const char* p) noexcept;
};

using FFxeditSerializer = CFxeditSerializer;

} // namespace fxedit
} // namespace acs::game
