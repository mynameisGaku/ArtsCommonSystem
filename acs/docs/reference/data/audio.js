/* ACS リファレンス — audio モジュール。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > は &lt; &gt;。 */
ACS_REF.modules.push({
  id: "audio",
  order: 37,
  title: "audio — オーディオ",
  blurb: "音(BGM や効果音)を鳴らすためのモジュール。<t>AAudioAsset</t>(wav/mp3/flac/ogg)を渡すと再生でき、音量やループ、一時停止をまとめて扱えます。内部では Windows の <t>XAudio2</t> を使います。",
  types: [
    {
      name: "CAudioEngine",
      kind: "クラス", header: "audio/AudioEngine.h",
      summary: "音声再生の中心となるエンジン。<t>AAudioAsset</t> を渡して鳴らし、音量変更・停止・一時停止などを行う。中身は <t>XAudio2</t> 実装を <t>pimpl</t> で隠している。<b>コピー不可</b>。",
      when: "ゲーム中で BGM や効果音を鳴らしたい時。普通はアプリ全体で 1 個だけ作り、<code>Init()</code> してから使い、終了時に <code>Shutdown()</code> する。",
      sample: "CAudioEngine engine;\nengine.Init();                       // XAudio2 初期化\n\nauto asset = registry.Load(L\"sound/bgm.ogg\").Value();\nauto* audio = static_cast&lt;AAudioAsset*&gt;(asset.Get());\n\nFSoundHandle h = engine.Play(*audio, 1.0f, /*loop=*/true);\nengine.SetVolume(h, 0.5f);           // 音量を半分に\nengine.Stop(h);                      // 停止\nengine.Shutdown();",
      members: [
        { sig: "TResult<void> Init()", ret: "成功/失敗", desc: "<t>XAudio2</t> を初期化する。再生前に 1 回だけ呼ぶ。失敗は <t>Result</t> で返る。", when: "エンジン起動直後。", sample: "if (engine.Init().IsErr()) { /* 音なしで続行など */ }" },
        { sig: "void Shutdown()", desc: "全音停止 + 内部リソース解放。<t>デストラクタ</t>でも呼ばれるが明示的に呼んでよい。", when: "アプリ終了時。" },
        { sig: "FSoundHandle Play(const AAudioAsset& asset, f32 volume = 1.0f, bool loop = false)", ret: "再生ハンドル", desc: "音声アセットの再生を開始し、その音を識別する <code>FSoundHandle</code> を返す。有限の <code>volume</code> は 0.0〜1.0 に収め、NaN と正負の無限大は 0.0 にする。backend操作が失敗した場合は音声資源を破棄して無効値を返す。<code>loop=true</code> で無限ループ。", when: "効果音や BGM を鳴らす瞬間。返ったハンドルを保持しておけば後で止められる。", sample: "FSoundHandle se = engine.Play(*hitSound);          // 1 回だけ\nFSoundHandle bgm = engine.Play(*music, 0.8f, true); // ループ再生" },
        { sig: "void Stop(FSoundHandle h)", desc: "指定したハンドルの音を止め、内部スロットを解放する。無効/再生済みハンドルなら何もしない。", when: "ループ BGM を止める、鳴り続けている音を切る時。" },
        { sig: "void SetVolume(FSoundHandle h, f32 volume)", desc: "個別の音の音量を変える。有限値は 0.0〜1.0 に収め、NaN と正負の無限大は 0.0 にする。backendが拒否した場合は以前の音量を維持して警告する。", when: "再生中にフェードしたい、距離で音量を変えたい時。", sample: "engine.SetVolume(bgm, 0.3f); // この音だけ小さく" },
        { sig: "void StopAll()", desc: "再生中の音をすべて停止して解放する。", when: "シーン切り替えやゲームオーバーで全部消したい時。" },
        { sig: "void SetMasterVolume(f32 volume)", desc: "最終出力全体(=すべての音)の音量を設定する。有限値は 0.0〜1.0 に収め、NaN と正負の無限大は 0.0 にする。backendが拒否した場合は以前の音量を維持して警告する。個別音量とは別の<b>マスター</b>側。", when: "設定画面の『全体音量』スライダーなど。", sample: "engine.SetMasterVolume(options.MasterVol);" },
        { sig: "void PauseAll()", desc: "再生中の音をすべて一時停止する。再生位置は保持される。", when: "ポーズメニューを開いた時。", sample: "if (paused) engine.PauseAll();" },
        { sig: "void ResumeAll()", desc: "<code>PauseAll()</code> で止めた音を止めた位置から再開する。", when: "ポーズ解除した時。" },
        { sig: "u32 ActiveCount() const", ret: "再生中スロット数", desc: "今鳴っている音の数を返す。主にデバッグ用。", when: "同時発音数を確認したい時。" },
        { sig: "using FAudioEngine = CAudioEngine", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CAudioEngine</code> を使う。" }
      ]
    },
    {
      name: "FSoundHandle",
      kind: "構造体", header: "audio/SoundHandle.h",
      summary: "再生中の 1 つの音を識別する軽量な<t>ハンドル</t>。<code>CAudioEngine::Play</code> が発行する。<b>世代付き</b>なので、止めた後に同じ内部スロットが別の音に再利用されても、古いハンドルは無効と判定される(取り違え防止)。",
      when: "再生した音をあとから止める/音量変更する時に保持しておく。値で持ち回ってよい小さな構造体。",
      sample: "FSoundHandle h = engine.Play(*se);\nif (h.IsValid()) {\n    engine.SetVolume(h, 0.5f);\n    engine.Stop(h);\n}\nFSoundHandle none = kInvalidSound;  // 無効を表す定数",
      members: [
        { sig: "u32 index", desc: "内部スロット番号(初期値は無効を表す <code>0xFFFFFFFF</code>)。" },
        { sig: "u32 generation", desc: "<b>世代</b>。同じスロットが使い回された時に新旧を区別するためのカウンタ。" },
        { sig: "constexpr bool IsValid() const", ret: "有効か", desc: "<code>index</code> が無効値でなければ true。", sample: "if (h.IsValid()) engine.Stop(h);" },
        { sig: "constexpr bool operator==(FSoundHandle o) const", ret: "等しいか", desc: "<code>index</code> と <code>generation</code> が両方一致する時だけ等しい。" }
      ]
    },
    {
      name: "kInvalidSound",
      kind: "定数", header: "audio/SoundHandle.h",
      summary: "『どの音も指していない』ことを表す無効な <t>FSoundHandle</t> の定数(<code>FSoundHandle{0xFFFFFFFF, 0}</code>)。",
      when: "ハンドルの初期値や『まだ鳴らしていない』状態を表したい時。",
      sample: "FSoundHandle m_Bgm = kInvalidSound;\n// ...\nif (m_Bgm == kInvalidSound) m_Bgm = engine.Play(*music, 1.0f, true);"
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "CAudioEngine": "音声再生の中心となるエンジン。<t>AAudioAsset</t> を鳴らし、音量・停止・一時停止を扱う。内部は <t>XAudio2</t>。",
  "FSoundHandle": "再生中の 1 つの音を識別する世代付きの<t>ハンドル</t>。止めた後の取り違えを防ぐ。",
  "AAudioAsset": "wav/mp3/flac/ogg を統一 PCM として読み込んだ音声アセット(asset モジュール)。<t>CAudioEngine</t> に渡して再生する。",
  "XAudio2": "Windows 標準の低レベル音声 API。ACS の音声再生はこの上に実装されている。",
  "pimpl": "実装の詳細(ここでは XAudio2 のヘッダ)をヘッダから隠し、<code>Impl*</code> だけ前方宣言する設計手法。"
});
