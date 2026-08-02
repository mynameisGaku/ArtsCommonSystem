/* ACS 手書き API リファレンス。 */
ACS_REF.modules.push({
  id: "app",
  order: 39,
  title: "app — アプリケーション",
  blurb: "アプリ全体の実行、asset、timer subsystemをまとめる公開入口。",
  types: [
    {
      name: "CApplication",
      kind: "クラス", header: "app/Application.h",
      summary: "アプリ全体の実行とapplication subsystemの所有をまとめる。",
      when: "起動、frame更新、終了、および共有subsystem取得を行う時。",
      members: [
        { sig: "using FApplication = CApplication", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CApplication</code> を使う。" }
      ]
    },
    {
      name: "AAssetSubsystem",
      kind: "クラス", header: "app/AssetSubsystem.h",
      summary: "applicationが所有し、asset registryへの共通アクセスを提供するsubsystem。",
      when: "application寿命でasset機能を取得する時。",
      members: [
        { sig: "using FAssetSubsystem = AAssetSubsystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>AAssetSubsystem</code> を使う。" }
      ]
    },
    {
      name: "ATimerSubsystem",
      kind: "クラス", header: "app/TimerSubsystem.h",
      summary: "applicationが所有し、timer更新と取得を提供するsubsystem。",
      when: "application寿命でtimer機能を共有する時。",
      members: [
        { sig: "using FTimerSubsystem = ATimerSubsystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ATimerSubsystem</code> を使う。" }
      ]
    }
  ]
});
