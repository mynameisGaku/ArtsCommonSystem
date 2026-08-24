/* ACS リファレンス — render（描画コア）モジュール。手書き。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "render_core", order: 30, title: "render — 描画(コア)",
  blurb: "GPU に絵を描くための土台。バックエンド(DX12 / Diligent)の違いを隠す低レベルな <t>RHI</t> 抽象と、その上に乗る「すぐ描ける」高レベルヘルパ(標準シェーダ・<t>スプライト</t>・フォント・空・<t>ポストプロセス</t>等)をまとめて提供します。",
  types: [
    {
      name: "FRenderGraphResourceLifetime",
      kind: "構造体", header: "render/RenderGraphResourceLifetime.h",
      summary: "render graph 資源が最初と最後に使われる pass 位置、および alias 可否を保持する寿命区間。",
      when: "一時 GPU 資源の占有期間が重ならないか解析する時。"
    },
    {
      name: "FRenderGraphAliasAssignment",
      kind: "構造体", header: "render/RenderGraphAliasAssignment.h",
      summary: "論理資源を再利用可能な物理 slot へ割り当てた結果。",
      when: "transient 資源の実割り当てと peak 使用量を確認する時。"
    },
    {
      name: "FRenderGraphAliasPlanSummary",
      kind: "構造体", header: "render/RenderGraphAliasPlanSummary.h",
      summary: "論理資源数、物理 slot 数、alias で節約した byte 数をまとめた計画診断値。",
      when: "render graph aliasing の効果を profiler やテストへ出す時。"
    },
    {
      name: "CRenderGraphTransientAliasPlanner",
      kind: "クラス", header: "render/RenderGraphTransientAlias.h",
      summary: "互換性と寿命区間を基に transient 資源を物理 slot へ決定的に割り当てる planner。",
      when: "同時に使わない render target や buffer の GPU メモリを再利用する時。",
      members: [
        { sig: "using FRenderGraphTransientAliasPlanner = CRenderGraphTransientAliasPlanner", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CRenderGraphTransientAliasPlanner</code> を使う。" }
      ]
    },
    {
      name: "CRenderer",
      kind: "クラス", header: "render/Renderer.h",
      summary: "ウィンドウへの描画ループを統括する<b>司令塔</b>。<t>デバイス</t>・<t>スワップチェイン</t>・<t>コマンドリスト</t>・深度バッファをまとめて初期化し、毎フレームの「開始→描く→終了」を 1 つにまとめる。",
      when: "とりあえず 3D / 2D を画面に出したいとき最初に作るもの。低レベル RHI を自分で組み立てる前の、いちばん簡単な入口。",
      members: [
        { sig: "TResult&lt;void&gt; Init(FWindow& w, bool enable_debug = false, bool enable_depth = true)", ret: "成功/失敗", desc: "ウィンドウに紐付けて初期化。<code>enable_depth=true</code> で深度バッファ(D32_Float)も自動作成。", when: "ウィンドウ作成直後に 1 回。" },
        { sig: "void BeginFrame(const FClearColor& clear)", desc: "フレーム開始。バックバッファをクリア色で塗り、深度を 1.0 でクリアする。" },
        { sig: "bool EndFrame()", ret: "Submit と Present の両方が成功したとき true", desc: "フレーム終了。積んだコマンドを GPU に投入して画面に <t>Present</t> する。device removal や提示失敗は false。" },
        { sig: "bool OnResize(u32 width, u32 height)", ret: "スワップチェインと深度の再作成に成功したとき true", desc: "ウィンドウサイズ変更時に一度だけ GPU idle を待ち、<t>スワップチェイン</t>と深度を作り直す。失敗時は false。" },
        { sig: "IRhiCommandList* CommandList() const", ret: "コマンドリスト", desc: "<code>BeginFrame</code> 後、この<t>コマンドリスト</t>に描画命令を積む。" },
        { sig: "IRhiDevice* Device() / IRhiSwapchain* Swapchain() / IRhiTexture* DepthBuffer()", desc: "内部リソースへのアクセサ。シェーダ等の Init に渡す。" },
        { sig: "EFormat ColorFormat() / EFormat DepthFormat()", ret: "フォーマット", desc: "描画ターゲットの<t>フォーマット</t>。パイプライン/シェーダ作成時に渡す。" },
        { sig: "u32 CurrentBuffer() const", ret: "バックバッファ番号", desc: "現在 <code>BeginFrame</code> で取得したバックバッファ index。反射等のマルチパスで再 bind する際に使う。" },
        { sig: "void Shutdown()", desc: "全リソースを解放する。" },
        { sig: "using FRenderer = CRenderer", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CRenderer</code> を使う。" }
      ]
    },
    {
      name: "EFormat",
      kind: "列挙(enum)", header: "render/RhiTypes.h",
      summary: "<t>ピクセルフォーマット</t>(1 ピクセルのビット配置)を表す論理的な列挙。DX12 / Vulkan で共通の名前で扱える。",
      when: "テクスチャ・レンダーターゲット・頂点要素のデータ形式を指定するとき。",
      members: [
        { sig: "R8G8B8A8_UNorm / _sRGB / _UInt", desc: "8bit×4ch。画像の標準。sRGB は <t>ガンマ</t>解釈、UInt は整数(ボーン index 等)。" },
        { sig: "B8G8R8A8_UNorm", desc: "BGRA 配置。Win32 ネイティブのバックバッファ既定。" },
        { sig: "R16G16B16A16_Float", desc: "<t>HDR</t> レンダーターゲット用の半精度 4ch。" },
        { sig: "R11G11B10_Float / R16G16_Float", desc: "32bit パックの省メモリ HDR(環境/IBL 向け)、2ch 半精度(BRDF LUT 等)。" },
        { sig: "R32G32_Float / R32G32B32_Float / R32G32B32A32_Float", desc: "2D/3D/4D の float 座標バッファ用。" },
        { sig: "D32_Float / D24_UNorm_S8_UInt", desc: "深度のみ / 深度+<t>ステンシル</t>。" },
        { sig: "Unknown", desc: "未指定。深度なしパイプライン等で「無効」を意味する。" }
      ]
    },
    {
      name: "FViewport / FScissorRect / FClearColor",
      kind: "構造体", header: "render/RhiTypes.h",
      summary: "描画の基本パラメータ。<b>FViewport</b>=描画する画面領域(x,y,幅,高さ,深度範囲)、<b>FScissorRect</b>=この矩形外を捨てる<t>シザー</t>、<b>FClearColor</b>=塗りつぶし色(RGBA 各 0..1)。",
      when: "画面の一部だけに描く・分割描画する・クリア色を指定するとき。",
      members: [
        { sig: "FViewport{ x, y, width, height, min_depth=0, max_depth=1 }", desc: "描画先の矩形と深度の出力範囲。" },
        { sig: "FScissorRect{ left, top, right, bottom }", desc: "この矩形の外側のピクセルを描かない。" },
        { sig: "FClearColor{ r, g, b, a }", desc: "クリア色。各成分 0..1。" }
      ]
    },
    {
      name: "EPrimitiveTopology / EResourceState",
      kind: "列挙(enum)", header: "render/RhiTypes.h",
      summary: "<b>EPrimitiveTopology</b>=頂点をどう繋いで図形にするか(点/線/三角形列)。<b>EResourceState</b>=GPU リソースが今どんな用途状態かを表す抽象(DX12/Vulkan の<t>バリア</t>共通化)。",
      when: "<code>EPrimitiveTopology</code> はパイプライン作成時に。<code>EResourceState</code> は普段は内部で自動管理され、手で触ることはまれ。",
      members: [
        { sig: "EPrimitiveTopology: PointList / LineList / LineStrip / TriangleList / TriangleStrip", desc: "点・線・線の連結・三角形・三角形帯。" },
        { sig: "EResourceState: RenderTarget / Present / DepthWrite / PixelShaderResource / CopySrc / CopyDst / UnorderedAccess / Common / DepthRead", desc: "RT / 提示用 / 深度書込 / シェーダ読取 / コピー元・先 / UAV / 既定 / 深度読取。" }
      ]
    },
    {
      name: "IRhiDevice",
      kind: "インターフェース", header: "render/IRhiDevice.h",
      summary: "GPU そのものとの対話を表す抽象(<t>RHI</t> の中心)。<t>バックエンド</t>(DX12 / Diligent)はこれを実装し、各種リソースはこのデバイスから作る。",
      when: "テクスチャ・バッファ・シェーダ・パイプラインなど、あらゆる GPU リソース作成の第一引数に渡す。普通は <code>CRenderer::Device()</code> から得る。",
      members: [
        { sig: "const char* BackendName() const", ret: "\"DX12\" 等", desc: "<t>バックエンド</t>名。" },
        { sig: "const char* AdapterName() const", ret: "GPU 名", desc: "GPU の表示名(デバッグ用)。" },
        { sig: "void WaitIdle()", desc: "GPU の処理が完了するまで待つ。<code>Shutdown</code> 前やリソース作り直しの前に必要。" }
      ]
    },
    {
      name: "FDeviceConfig / ERhiBackendKind / CreateRhiDevice",
      kind: "構造体 / 列挙 / ファクトリ関数", header: "render/IRhiDevice.h",
      summary: "デバイス作成のオプションと<t>バックエンド</t>選択、そして実際にデバイスを作る関数。<code>CRenderer</code> を使わず低レベルから組み立てるときの入口。",
      when: "描画パイプラインを自前で組むとき。普通は <code>CRenderer::Init</code> が裏でこれを呼ぶ。",
      members: [
        { sig: "struct FDeviceConfig{ enable_debug_layer, prefer_high_perf, backend }", desc: "デバッグレイヤ・高性能 GPU 優先・バックエンド種別。" },
        { sig: "enum ERhiBackendKind: Auto / D3D12 / Vulkan", desc: "Diligent 経由のときのみ意味を持つ。raw DX12 backend では無視。" },
        { sig: "TResult&lt;TUniquePtr&lt;IRhiDevice&gt;&gt; CreateRhiDevice(const FDeviceConfig& cfg)", ret: "デバイス", desc: "デバイスを作る。実際のバックエンドはビルド設定で決まる。" }
      ]
    },
    {
      name: "IRhiSwapchain",
      kind: "インターフェース", header: "render/IRhiSwapchain.h",
      summary: "画面に表示される<b>バックバッファ</b>を管理し、描き終えた絵を画面へ送り出す<t>スワップチェイン</t>。ダブル/トリプルバッファで描画と表示を分ける。",
      when: "毎フレーム「描く前にバックバッファを取得」「描いた後に提示」する。普通は <code>CRenderer</code> が裏で扱う。",
      members: [
        { sig: "u32 AcquireNextImage()", ret: "バックバッファ番号", desc: "次に書き込めるバックバッファをロックして取得する。" },
        { sig: "bool Present()", ret: "提示成功なら true", desc: "描き終えた絵を画面へ反映する。backend/device failure は false。" },
        { sig: "bool Resize(u32 width, u32 height)", ret: "成功か", desc: "サイズ変更。false のときは本フレームの描画をスキップし次フレームで再試行する。" },
        { sig: "u32 BufferCount() / Width() / Height() const", desc: "バックバッファ枚数 / 現在の解像度。" }
      ]
    },
    {
      name: "FSwapchainConfig / CreateRhiSwapchain",
      kind: "構造体 / ファクトリ関数", header: "render/IRhiSwapchain.h",
      summary: "<t>スワップチェイン</t>を作るための設定(対象ウィンドウ・フォーマット・バッファ枚数・<t>VSync</t>)と生成関数。",
      when: "RHI を自前で組むとき。",
      members: [
        { sig: "struct FSwapchainConfig{ window, format, buffer_count=2, vsync=true }", desc: "2=ダブル、3=トリプルバッファ。" },
        { sig: "TResult&lt;TUniquePtr&lt;IRhiSwapchain&gt;&gt; CreateRhiSwapchain(IRhiDevice& device, const FSwapchainConfig& cfg)", ret: "スワップチェイン", desc: "スワップチェインを作る。" }
      ]
    },
    {
      name: "IRhiCommandList",
      kind: "インターフェース", header: "render/IRhiCommandList.h",
      summary: "GPU に送る命令を<b>記録するバッファ</b>(<t>コマンドリスト</t>)。RT のバインド・パイプライン設定・バッファ/テクスチャの割当・描画命令をここに積み、最後に GPU へ投入する。",
      when: "実際の描画はすべてこれ経由。<code>CRenderer::CommandList()</code> から得たものに命令を積む。",
      members: [
        { sig: "void Begin() / End(); bool Submit()", desc: "記録開始 / 記録終了 / GPU へ投入。投入と completion fence の発行に失敗すると false。" },
        { sig: "void ResetStatistics() / const FRhiCommandStatistics& Statistics() const", desc: "呼び出し側のframe境界で命令統計を初期化 / draw・dispatch・推定三角形数を取得する。統計領域は無状態interfaceではなく各Raw/Diligent具象が所有する。" },
        { sig: "void BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index, const FClearColor& clear, IRhiTexture* depth = nullptr, f32 depth_clear = 1.0f)", desc: "バックバッファを RT としてバインド+クリア。depth を渡すと深度もバインド+クリア。", when: "画面への描画パスの最初。" },
        { sig: "void EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index)", desc: "バックバッファ描画を終え、<t>Present</t> 可能状態にする。" },
        { sig: "void BeginRenderToTexture(IRhiTexture& rt, const FClearColor& clear, IRhiTexture* depth = nullptr, f32 depth_clear = 1.0f)", desc: "オフスクリーン RT への描画開始(HDR RT / ポストプロセス用)。", when: "HDR パイプラインや反射の捕捉。" },
        { sig: "void EndRenderToTexture(IRhiTexture& rt)", desc: "RT 描画を終え、次パスで <t>SRV</t> としてサンプル可能な状態へ遷移する。" },
        { sig: "void BeginRenderToTextureLoad(IRhiTexture& rt, IRhiTexture* depth = nullptr)", desc: "クリアせずに RT を再 bind(opaque pass の上にさらに描く)。" },
        { sig: "void BeginRenderToTextureSlice(IRhiTexture& rt, u32 slice, u32 mip, const FClearColor& clear)", desc: "<t>キューブマップ</t>1 面 / 配列 1 スライス / 1 ミップに描く(per_slice_rtv 必須)。" },
        { sig: "bool BeginRenderToTextureMrt(IRhiTexture* const* rts, u32 rt_count, const FClearColor& clear, IRhiTexture* depth = nullptr, f32 depth_clear = 1.0f)", desc: "最大 8 枚の RT を同時バインドする <t>MRT</t> 描画。Raw DX12 / Diligent の双方で、全 color RT と任意 depth の view・寸法・sample count を bind 前に検証し、成功時だけ <code>true</code> を返す。<code>false</code> では backend state は不変で、呼び出し側は Draw と EndRenderToTextureMrt を実行してはならない。" },
        { sig: "void BeginShadowPass(IRhiTexture& depth, f32 depth_clear = 1.0f) / EndShadowPass(IRhiTexture& depth)", desc: "depth-only の<t>シャドウパス</t>開始/終了。終了時に深度を SRV 状態へ遷移し主パスでサンプル可能に。" },
        { sig: "void SetViewport(const FViewport&) / SetScissor(const FScissorRect&)", desc: "<t>ビューポート</t> / <t>シザー</t>を設定。" },
        { sig: "void SetStencilRef(u32 ref)", desc: "<t>ステンシル</t>参照値を設定(同じ PSO で ref だけ切替できる)。" },
        { sig: "void SetPipeline(IRhiPipeline&)", desc: "次の Draw で使うパイプライン(VS+PS+レイアウト等)を設定。" },
        { sig: "void SetVertexBuffer(IRhiBuffer& vb, u32 stride) / SetIndexBuffer(IRhiBuffer& ib)", desc: "頂点 / インデックスバッファをバインド。" },
        { sig: "void SetConstantBuffer(u32 slot, IRhiBuffer& cb) / SetTexture(u32 slot, IRhiTexture& tex)", desc: "<t>定数バッファ</t> / テクスチャを指定スロットにバインド。共有upload pageの論理sliceは親bufferとoffset範囲へ解決する。" },
        { sig: "void Draw(u32 vertex_count, u32 first_vertex = 0)", desc: "非インデックス描画。" },
        { sig: "void DrawIndexed(u32 index_count, u32 first_index = 0, i32 base_vertex = 0)", desc: "インデックス描画(普通はこちら)。" },
        { sig: "void* NativeHandle()", ret: "ネイティブハンドル", desc: "ImGui 等の外部統合用に、バックエンド固有のハンドルを取り出す。" }
      ]
    },
    {
      name: "EBufferUsage / FBufferDesc / IRhiBuffer / CreateRhiBuffer",
      kind: "列挙 / 構造体 / インターフェース / ファクトリ", header: "render/IRhiBuffer.h",
      summary: "GPU 上の<b>バッファ</b>(頂点・インデックス・<t>定数バッファ</t>等)の抽象と、その作成。用途を <code>EBufferUsage</code> で指定する。",
      when: "メッシュの頂点データやシェーダに渡すパラメータを GPU に置くとき。高レベル経路では <code>UploadMesh</code> 等が裏で作る。",
      members: [
        { sig: "enum EBufferUsage: Vertex / Index16 / Index32 / Uniform / Storage / Staging", desc: "頂点 / 16・32bit インデックス / 定数バッファ / UAV / アップロード用。" },
        { sig: "struct FBufferDesc{ size, usage, cpu_writable, initial_data }", desc: "サイズ・用途・CPU 書込み可否・初期データ。" },
        { sig: "usize Size() const / EBufferUsage Usage() const", desc: "サイズ・用途を返す。" },
        { sig: "void Update(const void* data, usize size, usize offset = 0)", desc: "CPU からデータを書き込む(<code>cpu_writable=true</code> のバッファのみ)。", when: "定数バッファを毎フレーム更新するとき。" },
        { sig: "IRhiBuffer& BindingBuffer() / usize BindingOffset() const", desc: "backendがbindする実bufferと先頭offset。通常bufferは自身と0、共有pageの論理sliceは親と256 byte境界offsetを返す。" },
        { sig: "TResult&lt;TUniquePtr&lt;IRhiBuffer&gt;&gt; CreateRhiBuffer(IRhiDevice&, const FBufferDesc&)", ret: "バッファ", desc: "バッファを作る。" }
      ]
    },
    {
      name: "CTransientUploadArena",
      kind: "クラス", header: "render/TransientUploadArena.h",
      summary: "同じ大きさの一時<t>定数バッファ</t>を256 byte境界の論理sliceへ分け、少数の共有GPU pageで保持するframe arena。",
      when: "多数drawの定数をdrawごとに上書きせず保持しながら、GPU buffer生成数を減らすとき。Standard/PBR shaderが内部で使用する。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, usize allocation_size, u32 initial_capacity) / void Reset()", desc: "論理slice寸法と初期pageを作る / 全pageを解放する。" },
        { sig: "bool BeginFrame(u32 required_allocations = 0) / bool Reserve(u32)", desc: "必要容量を確保してCPU cursorをO(1)で先頭へ戻す / pageを追加する。既存slice addressは保持する。" },
        { sig: "IRhiBuffer* Upload(const void* data, usize size) / Get(u32)", desc: "次sliceへ書いてbind可能な論理bufferを返す / 指定sliceを返す。" },
        { sig: "u32 Capacity() / Used() / GpuBufferCount() / usize ReservedBytes()", desc: "論理容量、frame使用数、実GPU buffer数、RHI page記述へ要求した一frame分の論理byte数。" },
        { sig: "using FTransientUploadArena = CTransientUploadArena", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CTransientUploadArena</code> を使う。" }
      ]
    },
    {
      name: "EShaderStage / FShaderDesc / IRhiShader / CreateRhiShader",
      kind: "列挙 / 構造体 / インターフェース / ファクトリ", header: "render/IRhiShader.h",
      summary: "<t>HLSL</t> ソースをコンパイルして保持する<b>シェーダ</b>の抽象。頂点・ピクセル・<t>コンピュート</t>のステージを指定する。",
      when: "独自シェーダで描きたいとき。標準ヘルパ(CStandardShader 等)を使う場合は内部で作られるので普通は触らない。",
      members: [
        { sig: "enum EShaderStage: Vertex / Pixel / Compute", desc: "シェーダステージ。" },
        { sig: "struct FShaderDesc{ stage, hlsl_source, entry_point, target, debug_name }", desc: "ステージ・HLSL 文字列・エントリ名・ターゲット(null で自動)。" },
        { sig: "EShaderStage Stage() const", desc: "このシェーダのステージ。" },
        { sig: "const byte* Bytecode() / usize BytecodeSize() const", desc: "コンパイル済みバイトコード(パイプライン作成時に内部参照)。" },
        { sig: "TResult&lt;TUniquePtr&lt;IRhiShader&gt;&gt; CreateRhiShader(IRhiDevice&, const FShaderDesc&)", ret: "シェーダ", desc: "HLSL をコンパイルしてシェーダを作る。" }
      ]
    },
    {
      name: "FPipelineDesc / IRhiPipeline / CreateRhiPipeline",
      kind: "構造体 / インターフェース / ファクトリ", header: "render/IRhiPipeline.h",
      summary: "<b>グラフィックスパイプライン</b>(<t>PSO</t>)。VS+PS・頂点入力レイアウト・出力<t>フォーマット</t>・カリング・ブレンド・深度/<t>ステンシル</t>テストなど「1 回の描画方式」を丸ごと固めたもの。",
      when: "描画方式(どのシェーダで、どんな頂点を、どうブレンドして描くか)を 1 つ定義するとき。<code>SetPipeline</code> で切り替える。",
      members: [
        { sig: "IRhiShader* vs / ps", desc: "頂点 / ピクセルシェーダ。" },
        { sig: "EPrimitiveTopology topology", desc: "プリミティブ種別(既定 TriangleList)。" },
        { sig: "EFormat rt_format / rt_formats[8] / u32 rt_count / EFormat depth_format", desc: "出力 RT の<t>フォーマット</t>。<code>rt_count&gt;0</code> で <t>MRT</t>。depth_format=Unknown で深度なし。" },
        { sig: "FInputElement layout[8] / u32 layout_count / u32 vertex_stride", desc: "頂点入力レイアウトと 1 頂点のバイト数。" },
        { sig: "u32 cbuffer_slots / texture_slots", desc: "公開する<t>定数バッファ</t>数 / テクスチャ数。Draw 前に SetConstantBuffer / SetTexture で割り当てる。" },
        { sig: "FSamplerDesc static_samplers[16] / u32 static_sampler_count", desc: "パイプラインに焼き込む静的<t>サンプラ</t>(最大 16)。" },
        { sig: "const char* cbuffer_names[16] / texture_names[16]", desc: "Diligent が名前で <t>SRB</t> を引くための HLSL リソース名(DX12 raw は無視)。" },
        { sig: "ECullMode cull_mode / EBlendMode blend_mode / bool depth_test / depth_write", desc: "カリング・ブレンド・深度テスト/書込み。" },
        { sig: "FStencilDesc stencil", desc: "<t>ステンシル</t>設定(既定 disable)。" },
        { sig: "TResult&lt;TUniquePtr&lt;IRhiPipeline&gt;&gt; CreateRhiPipeline(IRhiDevice&, const FPipelineDesc&)", ret: "パイプライン", desc: "パイプラインを作る。" }
      ]
    },
    {
      name: "FInputElement / ECullMode / EBlendMode / ECompareFunc",
      kind: "構造体 / 列挙(enum)", header: "render/IRhiPipeline.h",
      summary: "パイプラインを組む部品。<b>FInputElement</b>=頂点 1 要素(POSITION/COLOR 等の<t>セマンティック</t>+フォーマット+オフセット)、<b>ECullMode</b>=面の捨て方、<b>EBlendMode</b>=色の混ぜ方、<b>ECompareFunc</b>=深度/ステンシルの比較関数。",
      when: "<code>FPipelineDesc</code> の各フィールドを埋めるとき。",
      members: [
        { sig: "struct FInputElement{ semantic_name, semantic_index, format, offset }", desc: "HLSL の<t>セマンティック</t>名・index・フォーマット・頂点内バイトオフセット。" },
        { sig: "enum ECullMode: None / Front / Back", desc: "カリング無し(両面) / 表面 / 裏面(一般的)。" },
        { sig: "enum EBlendMode: Opaque / AlphaBlend / Additive", desc: "不透明 / 一般的な半透明 / 加算。" },
        { sig: "enum ECompareFunc: Never / Less / Equal / LessEqual / Greater / NotEqual / GreaterEqual / Always", desc: "深度・ステンシルテストの比較関数。" }
      ]
    },
    {
      name: "FStencilDesc / EStencilOp",
      kind: "構造体 / 列挙(enum)", header: "render/IRhiPipeline.h",
      summary: "<t>ステンシル</t>(マスク描画)の設定。テスト結果に応じてステンシル値をどう更新するかを <code>EStencilOp</code> で、比較関数を <code>ECompareFunc</code> で指定する。",
      when: "任意形状のマスクで描画範囲を制限したいとき(ミニマップの円形クリップ等)。stencil 付き深度バッファが必要。",
      members: [
        { sig: "struct FStencilDesc{ enable, func, pass_op, fail_op, depth_fail_op, read_mask, write_mask }", desc: "有効化・比較関数・各テスト結果での操作・読み書きマスク。" },
        { sig: "enum EStencilOp: Keep / Zero / Replace / IncrSat / DecrSat / Invert / IncrWrap / DecrWrap", desc: "保持 / 0 化 / 参照値で置換 / ±1(飽和) / ビット反転 / ±1(ラップ)。" }
      ]
    },
    {
      name: "FTextureDesc / IRhiTexture / CreateRhiTexture",
      kind: "構造体 / インターフェース / ファクトリ", header: "render/IRhiTexture.h",
      summary: "GPU 上の<b>テクスチャ</b>(画像)の抽象と作成。2D だけでなく<t>キューブマップ</t>・配列・<t>ミップマップ</t>・レンダーターゲット・深度バッファも表せる。",
      when: "画像を GPU に置きたい / 自分で RT や深度バッファを用意したいとき。アセットからは <code>UploadTexture</code> が裏で作る。",
      members: [
        { sig: "struct FTextureDesc{ width, height, format, mip_levels, array_size, is_cubemap, is_render_target, is_depth_target, shader_visible_depth, per_slice_rtv, initial_data, initial_data_size }", desc: "サイズ・フォーマットのほか、RT/深度/cubemap/配列/ミップ/per-slice RTV などの用途フラグ。" },
        { sig: "u32 Width() / Height() const / EFormat PixelFormat() const", desc: "サイズと<t>フォーマット</t>。" },
        { sig: "u32 MipLevels() / ArraySize() const / bool IsCubemap() const", desc: "ミップ数 / 配列枚数 / cubemap か。" },
        { sig: "TResult&lt;TUniquePtr&lt;IRhiTexture&gt;&gt; CreateRhiTexture(IRhiDevice&, const FTextureDesc&)", ret: "テクスチャ", desc: "テクスチャを作る。" }
      ]
    },
    {
      name: "FSamplerDesc / ESamplerFilter / ESamplerAddress",
      kind: "構造体 / 列挙(enum)", header: "render/IRhiSampler.h",
      summary: "テクスチャをシェーダで読むときの<b><t>サンプラ</t></b>設定。フィルタ(拡大縮小時の補間)と<t>アドレッシング</t>(UV が範囲外のときの扱い)を決める。本フレームワークでは静的サンプラとしてパイプラインに埋め込む。",
      when: "ドット絵をくっきり出したい(Point)・写真を滑らかに(Linear)・タイリングしたい(Wrap)など描き味を決めるとき。",
      members: [
        { sig: "struct FSamplerDesc{ filter, address_u/v/w, min_lod, max_lod, max_anisotropy }", desc: "フィルタ・3 軸のアドレッシング・LOD 範囲・異方性度合い。" },
        { sig: "enum ESamplerFilter: Point / Linear / Anisotropic", desc: "ニアレスト(ドット絵) / バイリニア(普通) / 異方性(高品質)。" },
        { sig: "enum ESamplerAddress: Wrap / Mirror / Clamp / Border", desc: "繰り返し / 鏡像 / 端で固定 / 境界色(黒)。" }
      ]
    },
    {
      name: "UploadTexture / UploadMesh / FGpuMesh",
      kind: "関数 / 構造体", header: "render/RenderAssets.h",
      summary: "アセット(<t>AImageAsset</t> / <t>AMeshAsset</t>)を GPU リソースに変換する高レベルヘルパ。画像→テクスチャ、メッシュ→頂点+インデックスバッファ(<code>FGpuMesh</code>)に一発で持っていける。",
      when: "ロードした画像やモデルをそのまま描きたいとき。これだけ呼べば描画できる。",
      members: [
        { sig: "TResult&lt;TUniquePtr&lt;IRhiTexture&gt;&gt; UploadTexture(IRhiDevice&, const AImageAsset&)", ret: "テクスチャ", desc: "画像アセットを GPU テクスチャに同期アップロード。" },
        { sig: "TResult&lt;void&gt; UploadMesh(IRhiDevice&, const AMeshAsset&, FGpuMesh& out)", desc: "メッシュアセットを <code>FGpuMesh</code> に変換(位置+法線+UV)。" },
        { sig: "struct FGpuMesh{ vertex_buffer, index_buffer, vertex_count, index_count, vertex_stride }", desc: "描画に必要な VB/IB と数。" },
        { sig: "TResult&lt;void&gt; UploadSkinnedMesh(IRhiDevice&, const ASkinnedMeshAsset&, FSkinnedGpuMesh& out)", desc: "スキンメッシュを <code>FSkinnedGpuMesh</code> に変換(<code>CSkinnedShader</code> が消費)。" },
        { sig: "struct FSkinnedGpuMesh{ vertex_buffer, index_buffer, vertex_count, index_count, vertex_stride }", desc: "スキンメッシュ用の VB/IB と数。vertex_stride は <code>FSkinnedVertex</code> のサイズ(<t>BLENDINDICES</t>/<t>WEIGHT</t> を含む)。" }
      ]
    },
    {
      name: "CStandardShader",
      kind: "クラス", header: "render/StandardShader.h",
      summary: "HLSL を書かずに使える<b>標準ライティングシェーダ</b>。Lambert 拡散 + Blinn-Phong スペキュラ + 環境光 + アルベドテクスチャで、有向光源 最大 4 灯 + 点光源 4 灯 + <t>シャドウマップ</t>に対応。",
      when: "とりあえず 3D メッシュをまともなライティングで描きたいとき。PBR まで要らない手軽な経路。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, EFormat rt_format, EFormat depth_format)", desc: "VS+PS・パイプライン・定数バッファ・既定白テクスチャを作る。" },
        { sig: "bool BeginFrame(u32 required_object_draws = 0)", desc: "毎フレームの描画記録前に呼び、実際に記録する Standard draw 数を正確に渡す。共有upload arenaの必要sliceを確保できなければfalse。" },
        { sig: "if (!shd.BeginFrame(/* exact standard draws this frame */ 1u)) return;", desc: "Standard の描画予定数を先に渡し、予約に失敗したフレームは描画を記録しない。" },
        { sig: "u32 ObjectBufferCapacity() / ObjectDrawCount() / ObjectBufferPageCount()", desc: "論理object slot容量、frame使用数、全slotを保持する実GPU buffer数。" },
        { sig: "[[deprecated]] kMaxObjectDrawsPerFrame = 256", desc: "旧固定リングとのソース互換用の目安値。プールは必要数まで増えるため、ハード上限ではない。" },
        { sig: "void SetFrame(view_projection, camera_pos, light_dir, light_color, ambient_color)", desc: "毎フレーム。カメラ + 1 灯の有向光源 + 環境光の簡易版。" },
        { sig: "void SetLights(view_projection, camera_pos, const FDirLight* lights, u32 count, ambient_color)", desc: "有向光源 最大 4 灯のマルチライト版。" },
        { sig: "void SetPointLights(const FPointLight* lights, u32 count)", desc: "点光源を最大 4 灯追加する(SetFrame/SetLights と独立)。" },
        { sig: "void SetShadowMap(IRhiTexture* tex, const FMat4& light_vp, f32 bias = 0.001f, f32 filter_radius = 1.0f)", desc: "<t>シャドウマップ</t>を設定。null で影なし。filter_radius は <t>PCSS</t> の柔らかさ。" },
        { sig: "bool SetObject(const FMat4& model, base_color, specular_strength, shininess)", desc: "描くオブジェクトごとの<t>モデル行列</t>と材質を draw 専用 CB に設定。失敗時は false なので、その draw を記録しない。" },
        { sig: "void DrawMesh(IRhiCommandList& cmd, const FGpuMesh& mesh, const FMat4& model, base_color, specular_strength, shininess, IRhiTexture* albedo = nullptr)", desc: "Object CB 更新 + 1 回の描画をまとめた便利 API。albedo=null で白テクスチャ。" },
        { sig: "bool IsShadowEnabled() const / IRhiTexture* ShadowTextureOrDefault() const", desc: "<t>シャドウマップ</t>が設定済みか / 設定された深度テクスチャ(未設定なら既定白テクスチャ)を返す。" },
        { sig: "IRhiPipeline* Pipeline() / IRhiBuffer* PerFrameCB() / PerObjectCB() / IRhiTexture* DefaultWhiteTexture()", desc: "細かく手で描きたいとき用のアクセサ。" },
        { sig: "using FStandardShader = CStandardShader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CStandardShader</code> を使う。" }
      ]
    },
    {
      name: "FDirLight / FPointLight",
      kind: "構造体", header: "render/StandardShader.h",
      summary: "ライトのパラメータ。<b>FDirLight</b>=向きと色だけを持つ<t>有向光源</t>(太陽光のような無限遠の平行光)、<b>FPointLight</b>=位置と到達距離を持つ<t>点光源</t>。",
      when: "<code>SetLights</code> / <code>SetPointLights</code> に渡す配列の要素として。CStandardShader / CPbrShader / CSkinnedShader で共通。",
      members: [
        { sig: "struct FDirLight{ FVec3 direction, FVec3 color }", desc: "光が向かう方向と色。" },
        { sig: "struct FPointLight{ FVec3 position, f32 range, FVec3 color }", desc: "位置・到達距離・色。range を超えると影響ゼロ。" }
      ]
    },
    {
      name: "CPbrShader",
      kind: "クラス", header: "render/PbrShader.h",
      summary: "物理ベースレンダリング(<t>PBR</t>、Cook-Torrance / Metalness-Roughness)の本格シェーダ。金属/粗さ + 環境光 + <t>IBL</t> + 影に加え、法線マップ・<t>SSAO</t>/<t>SSGI</t>/<t>SSR</t>・フォグ・布(Sheen)・薄膜(Iridescence)・<t>サブサーフェス</t>など多彩な質感を載せられる。",
      when: "IBL、影、画面空間効果を組み合わせる 3D 描画。金属・プラスチック・肌・布などを描き分けたいとき。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, EFormat rt_format, EFormat depth_format)", desc: "シェーダ・パイプライン・定数バッファ・fallback テクスチャを作る。" },
        { sig: "bool BeginFrame(u32 required_object_draws = 0)", desc: "PBR draw記録前に共有upload arenaの必要sliceを予約し、CPU cursorを先頭へ戻す。失敗時も既存容量は利用できる。" },
        { sig: "u32 ObjectBufferCapacity() / ObjectDrawCount() / ObjectBufferPageCount()", desc: "論理object slot容量、frame使用数、全slotを保持する実GPU buffer数。" },
        { sig: "void SetLights(...) / SetPointLights(...) / SetAreaLights(const FAreaLight*, u32)", desc: "有向光源 4 灯 / 点光源 4 灯 / 矩形<t>エリアライト</t> 2 個。" },
        { sig: "void SetIbl(IRhiTexture* irradiance, IRhiTexture* prefilter, IRhiTexture* brdf_lut, u32 prefilter_mips)", desc: "<t>IBL</t> 用 3 テクスチャをバインド。3 つ揃うと環境光が IBL になる。", when: "CImageBasedLighting の各 Map をそのまま渡す。" },
        { sig: "void BindIblTextures(IRhiCommandList& cmd)", desc: "albedo と並んで IBL slot 1/2/3 を bind するヘルパ。" },
        { sig: "void SetNormalMap(IRhiTexture*) / SetSsao(...) / SetSsgi(...) / SetSsr(...) / SetLightmap(...)", desc: "法線マップ / <t>SSAO</t> / <t>SSGI</t> / <t>SSR</t> / ライトマップを差し込む。null で OFF。" },
        { sig: "void SetSh9(const FVec4* sh9_or_null) / SetProbeGrid(const FLightProbe*, u32)", desc: "<t>球面調和</t>(SH9)1 個 / 静的光プローブグリッド(最大 4)で間接光を与える。" },
        { sig: "void SetFog(FVec3 color, f32 density, f32 height_falloff, f32 height_base)", desc: "指数<t>高さフォグ</t>。density=0 で OFF。" },
        { sig: "void SetShadowMap(IRhiTexture* depth, const FMat4& light_vp, ...) / SetShadowMapCascades(...)", desc: "単一 / <t>CSM</t>(カスケード)の<t>シャドウマップ</t>。null で OFF。" },
        { sig: "void SetObject(const FMat4& model, base_color, metallic, roughness, ao)", desc: "PBR 材質。metallic/roughness/ao を直接 GGX に渡す。" },
        { sig: "void SetExtParams(clearcoat, clearcoat_roughness, anisotropy, tangent) / SetEmissive(...) / SetSheen(...) / SetIridescence(...) / SetSubsurface(...)", desc: "クリアコート・異方性・自己発光・布の毛羽・薄膜干渉・内部散乱の拡張材質。SetObject の直後に呼ぶ。" },
        { sig: "void DrawMesh(IRhiCommandList& cmd, const FGpuMesh& mesh, const FMat4& model, base_color, metallic, roughness, ao, IRhiTexture* albedo = nullptr)", desc: "材質設定 + 描画をまとめた便利 API。" },
        { sig: "IRhiPipeline* Pipeline() / IRhiBuffer* PerFrameCB() / PerObjectCB() / IRhiTexture* DefaultWhiteTexture()", desc: "手描き用アクセサ。" },
        { sig: "using FPbrShader = CPbrShader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CPbrShader</code> を使う。" }
      ]
    },
    {
      name: "CPbrShader::FAreaLight / CPbrShader::FLightProbe",
      kind: "構造体(入れ子)", header: "render/PbrShader.h",
      summary: "CPbrShader 専用の入力。<b>FAreaLight</b>=矩形の<t>エリアライト</t>(中心 + 2 軸の半幅ベクトル + 色)、<b>FLightProbe</b>=静的光プローブ(位置 + <t>球面調和</t>9 係数)。",
      when: "面光源で柔らかい照明を作る / シーンに事前計算の間接光プローブを置くとき。",
      members: [
        { sig: "struct FAreaLight{ FVec3 center, axis_x, axis_y, color }", desc: "矩形中心 + 半幅 2 軸 + 放射輝度。法線方向の逆へ片面 emit。" },
        { sig: "struct FLightProbe{ FVec3 position, FVec4 sh9[9] }", desc: "world 位置と SH9 9 係数(xyz=RGB)。" }
      ]
    },
    {
      name: "CSkinnedShader",
      kind: "クラス", header: "render/SkinnedShader.h",
      summary: "<b>GPU スキニング</b>対応のライティングシェーダ。CStandardShader と同じライト設定 API に加え、最大 64 本の<t>ボーンパレット</t>行列を送り、頂点をボーンに従って変形して描く。",
      when: "スケルタルアニメーションするキャラを描くとき。<code>WritePalette</code> で得たボーン行列を毎フレーム渡す。",
      members: [
        { sig: "static constexpr u32 kMaxBones = 64", desc: "ボーンパレットの上限。" },
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, EFormat rt_format, EFormat depth_format)", desc: "初期化。Bones 用の定数バッファ(b2)を追加で持つ。" },
        { sig: "bool BeginFrame(u32 required_object_draws = 0)", desc: "毎フレームの描画記録前に呼び、実際に記録する skinned draw 数を正確に渡す。Object/Bones の CB ペアを先に確保できなければ false。" },
        { sig: "if (!shd.BeginFrame(/* exact skinned draws this frame */ 1u)) return;", desc: "Skinned の描画予定数を先に渡し、予約に失敗したフレームは描画を記録しない。" },
        { sig: "[[deprecated]] kMaxObjectDrawsPerFrame = 256", desc: "旧固定リングとのソース互換用の目安値。Object/Bones ペアのプールは必要数まで増えるため、ハード上限ではない。" },
        { sig: "void SetFrame(...) / SetLights(...) / SetPointLights(...)", desc: "CStandardShader と同じ形式のライト設定。" },
        { sig: "bool SetObject(...)", desc: "Object/Bones の draw 専用 CB ペアを選択して材質を設定。失敗時は false なので、その draw を記録しない。" },
        { sig: "bool SetBonePalette(const FMat4* palette, u32 count)", desc: "ボーン行列パレット(最大 64)を直前の SetObject と同じ draw 専用 CB へ設定。失敗時は false。残りは単位行列で埋める。" },
        { sig: "if (!shd.SetBonePalette(palette, nb)) return;", desc: "SetObject の直後にボーンパレットを書き込み、失敗した draw は記録しない。" },
        { sig: "IRhiBuffer* BonesCB() / PerFrameCB() / PerObjectCB() / IRhiPipeline* Pipeline()", desc: "手描き用アクセサ。<code>BonesCB()</code> は b2 にバインドする。" },
        { sig: "using FSkinnedShader = CSkinnedShader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSkinnedShader</code> を使う。" }
      ]
    },
    {
      name: "CSpriteBatch",
      kind: "クラス", header: "render/SpriteBatch.h",
      summary: "<b>2D スプライト描画</b>のバッチ式ヘルパ。ピクセル座標でスプライト・矩形・三角形・回転スプライト・テキストを描き、同じテクスチャの連続描画は自動でまとめて(<t>バッチ</t>)高速化する。座標は左上原点・Y 下向き。",
      when: "HUD・2D ゲームの絵描き全般。1 枚絵を貼る、図形を描く、文字を出す、2D カメラで世界をスクロールするなど。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, EFormat rt_format, u32 max_sprites = 4096)", desc: "シェーダ・パイプライン・頂点/インデックスバッファを作る。" },
        { sig: "void Begin(IRhiCommandList& cl, u32 screen_w, u32 screen_h) / void End()", desc: "描画開始 / 終了(残りバッチを GPU へ送る)。screen サイズは NDC 変換に必要。" },
        { sig: "void Draw(IRhiTexture& tex, f32 x, f32 y, f32 w, f32 h, FVec4 tint = {1,1,1,1})", desc: "テクスチャ全体を矩形に描く。" },
        { sig: "void DrawSub(IRhiTexture& tex, x, y, w, h, u0, v0, u1, v1, tint)", desc: "テクスチャの一部(UV 0..1)を描く。<t>アトラス</t>やタイル抜き出しに。" },
        { sig: "void DrawRect(f32 x, f32 y, f32 w, f32 h, FVec4 color)", desc: "テクスチャ無しの単色矩形。" },
        { sig: "void DrawString(const FFont& font, const char* utf8, f32 x, f32 y, FVec4 color)", desc: "UTF-8 テキスト描画。<code>\\n</code> で改行。" },
        { sig: "void DrawRotated(...) / DrawRectRotated(...)", desc: "中心 (cx,cy) 回りに radians 回転して描く。通常スプライトと同じバッチに乗る。" },
        { sig: "void DrawTriangle(...) / DrawTriangleVC(...) / DrawTriangleSub(...)", desc: "単色 / 頂点カラー(<t>グラデーション</t>) / テクスチャ付き三角形。水面や泡の表現に。" },
        { sig: "void SetView(f32 cam_x, f32 cam_y, f32 zoom)", desc: "2D カメラ。(cam_x,cam_y) を画面中心に映し zoom 倍で拡縮。Begin でリセット。" },
        { sig: "void SetClipRect(i32 x, i32 y, i32 w, i32 h) / ClearClipRect()", desc: "以降の描画を矩形内(画面座標)に制限 / 解除。" },
        { sig: "void SetBlendMode(EBlendMode mode)", desc: "ブレンドモード切替。Additive で光のきらめき等。Off に戻すには AlphaBlend を渡す。" },
        { sig: "void SetStencilMode(EStencilMode mode, u8 ref = 1)", desc: "<t>ステンシル</t>マスクで描画範囲を制限。WriteMask で形を焼き、KeepInside/Outside で通す。stencil 付き深度パスでのみ。" },
        { sig: "using FSpriteBatch = CSpriteBatch", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSpriteBatch</code> を使う。" }
      ]
    },
    {
      name: "TDrawPacketSortKeyLayout",
      kind: "テンプレート構造体", header: "render/DrawPacketSortKey.h",
      summary: "優先順に並べた整数fieldのbit幅から、64 bit描画sort keyのshiftとmaskをcompile time生成するlayout。",
      when: "pipeline、material、depthなど複数fieldのkey配置を型として固定し、手書きshiftの重複やbit超過を防ぐとき。",
      members: [
        { sig: "static constexpr u32 FieldCount() / TotalBits()", desc: "field数 / 全fieldの合計bit数。合計64 bit超過はcompile error。" },
        { sig: "template&lt;u32 I&gt; static constexpr u32 FieldBits() / FieldShift()", desc: "指定fieldのbit幅 / key内の下位bit位置。" },
        { sig: "template&lt;u32 I&gt; static constexpr u64 Insert(u64 value)", desc: "値をfield幅でmaskし、優先順の位置へ配置する。" }
      ]
    },
    {
      name: "FSpriteSortList",
      kind: "クラス", header: "render/SpriteSortList.h",
      summary: "sprite commandをlayer、depth、提出順で安定整列して<code>CSpriteBatch</code>へ再生する。大量時はcompile-time keyと安定LSD radixを使う。",
      when: "2D world、HUD、半透明spriteの前後関係を明示し、同じlayer/depthの合成順を維持したいとき。",
      members: [
        { sig: "void Submit(...) / SubmitSub(...) / SubmitRect(...)", desc: "texture sprite、UV範囲sprite、単色矩形をlayer/depth付きで蓄積する。" },
        { sig: "void Sort()", desc: "24件以下は挿入sort、それ以上は最大8 byte passの安定radixでlayer/depth昇順へ並べる。" },
        { sig: "void Replay(CSpriteBatch&) const / const FSpriteCmd& Ordered(u32) const", desc: "整列順にbatchへ流す / 検証や独自replay用に整列済みcommandを返す。" },
        { sig: "u32 LastSortPassCount() / u64 LastSortItemVisits()", desc: "直前sortのradix pass数とkey生成、比較、radixを含む決定的なitem走査数。" }
      ]
    },
    {
      name: "EStencilMode",
      kind: "列挙(enum)", header: "render/SpriteBatch.h",
      summary: "<code>CSpriteBatch::SetStencilMode</code> 用の<t>ステンシル</t>マスクモード。マスク形状を焼く・その内側/外側だけ描く、を切り替える。",
      when: "円形ミニマップや任意形状のくり抜きなど、2D で形マスクをかけたいとき。",
      members: [
        { sig: "Off / WriteMask / KeepInside / KeepOutside", desc: "テスト無し / 参照値を書く / 一致部だけ描く / 不一致部だけ描く。" }
      ]
    },
    {
      name: "FFont / FGlyphInfo / DecodeUtf8",
      kind: "クラス / 構造体 / 関数", header: "render/Font.h",
      summary: "<t>TTF</t>/OTF フォントを GPU テクスチャ<t>アトラス</t>に焼いて、<code>CSpriteBatch</code> で文字を描けるようにする。ASCII から平仮名・片仮名・全角、オプションで CJK 漢字まで対応。",
      when: "ゲーム内に日本語/英語テキストを表示するとき。OS のフォントファイルを読み込んで使う。",
      members: [
        { sig: "TResult&lt;void&gt; LoadFromFile(IRhiDevice&, const wchar_t* path, f32 pixel_size, u32 atlas_size = 1024, bool include_cjk = false)", desc: "ファイルからアトラスを構築。include_cjk=true で漢字も焼く(アトラスは 2048 に自動拡張)。" },
        { sig: "TResult&lt;void&gt; LoadFromBytes(IRhiDevice&, const u8* ttf_data, usize ttf_size, f32 pixel_size, ...)", desc: "メモリ上の TTF バイト列から構築。" },
        { sig: "IRhiTexture* AtlasTexture() / u32 AtlasSize() / f32 PixelSize()", desc: "焼いたアトラステクスチャとそのサイズ。" },
        { sig: "f32 Ascent() / Descent() / LineGap() / LineHeight()", desc: "行レイアウト用のメトリクス(ピクセル)。" },
        { sig: "bool GetGlyph(u32 codepoint, FGlyphInfo& out) const", desc: "コードポイントのグリフ情報(UV+オフセット)を取得。未収録なら false。" },
        { sig: "f32 MeasureWidth(const char* utf8_text)", ret: "幅(px)", desc: "文字列の描画幅を測る(改行は無視)。中央寄せ等に。" },
        { sig: "u32 DecodeUtf8(const char** p)", ret: "コードポイント", desc: "UTF-8 を 1 文字読み、ポインタを進める(自由関数)。" }
      ]
    },
    {
      name: "CSky",
      kind: "クラス", header: "render/Sky.h",
      summary: "テクスチャ不要の<b>手続き生成スカイ</b>。ピクセルシェーダで天頂・地平線・地面の色と太陽を補間して背景の空を描く。Day/Sunset/Night プリセット付き。低コスト雲は明示的に選ぶ fallback で、既定は無効。",
      when: "3D シーンの背景に空を出したいとき。シーン描画より先に呼ぶ。<t>IBL</t> の環境マップ元にもなる。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, EFormat rt_format, EFormat depth_format)", desc: "初期化。" },
        { sig: "void PresetDay() / PresetSunset() / PresetNight()", desc: "青空 / 茜色 / 紺青のプリセット。" },
        { sig: "void SetSunDirection(FVec3) / SetSunColor(FVec3) / SetSunRadius(f32) / SetSunGlow(f32)", desc: "太陽の方向・色・見かけサイズ・周囲のハロー。" },
        { sig: "void SetZenithColor(FVec3) / SetHorizonColor(FVec3) / SetGroundColor(FVec3)", desc: "天頂・地平線・地面方向の色。" },
        { sig: "void SetFallbackClouds(f32 coverage, f32 density) / SetFallbackCloudsEnabled(bool)", desc: "本格雲を使えない場合の固定刻み fallback。カメラ中心の仮想層なので production の雲には <code>CVolumetricClouds</code> を使う。" },
        { sig: "void SetFallbackCloudColor(FVec3) / SetFallbackCloudWind(f32) / SetFallbackCloudTime(f32)", desc: "fallback 雲の色・風速・時刻。Render は時刻を進めないため、呼び出し側が経過秒を渡す。" },
        { sig: "void SetClouds(...) / SetCloudsEnabled(...) / SetCloudColor(...) / SetCloudWind(...) / SetTime(...)", desc: "既存コード向けの fallback API 互換アダプター。" },
        { sig: "FVec3 SunDirection() / SunColor() / ZenithColor() / HorizonColor() / GroundColor() / f32 SunRadius() / SunGlow()", desc: "現在の太陽・空パラメータ取得。<code>CStandardShader</code> / <t>IBL</t> と太陽方向を整合させるのに使う。" },
        { sig: "void Render(IRhiCommandList& cl, const CCamera& camera)", desc: "空を描く。深度は背景塗り想定で書込み無し・テスト無し。", when: "シーン描画の先頭。" },
        { sig: "using FSky = CSky", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSky</code> を使う。" }
      ]
    },
    {
      name: "CParticleSystem / FEmitterDesc",
      kind: "クラス / 構造体", header: "render/Particles.h",
      summary: "CPU プール式の<b>2D パーティクルシステム</b>。火花・煙・爆発・魔法などのエフェクトを <code>FEmitterDesc</code> で定義し、毎フレーム <code>Update</code> して <code>CSpriteBatch</code> に積む。Fire/Sparks/Fountain/Smoke のプリセット付き。",
      when: "ゲームの視覚エフェクト全般。打撃の火花、たき火の炎、噴水など。",
      members: [
        { sig: "TResult&lt;void&gt; Init(u32 max_particles = 4096)", desc: "プールを確保。" },
        { sig: "void SetTexture(IRhiTexture* tex)", desc: "粒子のテクスチャ。null なら白矩形。" },
        { sig: "void SetEmitter(const FEmitterDesc& d) / FEmitterDesc& Emitter()", desc: "エミッタ設定 / 参照取得。" },
        { sig: "void Update(f32 dt)", desc: "1 フレーム分シミュレーション(生成・移動・寿命処理)。" },
        { sig: "void EmitBurst(u32 count)", desc: "即座に count 個生成(爆発などの単発)。" },
        { sig: "void Render(CSpriteBatch& sb)", desc: "粒子を <code>CSpriteBatch</code> に積む(事前に Begin 済みであること)。" },
        { sig: "u32 ActiveCount() / Capacity()", desc: "現在の生存数 / 上限。" },
        { sig: "static FEmitterDesc Fire/Sparks/Fountain/Smoke(FVec2 pos)", desc: "プリセットのエミッタを返す。" },
        { sig: "using FParticleSystem = CParticleSystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CParticleSystem</code> を使う。" }
      ]
    },
    {
      name: "CPostProcess / FPostProcessParams",
      kind: "クラス / 構造体", header: "render/PostProcess.h",
      summary: "<t>HDR</t> <b><t>ポストプロセス</t></b>パイプライン。シーンを HDR RT に描いてから <t>Bloom</t> + <t>トーンマップ</t>(ACES/AgX/Reinhard) + ビネット/色収差/フィルムグレイン + カラーグレーディング + シャープ化 + <t>TAA</t> + <t>自動露出</t>を一括適用して画面に出す。Diligent backend 前提。",
      when: "映画的な仕上がり(発光のにじみ・露出・色調整・アンチエイリアス)を最終段でかけたいとき。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, u32 width, u32 height, EFormat color_format)", desc: "HDR RT + Bloom ミップチェイン + 各パイプラインを作る。" },
        { sig: "TResult&lt;void&gt; Resize(u32 width, u32 height)", desc: "ウィンドウサイズ変更時に内部 RT を作り直す。" },
        { sig: "IRhiTexture* HdrRenderTarget() / EFormat HdrFormat()", desc: "シーンを描く HDR RT とそのフォーマット。" },
        { sig: "void Render(IRhiCommandList& cmd, IRhiSwapchain& sc, u32 buffer_index, const FPostProcessParams& params)", desc: "Bloom + Tonemap 一式を実行してバックバッファへ書き出す。" },
        { sig: "FPostProcessParams{ bloom_*, exposure, gamma, tonemap_kind, vignette_*, chromatic_aberration, grain_*, cg_* (color grading), cas_strength, taa_*, auto_exposure_*, fxaa_enabled }", desc: "各効果のパラメータ。bloom / トーンマップ種別 / シネマティック FX / カラーグレーディング / シャープ / TAA / 自動露出 / トーンマップ後FXAA。既定は無効で、TAA解像が有効な場合は重ねない。" },
        { sig: "using FPostProcess = CPostProcess", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CPostProcess</code> を使う。" }
      ]
    },
    {
      name: "CImageBasedLighting",
      kind: "クラス", header: "render/Ibl.h",
      summary: "<b><t>IBL</t></b>(環境マップから事前積分した光)を構築する。BRDF LUT・環境<t>キューブマップ</t>・拡散 irradiance・鏡面 prefilter を生成し、<code>CPbrShader::SetIbl</code> に渡す。equirect HDR(.hdr)読込や <t>球面調和</t>(SH9)計算、skybox 描画も持つ。<b>Diligent backend 専用</b>。",
      when: "PBR の環境光を本格化して、金属に映り込みを、全体に自然な間接光を与えたいとき。",
      members: [
        { sig: "TResult&lt;void&gt; EnsureBrdfLut(IRhiDevice&, IRhiCommandList&)", desc: "BRDF LUT(256x256 RG16F)を初回だけ生成。" },
        { sig: "TResult&lt;void&gt; EnsureEnvCubemap(IRhiDevice&, IRhiCommandList&, const CSky& sky)", desc: "<code>CSky</code> の空グラデーションと太陽から環境 cubemap(1024² ×6)をキャプチャ。動的な fallback 雲は含めない。" },
        { sig: "TResult&lt;void&gt; LoadEquirectHdrFromMemory(IRhiDevice&, IRhiCommandList&, const f32* rgba_float, u32 width, u32 height)", desc: "equirect HDR 画像(.hdr 等)から env cubemap を作り直す。" },
        { sig: "TResult&lt;void&gt; EnsureIrradiance(...) / EnsurePrefilter(...)", desc: "環境から拡散 irradiance(64² ×6) / 鏡面 prefilter(512² ×6, 7 mip)を生成。" },
        { sig: "static void ComputeSh9FromEquirect(const f32* rgba_float, u32 w, u32 h, FVec4 out_sh_rgb[9])", desc: "equirect 画像から SH9 係数 9 個を CPU 計算(diffuse irradiance の圧縮版)。" },
        { sig: "void DrawSkybox(...) / void DrawEnvSkybox(...)", desc: "cubemap を全画面 skybox として現在の RT に描く既存互換入口。" },
        { sig: "void DrawSkyboxCameraRelative(...) / void DrawEnvSkyboxCameraRelative(...)", desc: "カメラ相対逆行列から環境空を描き、遠方座標でも視線精度を保つ。" },
        { sig: "IRhiTexture* BrdfLut() / EnvCubemap() / IrradianceMap() / PrefilterMap() / u32 PrefilterMips()", desc: "生成済みテクスチャの取得(SetIbl に渡す)。" },
        { sig: "bool HasBrdfLut() / HasEnvCubemap() / HasIrradianceMap() / HasPrefilterMap()", desc: "各リソースが生成済みか。" },
        { sig: "void ResetEnvCubemap()", desc: "env(と依存先)だけ破棄。CSky preset 切替で作り直す前に <code>WaitIdle()</code> 必須。" },
        { sig: "using FImageBasedLighting = CImageBasedLighting", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CImageBasedLighting</code> を使う。" }
      ]
    },
    {
      name: "CShadowMap",
      kind: "クラス", header: "render/ShadowMap.h",
      summary: "有向光源の<b><t>シャドウマップ</t></b>(ortho 投影 depth)。単一カスケード(伝統的)と<b><t>CSM</t></b>(カメラ frustum を距離で 2〜4 分割し近景高解像・遠景広範囲を 1 枚の atlas に並べる)の 2 モード。",
      when: "3D シーンに影を落としたいとき。広い屋外で遠くまで鮮鋭な影が要るなら CSM。<code>CStandardShader</code>/<code>CPbrShader</code> の SetShadowMap に渡す。",
      members: [
        { sig: "static constexpr u32 kMaxCascades = 4", desc: "カスケード上限。" },
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, u32 size = 2048, u32 cascade_count = 1)", desc: "cascade_count=1 で単一 2D depth、2 以上で CSM atlas。" },
        { sig: "bool BeginFrame(u32 required_casters_per_cascade = 0)", desc: "毎フレームの shadow pass 記録前に呼び、各 cascade の正確な caster 数を渡す。Init で予約した全 cascade を先行確保できなければ false。" },
        { sig: "if (!sm.BeginFrame(static_cast&lt;u32&gt;(casters.Size()))) return;", desc: "shadow pass に記録する caster 数を先に渡し、予約に失敗したフレームは描画を記録しない。" },
        { sig: "[[deprecated]] kMaxCasterDrawsPerCascade = 256 / kMaxCasterDrawsPerFrame", desc: "旧固定リングとのソース互換用の目安値。caster プールは必要数まで増えるため、どちらもハード上限ではない。" },
        { sig: "void SetDirectionalLight(FVec3 light_dir, FVec3 scene_center, f32 scene_radius)", desc: "単一カスケード用に光源 ortho を設定。" },
        { sig: "void SetDirectionalLightCascades(FVec3 light_dir, const FMat4& view, const FMat4& proj, f32 near_z, f32 far_z, f32 lambda = 0.5f)", desc: "CSM 用に frustum を near→far で実用分割し各 ortho を計算。lambda は uniform↔log のブレンド。" },
        { sig: "void SetCurrentCascade(u32 cascade) / bool TrySetCaster(const FMat4& model)", desc: "描画する cascade を選ぶ / キャスターの<t>モデル行列</t>を draw 専用 CB に設定。false ならその draw を記録しない。" },
        { sig: "IRhiTexture* DepthTexture() / IRhiPipeline* CasterPipeline() / IRhiBuffer* LightCB() / CasterObjectCB()", desc: "シャドウパスで使う深度・パイプライン・定数バッファ。" },
        { sig: "FMat4 LightViewProjection(u32 cascade = 0) / f32 CascadeSplit(u32) / u32 CascadeCount()", desc: "主パスで影をサンプルするための light VP と分割情報。" },
        { sig: "FViewport CascadeViewport(u32) / FScissorRect CascadeScissor(u32)", desc: "atlas 内の各 cascade 領域の<t>ビューポート</t>/<t>シザー</t>。" },
        { sig: "using FShadowMap = CShadowMap", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CShadowMap</code> を使う。" }
      ]
    },
    {
      name: "FDebugDraw3D",
      kind: "クラス", header: "render/DebugDraw.h",
      summary: "3D の<b>デバッグ線描画</b>(LineList)。コライダーの<t>ワイヤーフレーム</t>・<t>AABB</t>・レイなどを色付きで重ねる。深度なしで常に手前に見える overlay。",
      when: "当たり判定や法線、デバッグ用の補助線を画面に重ねて確認したいとき。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, EFormat rt_format, u32 max_lines = 16384)", desc: "初期化。0 は 1 本へ補正する。容量計算、CPU 頂点領域、GPU 資源生成に失敗した場合は、以前の資源・頂点・容量を変更しない。" },
        { sig: "void Begin()", desc: "線の蓄積を開始(リセット)。" },
        { sig: "bool TryLine(FVec3 a, FVec3 b, FVec4 color)", desc: "2 点間の線を全頂点追加する。容量不足なら変更せず false。" },
        { sig: "void Line(FVec3 a, FVec3 b, FVec4 color)", desc: "TryLine の既存 void 互換入口。失敗時は何も変更しない。" },
        { sig: "bool TryAabb(const FAabb3& box, FVec4 color)", desc: "軸並行ボックスの 12 辺を一括追加する。容量不足なら変更せず false。" },
        { sig: "void Aabb(const FAabb3& box, FVec4 color)", desc: "TryAabb の既存 void 互換入口。失敗時は何も変更しない。" },
        { sig: "bool TryWireframe(const FVec3* positions, u32 vertex_count, const u32* indices, u32 index_count, FVec4 color)", desc: "範囲外 index の三角形を飛ばし、末尾 1、2 index を無視して有効な全辺を一括追加する。有効な三角形 0 件は true、null または容量不足は変更せず false。" },
        { sig: "void Wireframe(const FVec3* positions, u32 vertex_count, const u32* indices, u32 index_count, FVec4 color)", desc: "TryWireframe の既存 void 互換入口。同じ skip・末尾無視を保ち、失敗時は何も変更しない。" },
        { sig: "void End(IRhiCommandList& cl, const FMat4& view_proj)", desc: "現在 bind 中のターゲットへ全線を描画する。" },
        { sig: "u32 LineCount() const", desc: "蓄積済みの線数。" },
        { sig: "using CDebugDraw3D = FDebugDraw3D", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>FDebugDraw3D</code> を使う。" }
      ]
    },
    {
      name: "CLighting2D / FLight2D / CBlobShadow",
      kind: "クラス / 構造体", header: "render/Light2D.h",
      summary: "<b>2D 動的ライティング + ソフト影</b>(Core Keeper 風)。複数のカラー<t>点光源</t>と、スプライトのシルエットから落ちる柔らかい影を扱う。<b>CBlobShadow</b> は光源計算なしの激軽な足元の楕円影(fallback)。",
      when: "暗い洞窟をたいまつで照らす等、トップダウン/横スクロール 2D で雰囲気のあるライティングをしたいとき。",
      members: [
        { sig: "static constexpr u32 kMaxLights = 16", desc: "同時ライト上限。" },
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, EFormat color_format, u32 width, u32 height) / Resize(...)", desc: "scene/occluder RT と合成パイプラインを作る。" },
        { sig: "void SetAmbient(FVec3) / ClearLights() / bool AddLight(const FLight2D&) / u32 LightCount()", desc: "環境光・ライトのクリア/追加(上限到達で false)・数。" },
        { sig: "void SetShadowQuality(u32 march_steps, u32 ray_count)", desc: "影の march 数 / 面光源近似レイ本数(多いほど滑らか/重い)。" },
        { sig: "void BeginScene/EndScene/BeginOccluders/EndOccluders(IRhiCommandList&)", desc: "世界 albedo と影シルエットを描く各 bracket。" },
        { sig: "void Composite(IRhiCommandList& cl, u32 screen_w, u32 screen_h)", desc: "scene × (ambient + Σ light·影) を現在の RT(通常バックバッファ)へ合成。" },
        { sig: "struct FLight2D{ FVec2 pos, f32 radius, FVec3 color, f32 intensity, f32 softness }", desc: "2D 点光源 1 個。softness 0=くっきり〜1=とても柔らかい。" },
        { sig: "CBlobShadow: Init(IRhiDevice&, u32 resolution = 64) / Draw(CSpriteBatch& sb, cx, cy, w, h, alpha, color)", desc: "柔らかい放射状テクスチャを 1 枚作り、足元の影として描く軽量影。" },
        { sig: "using FBlobShadow = CBlobShadow", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CBlobShadow</code> を使う。" },
        { sig: "using FLighting2D = CLighting2D", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CLighting2D</code> を使う。" }
      ]
    },
    {
      name: "CRefractionShader",
      kind: "クラス", header: "render/RefractionShader.h",
      summary: "<b>スクリーンスペース屈折</b>シェーダ。ガラス・水・氷など透明屈折オブジェクトを、描画済みの背景を屈折方向にずらしてサンプルして表現する。粗さ(フロスト)・色収差(分散)・厚みマップ(Beer-Lambert 吸収)に対応。",
      when: "屈折するガラスや水を本格的に描きたいとき。背景の複製テクスチャと環境キューブマップを用意して使う。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, EFormat rt_format, EFormat depth_format)", desc: "初期化(rt は通常 HDR)。" },
        { sig: "void SetFrame(const FMat4& view_projection, FVec3 camera_pos)", desc: "毎フレームのカメラ設定。" },
        { sig: "void SetBackDepth(IRhiTexture* back_depth, f32 near, f32 far, u32 screen_w, u32 screen_h)", desc: "背面深度から per-pixel の実厚みを計算する厚みマップ。null でスカラー厚みに戻る。" },
        { sig: "void SetObject(const FMat4& model, f32 ior, f32 thickness, FVec3 tint, f32 roughness = 0, f32 dispersion = 0)", desc: "屈折率・厚み・吸収色・荒さ・分散。" },
        { sig: "void DrawMesh(IRhiCommandList& cmd, const FGpuMesh& mesh, const FMat4& model, IRhiTexture& background, IRhiTexture& env, f32 ior, f32 thickness, FVec3 tint, f32 roughness, f32 dispersion)", desc: "設定 + 描画をまとめた便利 API。background は背景の複製、env は反射用 cubemap。" },
        { sig: "IRhiPipeline* Pipeline() / IRhiBuffer* PerFrameCB() / PerObjectCB()", desc: "手描き用アクセサ。" },
        { sig: "using FRefractionShader = CRefractionShader", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CRefractionShader</code> を使う。" }
      ]
    },
    {
      name: "CSsr",
      kind: "クラス", header: "render/Ssr.h",
      summary: "<b><t>SSR</t></b>(Screen-Space Reflection)。シーンの HDR 色 + 深度 + 法線 G-buffer から、各ピクセルの反射レイを画面空間で <t>レイマーチ</t>(DDA)して映り込みを作る。<t>テンポラル</t>累積でジャギーを均し、Hi-Z で高速化できる。",
      when: "床や水面に周囲が映り込む反射を、リアルタイムで(IBL より動的に)出したいとき。<code>CPbrShader::SetSsr</code> や <code>CPostProcess</code> に渡す。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, EFormat hdr_format, u32 width, u32 height) / Resize(...)", desc: "出力テクスチャと march/temporal パイプラインを作る。" },
        { sig: "void Render(IRhiDevice&, IRhiCommandList&, scene_color, scene_depth, normal_gbuffer, view_proj, inv_view_proj, prev_view_proj, eye, intensity, motion_texture, hiz_texture)", desc: "raw march → temporal の 2 pass。motion/Hi-Z は任意(高速・ghost 低減)。" },
        { sig: "IRhiTexture* OutputTexture() / RawTexture() / EFormat OutputFormat()", desc: "temporal 累積後 / raw / 出力フォーマット。" },
        { sig: "using FSsr = CSsr", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSsr</code> を使う。" }
      ]
    },
    {
      name: "CSsao",
      kind: "クラス", header: "render/Ssao.h",
      summary: "<b><t>SSAO</t></b>(Screen-Space Ambient Occlusion、GTAO ベース)。深度 + 法線 G-buffer から各ピクセルの遮蔽量を求め、隙間や接地部を自然に暗くする。コンタクトシャドウも出力。深度対応の<t>バイラテラルブラー</t>でノイズを除去する。",
      when: "オブジェクトの隙間・接地・しわなどに陰影を足してリアル感を上げたいとき。出力を <code>CPbrShader::SetSsao</code> に渡す。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, u32 width, u32 height) / Resize(...)", desc: "出力 RT と AO/blur パイプラインを作る。" },
        { sig: "void Render(IRhiDevice&, IRhiCommandList&, scene_depth, normal_gbuffer, view_proj, inv_view_proj, view, eye, light_dir, intensity = 1.0f, radius = 0.5f)", desc: "AO + コンタクトシャドウを計算しブラーまでかける。" },
        { sig: "IRhiTexture* OutputTexture() / RawTexture()", desc: "ブラー後(通常こちら) / ブラー前。" },
        { sig: "using FSsao = CSsao", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSsao</code> を使う。" }
      ]
    },
    {
      name: "CSsgi",
      kind: "クラス", header: "render/Ssgi.h",
      summary: "<b><t>SSGI</t></b>(Screen-Space Global Illumination)。各ピクセルから法線半球にレイを画面空間で march し、当たった先の色を 1 バウンスの間接光として集める。SSAO に色サンプリングを足した発展版。ブラー + <t>テンポラル</t>累積でノイズを抑える。",
      when: "壁の色が床に移る等の「色のにじみ(color bleeding)」を加えて、間接光のリアル感を出したいとき。出力を <code>CPbrShader::SetSsgi</code> に渡す。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, u32 width, u32 height) / Resize(...)", desc: "出力 RT と raw/blur/temporal パイプラインを作る。" },
        { sig: "void Render(IRhiDevice&, IRhiCommandList&, scene_color, scene_depth, normal_gbuffer, view_proj, inv_view_proj, prev_view_proj, eye, intensity = 1.0f, max_distance = 5.0f, motion_texture = nullptr)", desc: "raw → blur → temporal の 3 pass。" },
        { sig: "IRhiTexture* OutputTexture() / RawTexture()", desc: "temporal 累積後 / raw。" },
        { sig: "using FSsgi = CSsgi", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSsgi</code> を使う。" }
      ]
    },
    {
      name: "CMotionVector",
      kind: "クラス", header: "render/MotionVector.h",
      summary: "シーン全 mesh を再ラスタライズして<b>モーションベクトル + 法線 G-buffer</b>を書き出すパス。motion(画面空間の移動量、prev_uv−curr_uv)は <t>TAA</t> の正確な reproject に、world 法線は <t>SSR</t>/<t>SSGI</t>/<t>SSAO</t> の入力に使う。",
      when: "TAA で動く物体のゴーストを消したい / 画面空間効果に高品質な法線を供給したいとき。全対象 draw の成功を確認してから出力を公開する。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, u32 width, u32 height) / Resize(...)", desc: "motion RT(RG16F)+ normal RT(RGBA16F)+ 内部 depth を作る。" },
        { sig: "bool BeginFrame(u32 required_draws = 0)", desc: "毎フレームの pass 前に正確な対象数を渡し、永続 per-object CB pool を先行確保する。UINT32_MAX は無効 sentinel のため false。確保失敗時も既存 pool は保持され、後続フレームで再試行できる。" },
        { sig: "bool Begin(IRhiCommandList& cl, const FMat4& view_proj, const FMat4& prev_view_proj)", desc: "motion RT を 0 クリアしパイプライン設定。jitter なしの VP を渡す。MRT 開始失敗時は false。" },
        { sig: "bool DrawMesh(IRhiCommandList& cl, const FGpuMesh& mesh, const FMat4& model, const FMat4& prev_model)", desc: "1 mesh の motion を描く。静的 mesh は prev_model に model と同値を渡す。draw を完全に記録できなければ false。" },
        { sig: "void End(IRhiCommandList& cl)", desc: "パス終了(主パス RT へ復帰)。" },
        { sig: "u32 ObjectBufferCapacity() / ObjectDrawCount()", desc: "永続 pool の使用可能数 / 今フレームに記録できた draw 数。対象数との一致を出力公開条件に使う。" },
        { sig: "IRhiTexture* OutputTexture() / OutputNormalTexture()", desc: "モーションベクトル / world 法線テクスチャ。BeginFrame・Begin・全 DrawMesh が成功したフレームだけ後段へ渡す。" },
        { sig: "using FMotionVector = CMotionVector", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CMotionVector</code> を使う。" }
      ]
    },
    {
      name: "CHiZ",
      kind: "クラス", header: "render/HiZ.h",
      summary: "<b>Hi-Z</b>(Hierarchical-Z)粗い min-depth バッファ。シーン深度から 1/8 解像度の「ブロック内最近接 NDC depth」を一発で焼く。<t>SSR</t> のレイマーチが空中の texel を一気にスキップ(skip-ahead)するのに使い、4〜6 倍高速化する。",
      when: "<code>CSsr</code> の高速化に。毎フレーム主パス深度の完成後・SSR の前に Build して Texture() を渡す。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, u32 src_width, u32 src_height) / Resize(...)", desc: "scene depth 解像度を渡す。Hi-Z は ceil(src/8) サイズで確保される。" },
        { sig: "void Build(IRhiDevice&, IRhiCommandList&, IRhiTexture& scene_depth)", desc: "shader-visible な D32 深度から coarse min を焼く。" },
        { sig: "IRhiTexture* Texture()", desc: "1/8 解像度の min-depth RT(.r=min depth)。SSR に渡す。" },
        { sig: "u32 SrcWidth() / SrcHeight() / Width() / Height() / static constexpr u32 kBlockSize = 8", desc: "元/縮約後の解像度とブロックサイズ。" },
        { sig: "using FHiZ = CHiZ", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CHiZ</code> を使う。" }
      ]
    },
    {
      name: "CBlit",
      kind: "クラス", header: "render/Blit.h",
      summary: "フルスクリーンの<b>テクスチャコピー(<t>ブリット</t>)</b>ユーティリティ。RHI に直接 GPU コピーが無いため、フルスクリーン三角形 + テクスチャサンプルで 1 枚の RT をもう 1 枚へピクセル等価でコピーする。",
      when: "屈折の背景捕捉など、描画済み RT を「複製」して後段が読めるようにしたいとき(同一 RT の read+write 不可を回避)。",
      members: [
        { sig: "TResult&lt;void&gt; Init(IRhiDevice&, EFormat rt_format)", desc: "出力 RT のフォーマットを焼き込んで初期化。" },
        { sig: "void Copy(IRhiCommandList& cmd, IRhiTexture& src, IRhiTexture& dst)", desc: "src を dst へフルスクリーンコピー。clear 不要(全 pixel 上書き)、viewport は dst サイズに自動。" },
        { sig: "IRhiPipeline* Pipeline()", desc: "内部パイプラインの取得。" },
        { sig: "using FBlit = CBlit", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CBlit</code> を使う。" }
      ]
    },
    {
      name: "EWaterSurface3DProfile",
      kind: "列挙型", header: "render/EWaterSurface3DProfile.h",
      summary: "共通3D水面へ用途別の安全な初期値を選ぶ。",
      when: "水たまり、プール、川、湖、海のauthoring開始値を揃える時。",
      members: [
        { sig: "Puddle / Pool / River / Lake / Ocean", desc: "形状は所有せず、波・流れ・透明度・泡の初期値だけを選ぶ。" }
      ]
    },
    {
      name: "FWaterSurface3DParams",
      kind: "構造体", header: "render/WaterSurface3DParams.h",
      summary: "形状に依存しない3D水面の波、流れ、光学、泡の値。",
      when: "CWaterSurface3DまたはAWaterSurface3DComponentへ水質を指定する時。",
      members: [
        { sig: "static FWaterSurface3DParams ForProfile(EWaterSurface3DProfile profile)", ret: "用途別の初期値", desc: "未知値ではLakeを返す。返した後に個別fieldを上書きできる。" }
      ]
    },
    {
      name: "CWaterSurface3D",
      kind: "クラス", header: "render/WaterSurface3D.h",
      summary: "3D水面の描画資源、反射、屈折、波、surface別interactionを管理する。",
      when: "有限な矩形水面またはlocal XZの任意輪郭水面をsceneへ描画する時。旧名FWaterSurface3Dも互換別名として使える。",
      members: [
        { sig: "static TResult&lt;void&gt; CreateAdaptivePlaneMesh(IRhiDevice&, FGpuMesh&, u32 cells = 96)", desc: "camera近傍へ密度を寄せる正規化XZ格子を成功時だけ出力する。" },
        { sig: "void DrawAdaptivePlane(...)", desc: "有限な全体寸法を保ったまま、矩形水面の格子密度をcamera近傍へ連続再配置して描く。" },
        { sig: "void DrawMesh(...)", desc: "水たまりや蛇行する川など、local XZの任意輪郭meshを形状変更せず描く。" },
        { sig: "using FWaterSurface3D = CWaterSurface3D", desc: "旧名を使う既存コード向けの互換別名。" }
      ]
    },
    {
      name: "FAtmosphere / FAtmosphereParams",
      kind: "クラス / 構造体", header: "render/Atmosphere.h",
      summary: "<b>物理大気散乱</b>(Rayleigh + Mie 単散乱)を CPU で評価して equirect 画像に焼く。結果を <code>CImageBasedLighting::LoadEquirectHdrFromMemory</code> に通せば、物理ベースの空から env→irradiance→prefilter の <t>IBL</t> 一式が組める。",
      when: "プリセット空ではなく、物理的に正しい青空/夕焼けを太陽方角から生成して背景・環境光に使いたいとき。",
      members: [
        { sig: "static TArray&lt;f32&gt; BakeEquirect(u32 width, u32 height, const FAtmosphereParams& params)", ret: "RGBA float 配列", desc: "CPU で equirect 画像(w×h×4 float、v=0 が天頂)を焼いて返す(move)。" },
        { sig: "struct FAtmosphereParams{ FVec3 sun_dir, sun_intensity, ground_albedo, u32 ray_steps, sun_steps }", desc: "太陽方角・輝度・地面アルベドとレイのサンプル数。" },
        { sig: "using FAtmosphere = CAtmosphere", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAtmosphere</code> を使う。" }
      ]
    },
    {
      name: "CSkyAtmosphere",
      kind: "クラス", header: "render/Atmosphere.h",
      summary: "空と大気散乱の描画資源および更新処理を管理する。",
      when: "物理大気をsceneへ描画する時。",
      members: [
        { sig: "IRhiTexture* BuildAerialPerspectiveCameraRelative(...)", desc: "カメラ相対逆行列から物理大気と高さ霧の体積表を作る。水平一様な媒質はX/Z移動だけでは再生成しない。" },
        { sig: "void CompositeAerialPerspectiveCameraRelative(...) / void CompositeLocalFogCameraRelative(...)", desc: "カメラ相対位置から深度距離を復元し、遠方でも大気と霧を正しい距離で終端する。" },
        { sig: "BuildAerialPerspective(...) / CompositeAerialPerspective(...) / CompositeLocalFog(...)", desc: "ワールド逆行列を受け取る既存コード向け互換入口。" },
        { sig: "using FSkyAtmosphere = CSkyAtmosphere", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSkyAtmosphere</code> を使う。" }
      ]
    },
    {
      name: "CBurnEffect",
      kind: "クラス", header: "render/BurnEffect.h",
      summary: "burn表現の描画資源と実行処理を管理する。",
      when: "材質へ燃焼表現を適用する時。",
      members: [
        { sig: "using FBurnEffect = CBurnEffect", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CBurnEffect</code> を使う。" }
      ]
    },
    {
      name: "CFxaa",
      kind: "クラス", header: "render/Fxaa.h",
      summary: "FXAAによる画面全体のアンチエイリアス処理を実行する。",
      when: "post processで輪郭のぎざつきを軽減する時。",
      members: [
        { sig: "using FFxaa = CFxaa", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CFxaa</code> を使う。" }
      ]
    },
    {
      name: "CVolumetricClouds",
      kind: "クラス", header: "render/Sky.h",
      summary: "volumetric cloudの描画資源と更新処理を管理する。",
      when: "立体的な雲をsceneへ描画する時。",
      members: [
        { sig: "using FVolumetricClouds = CVolumetricClouds", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CVolumetricClouds</code> を使う。" }
      ]
    },
    {
      name: "CSubsurfaceScattering",
      kind: "クラス", header: "render/SubsurfaceScattering.h",
      summary: "subsurface scatteringの描画資源とpost processを管理する。",
      when: "肌や蝋のような表面下散乱を表現する時。",
      members: [
        { sig: "using FSubsurfaceScattering = CSubsurfaceScattering", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSubsurfaceScattering</code> を使う。" }
      ]
    },
    {
      name: "CVertexScatter",
      kind: "クラス", header: "render/VertexScatter.h",
      summary: "頂点scatter処理の描画資源と実行手順を管理する。",
      when: "頂点単位の分散配置をGPUで処理する時。",
      members: [
        { sig: "using FVertexScatter = CVertexScatter", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CVertexScatter</code> を使う。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "RHI": "Render Hardware Interface。DX12 / Vulkan / Diligent などの GPU API の違いを隠す、バックエンド非依存の薄い抽象層。ACS では <t>IRhiDevice</t> 等がこれ。",
  "バックエンド": "実際に GPU を叩く実装。ACS は raw-DX12(軽量 2D/基本 3D)と Diligent(高度 3D 公式)の 2 つを持つ。",
  "IRhiDevice": "GPU そのものを表す <t>RHI</t> の中心インターフェース。あらゆる GPU リソースをここから作る。",
  "デバイス": "GPU とのやり取りを表す <t>IRhiDevice</t> のこと。",
  "スワップチェイン": "画面に表示する複数のバックバッファを回しながら、描画と表示を分けて行う仕組み。",
  "コマンドリスト": "GPU に送る描画命令を記録しておくバッファ。最後にまとめて GPU へ投入する。",
  "定数バッファ": "シェーダに渡す小さなパラメータ群(行列・色・ライト等)を入れる GPU バッファ(cbuffer / CB)。",
  "パイプライン": "1 回の描画方式(VS+PS・頂点レイアウト・ブレンド・深度等)を丸ごと固めたオブジェクト。PSO とも。",
  "PSO": "Pipeline State Object。<t>パイプライン</t>の別名。",
  "シェーダ": "GPU 上で頂点やピクセルを計算する小さなプログラム。ACS では <t>HLSL</t> で書く。",
  "HLSL": "DirectX 系のシェーダ言語(High-Level Shading Language)。",
  "コンピュート": "描画ではなく汎用計算を行うシェーダステージ(コンピュートシェーダ)。",
  "フォーマット": "1 ピクセル/要素のビット配置と型(RGBA8、HDR float、深度など)。ACS では <t>EFormat</t>。",
  "ピクセルフォーマット": "テクスチャ 1 ピクセルのデータ形式。<t>フォーマット</t>参照。",
  "ビューポート": "描画する画面上の矩形領域と深度の出力範囲。",
  "シザー": "この矩形の外側のピクセルを描かないクリッピング設定。",
  "バリア": "GPU リソースの用途状態を切り替える同期。ACS では普段は内部で自動管理される。",
  "SRV": "Shader Resource View。テクスチャ等をシェーダから読むためのビュー。",
  "SRB": "Shader Resource Binding。Diligent でシェーダにリソースを結び付ける仕組み。名前ベースで lookup する。",
  "MRT": "Multiple Render Targets。1 回の描画で複数のレンダーターゲットに同時出力すること。",
  "Present": "描き終えたバックバッファを実際の画面に表示する操作。",
  "VSync": "画面のリフレッシュに描画提示を同期させ、ティアリングを防ぐ設定。",
  "アトラス": "多数の小画像(グリフやタイル)を 1 枚にまとめたテクスチャ。UV で各部分を取り出す。",
  "セマンティック": "頂点要素の役割を示す HLSL の名前(POSITION / TEXCOORD / COLOR 等)。",
  "バッチ": "同じ設定の描画をまとめて 1 回の命令で送り、描画回数を減らして高速化すること。",
  "スプライト": "2D の 1 枚絵(キャラ・アイコン等)。矩形にテクスチャを貼って描く。",
  "ステンシル": "ピクセルごとの整数マスク。任意形状の描画範囲制限(くり抜き)に使う。",
  "モデル行列": "オブジェクトをワールド空間に配置する(平行移動・回転・拡縮)変換行列。",
  "有向光源": "太陽光のように無限遠から来る、向きだけを持つ平行光。",
  "点光源": "ある位置から全方向に広がり、距離で減衰する光。",
  "エリアライト": "面(矩形等)から放射される光。点光源より柔らかい陰影になる。",
  "PBR": "Physically Based Rendering。金属度・粗さなど物理量で材質を表すリアルな描画手法。",
  "IBL": "Image-Based Lighting。環境マップを事前積分し、周囲全体からの光として材質に映り込ませる手法。",
  "キューブマップ": "6 面の正方形テクスチャで全方位を覆う画像。環境マップやスカイボックスに使う。",
  "ミップマップ": "縮小版を段階的に持つテクスチャ。距離や粗さに応じて使い分けジャギーを防ぐ。",
  "球面調和": "方向ごとの光を少数の係数(SH)で表す近似。間接光(irradiance)の圧縮表現に使う。",
  "シャドウマップ": "光源視点の深度を焼いたテクスチャ。各点が影かどうかを判定して影を落とす技法。",
  "CSM": "Cascaded Shadow Map。カメラの距離帯ごとに解像度の違うシャドウマップを使い、遠景まで鮮鋭な影を保つ。",
  "PCSS": "Percentage-Closer Soft Shadows。遮蔽物との距離に応じて影の縁をぼかし、柔らかい影(penumbra)を作る技法。",
  "ボーンパレット": "スケルタルアニメで各ボーンの変換行列を並べた配列。GPU スキニングでシェーダに送る。",
  "ポストプロセス": "シーン描画後に画面全体へかける後処理(Bloom・トーンマップ・色調整など)。",
  "HDR": "High Dynamic Range。明暗の幅を float で広く保持する描画。最後にトーンマップで表示用に圧縮する。",
  "Bloom": "明るい部分を周囲ににじませて発光感を出すポストプロセス効果。",
  "トーンマップ": "HDR の広い輝度を表示可能な範囲へ圧縮する変換(ACES / AgX 等)。",
  "自動露出": "シーンの明るさを測り露出を自動調整する、目の順応(eye adaptation)を模した処理。",
  "TAA": "Temporal Anti-Aliasing。前フレームの結果と混ぜてジャギーを時間方向に均すアンチエイリアス。",
  "テンポラル": "複数フレームにわたって結果を累積/平均し、ノイズやジャギーを減らす手法。",
  "SSR": "Screen-Space Reflection。画面に映っている情報だけから反射(映り込み)をリアルタイムに作る技法。",
  "SSAO": "Screen-Space Ambient Occlusion。深度から隙間や接地部の遮蔽を求めて陰影を足す技法。",
  "SSGI": "Screen-Space Global Illumination。画面情報から 1 バウンスの間接光(色のにじみ)を加える技法。",
  "レイマーチ": "レイに沿って少しずつ進みながらヒットを探す手法。SSR 等の画面空間効果で使う。",
  "バイラテラルブラー": "深度などのエッジを保ちつつノイズだけをぼかすブラー。",
  "ブリット": "テクスチャ/バッファの内容を別の領域へコピーする操作。",
  "ワイヤーフレーム": "面を塗らずエッジ(線)だけで形を描く表示。デバッグに使う。",
  "AABB": "Axis-Aligned Bounding Box。軸に平行な直方体の境界ボックス。当たり判定の粗い包囲に使う。",
  "ガンマ": "表示用の非線形な明るさ変換(sRGB)。リニア計算後に適用する。",
  "サブサーフェス": "肌や蝋のように光が表面下で散乱して透ける質感(subsurface scattering)。",
  "サンプラ": "テクスチャをシェーダで読むときのフィルタ/アドレッシング設定。",
  "アドレッシング": "UV が 0..1 の外に出たときの扱い(繰り返し・鏡像・端固定・境界色)。",
  "グラデーション": "色が滑らかに変化する塗り。頂点カラーの補間で作れる。",
  "BLENDINDICES": "スキンメッシュ頂点が影響を受けるボーンの番号を持つ HLSL セマンティック。GPU スキニングで <t>ボーンパレット</t>を引くのに使う。",
  "WEIGHT": "スキンメッシュ頂点が各ボーンから受ける影響度(重み)を持つ HLSL セマンティック。合計 1 になるよう正規化される。"
});
