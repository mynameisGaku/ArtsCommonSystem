# Lua VM の登録失敗契約

`CLuaVm::RegisterNativeFunction` は、注入した `FAllocator` から登録簿を確保できない場合に
`EErrCategory::Memory` と `script_err::kSub_AllocationFailed` を返します。

`CLuaVm(FAllocator&)` は登録簿の確保元を所有せず、その参照をVMの寿命中保持します。
注入するallocatorは、構築した `CLuaVm` より後まで生存させてください。VMより先にallocatorを
破棄すると、登録簿の解放時に無効な参照を使います。

登録簿の確保後に Lua closure の作成またはグローバル公開が失敗した場合も、Lua stackと
追加した登録簿要素を元へ戻し、同名グローバルの復元を試みます。公開後は同じclosureを
関数名から取得できることも確認し、Lua側が書き込みを受け取らなかった場合は成功にしません。
復元処理自体もLuaのメモリ不足になった場合は、同名グローバルの値を保証しません。
Luaのメモリ不足は `EErrCategory::Memory` / `script_err::kSub_AllocationFailed`、
それ以外のLua公開失敗は `EErrCategory::Generic` / `script_err::kSub_CallFailed` です。

Lua公開処理中のメタ関数から既存native関数が呼ばれ、その関数が同じ `CLuaVm` へ登録を
再入した場合は、`EErrCategory::Generic` / `script_err::kSub_CallFailed` で拒否します。
注入allocatorの確保処理から同じ `CLuaVm` へ再入した場合も同じ分類で拒否します。
この拒否はLua stack、グローバル、native登録簿を変更しません。

Lua側の公開拒否処理が失敗予定のclosureを別の場所へ退避しても、そのclosureには登録ごとの
識別番号が残ります。rollback後に同じ登録簿位置を後続登録が使っても、退避されたclosureは
後続のC++関数を呼びません。Lua大域表への公開確認は `__index` を通さないraw lookupで行い、
メタ関数が未公開のclosureを返しても登録成功にしません。

登録識別番号は最大値を一度だけ有効なclosureへ割り当て、その後は0の枯渇状態へ飽和します。
登録簿の確保に失敗した段階ではclosureが存在しないため番号を消費せず、同じ最大番号で再試行できます。
登録簿へ追加した後はLua公開に失敗しても番号を消費し、退避closureとの再一致を防ぎます。
枯渇後の登録はLua APIと登録簿を変更する前に
`EErrCategory::Generic` / `script_err::kSub_CallFailed` で恒久拒否し、`Shutdown`後に再初期化しても
識別番号を1へ戻しません。

注入allocatorによる登録簿の確保失敗では、Luaグローバル公開処理へ入る前にstackを元の位置へ
戻すため、失敗した関数名はLuaのグローバルへ新しく公開されず、既存の同名グローバルも
変更されません。利用可能な
メモリが戻った後は、同じ `CLuaVm` へ登録を再試行できます。

公開APIの正規型は `CLuaVm` と `FAllocator` です。旧`FLuaVm`は正規型を指す一時的なsource互換aliasです。
旧object file向けのsymbol shimはなく、consumerは全量再buildします。Lua内部状態は `LuaVmImpl.h/.cpp`、値変換は
`LuaVmValueConversion.h` に分離し、公開ヘッダーからLua C APIを隠します。この2内部ヘッダーは
moduleの公開ヘッダー一覧と単一ヘッダー配布物から除外します。

この契約は `ACS.LuaVmAllocationSafety` の5件で、確保拒否、allocatorとLuaメタ関数からの再入、
Lua側の公開拒否、退避closure、最大登録番号の成功と公開失敗、枯渇後の恒久拒否、再登録、
Luaからの呼び出し、`Shutdown` 後の解放までを検証します。専用test executableは
`ACS::Scripting` をlinkせず、同moduleの正規source 3件をtest-only定義付きで再compileします。
これにより境界設定memberを全該当翻訳単位で同じclass定義に保ち、製品libraryと配布headerには
test用memberやsymbolを含めません。
