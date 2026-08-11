// SPDX-License-Identifier: Apache-2.0
namespace Acs.Build;

/// <summary>モジュールの build 区分。Module.cmake の TYPE へ出力する。</summary>
public enum ModuleType { Runtime, Editor, Test }

/// <summary>1 つのコンパイル時 feature (acs_module_feature 相当)。</summary>
/// <param name="DefaultExpr">非 null なら DEFAULT へ出力する CMake 式。</param>
public sealed record ModuleFeature(string Name, string Define, bool Default, string Description = "", string? DefaultExpr = null);

/// <summary>
/// CMake 条件 (<c>if(&lt;Condition&gt;)</c>) で有効になる追加内容のグループ。
/// 条件が有効なとき追加する source/header/dependency/link を一つの group として保持する。
/// </summary>
public sealed class ConditionalGroup
{
    /// <summary>group を有効にする CMake 条件式。</summary>
    public string Condition { get; }

    /// <summary>このサブディレクトリを走査して *.cpp をソースに追加 (ヘッダは追加しない)。</summary>
    public string? SubdirSources { get; set; }

    public List<string> Sources { get; } = new();
    public List<string> Headers { get; } = new();
    public List<string> Deps { get; } = new();
    public List<string> LibsPublic { get; } = new();
    public List<string> LibsPrivate { get; } = new();

    public ConditionalGroup(string condition) => Condition = condition;

    public ConditionalGroup SubdirSrc(string subdir) { SubdirSources = subdir; return this; }
    public ConditionalGroup Src(params string[] s) { Sources.AddRange(s); return this; }
    public ConditionalGroup Hdr(params string[] h) { Headers.AddRange(h); return this; }
    public ConditionalGroup Dep(params string[] d) { Deps.AddRange(d); return this; }
    public ConditionalGroup LinkPublic(params string[] l) { LibsPublic.AddRange(l); return this; }
    public ConditionalGroup LinkPrivate(params string[] l) { LibsPrivate.AddRange(l); return this; }
}

/// <summary>
/// 1 つの engine module の build 定義。
///
/// 各 <c>src/&lt;mod&gt;/&lt;Name&gt;.Build.cs</c> が本クラスを継承し、コンストラクタで
/// 依存モジュール・外部ライブラリ・feature を宣言する。ソース/ヘッダは acsbuild が
/// module directory を再帰走査して収集する。
///
/// クラス名がモジュール名 (= acs_module の NAME)、その小文字が src 配下のディレクトリ名。
/// </summary>
public abstract class AcsModule
{
    /// <summary>モジュール種別 (既定 Runtime)。</summary>
    public ModuleType Type { get; protected set; } = ModuleType.Runtime;

    /// <summary>利用側へ公開する module 依存。Module.cmake の PUBLIC_DEPS へ出力する。</summary>
    public List<string> PublicDeps { get; } = new();

    /// <summary>モジュール内だけで使う依存を Module.cmake の PRIVATE_DEPS へ出力する。</summary>
    public List<string> PrivateDeps { get; } = new();

    /// <summary>利用側へ伝播する外部 library。Module.cmake の LINK_PUBLIC へ出力する。</summary>
    public List<string> PublicLibs { get; } = new();

    /// <summary>module 内部だけで link する外部 library。Module.cmake の LINK_PRIVATE へ出力する。</summary>
    public List<string> PrivateLibs { get; } = new();

    /// <summary>コンパイル時 feature 群 (acs_module_feature)。</summary>
    public List<ModuleFeature> Features { get; } = new();

    /// <summary>自動収集に明示追加するファイル相対パス (条件付きソース等の調整用)。</summary>
    public List<string> ExtraFiles { get; } = new();

    /// <summary>自動収集から除外するファイル相対パス (glob 不一致の調整用)。</summary>
    public List<string> ExcludeFiles { get; } = new();

    /// <summary>CMake 条件付きの追加内容 (バックエンド分岐など)。非空だと「組み立て形式」で出力。</summary>
    public List<ConditionalGroup> Conditionals { get; } = new();

    /// <summary>非 null なら先頭に <c>if(NOT &lt;Guard&gt;) return() endif()</c> を出力 (gated module)。</summary>
    public string? Guard { get; protected set; }

    /// <summary>acs_module() の前に出力する生 CMake (third_party fetch / 外部 target 構築など)。</summary>
    public string? Preamble { get; protected set; }

    /// <summary>モジュール名 (既定はクラス名)。ディレクトリは name の小文字。</summary>
    public virtual string Name => GetType().Name;

    /// <summary>feature を 1 行で宣言する簡易ヘルパ。</summary>
    /// <param name="defaultExpr">非 null なら DEFAULT へ出力する CMake 式。</param>
    protected void Feature(string name, string define, bool def = true, string desc = "", string? defaultExpr = null)
        => Features.Add(new ModuleFeature(name, define, def, desc, defaultExpr));

    /// <summary>条件付きグループを開始する簡易ヘルパ (fluent)。</summary>
    protected ConditionalGroup When(string cmakeCondition)
    {
        var g = new ConditionalGroup(cmakeCondition);
        Conditionals.Add(g);
        return g;
    }
}
