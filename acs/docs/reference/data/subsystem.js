/* ACS 手書き API リファレンス。 */
ACS_REF.modules.push({
  id: "subsystem",
  order: 19,
  title: "subsystem — 所有単位の機能",
  blurb: "ownerと寿命を持つ共有機能を登録、取得、更新する基盤。",
  types: [
    {
      name: "ASubsystem",
      kind: "クラス", header: "subsystem/Subsystem.h",
      summary: "ownerと寿命を持つsubsystemの基底object。",
      when: "共有機能をapplication、game、sceneへ組み込む時。",
      members: [
        { sig: "FManagementAdapter ManagementAccess()", desc: "collectionやtestがowner配線だけを明示的に行う短寿命adapterを返す。内部処理はprivate。" },
        { sig: "using FSubsystem = ASubsystem", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>ASubsystem</code> を使う。" }
      ]
    },
    {
      name: "ESubsystemScope",
      kind: "列挙", header: "subsystem/SubsystemScope.h",
      summary: "subsystemを所有する寿命の範囲。",
      when: "factoryがapplication、game session、sceneのどこへ属するかを指定する時。",
      members: [
        { sig: "Engine", desc: "application全体の寿命。" },
        { sig: "GameInstance", desc: "一回のgame sessionの寿命。" },
        { sig: "World", desc: "一つのsceneまたはworldの寿命。" }
      ]
    },
    {
      name: "ESubsystemOwnerKind",
      kind: "列挙", header: "subsystem/SubsystemOwner.h",
      summary: "subsystemへ渡すownerの責務種別。",
      when: "非所有pointerをapplication、game、sceneの型へ安全に解釈する時。",
      members: [
        { sig: "Unknown", desc: "旧APIまたは責務を保証できないowner。" },
        { sig: "Application", desc: "application寿命を所有する <code>CApplication</code>。" },
        { sig: "Game", desc: "game session寿命を所有する <code>CGame</code>。" },
        { sig: "Scene", desc: "world寿命を所有する <code>AScene</code>。" }
      ]
    },
    {
      name: "FSubsystemOwner",
      kind: "構造体", header: "subsystem/SubsystemOwner.h",
      summary: "subsystemへ渡す非所有owner pointerと責務種別。",
      when: "共通のsubsystem APIからowner固有機能を取得する時。",
      members: [
        { sig: "void* pointer", desc: "subsystemが所有しないownerの生pointer。" },
        { sig: "ESubsystemOwnerKind kind", desc: "pointerが満たすowner責務。" }
      ]
    },
    {
      name: "ESubsystemTickPhase",
      kind: "列挙", header: "subsystem/SubsystemTickPhase.h",
      summary: "subsystemを呼び出すframe更新段階。",
      when: "factoryごとに自動更新の有無と順序を指定する時。",
      members: [
        { sig: "None", desc: "自動更新しない。" },
        { sig: "PreUpdate", desc: "利用側の通常更新より前に呼ぶ。" },
        { sig: "PostUpdate", desc: "利用側の通常更新より後に呼ぶ。" }
      ]
    },
    {
      name: "FSubsystemFrameContext",
      kind: "構造体", header: "subsystem/SubsystemFrameContext.h",
      summary: "一回のsubsystem更新へ渡す時間と更新段階。",
      when: "ownerがsubsystem collectionをframe更新する時。",
      members: [
        { sig: "f32 scaled_delta_seconds", desc: "時間倍率を反映した経過秒。" },
        { sig: "f32 unscaled_delta_seconds", desc: "時間倍率を反映しない経過秒。" },
        { sig: "u64 frame_number", desc: "呼び出し元が管理するframe番号。" },
        { sig: "ESubsystemTickPhase phase", desc: "今回呼び出す更新段階。" }
      ]
    },
    {
      name: "FSubsystemCreateFn",
      kind: "型エイリアス", header: "subsystem/SubsystemFactory.h",
      summary: "subsystemを一体生成する非捕捉関数の型。",
      when: "registryへfactory関数を登録する時。",
      members: [
        { sig: "using FSubsystemCreateFn = TUniquePtr&lt;ASubsystem&gt; (*)()", desc: "生成失敗時は空pointerを返す。" }
      ]
    },
    {
      name: "FSubsystemFactory",
      kind: "構造体", header: "subsystem/SubsystemFactory.h",
      summary: "subsystem型の生成条件と決定順序をまとめる登録値。",
      when: "subsystem registryへ生成関数、scope、更新段階を登録する時。",
      members: [
        { sig: "const void* kind", desc: "同一link image内で型を識別するID。" },
        { sig: "ESubsystemScope scope", desc: "生成先ownerの寿命範囲。" },
        { sig: "const char* name", desc: "決定順序と診断に使う非所有のNUL終端名。" },
        { sig: "FSubsystemCreateFn create", desc: "一体を生成し、失敗時は空を返す関数。" },
        { sig: "ESubsystemTickPhase phase", desc: "自動更新する段階。" },
        { sig: "i32 order", desc: "小さい値から初期化、更新する決定順序。" },
        { sig: "bool IsValidSubsystemScope(ESubsystemScope)", desc: "scopeが公開列挙値かを検査する。" },
        { sig: "bool IsValidSubsystemTickPhase(ESubsystemTickPhase)", desc: "更新段階が公開列挙値かを検査する。" },
        { sig: "bool IsValidSubsystemFactory(const FSubsystemFactory&amp;)", desc: "登録に必要な全項目が有効かを検査する。" }
      ]
    },
    {
      name: "CSubsystemCollection",
      kind: "クラス", header: "subsystem/SubsystemCollection.h",
      summary: "一つのownerに属するsubsystemの生成、取得、終了を管理する。",
      when: "owner lifecycleからsubsystemをまとめて扱う時。",
      members: [
        { sig: "using FSubsystemCollection = CSubsystemCollection", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSubsystemCollection</code> を使う。" }
      ]
    },
    {
      name: "CSubsystemAutoRegister",
      kind: "構造体", header: "subsystem/SubsystemRegistry.h",
      summary: "静的初期化時にsubsystem factoryをregistryへ登録する補助。",
      when: "subsystem登録macroの生成コードから使う時。",
      members: [
        { sig: "using FSubsystemAutoRegister = CSubsystemAutoRegister", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSubsystemAutoRegister</code> を使う。" }
      ]
    },
    {
      name: "CSubsystemRegistry",
      kind: "クラス", header: "subsystem/SubsystemRegistry.h",
      summary: "subsystem factoryとscope情報を登録、検索する。",
      when: "登録済みsubsystemをcollectionから生成する時。",
      members: [
        { sig: "using FSubsystemRegistry = CSubsystemRegistry", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSubsystemRegistry</code> を使う。" }
      ]
    }
  ]
});
