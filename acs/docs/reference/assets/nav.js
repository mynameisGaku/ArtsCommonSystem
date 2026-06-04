/* ACS リファレンス — ナビ・マニフェスト（手書き）。
   サイドバーとトップのモジュール一覧をここから組む。
   各 item: { id, label, page }。id は data/<id>.js の module.id と対応。
   これは“内容”ではなく目次の設定。本文は data/*.js（手書き）が持つ。 */
window.ACS_NAV = {
  // 上部ナビ（ドキュメント系ページ）
  docs: [
    { key: "home",     label: "ホーム",         page: "index.html" },
    { key: "guide",    label: "はじめに",       page: "guide.html" },
    { key: "trouble",  label: "トラブルシュート", page: "troubleshooting.html" },
    { key: "glossary", label: "用語集",         page: "glossary.html" }
  ],
  // サイドバーのモジュール（グループ順）
  groups: [
    { label: "入門", items: [
      { id: "easy", label: "easy", page: "easy.html" }
    ]},
    { label: "基盤", items: [
      { id: "foundation", label: "foundation", page: "foundation.html" },
      { id: "container",  label: "container",  page: "container.html" },
      { id: "memory",     label: "memory",     page: "memory.html" },
      { id: "math",       label: "math",       page: "math.html" },
      { id: "threading",  label: "threading",  page: "threading.html" }
    ]},
    { label: "ランタイム", items: [
      { id: "ecs",      label: "ecs",      page: "ecs.html" },
      { id: "event",    label: "event",    page: "event.html" },
      { id: "mvvm",     label: "mvvm",     page: "mvvm.html" },
      { id: "platform", label: "platform", page: "platform.html" }
    ]},
    { label: "描画", items: [
      { id: "render_core",    label: "render · core",    page: "render_core.html" },
      { id: "render_backend", label: "render · backend", page: "render_backend.html" },
      { id: "ui",             label: "ui",               page: "ui.html" }
    ]},
    { label: "アセット", items: [
      { id: "asset",     label: "asset",     page: "asset.html" },
      { id: "assetpack", label: "assetpack", page: "assetpack.html" },
      { id: "audio",     label: "audio",     page: "audio.html" },
      { id: "collision", label: "collision", page: "collision.html" }
    ]},
    { label: "ゲーム", items: [
      { id: "gameframework", label: "gameframework", page: "gameframework.html" },
      { id: "editor",        label: "editor / 制作支援", page: "editor.html" }
    ]},
    { label: "連携・運用", items: [
      { id: "network",       label: "network",       page: "network.html" },
      { id: "scripting",     label: "scripting",     page: "scripting.html" },
      { id: "steamworks",    label: "steamworks",    page: "steamworks.html" },
      { id: "openxr",        label: "openxr",        page: "openxr.html" },
      { id: "mlonnx",        label: "mlonnx",        page: "mlonnx.html" },
      { id: "localmatch",    label: "localmatch",    page: "localmatch.html" },
      { id: "crashwin",      label: "crashwin",      page: "crashwin.html" },
      { id: "telemetryfile", label: "telemetry",     page: "telemetryfile.html" },
      { id: "imgui",         label: "imgui",         page: "imgui.html" },
      { id: "test",          label: "test",          page: "test.html" }
    ]}
  ]
};
