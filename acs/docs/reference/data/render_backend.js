/* ACS リファレンス — render（描画バックエンド: DX12 / Diligent）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "render_backend",
  order: 31,
  title: "render — 描画バックエンド(DX12 / Diligent)",
  blurb: "GPU に実際に絵を描かせる<t>RHI</t>(描画ハードウェア抽象)の<b>中身</b>。共通インターフェイス <code>IRhiDevice</code> 等を、Windows ネイティブの <t>DX12</t> と、複数 API を束ねる <t>Diligent</t> の 2 系統で実装します。<b>ふだんはこれらを直接 new せず、<code>CreateRhiDevice()</code> 等のファクトリ越しに <code>IRhi*</code> ハンドルだけ触ります</b>。ここに載っているのは「どのバックエンドが裏で動いているか」を知りたい・拡張したい人向けの内部実装です。",
  types: [
    {
      name: "FDx12Device",
      kind: "クラス", header: "render/Dx12/Dx12Device.h",
      summary: "<t>DX12</t> ネイティブの<t>RHIデバイス</t>実装(<code>IRhiDevice</code> 派生)。<code>ID3D12Device</code>・コマンドキュー・<t>ディスクリプタヒープ</t>(SRV/RTV/DSV)・フレーム<t>フェンス</t>を抱える、DX12 描画の中枢です。",
      when: "DX12 バックエンドでビルドした時に <code>CreateRhiDevice()</code> が内部で生成する実体。直接 new せず、返ってきた <code>IRhiDevice</code> を使うのが基本。生の <code>ID3D12Device</code> が要る低レベル統合(ImGui バックエンド等)でだけダウンキャストして触ります。",
      sample: "FDeviceConfig cfg;\ncfg.enable_debug_layer = true;       // Debug ビルド推奨\nauto dev = CreateRhiDevice(cfg).Value();  // 中身が FDx12Device\nprintf(\"%s / %s\\n\", dev-&gt;BackendName(), dev-&gt;AdapterName());",
      members: [
        { sig: "const char* BackendName() const", ret: "\"DX12\"", desc: "バックエンド名。常に <code>\"DX12\"</code> を返す。" },
        { sig: "const char* AdapterName() const", ret: "GPU 名", desc: "選ばれた<t>アダプタ</t>(GPU)の名前。デバッグ表示用。" },
        { sig: "void WaitIdle()", desc: "GPU の処理が全部終わるまで CPU 側で待つ。<t>フェンス</t>を使う。シャットダウン前や破棄前に必須。" },
        { sig: "FHrResult Init(const FDeviceConfig& cfg)", ret: "成否(HRESULT)", desc: "DXGI ファクトリ・デバイス・キュー・各<t>ディスクリプタヒープ</t>を作る初期化。<code>CreateRhiDevice</code> から呼ばれる。", when: "通常は直接呼ばない。" },
        { sig: "ID3D12Device* D3DDevice() const", ret: "生デバイス", desc: "生の <code>ID3D12Device</code>。外部ライブラリと統合する時だけ使う。" },
        { sig: "ID3D12CommandQueue* GraphicsQueue() const", ret: "グラフィックスキュー", desc: "描画コマンドを投入するキュー。" },
        { sig: "i32 AllocateSrvSlot() / void FreeSrvSlot(i32)", ret: "スロット番号(-1=失敗)", desc: "シェーダ可視 SRV ヒープ(容量 1024)から 1 スロット確保/返却。テクスチャ作成・破棄時に内部で呼ばれる。" },
        { sig: "i32 AllocateRtvSlot() / AllocateDsvSlot()", desc: "オフスクリーン RT 用 RTV(容量 256)・深度用 DSV(容量 16)を確保。返却は <code>FreeRtvSlot</code>/<code>FreeDsvSlot</code>。" },
        { sig: "D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(i32) const", ret: "GPU ハンドル", desc: "SRV スロットの GPU ハンドル。<t>ルートディスクリプタテーブル</t>へのバインドに使う。" },
        { sig: "u64 SignalGraphicsQueue()", ret: "完了値", desc: "投入後にキューへ<t>フェンス</t>値を <code>Signal</code> し、その値を返す。後で <code>WaitForFenceValue</code> に渡す。" },
        { sig: "void WaitForFenceValue(u64 value)", desc: "指定<t>フェンス</t>値以上に GPU が到達するまで CPU 側で待つ(到達済みなら即 return)。" },
        { sig: "u32 CurrentFrameSlot() / void AdvanceFrameSlot()", ret: "0..1", desc: "<t>フレームインフライト</t>(<code>kFramesInFlight=2</code>)のリングインデックス。Uniform バッファ等が二重バッファリングに使う。" },
        { sig: "IDXGIFactory6* DxgiFactory() const", ret: "生ファクトリ", desc: "内部用。DXGI ファクトリ。<code>FDx12Swapchain</code> がスワップチェイン生成時に使う。" },
        { sig: "ID3D12DescriptorHeap* SrvHeap() / u32 SrvHandleSize() const", desc: "内部用。シェーダ可視 SRV/CBV/UAV ヒープ本体と、1 記述子あたりのバイト幅。バインド時にハンドル計算へ使う。" },
        { sig: "D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(i32 index) const", ret: "CPU ハンドル", desc: "内部用。SRV スロットの CPU ハンドル。テクスチャ作成時にビューを書き込む先。" },
        { sig: "D3D12_CPU_DESCRIPTOR_HANDLE DsvCpuHandle(i32) / RtvCpuHandle(i32) const", ret: "CPU ハンドル", desc: "内部用。確保した DSV / オフスクリーン RTV スロットの CPU ハンドル。<code>CommandList</code> がレンダーターゲット/深度のバインドに使う。" }
      ]
    },
    {
      name: "FDx12Swapchain",
      kind: "クラス", header: "render/Dx12/Dx12Swapchain.h",
      summary: "<t>DX12</t> の<t>スワップチェイン</t>(<code>IRhiSwapchain</code> 派生)。<code>IDXGISwapChain3</code> と、最大 3 枚のバックバッファ・その RTV ヒープを管理し、毎フレーム描画先を切り替えて画面に提示します。",
      when: "<code>CreateRhiSwapchain()</code> が DX12 バックエンドで生成する実体。ウィンドウへ最終結果を出すのに毎フレーム使う(取得→描画→提示)。",
      sample: "FSwapchainConfig sc;\nsc.window = &amp;window;  sc.buffer_count = 2;  sc.vsync = true;\nauto swap = CreateRhiSwapchain(*dev, sc).Value();\nu32 idx = swap-&gt;AcquireNextImage();   // 描く先のバッファ番号\n// ...cmd で描画...\nswap-&gt;Present();                      // 画面へ",
      members: [
        { sig: "u32 AcquireNextImage()", ret: "バッファ番号", desc: "次に書き込めるバックバッファをロックして取得。描画開始前に毎フレーム呼ぶ。" },
        { sig: "void Present()", desc: "描き終えた内容を画面へ反映。<code>vsync</code> 設定に従う。" },
        { sig: "bool Resize(u32 width, u32 height)", ret: "成否", desc: "ウィンドウサイズ変更時にバッファを作り直す。<code>false</code> なら本フレームの描画はスキップして次で再試行する。" },
        { sig: "u32 BufferCount() / Width() / Height() const", desc: "現在のバッファ枚数・解像度。" },
        { sig: "ID3D12Resource* BackBuffer(u32 i) const", ret: "バッファ or null", desc: "内部用。<code>i</code> 番目のバックバッファリソース。範囲外は <code>nullptr</code>。" },
        { sig: "D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRTV(u32 i) const", ret: "RTV ハンドル", desc: "内部用。<code>i</code> 番目バックバッファの RTV。<code>CommandList</code> がレンダーターゲットをバインドする時に使う。" },
        { sig: "ID3D12DescriptorHeap* RtvHeap() const", ret: "RTV ヒープ", desc: "内部用。バックバッファ RTV を並べた専用ディスクリプタヒープ。" }
      ]
    },
    {
      name: "FDx12CommandList",
      kind: "クラス", header: "render/Dx12/Dx12CommandList.h",
      summary: "<t>DX12</t> の<t>コマンドリスト</t>(<code>IRhiCommandList</code> 派生)。1 フレーム分の描画命令(クリア・パイプライン設定・バインド・Draw)を <code>ID3D12GraphicsCommandList</code> へ記録し、GPU に投入します。<t>リソース状態</t>の<t>バリア</t>も内部で面倒を見ます。",
      when: "毎フレームの描画ループの主役。<code>CreateRhiCommandList()</code> で作り、<code>Begin</code>→各種パス→<code>End</code>→<code>Submit</code> の順に回す。",
      sample: "auto cmd = CreateRhiCommandList(*dev).Value();\ncmd-&gt;Begin();\nu32 i = swap-&gt;AcquireNextImage();\ncmd-&gt;BeginRenderToSwapchain(*swap, i, {0.1f,0.1f,0.2f,1});\ncmd-&gt;SetPipeline(*pso);\ncmd-&gt;SetVertexBuffer(*vb, sizeof(FVertex));\ncmd-&gt;Draw(3);\ncmd-&gt;EndRenderToSwapchain(*swap, i);\ncmd-&gt;End();  cmd-&gt;Submit();\nswap-&gt;Present();",
      members: [
        { sig: "void Begin() / End() / Submit()", desc: "記録の開始・終了・GPU 投入。1 フレームにつき 1 セット。" },
        { sig: "void BeginRenderToSwapchain(sc, idx, clear, depth=null, depth_clear=1)", desc: "バックバッファをレンダーターゲットにして指定色でクリア。深度も任意で同時バインド。", when: "通常の画面描画パスの開始。" },
        { sig: "void EndRenderToSwapchain(sc, idx)", desc: "バックバッファ描画を終え、<t>Present 可能状態</t>へ遷移させる。" },
        { sig: "void BeginShadowPass(depth, depth_clear=1) / EndShadowPass(depth)", desc: "深度のみの RT(<t>シャドウマップ</t>)を bind+clear し、終了時に SHADER_RESOURCE 状態へ遷移して主パスでサンプル可能にする。" },
        { sig: "void BeginRenderToTexture(rt, clear, depth=null, depth_clear=1)", desc: "オフスクリーン RT へ描く(HDR / ポストプロセス用)。終了は <code>EndRenderToTexture</code>。", when: "HDR バッファや中間テクスチャに描いてから合成する時。" },
        { sig: "void BeginRenderToTextureLoad(rt, depth=null)", desc: "クリアせず既存内容の上に追記する load 版(opaque の上に屈折オブジェクトを足す等)。" },
        { sig: "void BeginRenderToTextureSlice(rt, slice, mip, clear)", desc: "cubemap 1 面 / 配列 1 スライス / 特定 mip に描く(<code>per_slice_rtv=true</code> で作成済が前提)。", when: "IBL の prefilter cube を面・mip 別に焼く時。" },
        { sig: "void BeginRenderToTextureMrt(rts, rt_count, clear, depth=null, depth_clear=1)", desc: "複数 color RT を同時 bind する MRT 開始。<b>DX12 raw では stub(no-op)</b>で、MRT は Diligent backend のみ実装。" },
        { sig: "void SetViewport(vp) / SetScissor(sr) / SetStencilRef(ref)", desc: "ビューポート・シザー矩形・<t>ステンシル</t>参照値を設定。ステンシル ref は PSO と独立に切り替えられる。" },
        { sig: "void SetPipeline(pipeline)", desc: "次の Draw で使う<t>パイプライン</t>(PSO + ルートシグネチャ)を設定。" },
        { sig: "void SetVertexBuffer(vb, stride) / SetIndexBuffer(ib)", desc: "頂点/インデックスバッファをバインド。インデックス型は <code>EBufferUsage</code> から判定。" },
        { sig: "void SetConstantBuffer(u32 slot, cb) / SetTexture(u32 slot, tex)", desc: "定数バッファ・テクスチャを <code>register(bN)</code>/<code>register(tN)</code> に対応する slot へバインド。" },
        { sig: "void Draw(vertex_count, first_vertex=0)", desc: "非インデックス描画。" },
        { sig: "void DrawIndexed(index_count, first_index=0, base_vertex=0)", desc: "インデックス描画。" },
        { sig: "void* NativeHandle()", ret: "ID3D12GraphicsCommandList*", desc: "生のコマンドリスト。ImGui 等の外部統合用。" }
      ]
    },
    {
      name: "FDx12Buffer",
      kind: "クラス", header: "render/Dx12/Dx12Buffer.h",
      summary: "<t>DX12</t> の GPU バッファ(<code>IRhiBuffer</code> 派生)。頂点・インデックス・定数バッファを <code>ID3D12Resource</code> として確保します。Uniform かつ <code>cpu_writable</code> なら自動でフレーム<t>リングバッファ</t>化され、毎フレーム安全に書き換えられます。",
      when: "<code>CreateRhiBuffer()</code> が DX12 で生成する実体。メッシュの頂点や、毎フレーム変わる行列(定数バッファ)を GPU へ載せる時。",
      sample: "FBufferDesc d;\nd.size = sizeof(FVertex)*3;\nd.usage = EBufferUsage::Vertex;\nd.initial_data = verts;\nauto vb = CreateRhiBuffer(*dev, d).Value();\n// 定数バッファを毎フレーム更新する例:\ncb-&gt;Update(&amp;mvp, sizeof(mvp));",
      members: [
        { sig: "usize Size() const", ret: "バイト数", desc: "1 フレームスロットあたりのサイズ。" },
        { sig: "EBufferUsage Usage() const", desc: "用途(Vertex / Index16 / Index32 / Uniform 等)。" },
        { sig: "void Update(const void* data, usize size, usize offset=0)", desc: "CPU からデータを書き込む(<code>cpu_writable=true</code> で作ったバッファのみ)。フレームリングなら現スロットへ書く。" },
        { sig: "ID3D12Resource* Resource() const", ret: "生リソース", desc: "内部用の生 <code>ID3D12Resource</code>。" },
        { sig: "D3D12_GPU_VIRTUAL_ADDRESS Gpu() const", ret: "GPU 仮想アドレス", desc: "内部用。リング有効なら現フレームスロット分のオフセットを足した先頭アドレスを返す。" }
      ]
    },
    {
      name: "FDx12Texture",
      kind: "クラス", header: "render/Dx12/Dx12Texture.h",
      summary: "<t>DX12</t> のテクスチャ(<code>IRhiTexture</code> 派生)。通常の 2D 画像のほか、レンダーターゲット・深度バッファ・cubemap・テクスチャ配列を扱い、SRV/RTV/DSV の<t>ディスクリプタ</t>を必要に応じて持ちます。",
      when: "<code>CreateRhiTexture()</code> が DX12 で生成する実体。画像を読む・オフスクリーン RT に描く・シャドウ深度を持つ、いずれの用途も <code>FTextureDesc</code> のフラグで切り替える。",
      sample: "FTextureDesc d;\nd.width = 256; d.height = 256;\nd.format = EFormat::R8G8B8A8_UNorm;\nd.initial_data = pixels; d.initial_data_size = 256*256*4;\nauto tex = CreateRhiTexture(*dev, d).Value();\ncmd-&gt;SetTexture(0, *tex);   // パイプラインで texture_slots&gt;=1 が必要",
      members: [
        { sig: "u32 Width() / Height() / MipLevels() / ArraySize() const", desc: "解像度・mip 数・配列枚数。" },
        { sig: "EFormat PixelFormat() const", desc: "ピクセル<t>フォーマット</t>。" },
        { sig: "bool IsCubemap() / IsDepth() / HasStencil() const", desc: "cubemap か、深度バッファか、ステンシル成分(<code>D24_UNorm_S8_UInt</code>)を持つか。" },
        { sig: "bool HasSrv() / HasRtv() const", desc: "シェーダから読める SRV / レンダーターゲット RTV を持つか。" },
        { sig: "ID3D12Resource* Resource() const", ret: "生リソース", desc: "内部の D3D12 リソース実体(<code>FDx12Buffer::Resource()</code> と対)。" },
        { sig: "D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle() const", ret: "SRV ハンドル", desc: "内部用。<t>ルートディスクリプタテーブル</t>にバインドする SRV。" },
        { sig: "D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle() / DsvCpuHandle() const", desc: "内部用。RTV(先頭スライス) / DSV ハンドル。" },
        { sig: "D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandleForSlice(u32 slice, u32 mip) const", ret: "RTV(範囲外=null)", desc: "内部用。<code>per_slice_rtv</code> で作った面/スライス/mip 別 RTV。<code>index = slice*mip_levels + mip</code>。" },
        { sig: "D3D12_RESOURCE_STATES CurrentState() / void SetCurrentState(...)", desc: "内部用。現在の<t>リソース状態</t>。<code>CommandList</code> が<t>バリア</t>発行後に反映する。" }
      ]
    },
    {
      name: "FDx12Pipeline",
      kind: "クラス", header: "render/Dx12/Dx12Pipeline.h",
      summary: "<t>DX12</t> のグラフィックス<t>パイプライン</t>(<code>IRhiPipeline</code> 派生)。<code>FPipelineDesc</code>(VS+PS・頂点レイアウト・ブレンド・深度/ステンシル・ターゲットフォーマット)から <t>PSO</t> と<t>ルートシグネチャ</t>を組み立てます。",
      when: "<code>CreateRhiPipeline()</code> が DX12 で生成する実体。描画の「設定一式」を 1 つに固めたもの。初期化時に作り、描画時は <code>cmd.SetPipeline</code> で切り替える。",
      sample: "FPipelineDesc pd;\npd.vs = vs.get(); pd.ps = ps.get();\npd.vertex_stride = sizeof(FVertex);\npd.layout[0] = {\"POSITION\",0,EFormat::R32G32B32_Float,0};\npd.layout_count = 1;\npd.cbuffer_slots = 1; pd.texture_slots = 1;\nauto pso = CreateRhiPipeline(*dev, pd).Value();",
      members: [
        { sig: "ID3D12PipelineState* Pso() const", ret: "PSO", desc: "内部用。組み上がった <code>ID3D12PipelineState</code>。" },
        { sig: "ID3D12RootSignature* RootSignature() const", ret: "ルートシグネチャ", desc: "内部用。CBV/SRV/サンプラの配置を定義する<t>ルートシグネチャ</t>。" },
        { sig: "EPrimitiveTopology Topology() const", desc: "プリミティブ種別(三角形リスト等)。" },
        { sig: "u32 CBufferSlots() / TextureSlots() const", desc: "このパイプラインが受け付ける定数バッファ / テクスチャの slot 数。" }
      ]
    },
    {
      name: "FDx12Shader",
      kind: "クラス", header: "render/Dx12/Dx12Shader.h",
      summary: "<t>DX12</t> のシェーダ(<code>IRhiShader</code> 派生)。HLSL ソース文字列を <code>D3DCompile</code> でコンパイルし、<t>バイトコード</t>を保持します。パイプライン作成時に VS/PS として参照されます。",
      when: "<code>CreateRhiShader()</code> が DX12 で生成する実体。HLSL を 1 つの文字列で渡して GPU プログラムを用意する時。",
      sample: "FShaderDesc sd;\nsd.stage = EShaderStage::Vertex;\nsd.hlsl_source = vsCode;     // HLSL 文字列\nsd.entry_point = \"main\";\nauto vs = CreateRhiShader(*dev, sd).Value();",
      members: [
        { sig: "EShaderStage Stage() const", desc: "ステージ(Vertex / Pixel / Compute)。" },
        { sig: "const byte* Bytecode() const", ret: "DXBC バイトコード", desc: "コンパイル済みバイトコード先頭。パイプライン作成時にバックエンドが参照する。" },
        { sig: "usize BytecodeSize() const", ret: "バイト数", desc: "バイトコード長。" }
      ]
    },
    {
      name: "FHrResult",
      kind: "構造体", header: "render/Dx12/Dx12Common.h",
      summary: "<t>DX12</t> 内部用の小さな結果ラッパ。<code>HRESULT</code>(Win32 のエラーコード)を保持し、ACS の <t>Result</t> 系と橋渡しします。",
      when: "DX12 バックエンドの <code>Init</code> 系が返す内部型。利用側コードでは普通 <code>TResult</code> を見るので、これを直接触ることは稀。",
      sample: "FHrResult r = device.Init(cfg);\nif (r.IsErr()) { /* HRESULT は r.hr */ }",
      members: [
        { sig: "HRESULT hr", desc: "生の <code>HRESULT</code>(既定 <code>S_OK</code>)。" },
        { sig: "bool IsOk() const / bool IsErr() const", desc: "成功(<code>SUCCEEDED</code>) / 失敗(<code>FAILED</code>)判定。" }
      ]
    },
    {
      name: "FDiligentDevice",
      kind: "クラス", header: "render/Diligent/DiligentDevice.h",
      summary: "<t>Diligent</t> Engine 経由の<t>RHIデバイス</t>(<code>IRhiDevice</code> 派生)。内部に <code>IRenderDevice</code>・<code>IDeviceContext</code>・<code>EngineFactory</code> を持ち、<b>D3D12 と(ビルド次第で)Vulkan</b>を 1 つの API で扱えます。ヘッダは Diligent 依存を漏らさず前方宣言だけで完結します。",
      when: "Diligent バックエンドでビルドした時に <code>CreateRhiDevice()</code> が生成する実体。<code>FDeviceConfig::backend</code> で D3D12 / Vulkan / Auto を選べる(Vulkan は <code>ACS_DILIGENT_VULKAN=ON</code> 時のみ)。",
      sample: "FDeviceConfig cfg;\ncfg.backend = ERhiBackendKind::Auto;   // 最良を自動選択\nauto dev = CreateRhiDevice(cfg).Value();\n// 実際に選ばれた API を見る:\n// static_cast&lt;FDiligentDevice*&gt;(dev.get())-&gt;ActualBackend();",
      members: [
        { sig: "const char* BackendName() const", ret: "\"Diligent\"", desc: "バックエンド名。" },
        { sig: "const char* AdapterName() const", ret: "GPU 名", desc: "選ばれた<t>アダプタ</t>名。" },
        { sig: "void WaitIdle()", desc: "GPU 完了まで CPU 側で待つ。<t>フェンス</t>を使う。" },
        { sig: "ERhiBackendKind ActualBackend() const", ret: "D3D12 / Vulkan", desc: "Auto を渡した時に<b>実際に選ばれた</b>下位 API。" },
        { sig: "TResult<void> Init(const FDeviceConfig& cfg)", ret: "成否", desc: "<code>EngineFactory</code> からデバイス・コンテキストを作る初期化。<code>CreateRhiDevice</code> から呼ばれる。", when: "通常は直接呼ばない。" },
        { sig: "u64 SignalGraphicsQueue() / void WaitForFenceValue(u64)", desc: "フレーム同期。投入後に<t>フェンス</t>値を Signal し、その値まで待つ。<code>CommandList::Submit</code> が使う。" },
        { sig: "u32 CurrentFrameSlot() / void AdvanceFrameSlot()", ret: "0..1", desc: "<t>フレームインフライト</t>(<code>kFramesInFlight=2</code>)のリングインデックス。" },
        { sig: "Diligent::IRenderDevice* RenderDev() const", ret: "生レンダーデバイス", desc: "内部公開。生の Diligent <code>IRenderDevice</code>。他の <code>Diligent*</code> 実装やネイティブ統合用。" },
        { sig: "Diligent::IDeviceContext* Context() const", ret: "生コンテキスト", desc: "内部公開。即時実行する <code>IDeviceContext</code>(コマンドキューを兼ねる)。" },
        { sig: "Diligent::IEngineFactory* FactoryGeneric() const", desc: "内部公開。API 非依存の <code>EngineFactory</code>。D3D12 専用は <code>Factory()</code>、Vulkan は <code>FactoryVk()</code>。" }
      ]
    },
    {
      name: "FDiligentSwapchain",
      kind: "クラス", header: "render/Diligent/DiligentSwapchain.h",
      summary: "<t>Diligent</t> の<t>スワップチェイン</t>(<code>IRhiSwapchain</code> 派生)。Diligent の <code>ISwapChain</code> を薄くラップし、取得・提示・リサイズを <code>FDx12Swapchain</code> と同じ API で提供します。",
      when: "<code>CreateRhiSwapchain()</code> が Diligent で生成する実体。使い方は DX12 版と同一(取得→描画→提示)。",
      sample: "auto swap = CreateRhiSwapchain(*dev, sc).Value();\nu32 idx = swap-&gt;AcquireNextImage();\n// ...描画...\nswap-&gt;Present();",
      members: [
        { sig: "u32 AcquireNextImage()", ret: "バッファ番号", desc: "次に描けるバックバッファを取得。" },
        { sig: "void Present()", desc: "画面へ反映。" },
        { sig: "bool Resize(u32 width, u32 height)", ret: "成否", desc: "サイズ変更でバッファを作り直す。" },
        { sig: "u32 BufferCount() / Width() / Height() const", desc: "バッファ枚数・解像度。" },
        { sig: "Diligent::ISwapChain* SwapChain() const", ret: "生スワップチェイン", desc: "内部公開。生の <code>ISwapChain</code>。" },
        { sig: "EFormat ColorFormat() const", desc: "内部公開。バックバッファの色<t>フォーマット</t>。パイプラインの <code>rt_format</code> 合わせに使う。" }
      ]
    },
    {
      name: "FDiligentCommandList",
      kind: "クラス", header: "render/Diligent/DiligentCommandList.h",
      summary: "<t>Diligent</t> の<t>コマンドリスト</t>(<code>IRhiCommandList</code> 派生)。Diligent は <code>IDeviceContext</code> 自体が即時 API なので、本クラスはその薄いラッパです。<code>FDx12CommandList</code> と同じインターフェイスで、加えて <b>MRT(複数 RT 同時描画)を実装</b>します。",
      when: "<code>CreateRhiCommandList()</code> が Diligent で生成する実体。描画ループの主役で、DX12 版と同じ <code>Begin</code>→パス→<code>End</code>→<code>Submit</code> で回す。",
      sample: "auto cmd = CreateRhiCommandList(*dev).Value();\ncmd-&gt;Begin();\nu32 i = swap-&gt;AcquireNextImage();\ncmd-&gt;BeginRenderToSwapchain(*swap, i, {0,0,0,1});\ncmd-&gt;SetPipeline(*pso);\ncmd-&gt;SetConstantBuffer(0, *cb);\ncmd-&gt;SetTexture(0, *tex);\ncmd-&gt;DrawIndexed(idxCount);\ncmd-&gt;EndRenderToSwapchain(*swap, i);\ncmd-&gt;End();  cmd-&gt;Submit();",
      members: [
        { sig: "void Begin() / End() / Submit()", desc: "記録の開始・終了・GPU 投入。" },
        { sig: "void BeginRenderToSwapchain(...) / EndRenderToSwapchain(...)", desc: "バックバッファへの描画パス開始/終了。<code>FDx12CommandList</code> と同シグネチャ。" },
        { sig: "void BeginShadowPass(...) / EndShadowPass(...)", desc: "深度のみ<t>シャドウマップ</t>パス。終了時に SHADER_RESOURCE へ遷移。" },
        { sig: "void BeginRenderToTexture(...) / EndRenderToTexture(...)", desc: "オフスクリーン RT への描画。途中で挟んでも main pass の RT(swapchain/depth)を記憶して復帰する。" },
        { sig: "void BeginRenderToTextureLoad(...) / BeginRenderToTextureSlice(...)", desc: "クリアせず追記する load 版 / cubemap 面・スライス・mip 別に描く版。" },
        { sig: "void BeginRenderToTextureMrt(rts, rt_count, clear, depth=null, depth_clear=1)", desc: "複数 color RT を同時 bind する MRT。<b>このバックエンドで実装済</b>(DX12 raw は no-op)。", when: "G-Buffer や SSGI など複数出力パスを書く時。" },
        { sig: "void SetViewport(...) / SetScissor(...) / SetStencilRef(...)", desc: "ビューポート・シザー・<t>ステンシル</t>参照値。" },
        { sig: "void SetPipeline(...) / SetVertexBuffer(...) / SetIndexBuffer(...)", desc: "パイプライン・頂点/インデックスバッファのバインド。" },
        { sig: "void SetConstantBuffer(u32 slot, cb) / SetTexture(u32 slot, tex)", desc: "定数バッファ・テクスチャを slot へ。Diligent は<b>名前ベース</b>で SRB を引くため、内部でパイプラインの HLSL リソース名を使う。" },
        { sig: "void Draw(...) / DrawIndexed(...)", desc: "描画。" },
        { sig: "void* NativeHandle()", ret: "IDeviceContext*", desc: "生の <code>IDeviceContext</code>。外部統合用。" }
      ]
    },
    {
      name: "FDiligentBuffer",
      kind: "クラス", header: "render/Diligent/DiligentBuffer.h",
      summary: "<t>Diligent</t> の GPU バッファ(<code>IRhiBuffer</code> 派生)。Diligent の <code>IBuffer</code> を頂点・インデックス・定数バッファとして確保し、<code>FDx12Buffer</code> と同じ API で更新できます。",
      when: "<code>CreateRhiBuffer()</code> が Diligent で生成する実体。",
      sample: "FBufferDesc d;\nd.size = sizeof(CB); d.usage = EBufferUsage::Uniform;\nd.cpu_writable = true;\nauto cb = CreateRhiBuffer(*dev, d).Value();\ncb-&gt;Update(&amp;data, sizeof(data));",
      members: [
        { sig: "usize Size() const", desc: "バイト数。" },
        { sig: "EBufferUsage Usage() const", desc: "用途。" },
        { sig: "void Update(const void* data, usize size, usize offset=0)", desc: "CPU から書き込む(<code>cpu_writable=true</code> のみ)。" },
        { sig: "Diligent::IBuffer* Native() const", ret: "生バッファ", desc: "内部公開。生の <code>IBuffer</code>。" }
      ]
    },
    {
      name: "FDiligentTexture",
      kind: "クラス", header: "render/Diligent/DiligentTexture.h",
      summary: "<t>Diligent</t> のテクスチャ(<code>IRhiTexture</code> 派生)。<code>ITexture</code> と、その SRV/RTV/DSV <t>ビュー</t>を持ちます。cubemap・テクスチャ配列・per-slice RTV にも対応します。",
      when: "<code>CreateRhiTexture()</code> が Diligent で生成する実体。使い方は <code>FDx12Texture</code> と同じく <code>FTextureDesc</code> のフラグで切り替える。",
      sample: "FTextureDesc d;\nd.width = 1024; d.height = 1024;\nd.format = EFormat::D32_Float;\nd.is_depth_target = true;\nd.shader_visible_depth = true;   // シャドウマップ: SRV も作る\nauto shadow = CreateRhiTexture(*dev, d).Value();",
      members: [
        { sig: "u32 Width() / Height() / MipLevels() / ArraySize() const", desc: "解像度・mip 数・配列枚数。" },
        { sig: "EFormat PixelFormat() const", desc: "ピクセル<t>フォーマット</t>。" },
        { sig: "bool IsCubemap() / IsRenderTarget() / IsDepthTarget() const", desc: "cubemap か / RT か / 深度バッファか。" },
        { sig: "bool HasStencil() / ShaderVisibleDepth() const", desc: "ステンシル成分を持つか / 深度を SRV としても読めるか(シャドウマップ)。" },
        { sig: "Diligent::ITextureView* SrvView() / RtvView() / DsvView() const", ret: "生ビュー", desc: "内部公開。SRV / RTV / DSV の<t>テクスチャビュー</t>。" },
        { sig: "Diligent::ITextureView* RtvSlice(u32 slice, u32 mip) const", ret: "RTV(無ければ null)", desc: "内部公開。<code>per_slice_rtv</code> で作った slice/mip 別 RTV。" },
        { sig: "Diligent::ITexture* Native() const", ret: "生テクスチャ", desc: "内部公開。生の <code>ITexture</code>。" }
      ]
    },
    {
      name: "FDiligentPipeline",
      kind: "クラス", header: "render/Diligent/DiligentPipeline.h",
      summary: "<t>Diligent</t> のグラフィックス<t>パイプライン</t>(<code>IRhiPipeline</code> 派生)。<code>IPipelineState</code> と <t>SRB</t>(シェーダリソースバインディング)を作ります。Diligent は<b>名前で</b>リソースを引くため、<code>FPipelineDesc</code> の HLSL リソース名(<code>cbuffer_names</code>/<code>texture_names</code>)を保持します。",
      when: "<code>CreateRhiPipeline()</code> が Diligent で生成する実体。DX12 版と異なり、定数バッファ・テクスチャの<b>名前</b>を <code>FPipelineDesc</code> に与えておく必要がある(null なら <code>cb0</code>/<code>t0</code> がフォールバック)。",
      sample: "FPipelineDesc pd;\npd.vs = vs.get(); pd.ps = ps.get();\npd.cbuffer_slots = 1; pd.cbuffer_names[0] = \"Frame\";\npd.texture_slots = 1; pd.texture_names[0] = \"albedo\";\nauto pso = CreateRhiPipeline(*dev, pd).Value();",
      members: [
        { sig: "Diligent::IPipelineState* Native() const", ret: "生 PSO", desc: "内部公開。生の <code>IPipelineState</code>。" },
        { sig: "Diligent::IShaderResourceBinding* Srb() const", ret: "SRB", desc: "内部公開。リソースを束ねる<t>SRB</t>。" },
        { sig: "u32 CbufferSlots() / TextureSlots() const", desc: "受け付ける定数バッファ / テクスチャの slot 数。" },
        { sig: "const char* CbufferName(u32 slot) / TextureName(u32 slot) const", ret: "HLSL 名", desc: "内部用。slot に対応する HLSL リソース名(<code>CommandList</code> の名前ベースバインドが使う)。最大 16 slot。" }
      ]
    },
    {
      name: "FDiligentShader",
      kind: "クラス", header: "render/Diligent/DiligentShader.h",
      summary: "<t>Diligent</t> のシェーダ(<code>IRhiShader</code> 派生)。HLSL を Diligent 内部で SPIR-V/MSL/GLSL へ変換するため、<code>Bytecode()</code> は便宜上 <code>nullptr</code> を返し、パイプライン作成時に生 <code>IShader*</code> を直接使います。HLSL ソースを parse して <code>register(bN/tN)</code> → 名前の対応も取得します。",
      when: "<code>CreateRhiShader()</code> が Diligent で生成する実体。HLSL の渡し方は <code>FDx12Shader</code> と同じ。",
      sample: "FShaderDesc sd;\nsd.stage = EShaderStage::Pixel;\nsd.hlsl_source = psCode;\nauto ps = CreateRhiShader(*dev, sd).Value();",
      members: [
        { sig: "EShaderStage Stage() const", desc: "ステージ。" },
        { sig: "const byte* Bytecode() const", ret: "nullptr", desc: "Diligent は内部変換するため常に <code>nullptr</code>。" },
        { sig: "usize BytecodeSize() const", ret: "0", desc: "常に 0。" },
        { sig: "Diligent::IShader* Native() const", ret: "生シェーダ", desc: "内部公開。生の <code>IShader</code>。パイプラインが直接参照する。" },
        { sig: "const char* CbufferNameAt(u32 slot) / TextureNameAt(u32 slot) const", ret: "HLSL 名 or null", desc: "内部用。HLSL ソースから取得した <code>register</code> slot → 名前(最大 16 slot)。" }
      ]
    },
    {
      name: "FDiligentMemoryAdapter",
      kind: "クラス", header: "render/Diligent/DiligentMemoryAdapter.h",
      summary: "<t>Diligent</t> の内部 malloc を ACS の<t>アロケータ</t>(<code>FAllocator</code>)へ橋渡しするアダプタ。<code>EngineCreateInfo::pRawMemAllocator</code> にこれを渡すと、Diligent のメモリ確保が ACS のメモリ管理(<code>ESegment::RenderInternal</code> 等)を経由するようになります。",
      when: "Diligent デバイス初期化時に、エンジン内部のメモリも ACS の予算管理・可視化に乗せたい場合。<code>FDiligentDevice::Init</code> が内部で使う想定。",
      sample: "FAllocator& a = DefaultAllocator();\nvoid* memAlloc = FDiligentMemoryAdapter::Create(&amp;a);\n// EngineCreateInfo::pRawMemAllocator = (Diligent::IMemoryAllocator*)memAlloc;",
      members: [
        { sig: "static void* Create(FAllocator* backing)", ret: "IMemoryAllocator*", desc: "<code>backing</code> を呼び出し先に使うアダプタを生成し、Diligent の <code>IMemoryAllocator*</code> として渡せる <code>void*</code> を返す(プロセス寿命)。" }
      ]
    },
    {
  name: "ToDxgiFormat / ToD3D12State",
  kind: "自由関数", header: "render/Dx12/Dx12Common.h",
  summary: "<t>DX12</t> バックエンド共通の小さな変換ヘルパ群(いずれも <code>inline noexcept</code>)。ACS の論理列挙を Direct3D12 / DXGI のネイティブ列挙へ写します。<code>Dx12Common.h</code> を include する DX12 系 <code>.cpp</code> から使われます。",
  when: "<t>DX12</t> バックエンド内部で <code>EFormat</code> や <code>EResourceState</code> をネイティブ型に直す時。利用側コードは普通 <code>IRhi*</code> 越しなので直接触りません。",
  sample: "DXGI_FORMAT fmt   = ToDxgiFormat(EFormat::R8G8B8A8_UNorm);\nD3D12_RESOURCE_STATES st = ToD3D12State(EResourceState::RenderTarget);",
  members: [
    { sig: "DXGI_FORMAT ToDxgiFormat(EFormat f) noexcept", ret: "DXGI フォーマット", desc: "<code>EFormat</code> を <code>DXGI_FORMAT</code> へ変換。未対応値は <code>DXGI_FORMAT_UNKNOWN</code>。" },
    { sig: "D3D12_RESOURCE_STATES ToD3D12State(EResourceState s) noexcept", ret: "D3D12 リソース状態", desc: "<code>EResourceState</code> を <code>D3D12_RESOURCE_STATES</code> へ変換。未対応値は <code>D3D12_RESOURCE_STATE_COMMON</code>。" }
  ]
},
    {
  name: "ACS_SAFE_RELEASE",
  kind: "マクロ", header: "render/Dx12/Dx12Common.h",
  summary: "COM ポインタを安全に解放するマクロ。非 null なら <code>Release()</code> を呼び、ポインタを <code>nullptr</code> に戻します(二重解放防止)。<t>DX12</t> バックエンドのデストラクタ・リソース破棄で多用されます。",
  when: "<code>ID3D12*</code> / <code>IDXGI*</code> など生 COM ポインタのメンバを破棄する時。",
  sample: "ACS_SAFE_RELEASE(m_Device);   // m_Device が非 null なら Release し、nullptr 化",
  members: [
    { sig: "ACS_SAFE_RELEASE(p)", desc: "<code>if (p) { (p)->Release(); (p)=nullptr; }</code> を <code>do{...}while(0)</code> で包んだ式。<code>p</code> は左辺値である必要がある。" }
  ]
}
  ]
});

Object.assign(ACS_REF.glossary, {
  "RHI": "Render Hardware Interface。GPU API(DX12 / Vulkan 等)の差を吸収する抽象層。ACS では <code>IRhiDevice</code> 等の <code>IRhi*</code> がこれにあたる。",
  "RHIデバイス": "<t>RHI</t> の入口となる GPU 抽象。バッファ・テクスチャ・パイプライン等を作る親。",
  "DX12": "Direct3D 12。Windows の低レベル GPU API。ACS の <code>Dx12*</code> 系がネイティブ実装。",
  "Diligent": "Diligent Engine。1 つの API で D3D12 / Vulkan 等を扱えるクロスバックエンド層。ACS の <code>Diligent*</code> 系が利用する。",
  "コマンドリスト": "GPU に送る描画命令をいったん記録しておくバッファ。<code>Begin</code>→記録→<code>End</code>→<code>Submit</code> で実行する。",
  "スワップチェイン": "画面に出すバックバッファ(複数枚)を管理し、描画先を切り替えて提示する仕組み。",
  "ディスクリプタ": "テクスチャやバッファを GPU から参照するための小さな「住所メモ」。<t>ディスクリプタヒープ</t>に並べて使う。",
  "ディスクリプタヒープ": "<t>ディスクリプタ</t>を並べて確保しておく GPU 上の表。SRV/RTV/DSV など種類別に持つ。",
  "ルートディスクリプタテーブル": "<t>DX12</t> でシェーダにリソース群をまとめてバインドする仕組み。<t>ルートシグネチャ</t>で定義する。",
  "ルートシグネチャ": "<t>DX12</t> のパイプラインで、どの slot にどのリソース(CBV/SRV/サンプラ)が来るかを定義する設計図。",
  "PSO": "Pipeline State Object。シェーダ・頂点レイアウト・ブレンド・深度設定などを 1 つに固めた<t>DX12</t>の状態オブジェクト。",
  "SRB": "Shader Resource Binding。<t>Diligent</t> でシェーダのリソース(定数バッファ・テクスチャ等)を束ねて差し込む単位。",
  "フェンス": "GPU の進捗を表すカウンタ。CPU が「この値まで終わったか」を待つのに使い、フレーム同期を取る。",
  "フレームインフライト": "GPU が前フレームを処理中でも CPU が次フレームを準備できるよう、リソースを複数枚(ここでは 2)用意して使い回すこと。",
  "リングバッファ": "複数スロットを順番に使い回すバッファ。毎フレーム書き換える定数バッファを安全に更新するために<t>フレームインフライト</t>と組み合わせる。",
  "リソース状態": "GPU リソースが今どう使われているか(レンダーターゲット / シェーダ読取 / 提示 等)。切り替えには<t>バリア</t>が要る。",
  "バリア": "<t>リソース状態</t>を切り替える境界。GPU に「ここで用途が変わる」と知らせ、読み書きの整合を取る。",
  "フォーマット": "ピクセルやバッファのビット配置(<code>R8G8B8A8_UNorm</code> 等)。論理フォーマット <code>EFormat</code> で表す。",
  "ビュー": "同じリソースを「どう解釈して読むか」を表す窓。1 つのテクスチャに SRV / RTV / DSV など複数のビューを作れる。",
  "テクスチャビュー": "テクスチャに対する<t>ビュー</t>。シェーダ読取(SRV)・描画先(RTV)・深度(DSV)などの用途別に作る。",
  "アダプタ": "PC に載っている GPU。複数あれば高性能なものを優先選択できる。",
  "シャドウマップ": "光源から見た深度を描いておき、本描画で影かどうか判定するためのテクスチャ。",
  "ステンシル": "ピクセルごとの整数マスク。描画範囲を型抜きしたり、特定領域だけ描く制御に使う。",
  "バイトコード": "HLSL をコンパイルした GPU 向け中間表現(DXBC 等)。パイプライン作成時に GPU へ渡す。",
  "パイプライン": "1 回の描画に使う設定一式(シェーダ + 頂点レイアウト + ブレンド + 深度/ステンシル + 出力先)。"
});
