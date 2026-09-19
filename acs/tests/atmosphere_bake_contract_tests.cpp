// SPDX-License-Identifier: Apache-2.0
#include "render/Atmosphere.h"
#include "container/StringView.h"
#include "foundation/Move.h"
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"
#include "test/Expect.h"
#include "test/Test.h"

// 製品Atmosphere.cppを直接リンクし、下の生成関数で実GPUを完全に置き換える専用試験。
// ACS::Renderや実バックエンドの生成関数とは同じ実行ファイルへリンクしない。
// Dispatchの呼び出し中に画像を書く即時実行型で、出力配列の契約だけを検証する。
// 追加inlでは別の遅延型を使い、命令記録と提出・完了の分離を検査する。実GPUや参照表の計算は検証しない。

namespace acs {
namespace {

/** 端数を持つ小さい画像の幅。巨大寸法や実際の確保不足は対象外。 */
constexpr u32 kBakeWidth = 9u;
/** 小さい画像の高さ。 */
constexpr u32 kBakeHeight = 3u;
/** RGBA画像の全要素数。 */
constexpr u32 kBakeElements = kBakeWidth * kBakeHeight * 4u;
// 小さい試験画像でも、ReadTextureのu32バイト数へ変換する前に収まることを固定する。
static_assert(kBakeElements <= ~u32{0} / sizeof(f32));
/** 読み戻し口へ渡す正確なバイト数。要素数とは区別する。 */
constexpr u32 kBakeTransferBytes = static_cast<u32>(static_cast<usize>(kBakeElements) * sizeof(f32));
/** 一部を書いた後の失敗で、実際に上書きする要素数。 */
constexpr u32 kPartialWriteElements = 3u;
/** 生成画像と値・サイズの両方が異なる、利用者の既存出力。 */
constexpr f32 kOriginalOutput[] = {-11.0f, -22.0f, -33.0f, -44.0f, -55.0f};
/** 既存出力の要素数。 */
constexpr usize kOriginalElements = sizeof(kOriginalOutput) / sizeof(kOriginalOutput[0]);

/** 初期化用シェーダの種類だけを保持する。実コンパイルは行わない。 */
class ABakeContractShader final : public IRhiShader {
public:
    /** 製品から渡された入口名で画像生成処理を識別する。 */
    explicit ABakeContractShader(const FShaderDesc& description) noexcept : m_Stage(description.stage), m_IsBake(FStringView(description.entry_point) == FStringView("CSBake")) {}
    /** 初期化時に指定された段階を返す。 */
    EShaderStage Stage() const noexcept override { return m_Stage; }
    /** CPU試験なので実バイトコードは持たない。 */
    const byte* Bytecode() const noexcept override { return nullptr; }
    /** 実コンパイルをしないため0を返す。 */
    usize BytecodeSize() const noexcept override { return 0u; }
    /** 正距円筒画像の生成処理かを返す。 */
    bool IsBake() const noexcept { return m_IsBake; }

private:
    /** 製品が指定したシェーダ段階。 */
    EShaderStage m_Stage;
    /** CSBakeの入口ならtrue。 */
    bool m_IsBake;
};

/** 画像生成か、それ以外の初期化用処理かだけを保持する。 */
class ABakeContractPipeline final : public IRhiPipeline {
public:
    /** シェーダから判定した画像生成の役割を受け取る。 */
    explicit ABakeContractPipeline(bool is_bake) noexcept : m_IsBake(is_bake) {}
    /** Dispatchで試験画像を書く処理かを返す。 */
    bool IsBake() const noexcept { return m_IsBake; }

private:
    /** 出力画像を書く処理ならtrue。 */
    bool m_IsBake;
};

/** 初期化が要求するバッファの寸法と用途だけを保持する。 */
class ABakeContractBuffer final : public IRhiBuffer {
public:
    /** 製品の指定値と、資源より長く生きるデバイスの更新回数を借用する。 */
    ABakeContractBuffer(const FBufferDesc& description, u32& updates) noexcept : m_Size(description.size), m_Usage(description.usage), m_Updates(updates) {}
    /** 指定されたバイト数を返す。 */
    usize Size() const noexcept override { return m_Size; }
    /** 指定された用途を返す。 */
    EBufferUsage Usage() const noexcept override { return m_Usage; }
    /** 定数値は使わず、無効入力でも更新してしまう不備を回数で検査する。 */
    void Update(const void*, usize, usize) noexcept override { ++m_Updates; }

private:
    /** 初期化時のバッファ寸法。 */
    usize m_Size;
    /** 初期化時の用途。 */
    EBufferUsage m_Usage;
    /** この資源より長く生きる更新回数。 */
    u32& m_Updates;
};

/** 小さい試験画像だけを実体として持つ。大気表と3D体積は寸法のみ保持する。 */
class ABakeContractTexture final : public IRhiTexture {
public:
    /** 画像生成の対象を区別するため、製品の寸法・形式・用途を保存する。 */
    explicit ABakeContractTexture(const FTextureDesc& description) noexcept : m_Description(description) {}
    /** 指定された幅を返す。 */
    u32 Width() const noexcept override { return m_Description.width; }
    /** 指定された高さを返す。 */
    u32 Height() const noexcept override { return m_Description.height; }
    /** 指定された形式を返す。 */
    EFormat PixelFormat() const noexcept override { return m_Description.format; }
    /** 固定長の試験画像として扱える寸法・形式・用途かを返す。 */
    bool IsTestImage() const noexcept
    {
        return Width() == kBakeWidth && Height() == kBakeHeight && PixelFormat() == EFormat::R32G32B32A32_Float && m_Description.depth == 1u && m_Description.is_uav;
    }
    /** Dispatchだけが呼ぶ書込み。世代と要素位置を識別できる厳密な整数値を書く。 */
    void WriteFromDispatch_Internal(u32 generation) noexcept
    {
        // 全RGBA要素を独立に埋め、部分書込み・古い世代の再利用を判別する。
        for (u32 index = 0u; index < kBakeElements; ++index) {
            m_Pixels[index] = static_cast<f32>(generation * 1024u + index + 1u);
        }
        m_Generation = generation;
    }
    /** 実際にDispatchで書かれた世代。未書込みなら0。 */
    u32 Generation() const noexcept { return m_Generation; }
    /** Dispatchが保存した画素を読む。呼出側で添字を検証する。 */
    f32 Pixel(u32 index) const noexcept { return m_Pixels[index]; }

private:
    /** 資源を識別する初期化情報。 */
    FTextureDesc m_Description;
    /** ReadTextureからは変更しない、Dispatch専用の画像保存先。 */
    f32 m_Pixels[kBakeElements]{};
    /** 保存済み画像の世代。 */
    u32 m_Generation = 0u;
};

/** 即時実行された試験画像だけを読み戻す偽デバイス。実GPUへは一切接続しない。 */
class ABakeContractDevice : public IRhiDevice {
public:
    /** 同期口の対応契約を模擬する名前。生成関数も偽物なので、実Diligentへの型変換は行われない。 */
    const char* BackendName() const noexcept override { return "Diligent"; }
    /** 実GPUを使用していないことを識別できる名前を返す。 */
    const char* AdapterName() const noexcept override { return "CPUOnly"; }
    /** Dispatch内で書込みが終わるため、待機による画像生成は行わない。 */
    void WaitIdle() noexcept override {}
    /** 既に書かれた最新世代だけをコピーする。画像の生成や世代の更新は行わない。 */
    bool ReadTexture(IRhiTexture& texture, void* destination, u32 destination_size) noexcept override
    {
        ++ReadCalls;
        LastCopiedElements = 0u;
        LastDestinationSize = destination_size;
        // この専用実行ファイルの生成関数だけが作る偽画像。
        const auto& source = static_cast<const ABakeContractTexture&>(texture);
        if (destination == nullptr || !source.IsTestImage() || source.Generation() == 0u || source.Generation() != WrittenGeneration || destination_size < kBakeTransferBytes) {
            return false;
        }
        // 出力先は本物の製品関数が渡す配列。一時配列への変更も同じ条件で検証できる。
        auto* pixels = static_cast<f32*>(destination);
        // 失敗時も実際に一部を上書きし、利用者出力へ直接読む不備を露出させる。
        const u32 count = FailAfterPartialWrite ? kPartialWriteElements : kBakeElements;
        for (u32 index = 0u; index < count; ++index) {
            pixels[index] = source.Pixel(index);
            ++LastCopiedElements;
        }
        if (FailAfterPartialWrite) return false;
        ++SuccessfulReads;
        return true;
    }

    /** 次の読み戻しを一部上書き後に失敗させるか。 */
    bool FailAfterPartialWrite = false;
    /** 画像用Dispatchで書かれた最新世代。ReadTextureは更新しない。 */
    u32 WrittenGeneration = 0u;
    /** 読み戻しが本当に呼ばれた回数。 */
    u32 ReadCalls = 0u;
    /** 直前の読み戻しが実際に書き込んだ要素数。 */
    u32 LastCopiedElements = 0u;
    /** 製品が直前の読み戻しへ渡したu32の転送先バイト数。 */
    u32 LastDestinationSize = 0u;
    /** 全画像をコピーしてtrueを返した回数。確保失敗との前後関係を確認する。 */
    u32 SuccessfulReads = 0u;
    /** 偽画像の生成関数へ入った回数。準備後の再確保も検出する。 */
    u32 TextureFactoryCalls = 0u;
    /** 無効な寸法の先行試験でも、画像や出力領域の巨大確保へ到達させない。 */
    bool RejectTextureCreation = false;
    /** 直前の画像生成要求の幅。丸めず渡したことも確認する。 */
    u32 LastTextureWidth = 0u;
    /** 直前の画像生成要求の高さ。 */
    u32 LastTextureHeight = 0u;
    /** 全定数バッファへの更新回数。 */
    u32 BufferUpdates = 0u;
};

/** この直列CPU試験だけで使う、許可回数を超えた確保を拒否する確保元。 */
class ABakeContractBudgetAllocator final : public IAllocator {
public:
    /** 実確保元と読み戻し履歴を借用する。両者はこの確保元より長く生存させる。 */
    ABakeContractBudgetAllocator(IAllocator& backing, const ABakeContractDevice& device) noexcept : m_Backing(backing), m_Device(device) {}
    /** 次の呼出区間で許可する確保回数を設定し、観測値を初期化する。 */
    void SetBudget(u32 allowed_allocations) noexcept
    {
        m_Remaining = allowed_allocations;
        AllocationAttempts = 0u;
        AllocCalls = 0u;
        ReallocCalls = 0u;
        RejectedAllocations = 0u;
        LastRejectedBytes = 0u;
        SuccessfulReadsAtRejection = 0u;
    }
    /** 予算0なら実確保を呼ばず失敗する。再配置時の旧領域と新領域だけを追跡する。 */
    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override
    {
        ++AllocCalls;
        if (!ConsumeBudget_Internal(size)) return nullptr;
        // 配列の再配置では、旧領域を解放する前に新領域を確保する場合がある。
        for (u32 slot = 0u; slot < 2u; ++slot) {
            if (m_LivePointers[slot] != nullptr) continue;
            // 予算内の小さい要求だけを実確保元へ渡す。
            void* const pointer = m_Backing.Alloc(size, alignment, location);
            m_LivePointers[slot] = pointer;
            if (pointer != nullptr) ++LiveAllocations;
            return pointer;
        }
        ++InvalidLifetimeOperations;
        return nullptr;
    }
    /** 所有中の領域だけを再確保する。拒否した場合は旧領域をそのまま保持する。 */
    void* Realloc(void* pointer, usize old_size, usize new_size, usize alignment, FSourceLoc location) noexcept override
    {
        ++ReallocCalls;
        if (pointer == nullptr) return Alloc(new_size, alignment, location);
        if (new_size == 0u) { Free(pointer); return nullptr; }
        // 再確保対象の所有権を先に検査し、別の確保元の領域を渡さない。
        for (u32 slot = 0u; slot < 2u; ++slot) {
            if (m_LivePointers[slot] != pointer) continue;
            if (!ConsumeBudget_Internal(new_size)) return nullptr;
            // 実確保元が成功した場合だけ、追跡するアドレスを差し替える。
            void* const replacement = m_Backing.Realloc(pointer, old_size, new_size, alignment, location);
            if (replacement != nullptr) m_LivePointers[slot] = replacement;
            return replacement;
        }
        ++InvalidLifetimeOperations;
        return nullptr;
    }
    /** 所有中の領域だけを元へ返し、二重解放や別確保元の領域は拒否して記録する。 */
    void Free(void* pointer) noexcept override
    {
        if (pointer == nullptr) return;
        // 予算0でも正当な解放は許す。解放後のポインタは追跡集合から除く。
        for (u32 slot = 0u; slot < 2u; ++slot) {
            if (m_LivePointers[slot] != pointer) continue;
            m_Backing.Free(pointer);
            m_LivePointers[slot] = nullptr;
            --LiveAllocations;
            return;
        }
        ++InvalidLifetimeOperations;
    }

    /** 予算設定後に試みた確保回数。 */
    u32 AllocationAttempts = 0u;
    /** Alloc入口へ到達した回数。 */
    u32 AllocCalls = 0u;
    /** Realloc入口へ到達した回数。 */
    u32 ReallocCalls = 0u;
    /** 実確保を呼ぶ前に拒否した回数。 */
    u32 RejectedAllocations = 0u;
    /** 最後に拒否した要求のバイト数。 */
    usize LastRejectedBytes = 0u;
    /** 最後の拒否時点で既に成功していた読み戻し回数。 */
    u32 SuccessfulReadsAtRejection = 0u;
    /** この確保元が現在所有する領域数。配列破棄後は0でなければならない。 */
    u32 LiveAllocations = 0u;
    /** 二重解放、他の所有者の領域、試験の想定を超える同時確保を数える。 */
    u32 InvalidLifetimeOperations = 0u;

private:
    /** 要求を数え、許可回数を消費する。予算0では読み戻しとの順序を記録する。 */
    bool ConsumeBudget_Internal(usize size) noexcept
    {
        ++AllocationAttempts;
        if (m_Remaining == 0u) {
            ++RejectedAllocations;
            LastRejectedBytes = size;
            SuccessfulReadsAtRejection = m_Device.SuccessfulReads;
            return false;
        }
        --m_Remaining;
        return true;
    }
    /** 小さい通常確保と全解放を担当する、長寿命の確保元。 */
    IAllocator& m_Backing;
    /** 読み戻し成功と確保失敗の順序を観測するデバイス。 */
    const ABakeContractDevice& m_Device;
    /** これから許可する確保回数。 */
    u32 m_Remaining = 0u;
    /** 配列1個の旧領域と新領域だけを保持する、固定長の所有記録。 */
    void* m_LivePointers[2]{};
};

/** 既定の確保元を局所的に差し替え、途中で抜けても必ず元へ戻す。 */
class FScopedBakeContractDefaultAllocator final {
public:
    /** 差替え先は、この区間で確保する全領域の解放まで生存させる。 */
    explicit FScopedBakeContractDefaultAllocator(IAllocator& replacement) noexcept : m_Previous(&DefaultAllocator()) { SetDefaultAllocator(&replacement); }
    /** 以前の既定確保元を復元する。 */
    ~FScopedBakeContractDefaultAllocator() noexcept { SetDefaultAllocator(m_Previous); }
    /** 復元責任の重複を防ぐためコピーしない。 */
    FScopedBakeContractDefaultAllocator(const FScopedBakeContractDefaultAllocator&) = delete;
    /** 復元先の取り違えを防ぐため代入しない。 */
    FScopedBakeContractDefaultAllocator& operator=(const FScopedBakeContractDefaultAllocator&) = delete;

private:
    /** 差替え前の確保元。 */
    IAllocator* m_Previous;
};

/** 呼出時に画像を書く即時型の最小命令口。未提出命令やGPU同期は模擬しない。 */
class ABakeContractCommand : public IRhiCommandList {
public:
    /** 世代を共有するデバイスを借用する。デバイスを先に破棄してはならない。 */
    explicit ABakeContractCommand(ABakeContractDevice& device) noexcept : m_Device(device) {}
    /** 束縛を初期化するだけで、画像は生成しない。 */
    void Begin() noexcept override { m_Pipeline = nullptr; m_Output = nullptr; }
    /** 即時型なので未実行処理は残らない。 */
    void End() noexcept override {}
    /** 同期検証の代用にしない。提出では画像を生成しない。 */
    bool Submit() noexcept override { return true; }
    /** 画像生成の処理を選び、前の書込み先を失効させる。 */
    void SetComputePipeline(IRhiPipeline& pipeline) noexcept override
    {
        m_Pipeline = &static_cast<ABakeContractPipeline&>(pipeline);
        m_Output = nullptr;
    }
    /** 製品の画像出力スロット0だけを借用する。 */
    void BindUav(u32 slot, IRhiTexture& texture) noexcept override
    {
        if (slot == 0u) m_Output = &static_cast<ABakeContractTexture&>(texture);
    }
    /** 画像生成用のDispatchだけが書き込む。大気表の計算式は模擬しない。 */
    void Dispatch(u32 groups_x, u32 groups_y, u32 groups_z) noexcept override
    {
        if (m_Pipeline == nullptr || groups_x == 0u || groups_y == 0u || groups_z == 0u) return;
        RecordDispatch(m_Statistics);
        if (!m_Pipeline->IsBake() || m_Output == nullptr || !m_Output->IsTestImage()) return;
        if (groups_x < (kBakeWidth + 7u) / 8u || groups_y < (kBakeHeight + 7u) / 8u) return;
        ++m_Device.WrittenGeneration;
        m_Output->WriteFromDispatch_Internal(m_Device.WrittenGeneration);
    }
    /** 画面描画は今回の試験対象外。 */
    void BeginRenderToSwapchain(IRhiSwapchain&, u32, const FClearColor&, IRhiTexture*, f32) noexcept override {}
    /** 画面への追加描画は今回の試験対象外。 */
    void BeginRenderToSwapchainLoad(IRhiSwapchain&, u32) noexcept override {}
    /** 画面描画の終了は今回の試験対象外。 */
    void EndRenderToSwapchain(IRhiSwapchain&, u32) noexcept override {}
    /** 影の描画は今回の試験対象外。 */
    void BeginShadowPass(IRhiTexture&, f32) noexcept override {}
    /** 影の描画終了は今回の試験対象外。 */
    void EndShadowPass(IRhiTexture&) noexcept override {}
    /** 画面外描画は今回の試験対象外。 */
    void BeginRenderToTexture(IRhiTexture&, const FClearColor&, IRhiTexture*, f32) noexcept override {}
    /** 画面外描画の終了は今回の試験対象外。 */
    void EndRenderToTexture(IRhiTexture&) noexcept override {}
    /** 画面外への追加描画は今回の試験対象外。 */
    void BeginRenderToTextureLoad(IRhiTexture&, IRhiTexture*) noexcept override {}
    /** 配列画像の描画は今回の試験対象外。 */
    void BeginRenderToTextureSlice(IRhiTexture&, u32, u32, const FClearColor&) noexcept override {}
    /** 複数描画先は扱わず、未対応として拒否する。 */
    bool BeginRenderToTextureMrt(IRhiTexture* const*, u32, const FClearColor&, IRhiTexture*, f32) noexcept override { return false; }
    /** 複数描画先への追加描画も未対応として拒否する。 */
    bool BeginRenderToTextureMrtLoad(IRhiTexture* const*, u32, const FClearColor&, u32, IRhiTexture*, bool, f32) noexcept override { return false; }
    /** 複数描画先の終了は今回の試験対象外。 */
    void EndRenderToTextureMrt(IRhiTexture* const*, u32) noexcept override {}
    /** 描画範囲は今回の試験画像に影響させない。 */
    void SetViewport(const FViewport&) noexcept override {}
    /** 切り取り範囲は今回の試験画像に影響させない。 */
    void SetScissor(const FScissorRect&) noexcept override {}
    /** ステンシルによる描画制限は今回の試験対象外。 */
    void SetStencilRef(u32) noexcept override {}
    /** 通常描画では試験画像を書かない。 */
    void SetPipeline(IRhiPipeline&) noexcept override { m_Pipeline = nullptr; }
    /** 頂点は今回の試験対象外。 */
    void SetVertexBuffer(IRhiBuffer&, u32) noexcept override {}
    /** 頂点番号は今回の試験対象外。 */
    void SetIndexBuffer(IRhiBuffer&) noexcept override {}
    /** 定数値の物理的な意味は今回の試験対象外。 */
    void SetConstantBuffer(u32, IRhiBuffer&) noexcept override {}
    /** 参照表の数値と遮蔽判定は今回の試験対象外。 */
    void SetTexture(u32, IRhiTexture&) noexcept override {}
    /** 通常描画から画像を生成しない。 */
    void Draw(u32, u32) noexcept override {}
    /** 番号付き描画からも画像を生成しない。 */
    void DrawIndexed(u32, u32, i32) noexcept override {}
    /** 実バックエンドへ接続するハンドルを持たない。 */
    void* NativeHandle() noexcept override { return nullptr; }

private:
    /** この命令口自身の統計を返す。 */
    FRhiCommandStatistics& StatisticsStorage() noexcept override { return m_Statistics; }
    /** この命令口自身の統計を読み取る。 */
    const FRhiCommandStatistics& StatisticsStorage() const noexcept override { return m_Statistics; }
    /** 命令口より長く生きる試験デバイス。 */
    ABakeContractDevice& m_Device;
    /** 現在選ばれた処理。資源の所有権は製品の大気オブジェクトが持つ。 */
    ABakeContractPipeline* m_Pipeline = nullptr;
    /** 現在の書込み先。資源の所有権は製品の大気オブジェクトが持つ。 */
    ABakeContractTexture* m_Output = nullptr;
    /** 他の命令口と共有しない統計。 */
    FRhiCommandStatistics m_Statistics{};
};

/** 既存出力を小さい既知値で用意する。通常の確保失敗時はfalseを返す。 */
bool PrepareOriginalOutput_Internal(TArray<f32>& output) noexcept
{
    if (!output.TrySetNum(kOriginalElements)) return false;
    // 変更前の全要素を、生成画像と重ならない値にする。
    for (usize index = 0u; index < kOriginalElements; ++index) output[index] = kOriginalOutput[index];
    return true;
}

/** 失敗後の値・サイズ・確保元が、呼出前と同じかを検査する。 */
void ExpectOriginalOutput_Internal(const TArray<f32>& output, IAllocator& allocator) noexcept
{
    EXPECT_EQ(output.Num(), kOriginalElements);
    EXPECT_TRUE(output.GetAllocator() == &allocator);
    // サイズ検査が失敗しても試験自体が範囲外へアクセスしないようにする。
    for (usize index = 0u; index < kOriginalElements && index < output.Num(); ++index) EXPECT_EQ(output[index], kOriginalOutput[index]);
}

/** 指定世代の全RGBA値・サイズ・確保元が揃っていることを検査する。 */
void ExpectGeneratedOutput_Internal(const TArray<f32>& output, IAllocator& allocator, u32 generation) noexcept
{
    EXPECT_EQ(output.Num(), static_cast<usize>(kBakeElements));
    EXPECT_TRUE(output.GetAllocator() == &allocator);
    // サイズ不一致で失敗しても、検査側は範囲外へアクセスしない。
    for (u32 index = 0u; index < kBakeElements && index < output.Num(); ++index) {
        EXPECT_EQ(output[index], static_cast<f32>(generation * 1024u + index + 1u));
    }
}

} // namespace

/** 専用試験の偽シェーダを作る。実コンパイルをせず、確保失敗はエラーで返す。 */
TResult<TUniquePtr<IRhiShader>> CreateRhiShader(IRhiDevice&, const FShaderDesc& description) noexcept
{
    // 入口名と種類だけを保存する所有資源。
    auto resource = MakeUnique<ABakeContractShader>(description);
    if (!resource) return ACS_ERR(Memory, 990, "bake contract shader allocation failed");
    // 仮想破棄と確保元を保ったまま、製品が受け取る型へ所有権を移す。
    TUniquePtr<IRhiShader> base(resource.Release(), resource.GetAllocator());
    return TResult<TUniquePtr<IRhiShader>>(OkInit, Move(base));
}

/** 専用試験の計算処理を作る。入口を渡さない指定は拒否する。 */
TResult<TUniquePtr<IRhiPipeline>> CreateRhiComputePipeline(IRhiDevice&, const FComputePipelineDesc& description) noexcept
{
    if (description.cs == nullptr) return ACS_ERR(Render, 991, "bake contract compute shader missing");
    // この実行ファイルの偽生成関数が作ったシェーダだけを受け取る。
    const auto& shader = static_cast<const ABakeContractShader&>(*description.cs);
    // 画像生成の役割だけを保存する所有資源。
    auto resource = MakeUnique<ABakeContractPipeline>(shader.IsBake());
    if (!resource) return ACS_ERR(Memory, 992, "bake contract compute pipeline allocation failed");
    // 確保元を保持して所有権を製品へ渡す。
    TUniquePtr<IRhiPipeline> base(resource.Release(), resource.GetAllocator());
    return TResult<TUniquePtr<IRhiPipeline>>(OkInit, Move(base));
}

/** 大気の初期化が要求する通常描画用の処理を作る。画像生成には使わない。 */
TResult<TUniquePtr<IRhiPipeline>> CreateRhiPipeline(IRhiDevice&, const FPipelineDesc&) noexcept
{
    // 通常描画用は試験画像を書かない。
    auto resource = MakeUnique<ABakeContractPipeline>(false);
    if (!resource) return ACS_ERR(Memory, 993, "bake contract graphics pipeline allocation failed");
    // 確保元を保持して所有権を製品へ渡す。
    TUniquePtr<IRhiPipeline> base(resource.Release(), resource.GetAllocator());
    return TResult<TUniquePtr<IRhiPipeline>>(OkInit, Move(base));
}

/** 初期化用の偽バッファを作る。大きな実バッファは確保しない。 */
TResult<TUniquePtr<IRhiBuffer>> CreateRhiBuffer(IRhiDevice& device, const FBufferDesc& description) noexcept
{
    // 製品のバッファ寸法と用途だけを保持する。
    auto resource = MakeUnique<ABakeContractBuffer>(description, static_cast<ABakeContractDevice&>(device).BufferUpdates);
    if (!resource) return ACS_ERR(Memory, 994, "bake contract buffer allocation failed");
    // 確保元を保持して所有権を製品へ渡す。
    TUniquePtr<IRhiBuffer> base(resource.Release(), resource.GetAllocator());
    return TResult<TUniquePtr<IRhiBuffer>>(OkInit, Move(base));
}

/** 小さい試験画像の保存先を持つ偽画像を作る。大気の体積画像は実確保しない。 */
TResult<TUniquePtr<IRhiTexture>> CreateRhiTexture(IRhiDevice& device, const FTextureDesc& description) noexcept
{
    // この専用実行ファイルだけで使う偽デバイス。
    auto& test_device = static_cast<ABakeContractDevice&>(device);
    // 確保に失敗しても生成要求を数え、一時配列の失敗との取り違えを防ぐ。
    ++test_device.TextureFactoryCalls;
    test_device.LastTextureWidth = description.width;
    test_device.LastTextureHeight = description.height;
    // 製品の事前検査が欠けていても、先行試験はここで止まり巨大領域を確保しない。
    if (test_device.RejectTextureCreation) return ACS_ERR(Render, 996, "bake contract texture creation rejected");
    // 生成直後は世代0であり、ReadTextureから成功画像を得ることはできない。
    auto resource = MakeUnique<ABakeContractTexture>(description);
    if (!resource) return ACS_ERR(Memory, 995, "bake contract texture allocation failed");
    // 確保元を保持して所有権を製品へ渡す。
    TUniquePtr<IRhiTexture> base(resource.Release(), resource.GetAllocator());
    return TResult<TUniquePtr<IRhiTexture>>(OkInit, Move(base));
}

ACS_TEST(AtmosphereBakeContract, UninitializedFailurePreservesOutput)
{
    // 出力専用の確保元。配列が解放されるまで生存する。
    CSystemAllocator output_allocator;
    // 製品資源と命令口より長く生存する偽デバイス。
    ABakeContractDevice device;
    // 未初期化の製品オブジェクト。
    CSkyAtmosphere atmosphere;
    // 即時型の命令口。ここでは何も書かれないことを確認する。
    ABakeContractCommand command(device);
    // 利用者が既に持っている出力。
    TArray<f32> output(output_allocator);
    // 小さい既存出力の確保結果。
    const bool prepared = PrepareOriginalOutput_Internal(output);
    EXPECT_TRUE(prepared);
    if (!prepared) return;
    // 出力配列の保証だけが対象なので、物理パラメータは既定値を使う。
    const FAtmosphereParams parameters{};
    EXPECT_FALSE(atmosphere.BakeEquirectAtAltitude(device, command, parameters, kBakeWidth, kBakeHeight, 2.0f, output));
    ExpectOriginalOutput_Internal(output, output_allocator);
    EXPECT_EQ(device.ReadCalls, 0u);
    EXPECT_EQ(device.WrittenGeneration, 0u);
}

ACS_TEST(AtmosphereBakeContract, PartialReadFailurePreservesOutput)
{
    // 既定の確保元とは別の出力用確保元。取り違えも検出する。
    CSystemAllocator output_allocator;
    // 資源より長く生きる即時型の偽デバイス。
    ABakeContractDevice device;
    // 実際の製品関数を、偽資源だけで初期化する。
    CSkyAtmosphere atmosphere;
    // 製品へ渡す即時型の命令口。
    ABakeContractCommand command(device);
    // 失敗時に維持すべき既存出力。
    TArray<f32> output(output_allocator);
    // 出力を小さい既知値で準備した結果。
    const bool prepared = PrepareOriginalOutput_Internal(output);
    EXPECT_TRUE(prepared);
    if (!prepared) return;
    EXPECT_TRUE(output.GetAllocator() != &DefaultAllocator());
    // 初期化自体の失敗で読み戻し失敗試験が誤合格しないようにする。
    const auto initialized = atmosphere.Init(device);
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;
    device.FailAfterPartialWrite = true;
    // 出力契約に影響しない既定の物理パラメータ。
    const FAtmosphereParams parameters{};
    command.Begin();
    EXPECT_FALSE(atmosphere.BakeEquirectAtAltitude(device, command, parameters, kBakeWidth, kBakeHeight, 2.0f, output));
    command.End();
    EXPECT_EQ(device.WrittenGeneration, 1u);
    EXPECT_EQ(device.ReadCalls, 1u);
    EXPECT_EQ(device.LastCopiedElements, kPartialWriteElements);
    EXPECT_EQ(device.LastDestinationSize, kBakeTransferBytes);
    // outを直接拡張して読み戻す実装では、サイズと先頭値の上書きを検出する。
    ExpectOriginalOutput_Internal(output, output_allocator);
}

ACS_TEST(AtmosphereBakeContract, SuccessfulReadsPreserveAllocatorAndReplaceWholeImage)
{
    // 成功時にも維持すべき、利用者指定の確保元。
    CSystemAllocator output_allocator;
    // 全資源より長く生きる即時型の偽デバイス。
    ABakeContractDevice device;
    // 製品の画像生成関数と資源所有をそのまま通す。
    CSkyAtmosphere atmosphere;
    // 即時型の命令口。提出順の合否判定には使わない。
    ABakeContractCommand command(device);
    // 一度目はサイズ変更、二度目は同じサイズへの全要素更新となる出力。
    TArray<f32> output(output_allocator);
    // 生成画像と異なる既存出力を準備した結果。
    const bool prepared = PrepareOriginalOutput_Internal(output);
    EXPECT_TRUE(prepared);
    if (!prepared) return;
    EXPECT_TRUE(output.GetAllocator() != &DefaultAllocator());
    // 偽生成関数を経由した製品初期化の結果。
    const auto initialized = atmosphere.Init(device);
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;
    // 出力契約に影響しない既定の物理パラメータ。
    const FAtmosphereParams parameters{};
    // 一度目の成功画像を返し続ける不備を、二度目の全要素比較でも検出する。
    for (u32 generation = 1u; generation <= 2u; ++generation) {
        command.Begin();
        EXPECT_TRUE(atmosphere.BakeEquirectAtAltitude(device, command, parameters, kBakeWidth, kBakeHeight, 2.0f, output));
        command.End();
        EXPECT_EQ(device.WrittenGeneration, generation);
        EXPECT_EQ(device.ReadCalls, generation);
        EXPECT_EQ(device.LastCopiedElements, kBakeElements);
        EXPECT_EQ(device.LastDestinationSize, kBakeTransferBytes);
        EXPECT_EQ(output.Num(), static_cast<usize>(kBakeElements));
        EXPECT_TRUE(output.GetAllocator() == &output_allocator);
        // 画像用Dispatchが書く世代別の全RGBA値を、欠落なく検査する。
        for (u32 index = 0u; index < kBakeElements && index < output.Num(); ++index) {
            EXPECT_EQ(output[index], static_cast<f32>(generation * 1024u + index + 1u));
        }
    }
}

ACS_TEST(AtmosphereBakeContract, OutputGrowthFailureAfterSuccessfulReadPreservesOutput)
{
    // falseは空配列へのAlloc、trueは容量を使い切った既存配列へのRealloc。
    constexpr bool reallocation_cases[] = {false, true};
    // 失敗の入口だけを変え、寸法や読み戻し内容は同じにする。
    for (bool use_realloc : reallocation_cases) {
        // 出力配列と失敗注入器より長く生きる、小さい通常確保の所有者。
        CSystemAllocator backing_allocator;
        // 失敗注入器が参照する読み戻し履歴。
        ABakeContractDevice device;
        // 出力だけを拒否し、一時配列や偽GPU資源の確保には影響させない。
        ABakeContractBudgetAllocator output_allocator(backing_allocator, device);
        // 初期化と画像生成は本物の製品関数を通す。
        CSkyAtmosphere atmosphere;
        // 即時型で生成結果を記録する命令口。
        ABakeContractCommand command(device);
        // 読み戻し失敗とは独立に、初期化が成功したことを先に確認する。
        const auto initialized = atmosphere.Init(device);
        EXPECT_TRUE(initialized.IsOk());
        if (initialized.IsErr()) return;
        {
            // この区間末尾で出力を破棄し、元の確保元へ戻ったことまで検査する。
            TArray<f32> output(output_allocator);
            if (use_realloc) {
                output_allocator.SetBudget(1u);
                // TrySetNumだけでは容量8になるため、先に容量を正確に5へ指定する。
                const bool reserved = output.TryReserve(kOriginalElements);
                EXPECT_TRUE(reserved);
                if (!reserved) return;
                // 追加の確保なしで、容量5の全要素を初期値で埋める。
                const bool prepared = PrepareOriginalOutput_Internal(output);
                EXPECT_TRUE(prepared);
                if (!prepared) return;
                EXPECT_EQ(output.Num(), kOriginalElements);
                EXPECT_EQ(output.Max(), output.Num());
                if (output.Max() != output.Num()) return;
            }
            EXPECT_TRUE(output.Max() < kBakeElements);
            if (output.Max() >= kBakeElements) return;
            EXPECT_TRUE(output.GetAllocator() != &DefaultAllocator());
            output_allocator.SetBudget(0u);
            // 数式は試験対象外なので既定の物理パラメータを使う。
            const FAtmosphereParams parameters{};
            command.Begin();
            // 読み戻し成功後のout.TrySetNumだけを失敗させる。
            const bool baked = atmosphere.BakeEquirectAtAltitude(device, command, parameters, kBakeWidth, kBakeHeight, 2.0f, output);
            command.End();
            EXPECT_FALSE(baked);
            EXPECT_EQ(device.WrittenGeneration, 1u);
            EXPECT_EQ(device.ReadCalls, 1u);
            EXPECT_EQ(device.SuccessfulReads, 1u);
            EXPECT_EQ(device.LastCopiedElements, kBakeElements);
            EXPECT_EQ(device.LastDestinationSize, kBakeTransferBytes);
            EXPECT_EQ(output_allocator.AllocationAttempts, 1u);
            EXPECT_EQ(output_allocator.AllocCalls, use_realloc ? 0u : 1u);
            EXPECT_EQ(output_allocator.ReallocCalls, use_realloc ? 1u : 0u);
            EXPECT_EQ(output_allocator.RejectedAllocations, 1u);
            EXPECT_TRUE(output_allocator.LastRejectedBytes >= kBakeTransferBytes);
            EXPECT_EQ(output_allocator.SuccessfulReadsAtRejection, 1u);
            EXPECT_TRUE(output.GetAllocator() == &output_allocator);
            if (use_realloc) ExpectOriginalOutput_Internal(output, output_allocator);
            else EXPECT_EQ(output.Num(), usize{0});
        }
        EXPECT_EQ(output_allocator.LiveAllocations, 0u);
        EXPECT_EQ(output_allocator.InvalidLifetimeOperations, 0u);
        EXPECT_EQ(backing_allocator.AllocationCount(), u64{0});
    }
}

ACS_TEST(AtmosphereBakeContract, TemporaryAllocationFailureNeverReadsOrChangesOutput)
{
    // 利用者出力は既定確保元の差替えに巻き込まない。
    CSystemAllocator output_allocator;
    // 失敗注入器と製品資源より長く生きるデバイス。
    ABakeContractDevice device;
    // 差替えを戻す先。失敗注入器の実確保元としても借用する。
    IAllocator* const previous_default = &DefaultAllocator();
    // 予算0なので実確保は行わず、最初の一時配列確保を拒否する。
    ABakeContractBudgetAllocator temporary_allocator(*previous_default, device);
    // 同じ製品オブジェクトを準備・失敗試験に継続して使う。
    CSkyAtmosphere atmosphere;
    // 即時型の命令口。
    ABakeContractCommand command(device);
    // 画像資源のキャッシュを準備するための捨て出力。
    TArray<f32> warmup_output(output_allocator);
    // 本試験で維持する、サイズの異なる利用者出力。
    TArray<f32> output(output_allocator);
    // 初期化が失敗した場合は、確保失敗試験を誤合格させない。
    const auto initialized = atmosphere.Init(device);
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;
    // 準備と失敗試験で同じ寸法・物理パラメータを使う。
    const FAtmosphereParams parameters{};
    command.Begin();
    // 画像資源の初回確保は、既定確保元を差し替える前に済ませる。
    const bool warmed = atmosphere.BakeEquirectAtAltitude(device, command, parameters, kBakeWidth, kBakeHeight, 2.0f, warmup_output);
    command.End();
    EXPECT_TRUE(warmed);
    if (!warmed) return;
    ExpectGeneratedOutput_Internal(warmup_output, output_allocator, 1u);
    EXPECT_EQ(device.ReadCalls, 1u);
    EXPECT_EQ(device.SuccessfulReads, 1u);
    // 失敗区間で画像の生成関数へ再入していないことを確認する基準値。
    const u32 cached_texture_calls = device.TextureFactoryCalls;
    // 利用者出力も失敗注入より前に確保しておく。
    const bool prepared = PrepareOriginalOutput_Internal(output);
    EXPECT_TRUE(prepared);
    if (!prepared) return;
    temporary_allocator.SetBudget(0u);
    // 差替え区間では検査ログを出さず、結果だけを持ち帰る。
    bool baked = true;
    {
        // この区間を抜けた時点で、成功・失敗にかかわらず既定確保元を戻す。
        FScopedBakeContractDefaultAllocator allocator_scope(temporary_allocator);
        command.Begin();
        baked = atmosphere.BakeEquirectAtAltitude(device, command, parameters, kBakeWidth, kBakeHeight, 2.0f, output);
        command.End();
    }
    EXPECT_TRUE(&DefaultAllocator() == previous_default);
    EXPECT_FALSE(baked);
    EXPECT_EQ(temporary_allocator.AllocationAttempts, 1u);
    EXPECT_EQ(temporary_allocator.RejectedAllocations, 1u);
    EXPECT_TRUE(temporary_allocator.LastRejectedBytes >= kBakeTransferBytes);
    EXPECT_EQ(temporary_allocator.SuccessfulReadsAtRejection, 1u);
    EXPECT_EQ(device.TextureFactoryCalls, cached_texture_calls);
    EXPECT_EQ(device.ReadCalls, 1u);
    EXPECT_EQ(device.SuccessfulReads, 1u);
    ExpectOriginalOutput_Internal(output, output_allocator);
    EXPECT_EQ(temporary_allocator.AllocCalls, 1u);
    EXPECT_EQ(temporary_allocator.ReallocCalls, 0u);
    EXPECT_EQ(temporary_allocator.LiveAllocations, 0u);
    EXPECT_EQ(temporary_allocator.InvalidLifetimeOperations, 0u);
}

ACS_TEST(AtmosphereBakeContract, WrapperPreservesSameSizeImageAcrossPartialFailureAndRetry)
{
    // 初回成功で拡張・同サイズ・縮小となる、出力配列だけのサイズ。画像寸法は変えない。
    constexpr usize initial_sizes[] = {kOriginalElements, kBakeElements, kBakeElements + kOriginalElements};
    // 同じ公開入口と失敗・復帰の流れを、三つの出力サイズで共用する。
    for (usize initial_size : initial_sizes) {
        // 出力領域の本来の所有者。配列破棄後の生存件数も検査する。
        CSystemAllocator backing_allocator;
        // 世代ごとの画像を作る即時型の偽デバイス。
        ABakeContractDevice device;
        // 準備1回と拡張1回だけを許し、解放先を追跡する出力専用確保元。
        ABakeContractBudgetAllocator output_allocator(backing_allocator, device);
        output_allocator.SetBudget(2u);
        // 高度を省略する公開関数も同じ製品オブジェクトで反復する。
        CSkyAtmosphere atmosphere;
        // 出力保証だけを検査し、提出や完了待ちは判定しない。
        ABakeContractCommand command(device);
        // 製品資源の準備結果。
        const auto initialized = atmosphere.Init(device);
        EXPECT_TRUE(initialized.IsOk());
        if (initialized.IsErr()) return;
        {
            // この区間末尾で解放し、二重解放と未解放も確認する。
            TArray<f32> output(output_allocator);
            // 初回成功に対する、拡張・同サイズ・縮小の開始状態を作る。
            const bool prepared = output.TrySetNum(initial_size);
            EXPECT_TRUE(prepared);
            if (!prepared) return;
            EXPECT_TRUE(output.GetAllocator() != &DefaultAllocator());
            // 未更新の要素が残れば全画像比較で検出できる、異なる初期値。
            for (usize index = 0u; index < output.Num(); ++index) output[index] = -100.0f;
            // 画像の保存契約だけが対象なので物理パラメータは既定値。
            const FAtmosphereParams parameters{};
            // 成功、部分書込み後の失敗、成功の順で同じ薄い入口を通す。
            for (u32 attempt = 0u; attempt < 3u; ++attempt) {
                device.FailAfterPartialWrite = attempt == 1u;
                command.Begin();
                // 失敗時には、直前に成功した全画像が残らなければならない。
                const bool baked = atmosphere.BakeEquirect(device, command, parameters, kBakeWidth, kBakeHeight, output);
                command.End();
                EXPECT_EQ(baked, attempt != 1u);
                EXPECT_EQ(device.WrittenGeneration, attempt + 1u);
                EXPECT_EQ(device.ReadCalls, attempt + 1u);
                EXPECT_EQ(device.SuccessfulReads, attempt < 2u ? 1u : 2u);
                EXPECT_EQ(device.LastCopiedElements, attempt == 1u ? kPartialWriteElements : kBakeElements);
                EXPECT_EQ(device.LastDestinationSize, kBakeTransferBytes);
                // 二度目は生成世代2ではなく、直前に成功した世代1を保持する。
                const u32 expected_generation = attempt == 1u ? 1u : attempt + 1u;
                ExpectGeneratedOutput_Internal(output, output_allocator, expected_generation);
            }
        }
        EXPECT_EQ(output_allocator.RejectedAllocations, 0u);
        EXPECT_EQ(output_allocator.LiveAllocations, 0u);
        EXPECT_EQ(output_allocator.InvalidLifetimeOperations, 0u);
        EXPECT_EQ(backing_allocator.AllocationCount(), u64{0});
    }
}

ACS_TEST(AtmosphereBakeContract, TrackedAllocationsCoverExistingOutputAndTemporaryCleanup)
{
    // 容量に余りがある配列のAllocと、容量を使い切った配列のReallocを分ける。
    constexpr bool packed_cases[] = {false, true};
    // 同じ既存画像から、成功・読み戻し失敗・出力確保失敗をそれぞれ検査する。
    for (bool packed : packed_cases) {
        // 0は成功、1は読み戻し途中の失敗、2は読み戻し後の出力拡張失敗。
        for (u32 failure_stage = 0u; failure_stage < 3u; ++failure_stage) {
            // 出力専用の実確保元。最後に全領域が戻ったことを確認する。
            CSystemAllocator backing_allocator;
            // 各確保元より長く生きる、生成と読み戻しの履歴。
            ABakeContractDevice device;
            // 利用者出力の新規確保・再確保・解放先を追跡する。
            ABakeContractBudgetAllocator output_allocator(backing_allocator, device);
            // 一時配列を本当に確保させ、関数終了時の解放まで追跡する。
            ABakeContractBudgetAllocator temporary_allocator(DefaultAllocator(), device);
            // 差替え前の既定確保元。準備用出力はこの元を保持する。
            IAllocator* const previous_default = &DefaultAllocator();
            // 製品の初期化と画像資源の準備を失敗注入の前に済ませる。
            CSkyAtmosphere atmosphere;
            // 即時実行で画像を書く命令口。
            ABakeContractCommand command(device);
            // 同じ寸法の画像資源を準備するためだけの出力。
            TArray<f32> warmup_output;
            // 出力契約に影響しない既定の物理パラメータ。
            const FAtmosphereParams parameters{};
            // 実際に製品資源の準備が成功したことを確認する。
            const auto initialized = atmosphere.Init(device);
            EXPECT_TRUE(initialized.IsOk());
            if (initialized.IsErr()) return;
            command.Begin();
            // この後は画像の生成関数を呼ばず、一時配列だけを既定確保元から確保する。
            const bool warmed = atmosphere.BakeEquirect(device, command, parameters, kBakeWidth, kBakeHeight, warmup_output);
            command.End();
            EXPECT_TRUE(warmed);
            if (!warmed) return;
            // 試験区間で画像が再確保されないことを確認する基準値。
            const u32 cached_texture_calls = device.TextureFactoryCalls;
            {
                // この区間末尾で利用者出力を解放し、出力側の寿命も確認する。
                TArray<f32> output(output_allocator);
                output_allocator.SetBudget(1u);
                // 容量5または8を明示し、同じ5要素から異なる確保分岐へ進める。
                const bool reserved = output.TryReserve(packed ? kOriginalElements : kOriginalElements + 3u);
                EXPECT_TRUE(reserved);
                if (!reserved) return;
                // どちらの確保分岐でも失敗時に維持すべき同じ既存値。
                const bool prepared = PrepareOriginalOutput_Internal(output);
                EXPECT_TRUE(prepared);
                if (!prepared) return;
                EXPECT_EQ(output.Num() == output.Max(), packed);
                output_allocator.SetBudget(failure_stage == 2u ? 0u : 1u);
                temporary_allocator.SetBudget(1u);
                device.FailAfterPartialWrite = failure_stage == 1u;
                // 既定確保元の差替え中は検査ログを出さず、結果だけを保存する。
                bool baked = false;
                {
                    // 一時配列以外の生成が紛れれば、予算または所有記録の検査で失敗する。
                    FScopedBakeContractDefaultAllocator allocator_scope(temporary_allocator);
                    command.Begin();
                    baked = atmosphere.BakeEquirect(device, command, parameters, kBakeWidth, kBakeHeight, output);
                    command.End();
                }
                EXPECT_TRUE(&DefaultAllocator() == previous_default);
                EXPECT_EQ(baked, failure_stage == 0u);
                EXPECT_EQ(device.TextureFactoryCalls, cached_texture_calls);
                EXPECT_EQ(device.WrittenGeneration, 2u);
                EXPECT_EQ(device.ReadCalls, 2u);
                EXPECT_EQ(device.SuccessfulReads, failure_stage == 1u ? 1u : 2u);
                EXPECT_EQ(device.LastCopiedElements, failure_stage == 1u ? kPartialWriteElements : kBakeElements);
                EXPECT_EQ(device.LastDestinationSize, kBakeTransferBytes);
                EXPECT_EQ(temporary_allocator.AllocCalls, 1u);
                EXPECT_EQ(temporary_allocator.RejectedAllocations, 0u);
                EXPECT_EQ(temporary_allocator.LiveAllocations, 0u);
                EXPECT_EQ(temporary_allocator.InvalidLifetimeOperations, 0u);
                EXPECT_EQ(output_allocator.AllocCalls, failure_stage != 1u && !packed ? 1u : 0u);
                EXPECT_EQ(output_allocator.ReallocCalls, failure_stage != 1u && packed ? 1u : 0u);
                EXPECT_EQ(output_allocator.RejectedAllocations, failure_stage == 2u ? 1u : 0u);
                if (failure_stage == 0u) ExpectGeneratedOutput_Internal(output, output_allocator, 2u);
                else ExpectOriginalOutput_Internal(output, output_allocator);
            }
            EXPECT_EQ(output_allocator.LiveAllocations, 0u);
            EXPECT_EQ(output_allocator.InvalidLifetimeOperations, 0u);
            EXPECT_EQ(backing_allocator.AllocationCount(), u64{0});
        }
    }
}

#include "atmosphere_bake_size_tests.inl"
#include "atmosphere_bake_execution_tests.inl"

} // namespace acs
