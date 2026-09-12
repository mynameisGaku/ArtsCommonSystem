# Raw DirectX 12のシェーダー作成と配布

## 対応範囲

`CSky::Init`と`CSky::CompileShadersCpu`は、Raw DirectX 12構成でShader Model 6.0を使います。
空の物理式の加算残差が従来のFXC最適化経路で失われる反例を確認したためです。
式・物理係数・許容誤差を緩めて回避するのではなく、DXCで同じ式を作成・検証します。

`FShaderDesc.target`を未指定にした既存シェーダーは、従来どおり段階に応じた5.1を使います。
Raw DirectX 12では`vs_6_0`、`ps_6_0`、`cs_6_0`等の明示指定をDXCへ渡します。
段階が一致しない指定、不正な形式、作成・検証失敗はエラーになります。
事前作成した命令も含め、描画設定の作成前にGPUの対応版を確認します。
DXCが形式を作成できても、そのGPU・ドライバー・実行環境で実行可能とは限りません。

Diligent／Vulkan経路は今回の対応に含めません。現在のDiligent実装は独自の形式選択を行うため、
Raw側の合格を他の描画経路の合格へ読み替えないでください。

## 通常C++とEditorでの配置

固定版は公式安定版`v1.9.2602.24`です。CMakeが対象CPUに合った取得物をSHA-256で検証し、
`dxcompiler.dll`と`dxil.dll`を同じ版の組として配置します。SDKやPATHからは補完しません。

手書きの実行物やDLLでは、既存のACSビルド内で対象を作成した後に次を呼びます。

```cmake
add_executable(MyGame Game.cpp)
target_link_libraries(MyGame PRIVATE ACS::GameFramework)
acs_dxc_runtime(MyGame)
```

`acs_dxc_runtime`は毎回必要なファイルを確認するため、再リンクが不要でも失われた配置を復元します。
Editorが生成する通常C++の実行物とリフレクションDLLには、この呼出しをそれぞれ追加しています。
エンジンのEditor ABI DLLにも同様に配置します。既存の手書きCMakeは自動では変更されません。

読み込み基点は、シェーダー作成コードがあるexeまたはDLLの隣です。
例えばEditor ABIを別フォルダーへ移すなら、両DLLと表記ファイルもその隣へ配置してください。
システムフォルダーへコピーする必要はありません。

## 配布と失敗時の契約

`acs_package_game`を使う配布は、両DLLと`Licenses/ThirdParty/DXC-v1.9.2602.24/`以下の
7つの表記ファイルを必須収録します。独自配布では同じ一式を保持してください。
取得物の`ReleaseNotes.md`にある適用対応表、各ライセンス、第三者表記も残します。
ファイル同梱の成功は、ゲーム全体の商用出荷条件を保証するものではありません。

どちらかのDLLが欠けた場合やGPUが要求版に対応しない場合、作成を失敗させます。
誤判定を再発させる旧形式へ黙って切り替えません。
作成済みの命令は呼出し元が所有する独立した領域に複製します。
COMオブジェクトは呼出し内で解放し、両DLL本体だけは公式の推奨に沿ってプロセス終了まで保持します。

## 検証

小さい反復試験は`ACS.DxcShaderCompiler`、`ACS.DxcRuntimeIsolation`、`ACS.Dx12ShaderModel`、
`ACS.SkyGroundGpu`です。隔離試験は正常配置と3種類の欠落を別プロセスで検査します。
作業場所に正常なDLLがあっても、実行物の隣の欠落を隠せないことを確認します。

Editorの実際の生成処理は、明示した一時保存先を使って次のように検査できます。
`ArtifactRoot`は自分の検証用フォルダーの絶対パス、`RunName`はまだ存在しない短い名前です。

```powershell
& acs/tests/dxc_generated_project_test.ps1 -ArtifactRoot '<検証用フォルダーの絶対パス>' -Configuration Debug -RunName dbg1
```

同じ検査をReleaseでも実行します。生成時にユーザーソースを上書きしないこと、再生成が不要な書換えを
起こさないこと、実行物とリフレクションDLLの個別ビルドで配置されることを確認します。
この小さい生成試験は実画面の描画・速度・商用品質の試験ではありません。
計算式の反例と検証の範囲は[数値監査記録](AtmosphereViewIntegralResearch.md)に分けて記録します。

## 一次資料

- [DXCの固定版と配布物](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2602.24)
- [DXC DLLの利用・寿命](https://github.com/microsoft/DirectXShaderCompiler/wiki/Using-dxc.exe-and-dxcompiler.dll)
- [GPUの対応Shader Model照会](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_shader_model)
