/* ACS リファレンス — test モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "test",
  order: 62,
  title: "test — テスト基盤(ACS_TEST / EXPECT)",
  blurb: "GoogleTest を使わない<b>軽量な自前テスト基盤</b>。<code>ACS_TEST</code> でテストを書き、<code>EXPECT_*</code> マクロで結果を検証し、<code>RunAll()</code> でまとめて実行します。",
  types: [
    {
      name: "ACS_TEST(Suite, Name)",
      kind: "マクロ", header: "test/Test.h",
      summary: "<b>テストケースを 1 つ宣言する</b>マクロ。直後に <code>{ ... }</code> でテスト本体を書く。プログラム起動時に自動でテスト一覧へ登録され、<code>RunAll()</code> で実行される。<code>Suite</code> はグループ名、<code>Name</code> はその中の個別テスト名。",
      when: "新しいテストを 1 つ書くとき。関数を自分で <code>main</code> から呼ぶ必要はなく、書いただけで実行対象になる。",
      sample: "ACS_TEST(MathVec3, Add) {\n    EXPECT_EQ(FVec3(1,2,3) + FVec3(4,5,6), FVec3(5,7,9));\n}\n\nACS_TEST(MathVec3, Length) {\n    EXPECT_NEAR(Length(FVec3(3,4,0)), 5.0f, 0.001f);\n}"
    },
    {
      name: "RunAll()",
      kind: "関数", header: "test/Test.h",
      summary: "登録済みの<b>全テストケースを順に実行</b>する。失敗が 1 件でもあれば <code>1</code> を、すべて成功なら <code>0</code> を返す。",
      when: "テスト実行ファイルの <code>main</code> の戻り値にそのまま使う。CI はこの終了コードで成否を判定する。",
      sample: "int main() {\n    return ::acs::test::RunAll();\n}",
      members: [
        { sig: "int RunAll() noexcept", ret: "失敗があれば 1、全成功なら 0", desc: "全ケースを実行し、結果サマリを出力してプロセスの終了コードを返す。", when: "<code>main</code> の <code>return</code> に直接書くのが定石。" }
      ]
    },
    {
      name: "TestCase",
      kind: "構造体", header: "test/Test.h",
      summary: "<b>1 つのテストケースの記述子</b>。スイート名・テスト名・ソース位置・テスト本体への<t>関数ポインタ</t>を持ち、<code>next</code> で次のケースへ繋がる<t>連結リスト</t>のノードでもある。<code>ACS_TEST</code> が内部で自動生成するので、通常は手で触らない。",
      when: "通常は意識しなくてよい(<code>ACS_TEST</code> が裏で作る)。テストランナーを自作したり一覧を走査したい上級者向け。",
      sample: "// ACS_TEST が裏で次のような記述子を作って登録している\n::acs::test::TestCase tc {\n    \"MathVec3\", \"Add\",      // suite, name\n    __FILE__, __LINE__,      // file, line\n    &myTestFn, nullptr       // fn, next\n};",
      members: [
        { sig: "const char* suite", desc: "テストの<b>グループ名</b>(<code>ACS_TEST</code> の第1引数)。" },
        { sig: "const char* name", desc: "グループ内での<b>個別テスト名</b>(第2引数)。" },
        { sig: "const char* file", desc: "テストが書かれた<b>ソースファイル名</b>(<code>__FILE__</code>)。" },
        { sig: "u32 line", desc: "テストの<b>行番号</b>(<code>__LINE__</code>)。失敗時の場所表示に使う。" },
        { sig: "TestFn fn", ret: "テスト本体", desc: "実行する<b>テスト関数への<t>関数ポインタ</t></b>。" },
        { sig: "TestCase* next", desc: "<t>連結リスト</t>の<b>次のケース</b>。末尾は <code>nullptr</code>。" }
      ]
    },
    {
      name: "TestFn",
      kind: "型エイリアス", header: "test/Test.h",
      summary: "テスト本体の<b><t>関数ポインタ</t>型</b>。<code>void (*)()</code> の別名で、引数なし・戻り値なしの関数を指す。",
      when: "<code>TestCase::fn</code> の型。テストランナーを自作するときに型として使う程度。",
      sample: "using TestFn = void (*)();  // 引数なし・戻り値なしの関数ポインタ"
    },
    {
      name: "Register()",
      kind: "関数", header: "test/Test.h",
      summary: "テストケースを<b>実行リストへ登録</b>する。<code>ACS_TEST</code> が起動時(<code>main</code> 開始前)に自動で呼ぶため、利用者が直接呼ぶことはまずない。",
      when: "テストランナーを自作する等の特殊用途のみ。普段は <code>ACS_TEST</code> 経由で間接的に使われる。",
      sample: "// ACS_TEST 内部から自動で呼ばれる\n::acs::test::Register(&tc);",
      members: [
        { sig: "void Register(TestCase* tc) noexcept", desc: "渡された記述子をグローバルなテスト一覧の<t>連結リスト</t>に繋ぐ。" }
      ]
    },
    {
      name: "RecordFailure()",
      kind: "関数", header: "test/Test.h",
      summary: "テスト本体内から<b>失敗を 1 件記録</b>する関数。失敗してもテストはそこで止まらず続行する。<code>EXPECT_*</code> マクロが条件不成立のときに内部で呼ぶ。<code>printf</code> 風の書式で詳細メッセージを付けられる。",
      when: "<code>EXPECT_*</code> でカバーできない独自の検証で、自分で失敗を報告したいとき。",
      sample: "if (player.hp &lt; 0) {\n    ::acs::test::RecordFailure(::acs::FSourceLoc::Current(),\n        \"player.hp &gt;= 0\", \"hp was %d\", player.hp);\n}",
      members: [
        { sig: "void RecordFailure(FSourceLoc loc, const char* expr, const char* fmt, ...) noexcept", desc: "失敗を 1 件記録する。<code>loc</code>=発生位置(<t>FSourceLoc</t>)、<code>expr</code>=失敗した式の文字列、<code>fmt</code> 以降=<code>printf</code> 風の補足メッセージ。" }
      ]
    },
    {
      name: "RecordInfo()",
      kind: "関数", header: "test/Test.h",
      summary: "テスト本体内から<b>補助的な情報メッセージを出力</b>する関数。失敗扱いにはならず、ログとして残るだけ。<code>printf</code> 風の書式が使える。",
      when: "テストの途中経過や、失敗時の手がかりになる値をログに残したいとき。",
      sample: "::acs::test::RecordInfo(::acs::FSourceLoc::Current(),\n    \"seed = %u\", rngSeed);",
      members: [
        { sig: "void RecordInfo(FSourceLoc loc, const char* fmt, ...) noexcept", desc: "情報メッセージを出力する(失敗にはしない)。<code>loc</code>=位置、<code>fmt</code> 以降=<code>printf</code> 風メッセージ。" }
      ]
    },
    {
      name: "EXPECT_TRUE / EXPECT_FALSE",
      kind: "マクロ", header: "test/Expect.h",
      summary: "式が<b>真(または偽)であることを検証</b>するアサートマクロ。条件を満たさなければ失敗を記録するが、<b>テストはそのまま続行</b>する(以降のチェックも走る)。",
      when: "<code>bool</code> を返す条件をそのまま確かめたいとき。一番素朴な検証。",
      sample: "EXPECT_TRUE(list.IsEmpty());\nEXPECT_FALSE(ptr == nullptr);",
      members: [
        { sig: "EXPECT_TRUE(expr)", desc: "<code>expr</code> が <code>false</code> なら失敗を記録する。" },
        { sig: "EXPECT_FALSE(expr)", desc: "<code>expr</code> が <code>true</code> なら失敗を記録する。" }
      ]
    },
    {
      name: "EXPECT_EQ / EXPECT_NE",
      kind: "マクロ", header: "test/Expect.h",
      summary: "2 つの値が<b>等しい(または等しくない)ことを検証</b>するアサートマクロ。内部で <code>a == b</code> を 1 度だけ評価して比べる。条件を満たさなくてもテストは続行する。",
      when: "計算結果が期待値と一致するかを確かめたいとき。型は <code>==</code> が定義されていれば何でもよい。",
      sample: "EXPECT_EQ(Add(2, 3), 5);\nEXPECT_NE(FindIndex(arr, x), -1);",
      members: [
        { sig: "EXPECT_EQ(a, b)", desc: "<code>a == b</code> でなければ失敗を記録する。" },
        { sig: "EXPECT_NE(a, b)", desc: "<code>a == b</code> なら(=等しければ)失敗を記録する。" }
      ]
    },
    {
      name: "EXPECT_NEAR",
      kind: "マクロ", header: "test/Expect.h",
      summary: "2 つの値の<b>差が許容誤差 <code>eps</code> 以内であることを検証</b>する、<t>浮動小数</t>用のアサートマクロ。<code>|a - b| &gt; eps</code> なら失敗を記録する。",
      when: "<code>float</code> の計算結果を比べるとき。浮動小数は丸め誤差で完全一致しないため、<code>EXPECT_EQ</code> ではなくこちらを使う。",
      sample: "EXPECT_NEAR(std::sqrt(2.0f), 1.41421f, 0.0001f);\nEXPECT_NEAR(dot, 1.0f, 1e-5f);",
      members: [
        { sig: "EXPECT_NEAR(a, b, eps)", desc: "<code>a</code> と <code>b</code> の差の絶対値が <code>eps</code> を超えたら失敗を記録する。差は <code>f32</code> として計算される。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "関数ポインタ": "関数そのものを指し示す値。あとで <code>fn()</code> のように呼び出せる。テスト本体を一覧に登録する仕組みに使う。",
  "連結リスト": "各要素が「次の要素」への参照を持って数珠つなぎになったデータ構造。テストケースを順に辿るのに使う。",
  "浮動小数": "<code>float</code> / <code>double</code> など小数を表す数値型。丸め誤差があるため <code>==</code> ではなく <t>EXPECT_NEAR</t> で許容誤差つきで比べる。",
  "EXPECT_NEAR": "2 値の差が許容誤差以内かを確かめるテスト用マクロ。浮動小数の比較に使う。"
});
