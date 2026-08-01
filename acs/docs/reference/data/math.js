/* ACS リファレンス — math モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > & は &lt; &gt; &amp;。 */
ACS_REF.modules.push({
  id: "math",
  order: 12,
  title: "math — ベクトル / 行列 / クォータニオン / 数学",
  blurb: "ゲームの座標計算に必要な<t>ベクトル</t>・<t>行列</t>・<t>クォータニオン</t>・カメラ・衝突判定をまとめたモジュール。内部は <t>DirectXMath</t> に委譲し <t>SIMD</t> で高速。3D/2D 両方の当たり判定プリミティブも含む。",
  types: [
    {
      name: "EBatchTransformPolicy",
      kind: "列挙", header: "math/BatchTransformPolicy.h",
      summary: "batch 変換を点、方向、法線のどの規則で行うかをコンパイル時に選ぶ方針。",
      when: "runtime 分岐なしで同じ行列を大量のベクトルへ適用する時。"
    },
    {
      name: "FVec2",
      kind: "構造体", header: "math/Vec.h",
      summary: "2 次元<t>ベクトル</t>（x, y）。8 バイト。2D の位置・速度・方向を表す基本型。小さいので<t>SIMD</t>は使わず素直に計算する。",
      when: "2D ゲームの座標、UI の位置、スプライトの移動量など、平面上の量を扱う時。",
      sample: "FVec2 pos{100, 50};\nFVec2 vel{0, -9.8f};\npos += vel * dt;            // 位置を更新\nf32 dist = Length(pos);     // 原点からの距離\nFVec2 dir = Normalize(vel); // 単位ベクトル化",
      members: [
        { sig: "FVec2(f32 x, f32 y)", desc: "x, y を指定して作る。<code>FVec2(v)</code> は両成分 v。" },
        { sig: "static FVec2 Zero() / One()", ret: "定数ベクトル", desc: "<code>{0,0}</code> / <code>{1,1}</code> を返す。" },
        { sig: "operator+ - * (単項 -) += -= *= /=", desc: "ベクトル同士の加減・スカラ倍。<b>除算は複合 <code>/=</code> のみ</b>(二項 <code>operator/</code> は無い)。<code>pos += vel * dt;</code> のように書ける。" },
        { sig: "operator== !=", desc: "成分が完全一致するか比較する。" }
      ]
    },
    {
      name: "FVec3",
      kind: "構造体", header: "math/Vec.h",
      summary: "3 次元<t>ベクトル</t>（x, y, z）。16 バイト整列で<t>SIMD</t>演算に最適。3D の位置・方向・色などに使う最重要型。",
      when: "3D 空間の座標・速度・法線・カメラ方向など、立体的な量を扱うほぼ全ての場面。",
      sample: "FVec3 a{1,0,0}, b{0,1,0};\nFVec3 c = Cross(a, b);       // 外積 → {0,0,1}\nf32  d = Dot(a, b);          // 内積 → 0\nFVec3 mid = Lerp(a, b, 0.5f); // 中間点\nFVec3 up = FVec3::Up();      // {0,1,0}",
      members: [
        { sig: "FVec3(f32 x, f32 y, f32 z)", desc: "x, y, z を指定して作る。" },
        { sig: "static FVec3 Zero() / One() / UnitX() / UnitY() / UnitZ()", ret: "定数ベクトル", desc: "ゼロ・全 1・各軸の単位ベクトルを返す。" },
        { sig: "static FVec3 Up() / Forward() / Right()", ret: "向きベクトル", desc: "上 <code>{0,1,0}</code> / 前 <code>{0,0,1}</code> / 右 <code>{1,0,0}</code>。カメラやキャラの向き計算に便利。" },
        { sig: "operator+ - * += -= *= (単項 -)", desc: "ベクトル加減・スカラ倍と単項マイナス。内部で<t>SIMD</t>計算される。<b>除算演算子は無い</b>(<code>operator/</code> も <code>/=</code> も未定義。<t>FVec2</t> だけが <code>/=</code> を持つ)。スカラ除算は <code>v * (1/s)</code> で書く。" },
        { sig: "operator== !=", desc: "x,y,z 成分が完全一致するか比較する。" }
      ]
    },
    {
      name: "FVec4",
      kind: "構造体", header: "math/Vec.h",
      summary: "4 次元<t>ベクトル</t>（x, y, z, w）。16 バイト整列。<t>同次座標</t>や RGBA 色、行列との掛け算に使う。",
      when: "色（RGBA）や、<t>行列</t>で点・方向を変換する時の中間表現（w で点か方向かを区別）。",
      sample: "FVec4 color{1, 0.5f, 0.2f, 1};   // RGBA\nFVec4 p{1, 2, 3, 1};             // 点 (w=1)\nFVec4 r = Transform(p, mat);     // 行列変換\nf32 d = Dot(color, color);",
      members: [
        { sig: "FVec4(f32 x, f32 y, f32 z, f32 w)", desc: "4 成分を指定。<code>FVec4(vec3, w)</code> で <t>FVec3</t> + w からも作れる。" },
        { sig: "static FVec4 Zero() / One()", ret: "定数ベクトル", desc: "ゼロ / 全 1 を返す。" },
        { sig: "operator+ - *", desc: "ベクトル加減・スカラ倍(二項 <code>operator*(FVec4, f32)</code>)。<b>単項 <code>-</code>・複合代入・除算は無い</b>(<t>FVec3</t> と異なり <code>+=</code> 等も未定義)。" },
        { sig: "operator== !=", desc: "x,y,z,w の 4 成分が完全一致するか比較する。" }
      ]
    },
    {
      name: "FVec ベクトル関数",
      kind: "自由関数群", header: "math/Vec.h",
      summary: "<t>FVec2</t>/<t>FVec3</t>/<t>FVec4</t> に共通する演算関数。<t>内積</t>・<t>外積</t>・長さ・正規化・線形補間など。型ごとにオーバーロードされている。",
      when: "方向の角度を測る（内積）、面の法線を作る（外積）、移動量を単位化する（正規化）などベクトル計算の定番処理。",
      sample: "FVec3 v{3, 4, 0};\nf32 len  = Length(v);     // 5\nf32 len2 = LengthSq(v);   // 25 (sqrt 不要で高速)\nFVec3 n  = Normalize(v);  // 長さ 1 に\nf32 dot  = Dot(a, b);     // 内積",
      members: [
        { sig: "f32 Dot(a, b)", ret: "<t>内積</t>", desc: "2 ベクトルの内積。同じ向きほど大きく、直角で 0。角度判定に使う。" },
        { sig: "FVec3 Cross(FVec3 a, FVec3 b)", ret: "<t>外積</t>", desc: "両方に直交するベクトル。面の法線や回転軸を作る。<t>FVec3</t> 専用。" },
        { sig: "f32 Length(v) / f32 LengthSq(v)", ret: "長さ / 長さの 2 乗", desc: "ベクトルの長さ。比較だけなら <code>LengthSq</code>（平方根を省けて速い）。" },
        { sig: "Vec Normalize(v)", ret: "単位ベクトル", desc: "長さを 1 にした方向だけのベクトル。<t>FVec2</t> は長さ 0 なら Zero を返す。" },
        { sig: "FVec3 Lerp(FVec3 a, FVec3 b, f32 t)", ret: "補間値", desc: "a と b を t（0〜1）で<t>線形補間</t>。アニメーションや滑らかな移動に。" }
      ]
    },
    {
      name: "FMat4",
      kind: "構造体", header: "math/Mat.h",
      summary: "4x4 <t>行列</t>。平行移動・回転・拡大縮小、ビュー変換、投影変換を 1 つにまとめて表せる。<t>行優先</t>レイアウトで <t>DirectXMath</t> に委譲。",
      when: "オブジェクトの<t>ワールド行列</t>、カメラの<t>ビュー行列</t>、<t>投影行列</t>を作って GPU に送る時。3D 描画の心臓部。",
      sample: "FMat4 t = FMat4::Translation({0,1,0});\nFMat4 r = FMat4::RotationY(kHalfPi);\nFMat4 world = r * t;           // まず回転、次に移動\nFMat4 proj = FMat4::PerspectiveFovLH(kHalfPi, 16.0f/9.0f, 0.1f, 1000.0f);\nFMat4 forGpu = Transpose(world); // HLSL 列優先へ送る前に転置",
      members: [
        { sig: "static FMat4 Identity()", ret: "単位行列", desc: "何も変換しない<t>単位行列</t>。デフォルトコンストラクタも単位行列。" },
        { sig: "static FMat4 Translation(FVec3 t)", ret: "平行移動行列", desc: "t だけ位置をずらす行列。" },
        { sig: "static FMat4 Scale(FVec3 s)", ret: "拡大縮小行列", desc: "各軸を s 倍する行列。" },
        { sig: "static FMat4 RotationX/Y/Z(f32 rad)", ret: "回転行列", desc: "指定軸まわりに rad ラジアン回す行列。" },
        { sig: "static FMat4 PerspectiveFovLH/RH(fov_y, aspect, near, far)", ret: "<t>投影行列</t>", desc: "<t>透視投影</t>（遠近感あり）。LH=左手系、RH=右手系。<code>aspect</code> は画面の横/縦比。" },
        { sig: "static FMat4 OrthoLH(width, height, near, far)", ret: "<t>投影行列</t>", desc: "<t>正射影</t>（遠近感なし）。2D や UI、見下ろし視点に。" },
        { sig: "static FMat4 LookAtLH/RH(FVec3 eye, FVec3 at, FVec3 up)", ret: "<t>ビュー行列</t>", desc: "eye（視点）から at（注視点）を見るカメラ行列。up は上方向。" }
      ]
    },
    {
      name: "FMat4 行列演算",
      kind: "自由関数群", header: "math/Mat.h",
      summary: "<t>行列</t>の掛け算・<t>転置</t>・<t>逆行列</t>と、行列でベクトルを変換する関数群。",
      when: "複数の変換を合成（掛け算）、シェーダ用に転置、カメラ逆変換（逆行列）、点や方向の座標変換をする時。",
      sample: "FMat4 mvp = world * view * proj; // 変換を合成\nFVec3 p = TransformPoint({1,0,0}, world);  // 点を変換\nFVec3 d = TransformVector(dir, world);      // 方向を変換 (移動成分無視)\nFMat4 inv = Inverse(view);                  // 逆変換",
      members: [
        { sig: "FMat4 operator*(const FMat4& a, const FMat4& b)", ret: "合成行列", desc: "a を適用してから b を適用する合成。順序が重要。" },
        { sig: "FMat4 Transpose(const FMat4& m)", ret: "転置行列", desc: "行と列を入れ替える。<t>HLSL</t> の列優先側へアップロードする前に使う。" },
        { sig: "FMat4 Inverse(const FMat4& m)", ret: "逆行列", desc: "変換を打ち消す<t>逆行列</t>。スクリーン→ワールド変換などに。" },
        { sig: "FVec4 Transform(FVec4 v, const FMat4& m)", ret: "変換後ベクトル", desc: "任意の 4D ベクトルを行列で変換（点・方向・同次座標すべて対応）。" },
        { sig: "FVec3 TransformPoint(FVec3 p, const FMat4& m)", ret: "変換後の点", desc: "点として変換（w=1 扱い、平行移動も効く）。頂点座標に。" },
        { sig: "FVec3 TransformVector(FVec3 v, const FMat4& m)", ret: "変換後の方向", desc: "方向として変換（w=0 扱い、平行移動は無視）。法線や速度に。" }
      ]
    },
    {
      name: "FQuat",
      kind: "構造体", header: "math/Quat.h",
      summary: "<t>クォータニオン</t>（x, y, z, w）。3D の<b>回転</b>を表す。<t>ジンバルロック</t>が起きず、回転同士の<t>球面線形補間</t>が滑らか。",
      when: "キャラやカメラの向きを保持・補間する時。回転を貯めても破綻しにくいので、オイラー角より回転状態の保存に向く。",
      sample: "FQuat q = FQuat::AxisAngle(FVec3::Up(), kHalfPi); // Y 軸 90 度\nFVec3 v = Rotate(q, {1,0,0});      // ベクトルを回す → ほぼ {0,0,1}\nFQuat a = FQuat::Identity(), b = q;\nFQuat mid = Slerp(a, b, 0.5f);     // 滑らかに 45 度\nFMat4 m = ToMatrix(q);             // 行列に変換",
      members: [
        { sig: "static FQuat Identity()", ret: "無回転", desc: "回転しない<t>クォータニオン</t> <code>{0,0,0,1}</code>。" },
        { sig: "static FQuat AxisAngle(FVec3 axis, f32 rad)", ret: "回転", desc: "軸 axis まわりに rad ラジアン回す回転を作る。" },
        { sig: "static FQuat Euler(f32 pitch, f32 yaw, f32 roll)", ret: "回転", desc: "<t>オイラー角</t>（ピッチ・ヨー・ロール、ラジアン）から作る。" },
        { sig: "static FQuat FromMatrix(const FMat4& m)", ret: "回転", desc: "回転<t>行列</t>からクォータニオンを取り出す。" },
        { sig: "FQuat operator*(FQuat a, FQuat b)", ret: "合成回転", desc: "回転の合成（まず b、次に a を適用）。" },
        { sig: "FQuat Slerp(FQuat a, FQuat b, f32 t)", ret: "補間回転", desc: "2 回転を t で<t>球面線形補間</t>。一定速度で滑らかに回る。", when: "向きをアニメーションさせる時。" },
        { sig: "FVec3 Rotate(FQuat q, FVec3 v)", ret: "回転後ベクトル", desc: "ベクトル v を q で回す。" },
        { sig: "FQuat Conjugate(q) / Inverse(q) / Normalize(q)", desc: "共役（単位 quat なら逆回転）・逆元・正規化。" },
        { sig: "FMat4 ToMatrix(FQuat q)", ret: "回転行列", desc: "クォータニオンを 4x4 回転行列に変換。" }
      ]
    },
    {
      name: "数学定数 (kPi ほか)",
      kind: "定数群", header: "math/Math.h",
      summary: "<code>kPi</code>（円周率）や度↔ラジアン変換係数、比較用<t>イプシロン</t>などの定数。",
      when: "角度計算（ラジアン）や浮動小数の近似比較をする時。",
      sample: "f32 rad = 90.0f * kDeg2Rad;  // 度 → ラジアン\nf32 deg = rad * kRad2Deg;    // ラジアン → 度\nif (Abs(x) < kEpsilon) { /* ほぼ 0 */ }",
      members: [
        { sig: "kPi / kTwoPi / kHalfPi / kInvPi", desc: "円周率 π、2π、π/2、1/π。" },
        { sig: "kDeg2Rad / kRad2Deg", desc: "度→ラジアン / ラジアン→度 の変換係数。角度は内部ではラジアンで扱う。" },
        { sig: "kEpsilon", desc: "比較用の微小値 <code>1e-6</code>。浮動小数の「ほぼ等しい」判定に。" }
      ]
    },
    {
      name: "スカラ数学関数",
      kind: "自由関数群", header: "math/Math.h",
      summary: "<code>&lt;cmath&gt;</code> をラップした単精度の数学関数群（Sqrt / Sin / Cos など）と、<t>線形補間</t>・<t>クランプ</t>・近似比較のヘルパ。",
      when: "三角関数や平方根、値の範囲制限、滑らかな補間など、1 つの数値（スカラ）に対する計算全般。",
      sample: "f32 s = Sin(angle);\nf32 v = Lerp(0.0f, 100.0f, t);   // 0〜100 を t で補間\nf32 c = Saturate(x);             // 0〜1 にクランプ\nif (IsNearlyEqual(a, b)) { /* ほぼ同じ */ }\nf32 r = ToRadians(45.0f);",
      members: [
        { sig: "f32 Abs/Sqrt/Sin/Cos/Tan/ASin/ACos(f32)", desc: "絶対値・平方根・三角関数群。" },
        { sig: "f32 ATan2(f32 y, f32 x)", ret: "角度", desc: "y/x の逆正接。ベクトルの向き（角度）を求めるのに使う。" },
        { sig: "f32 Pow/Exp/Log/Floor/Ceil/Round/Mod(...)", desc: "累乗・指数・対数・切り捨て/上げ・四捨五入・剰余。" },
        { sig: "f32 ToRadians(deg) / ToDegrees(rad)", desc: "度とラジアンの相互変換。" },
        { sig: "f32 Lerp(f32 a, f32 b, f32 t)", ret: "補間値", desc: "a と b を t で<t>線形補間</t>（t=0 で a、t=1 で b）。" },
        { sig: "f32 Saturate(f32 v)", ret: "[0,1] の値", desc: "v を 0〜1 に<t>クランプ</t>する。" },
        { sig: "bool IsNearlyEqual(f32 a, f32 b, f32 eps = kEpsilon)", ret: "ほぼ等しいか", desc: "誤差 eps 以内なら true。浮動小数を <code>==</code> せず比較する時に。" }
      ]
    },
    {
      name: "CCamera",
      kind: "クラス", header: "math/Camera.h",
      summary: "<t>ビュー行列</t>と<t>投影行列</t>をまとめて持つカメラのヘルパ。視点・注視点・画角を設定すると GPU に送る <code>ViewProjection</code> 行列が得られる。",
      when: "3D シーンを描くカメラを 1 つ用意したい時。手で <t>FMat4</t> を組まずに済む。",
      sample: "CCamera cam;\ncam.SetPerspective(60.0f * kDeg2Rad, 16.0f/9.0f, 0.1f, 1000.0f);\ncam.SetLookAt({0,2,-5}, {0,0,0}, FVec3::Up());\nFMat4 vp = cam.ViewProjection();  // GPU に送る\nFVec3 eye = cam.Eye();",
      members: [
        { sig: "void SetPerspective(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z)", desc: "<t>透視投影</t>（左手系）を設定。fov_y は縦画角（ラジアン）。" },
        { sig: "void SetOrthographic(f32 width, f32 height, f32 near_z, f32 far_z)", desc: "<t>正射影</t>を設定。2D や見下ろし視点に。" },
        { sig: "void SetLookAt(FVec3 eye, FVec3 target, FVec3 up = FVec3::Up())", desc: "視点 eye から target を見るビューを設定。" },
        { sig: "const FMat4& View() / Projection()", ret: "行列", desc: "ビュー / 投影行列を個別に取得。" },
        { sig: "FMat4 ViewProjection()", ret: "VP 行列", desc: "ビュー × 投影。シェーダの定数バッファに入れる定番。" },
        { sig: "FVec3 Eye()", ret: "視点位置", desc: "現在の視点（カメラ位置）。ライティングや距離計算に使う。" },
        { sig: "void UpdateAspect(fov_y_rad, aspect, near_z, far_z)", desc: "ウィンドウリサイズ時にアスペクト比だけ更新する。" },
        { sig: "using FCamera = CCamera", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CCamera</code> を使う。" }
      ]
    },
    {
      name: "FCpuFeatures / Cpu()",
      kind: "構造体・関数", header: "math/Cpu.h",
      summary: "実行中の <t>CPU</t> が対応する命令セット（<t>SSE</t>/<t>AVX</t>/FMA など）を <t>CPUID</t> で 1 度だけ調べてキャッシュする。",
      when: "CPU 機能に応じて最適な実装を選びたい時。通常は <t>FMathDispatch</t> が内部で使うので直接触る必要は少ない。",
      sample: "const FCpuFeatures& f = Cpu();\nif (f.avx2)  { /* AVX2 パス */ }\nif (HasAvx2()) { /* 簡易版 */ }",
      members: [
        { sig: "const FCpuFeatures& Cpu()", ret: "機能フラグ", desc: "初回呼び出しで <t>CPUID</t> を実行、以降キャッシュした結果を返す。" },
        { sig: "FCpuFeatures{ sse2, sse3, ssse3, sse41, sse42, avx, avx2, fma3, f16c, bmi1, bmi2, aes, popcnt, avx512f }", desc: "全 14 フラグを <code>bool</code> で持つ: <code>sse2/sse3/ssse3/sse41/sse42</code>(SSE 系)、<code>avx/avx2/avx512f</code>(AVX 系)、<code>fma3</code>(積和)、<code>f16c</code>(半精度変換)、<code>bmi1/bmi2</code>(ビット操作)、<code>aes</code>(暗号)、<code>popcnt</code>(ビット数え)。<code>sse2</code> は x64 で常に true。" },
        { sig: "bool HasAvx2() / HasSse41()", ret: "対応するか", desc: "頻出機能だけの簡易判定ヘルパ。" }
      ]
    },
    {
      name: "FMathDispatch",
      kind: "構造体・関数", header: "math/MathDispatch.h",
      summary: "N 個のベクトルを一括変換するような<b>バッチ演算</b>を、起動時に検出した <t>CPU</t> 機能に応じた最適実装へ振り分ける関数ポインタテーブル。",
      when: "大量の点や方向ベクトルを 1 つの<t>行列</t>でまとめて変換する時。1 個ずつ <code>TransformPoint</code> を呼ぶより速い。",
      sample: "FVec3 in[1000], out[1000];\nFMat4 world = FMat4::Translation({0,1,0});\nTransformPoints(in, out, 1000, world);   // 一括変換\nTransformVectors(normals, out, 1000, world);",
      members: [
        { sig: "void TransformPoints(const FVec3* in, FVec3* out, usize count, const FMat4& m)", desc: "count 個の点をまとめて行列変換する（移動成分も効く）。" },
        { sig: "void TransformVectors(const FVec3* in, FVec3* out, usize count, const FMat4& m)", desc: "count 個の方向ベクトルをまとめて変換する（移動成分は無視）。" },
        { sig: "const FMathDispatch& GetMathDispatch()", ret: "ディスパッチテーブル", desc: "選ばれた関数ポインタ群を取得（初回で初期化）。通常は上の利便関数で十分。" }
      ]
    },
    {
      name: "FAabb2 / FCircle / FRay2",
      kind: "構造体", header: "math/Collision2D.h",
      summary: "2D の衝突判定で使う基本形状。<t>FAabb2</t>=軸並行の矩形（中心+半サイズ）、<t>FCircle</t>=円、<t>FRay2</t>=レイ（始点+方向）。",
      when: "2D ゲームの当たり判定。プレイヤーや壁を矩形・円で表し、レイで射撃や視線を飛ばす。",
      sample: "FAabb2 player{ {x, y}, {8, 12} };       // 中心 + 半サイズ\nAabb2 wall = FAabb2::FromTopLeftSize({0,0}, {100, 8});\nCircle bullet{ {bx, by}, 4 };\nRay2 sight{ {px, py}, {1, 0} };",
      members: [
        { sig: "FAabb2{ FVec2 center, FVec2 half_size }", desc: "中心と半サイズ（幅/2, 高さ/2）で矩形を表す。" },
        { sig: "static FAabb2 FromMinMax(min, max) / FromTopLeftSize(tl, size)", ret: "矩形", desc: "左上+右下、または左上+サイズから作る。スプライト系に便利。" },
        { sig: "FVec2 FAabb2::Min() / Max()", ret: "角の座標", desc: "矩形の左上・右下の座標を返す。" },
        { sig: "FCircle{ FVec2 center, f32 radius }", desc: "中心と半径で円を表す。" },
        { sig: "FRay2{ FVec2 origin, FVec2 direction }", desc: "始点と方向のレイ。方向は正規化しなくてもよい（t は方向長さ依存）。" }
      ]
    },
    {
      name: "FConvexPoly2 / FObb2",
      kind: "構造体", header: "math/Collision2D.h",
      summary: "<t>FConvexPoly2</t>=<t>凸多角形</t>コライダー（最大 16 頂点）。<t>FObb2</t>=回転できる矩形（<t>OBB</t>、中心+半サイズ+回転角）。複雑な形のスプライト当たり判定に。",
      when: "回転する箱や、スプライトの凸包など、軸並行矩形では表せない形の当たり判定が要る時。判定は内部で <t>SAT</t> に委譲される。",
      sample: "FConvexPoly2 tri;\ntri.Add({0,0}); tri.Add({10,0}); tri.Add({5,10});\nObb2 box{ {x,y}, {16,8}, 0.5f };  // 中心,半サイズ,回転(rad)\nif (Intersect(box, tri)) { /* 衝突 */ }\nConvexPoly2 p = ToPoly(box);      // OBB を多角形に",
      members: [
        { sig: "void FConvexPoly2::Add(FVec2 v) / Clear()", desc: "頂点を追加 / 全消去。最大 <code>kMaxVerts</code>(16) まで。" },
        { sig: "FObb2{ FVec2 center, FVec2 half_size, f32 rotation }", desc: "回転矩形。rotation はラジアン（反時計回り）。" },
        { sig: "FVec2 FObb2::AxisX() / AxisY()", ret: "ローカル軸", desc: "回転後のローカル X/Y 軸（単位ベクトル）。" },
        { sig: "FConvexPoly2 ToPoly(const FAabb2/FObb2&)", ret: "多角形", desc: "矩形/OBB を 4 頂点の凸多角形に変換し、共通の SAT 判定へ載せる。" },
        { sig: "FAabb2 AabbOf(const FConvexPoly2/FObb2&)", ret: "外接矩形", desc: "形状を囲む軸並行矩形を求める。広域カリングの前段に。" },
        { sig: "FVec2 Centroid(const FConvexPoly2&)", ret: "重心", desc: "多角形の頂点平均（重心の近似）。" }
      ]
    },
    {
      name: "2D 衝突関数 (Intersect / Resolve / Raycast)",
      kind: "自由関数群", header: "math/Collision2D.h",
      summary: "2D 形状同士の重なり判定（<t>Intersect</t>）、めり込みを押し戻す<t>MTV</t>（<t>Resolve</t>）、レイの命中判定（Raycast）。<t>FAabb2</t>/<t>FCircle</t>/<t>FConvexPoly2</t>/<t>FObb2</t> の組み合わせにオーバーロードされている。",
      when: "2D の当たり判定そのもの。衝突したか（Intersect）、どれだけ押し戻すか（Resolve）、レイが当たった点と法線（Raycast）を得る。",
      sample: "if (Intersect(player, wall)) { /* 重なり */ }\nFVec2 push;\nif (Resolve(playerBox, wallBox, push)) player.center += push; // めり込み解消\nRayHit2 h = RaycastAabb(sight, wall);\nif (h.hit) { drawHit(h.point, h.normal); }\nbool inside = Contains(wall, mousePos);",
      members: [
        { sig: "bool Intersect(A, B)", ret: "重なるか", desc: "2 形状が重なっていれば true。AABB/円/凸多角形/OBB の全組合せ対応。" },
        { sig: "bool Contains(shape, FVec2 p)", ret: "内側か", desc: "点 p が形状の内部にあるか。マウス当たり判定などに。" },
        { sig: "bool Resolve(A, B, FVec2& push)", ret: "衝突したか", desc: "衝突時 true。push に「A を B から離す最小移動量」(<t>MTV</t>)が入る。" },
        { sig: "FRayHit2 RaycastAabb/RaycastCircle/RaycastConvexPoly2/RaycastObb2(ray, shape, t_max)", ret: "命中情報", desc: "レイと形状の交差。<code>hit</code>・<code>t</code>・<code>point</code>・<code>normal</code> を返す。" }
      ]
    },
    {
      name: "FRayHit2",
      kind: "構造体", header: "math/Collision2D.h",
      summary: "2D レイキャストの結果。当たったか・距離 t・衝突点・表面の<t>法線</t>を持つ。",
      when: "Raycast 系関数の戻り値を受け取り、命中したかと当たった場所を調べる時。",
      sample: "FRayHit2 h = RaycastCircle(ray, enemy);\nif (h.hit) {\n    FVec2 p = h.point;     // 衝突座標\n    FVec2 n = h.normal;    // 表面法線\n    f32   d = h.t;         // origin から t 倍の位置\n}",
      members: [
        { sig: "bool hit", desc: "命中したら true。" },
        { sig: "f32 t", ret: "距離係数", desc: "<code>origin + direction * t</code> が衝突点。" },
        { sig: "FVec2 point / FVec2 normal", desc: "衝突座標と、命中面の外向き<t>法線</t>。" }
      ]
    },
    {
      name: "FAabb3 / FSphere / FPlane / FRay3",
      kind: "構造体", header: "math/Collision3D.h",
      summary: "3D の衝突判定で使う形状。<t>FAabb3</t>=軸並行ボックス、<t>FSphere</t>=球、<t>FPlane</t>=平面、<t>FRay3</t>=レイ。",
      when: "3D ゲームの当たり判定。キャラを球、地形を箱や平面で近似し、レイで射撃・ピッキングを行う。",
      sample: "FAabb3 box = FAabb3::FromCenterExtents({0,0,0}, {1,1,1});\nFSphere s{ {3,0,0}, 0.5f };\nFPlane ground = FPlane::FromPointNormal({0,0,0}, FVec3::Up());\nRay3 ray{ cam.Eye(), forward };",
      members: [
        { sig: "FAabb3{ FVec3 center, FVec3 half_size }", desc: "中心と半サイズの 3D ボックス。" },
        { sig: "static FAabb3 FromMinMax(min, max) / FromCenterExtents(center, extents)", ret: "ボックス", desc: "最小/最大、または中心/範囲から作る。" },
        { sig: "FVec3 FAabb3::Min() / Max()", ret: "角の座標", desc: "ボックスの最小・最大コーナー。" },
        { sig: "FSphere{ FVec3 center, f32 radius }", desc: "中心と半径の球。" },
        { sig: "FPlane{ FVec3 normal, f32 d }", desc: "<t>法線</t>と原点からの距離 d で平面を表す（<code>dot(n,p)+d=0</code>）。" },
        { sig: "static FPlane FromPointNormal(FVec3 p, FVec3 n)", ret: "平面", desc: "平面上の 1 点 p と法線 n から平面を作る。" },
        { sig: "FRay3{ FVec3 origin, FVec3 direction }", desc: "始点と方向の 3D レイ。" }
      ]
    },
    {
      name: "3D 衝突関数 (Intersect / Resolve / Raycast)",
      kind: "自由関数群", header: "math/Collision3D.h",
      summary: "3D 形状の重なり判定（<t>Intersect</t>）・押し戻し（<t>Resolve</t>）・レイキャスト（Raycast）。三角形との交差（<t>Möller–Trumbore</t>）も含む。",
      when: "3D の当たり判定。物体同士の衝突、めり込み解消、マウスピッキングや弾道のレイ、メッシュ三角形との交差を取る時。",
      sample: "if (Intersect(box, s)) { /* 重なり */ }\nFVec3 push;\nif (Resolve(ballA, ballB, push)) ballA.center += push;\nRayHit3 h = RaycastAabb(ray, box);\nRayHit3 t = RaycastTriangle(ray, v0, v1, v2); // メッシュ当たり\nRayHit3 g = RaycastPlane(ray, ground);",
      members: [
        { sig: "bool Intersect(A, B)", ret: "重なるか", desc: "AABB・球の組合せで重なり判定。" },
        { sig: "bool Contains(FAabb3/FSphere, FVec3 p)", ret: "内側か", desc: "点 p が形状内部にあるか。" },
        { sig: "bool Resolve(const FSphere& a, const FSphere& b, FVec3& push)", ret: "衝突したか", desc: "球同士のめり込みを押し出す <t>MTV</t> を求める。" },
        { sig: "FRayHit3 RaycastAabb/RaycastSphere/RaycastPlane(ray, shape, t_max)", ret: "命中情報", desc: "レイと AABB/球/平面の交差。" },
        { sig: "FRayHit3 RaycastTriangle(ray, FVec3 v0, FVec3 v1, FVec3 v2, t_max)", ret: "命中情報", desc: "三角形（両面）との交差を<t>Möller–Trumbore</t>法で計算。メッシュの精密ピッキングに。" }
      ]
    },
    {
      name: "FRayHit3",
      kind: "構造体", header: "math/Collision3D.h",
      summary: "3D レイキャストの結果。命中したか・距離 t・衝突点・表面<t>法線</t>を持つ。",
      when: "3D の Raycast 系関数の戻り値。命中位置や面の向きを使って描画やヒット処理をする時。",
      sample: "FRayHit3 h = RaycastSphere(ray, enemy);\nif (h.hit) {\n    spawnImpact(h.point, h.normal);\n    f32 dist = h.t * Length(ray.direction);\n}",
      members: [
        { sig: "bool hit", desc: "命中したら true。" },
        { sig: "f32 t", ret: "距離係数", desc: "<code>origin + direction * t</code> が衝突点。" },
        { sig: "FVec3 point / FVec3 normal", desc: "衝突座標と命中面の<t>法線</t>。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "ベクトル": "向きと大きさを持つ量。2D/3D の位置・速度・方向を <code>FVec2</code>/<code>FVec3</code> で表す。",
  "行列": "複数の変換（移動・回転・拡大）をまとめて表す数値の表。3D では <code>FMat4</code>(4x4)。<t>ベクトル</t>に掛けて座標変換する。",
  "クォータニオン": "4 つの数で 3D の回転を表す方式。<t>ジンバルロック</t>がなく、回転の補間が滑らか。ACS では <code>FQuat</code>。",
  "内積": "2 ベクトルの「向きの近さ」を表す値（dot）。同じ向きで最大、直角で 0。角度や影の濃さの計算に使う。",
  "外積": "2 つの 3D ベクトルの両方に直交するベクトルを作る演算（cross）。面の<t>法線</t>や回転軸を求める。",
  "線形補間": "2 値の間を一定割合 t(0〜1) で混ぜる（lerp）。t=0 で始点、t=1 で終点。滑らかな移動・色変化に。",
  "球面線形補間": "<t>クォータニオン</t>の回転を一定速度で滑らかに補間する方法（slerp）。向きのアニメーションに使う。",
  "法線": "面や線に垂直な向きのベクトル。当たった面の向きやライティングの計算に使う。",
  "ジンバルロック": "<t>オイラー角</t>で回転を表す時、特定の角度で 1 軸ぶんの自由度が失われる現象。<t>クォータニオン</t>なら起きない。",
  "オイラー角": "ピッチ・ヨー・ロールの 3 角度で回転を表す方式。直感的だが<t>ジンバルロック</t>の弱点がある。",
  "同次座標": "3D の点を (x,y,z,w) の 4 成分で表す方式。w=1 が点、w=0 が方向。<t>行列</t>1 つで移動も含めて変換できる。",
  "投影行列": "3D の世界を 2D 画面に映すための<t>行列</t>。遠近感ありが<t>透視投影</t>、なしが<t>正射影</t>。",
  "透視投影": "遠くの物ほど小さく見える、人間の目に近い投影。3D ゲームの基本。",
  "正射影": "距離に関係なく同じ大きさで映す投影。2D ゲームや UI、見下ろし視点に使う。",
  "ビュー行列": "カメラの位置・向きに合わせて世界を変換する<t>行列</t>。<code>LookAtLH</code> 等で作る。",
  "ワールド行列": "オブジェクト固有の座標を、ゲーム世界の共通座標へ移す<t>行列</t>（移動・回転・拡大の合成）。",
  "行優先": "<t>行列</t>をメモリに行ごとに並べる方式（row-major）。ACS は行優先で、<t>HLSL</t>(列優先)へ送る前に <code>Transpose</code> する。",
  "クランプ": "値を指定範囲内に収めること。<code>Saturate</code> は 0〜1 に収める。",
  "イプシロン": "浮動小数の比較に使う非常に小さな許容誤差。<code>kEpsilon</code> = 1e-6。",
  "FAabb2": "軸並行の 2D 矩形コライダー（中心+半サイズ）。最も軽い当たり判定。",
  "FCircle": "2D の円コライダー（中心+半径）。",
  "FRay2": "2D のレイ（始点+方向）。射撃や視線の判定に。",
  "FConvexPoly2": "2D の<t>凸多角形</t>コライダー（最大 16 頂点）。複雑な形のスプライトに。",
  "FObb2": "回転できる 2D 矩形コライダー（<t>OBB</t>）。",
  "FAabb3": "軸並行の 3D ボックスコライダー。",
  "FSphere": "3D の球コライダー（中心+半径）。",
  "FPlane": "3D の無限平面（<t>法線</t>+距離）。地面や壁の近似に。",
  "FRay3": "3D のレイ（始点+方向）。ピッキングや弾道に。",
  "Intersect": "2 形状が重なっているかを bool で返す判定関数。",
  "Resolve": "重なった 2 形状を引き離す最小移動量(<t>MTV</t>)を求める関数。めり込み解消に。",
  "MTV": "最小並進ベクトル（Minimum Translation Vector）。重なりを解消する最短の押し戻し方向と距離。",
  "凸多角形": "へこみのない多角形。どの 2 点を結んでも内部に収まる。当たり判定が単純で高速。",
  "OBB": "有向境界ボックス（Oriented Bounding Box）。回転を持てる矩形/直方体コライダー。",
  "SAT": "分離軸判定（Separating Axis Theorem）。凸形状同士の重なりを「どこかの軸で離れているか」で調べる手法。",
  "Möller–Trumbore": "レイと三角形の交差を高速に計算する定番アルゴリズム。メッシュのピッキングに使う。",
  "DirectXMath": "Microsoft の <t>SIMD</t> 数学ライブラリ。ACS の Vec/Mat/Quat はこれに委譲して高速化している。",
  "SIMD": "1 命令で複数のデータをまとめて計算する CPU 機能。ベクトル計算を高速化する。",
  "CPUID": "CPU 自身に対応命令セットを問い合わせる命令。<code>Cpu()</code> が起動時に使う。",
  "SSE": "x86 CPU の<t>SIMD</t>命令セットの一系統。SSE2 は 64bit で常に使える。",
  "AVX": "SSE の後継の<t>SIMD</t>命令セット。AVX2 などより広い幅で計算できる。",
  "FMathDispatch": "CPU 機能に応じてバッチ演算の最適実装を選ぶ仕組み。",
  "CPU": "中央演算装置。対応する<t>SIMD</t>命令により計算速度が変わる。",
  "HLSL": "DirectX のシェーダ言語。既定で列優先のため、行列を送る前に <code>Transpose</code> が要る。"
});
