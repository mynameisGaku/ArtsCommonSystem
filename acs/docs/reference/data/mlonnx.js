/* ACS リファレンス — mlonnx モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "mlonnx", order: 55, title: "mlonnx — 機械学習(ONNX)推論",
  blurb: "<b>学習済み AI モデル</b>を <t>ONNX</t> 形式で読み込み、ゲーム内で<t>推論</t>(入力 → 出力の計算)を実行するモジュール。NPC の行動判断・画像認識・音声処理などに使えます。ゲーム本体は<t>backend</t>(具体的な ML 実行エンジン)を意識せず、<code>acs::game</code> の共通窓口越しに呼べるのが特徴です。",
  types: [
    {
      name: "FOnnxMlRuntime",
      kind: "クラス", header: "mlonnx/OnnxMlRuntime.h",
      summary: "<t>ONNX Runtime</t>(Microsoft 製の ML 実行エンジン)を裏で使う、本物の<t>推論</t>ランタイム。<code>acs::game::IMlRuntime</code> という抽象<t>インターフェース</t>を実装しているので、ゲーム側は backend 非依存のまま実 ONNX 推論を回せます。",
      when: "学習済み <code>.onnx</code> モデルファイルを読み込み、入力データ(<t>f32</t> 配列)を渡して出力を得たい時。例: 敵 AI の判断、手書き数字認識、簡単な推薦など。",
      sample: "acs::mlonnx::FOnnxMlRuntime rt;\nif (rt.Init().IsOk()) {\n    auto h = rt.LoadModel(\"models/classifier.onnx\");\n    acs::f32 in[4]  = { 1.0f, 2.0f, 3.0f, 4.0f };\n    acs::f32 out[2] = {};\n    rt.RunInference(h.Value(), in, 4, out, 2);  // 推論 1 回\n    rt.UnloadModel(h.Value());\n}\nrt.Shutdown();",
      members: [
        { sig: "TResult<void> Init() noexcept", ret: "成功/失敗", desc: "backend を初期化する(<t>ONNX Runtime</t> の <t>DLL</t> 読み込み・<t>セッション</t>準備など)。<t>Result</t> で結果が返るので必ず成否を確認する。", when: "モデルを読み込む前に最初に 1 度だけ呼ぶ。失敗したら ML 経路を使わずフォールバックする。" },
        { sig: "void Shutdown() noexcept", desc: "backend を破棄する。読み込み済みのモデルもまとめて解放してよい。", when: "アプリ終了時やランタイムを使い終えた時に 1 度だけ呼ぶ。" },
        { sig: "TResult<MlModelHandle> LoadModel(const char* ModelPath) noexcept", ret: "モデルハンドル", desc: "<code>.onnx</code> ファイルを読み込み、不透明な <t>MlModelHandle</t> を返す。<code>ModelPath</code> の寿命は呼び出し側が保証する(文字列リテラル等)。<code>nullptr</code> は失敗。", when: "推論したいモデルごとに 1 回。返ったハンドルを <code>RunInference</code> に渡す。", sample: "auto h = rt.LoadModel(\"models/npc.onnx\");\nif (h.IsOk()) { /* h.Value() を使う */ }" },
        { sig: "TResult<void> UnloadModel(MlModelHandle h) noexcept", ret: "成功/失敗", desc: "モデルハンドルを解放する。無効ハンドルを渡しても安全(<t>no-op</t> 相当で Ok を返してよい)。", when: "そのモデルをもう使わない時。解放後のハンドルは再利用しない。" },
        { sig: "TResult<void> RunInference(MlModelHandle h, const f32* Inputs, u32 InCount, f32* Outputs, u32 OutCount) noexcept", ret: "成功/失敗", desc: "推論を 1 回実行する。<code>Inputs</code>/<code>Outputs</code> は呼び出し側が確保する <t>f32</t> 配列で、<code>InCount</code>/<code>OutCount</code> はモデル定義と一致させる(不一致は失敗)。<b>この呼び出しは完了までブロックする</b>。", when: "毎フレーム or イベントごとに、入力を出力へ変換したい時。", sample: "acs::f32 in[8], out[3] = {};\n// in[] に特徴量を詰める...\nrt.RunInference(h, in, 8, out, 3);\n// out[] に結果が入る" }
      ]
    },
    {
      name: "GetDefaultOnnxMlRuntime()",
      kind: "関数", header: "mlonnx/OnnxMlRuntime.h",
      summary: "プロセス全体で 1 個だけ共有される、既定の <code>FOnnxMlRuntime</code> <t>シングルトン</t>への参照を返す。戻り値は基底の <code>acs::game::IMlRuntime&amp;</code> 型。",
      when: "自分でランタイムを new せず、プロセス共有の 1 個を使い回したい時。<code>InstallOnnxAsDefault</code> が裏でこれを <t>provider</t> として登録する。",
      sample: "acs::game::IMlRuntime& rt = acs::mlonnx::GetDefaultOnnxMlRuntime();\nrt.Init();\nauto h = rt.LoadModel(\"models/m.onnx\");",
      members: [
        { sig: "acs::game::IMlRuntime& GetDefaultOnnxMlRuntime() noexcept", ret: "共有ランタイム", desc: "プロセス共有の既定 <code>FOnnxMlRuntime</code> を <code>IMlRuntime&amp;</code> として返す。" }
      ]
    },
    {
      name: "InstallOnnxAsDefault()",
      kind: "関数", header: "mlonnx/OnnxMlRuntime.h",
      summary: "gameframework 側の <t>provider</t> 窓口に <code>GetDefaultOnnxMlRuntime</code> を登録する結線ヘルパ。以降 <code>acs::game::GetDefaultMlRuntime()</code> が<b>本物の ONNX backend</b> を返すようになる。",
      when: "アプリ起動時に <b>一度だけ</b>呼ぶ。これでゲーム本体は backend 非依存のまま <code>GetDefaultMlRuntime()</code> だけで実 runtime を取得でき、<code>#if WITH_ACS_ML_ONNX</code> はこの呼び出し 1 箇所に集約できる。",
      sample: "// アプリ起動時に一度だけ:\n#if WITH_ACS_ML_ONNX\n    acs::mlonnx::InstallOnnxAsDefault();   // provider を登録\n#endif\n// 以降どこでも本物の ONNX runtime が得られる:\nacs::game::IMlRuntime& rt = acs::game::GetDefaultMlRuntime();",
      members: [
        { sig: "void InstallOnnxAsDefault() noexcept", desc: "既定 MlRuntime provider として ONNX backend を登録する。未登録の状態では <code>GetDefaultMlRuntime()</code> は stub を返す。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "ONNX": "学習済み<t>機械学習</t>モデルを保存する共通ファイル形式(拡張子 <code>.onnx</code>)。フレームワークをまたいでモデルをやり取りできる。",
  "ONNX Runtime": "Microsoft 製の、<t>ONNX</t> モデルを実際に実行する<t>推論</t>エンジン。<code>mlonnx</code> が裏で使う <t>backend</t>。",
  "推論": "学習済みモデルに入力を与えて出力(予測・分類など)を計算すること。学習(training)ではなく実行(inference)の側。",
  "機械学習": "データから規則を自動で学習させ、未知の入力に対して予測や判断をさせる技術。略して ML。",
  "MlModelHandle": "読み込んだ ML モデルを指す不透明な識別子(中身は <t>u64</t>)。backend が変わってもゲーム側コードを書き換えずに済むための窓口。0 は無効を表す。",
  "backend": "同じ<t>インターフェース</t>の裏で実際の処理を行う具体実装。ここでは ONNX Runtime のこと。差し替え可能にすることで上位コードを backend 非依存にできる。",
  "provider": "「既定の実装を返す関数」を後から登録する仕組み。gameframework が backend モジュールに直接依存せずに済むよう、実 backend 側がこれを登録する。",
  "セッション": "ML ランタイムが推論を実行するための作業単位(モデル・実行環境を束ねたもの)。",
  "no-op": "何もしない操作。無効な入力に対して安全に「何もせず成功」を返す挙動を指す。"
});
