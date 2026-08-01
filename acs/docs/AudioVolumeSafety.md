# 音量入力と失敗時の安全規則

`CAudioEngine::Play`、`SetVolume`、`SetMasterVolume` は共通の規則で音量を正規化する。有限値は0.0から1.0へ収め、NaNと正負の無限大は0.0へ置き換える。壊れた計算結果を音声APIへそのまま渡さないための境界処理である。

`Play` は音声データの複製、backendへの登録、個別音量の設定、再生開始をすべて完了した後だけ発音枠を公開する。途中で失敗した場合はボイスと複製済みデータを破棄し、`kInvalidSound` を返す。使用中の発音枠数と常駐データ量は増やさない。

`SetVolume` と `SetMasterVolume` は、backendが設定を拒否した場合に以前のbackend音量を維持し、警告を記録する。戻り値を持たない公開APIでも失敗を無言で扱わない。

既存の寿命契約も維持する。`Shutdown` は実行中の共有操作を待ち、新しい操作を拒否してから全資源を解放する。発音枠は世代付きの `FSoundHandle` で識別され、停止済みの古い識別値を再利用後の音声へ適用しない。同時発音枠と `kAudioEngineResidentBufferBudgetBytes` の上限を超える再生は状態を変えず失敗する。

正規の公開型は `CAudioEngine` である。旧`FAudioEngine`は正規型を指す一時的なsource互換aliasとしてのみ残す。旧object file向けのsymbol shimは提供しないためconsumerは全量再buildする。

専用テストは実音声機器を使わず、公開されている3つの音量経路、backend失敗時のrollback、警告診断、音声データ解放を検証する。

```powershell
cmake --build <build-directory> --config Debug --target acs_audio_volume_safety_tests
ctest --test-dir <build-directory> -C Debug -R "^ACS.AudioVolumeSafety$" --output-on-failure
```
