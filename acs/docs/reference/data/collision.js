/* ACS リファレンス — collision モジュール（手書き）。
   形式: ACS_REF.modules.push({...}) + Object.assign(ACS_REF.glossary,{...})
   記法: 本文の専門用語は <t>用語</t> で囲む。コード内の < > は &lt; &gt;。 */
ACS_REF.modules.push({
  id: "collision",
  order: 38,
  title: "collision — 当たり判定",
  blurb: "形(スプライトの絵やメッシュ)から<b>当たり判定の形状</b>を自動で作り、当たっているか・どこで当たったかを調べる道具です。アルファ画像から<t>輪郭</t>や<t>凸包</t>を起こしたり、点群から 3D <t>凸包</t>を組み立てたり、メッシュに<t>レイキャスト</t>したりできます。",
  types: [
    {
      name: "CSpriteCollider",
      kind: "クラス", header: "collision/SpriteCollider.h",
      summary: "2D スプライトの<b>アルファチャンネル</b>(透明度)から、絵の形に沿った当たり判定形状を作るクラス。<t>凸包</t>(扱いやすい)と簡略化済みの<t>輪郭</t>ポリゴン(凹形状にも追従)の両方を生成し、点の内外判定や <t>AABB</t> も得られる。",
      when: "キャラやアイテムのスプライト画像から、見た目どおりの当たり判定をワンタッチで作りたい時。手で頂点を並べる必要がなくなる。",
      sample: "acs::CSpriteCollider col;\ncol.BuildFromAlpha(rgba, w, h, /*threshold=*/128, /*simplify=*/1.5f);\nif (col.ContainsPoint({mx, my})) { /* 絵の上をクリック */ }\nFConvexPoly2 poly = col.HullPolygon();  // 物理に渡せる凸ポリゴン",
      members: [
        { sig: "TResult<void> BuildFromAlpha(const u8* rgba, u32 w, u32 h, u8 alpha_threshold = 128, f32 simplify_epsilon = 1.5f)", ret: "成功/失敗(<t>Result</t>)", desc: "RGBA8 画像(上から下・左から右に詰めた配列)を読み、<code>alpha &gt;= threshold</code> の画素を「内側」として<t>凸包</t>と<t>輪郭</t>を作る。<code>simplify_epsilon</code> は輪郭の許容誤差(px)で、大きいほど頂点が減る。", when: "テクスチャを読み込んだ直後に 1 回呼ぶ。単一の塊・穴なしの絵が前提。" },
        { sig: "bool ContainsPoint(FVec2 p) const", ret: "内側なら true", desc: "簡略化済み<t>輪郭</t>ポリゴンで点の内外を判定する(<t>レイ交差法</t>、凹形状にも対応)。座標はピクセル空間(左上原点)。", sample: "if (col.ContainsPoint({px, py})) Select();" },
        { sig: "FConvexPoly2 HullPolygon() const", ret: "凸ポリゴン", desc: "<t>凸包</t>を物理用の <code>FConvexPoly2</code>(最大 16 頂点)に変換して返す。頂点が多ければ均等に間引く。<code>FCollisionWorld2D::AddPolygon</code> にそのまま渡せる。", when: "2D 物理ワールドにこのスプライトを当たり判定として登録する時。" },
        { sig: "const FVec2* Hull() const / u32 HullCount() const", ret: "凸包の頂点配列と個数", desc: "生成された<t>凸包</t>の頂点列。常に有効で、物理で扱いやすい形。" },
        { sig: "const FVec2* Outline() const / u32 OutlineCount() const", ret: "輪郭の頂点配列と個数", desc: "簡略化された<t>輪郭</t>の頂点列。凹んだ形にも沿う、見た目に忠実な形。" },
        { sig: "FAabb2 Bounds() const", ret: "外接矩形", desc: "形を囲む 2D の<t>AABB</t>(軸並行の最小矩形)。粗い当たり判定の前段に使える。" },
        { sig: "void Clear()", desc: "生成した形状をすべて捨てて空に戻す。" },
        { sig: "static constexpr u32 kMaxVertices = 256", desc: "凸包・輪郭それぞれの頂点数の上限(定数)。" },
        { sig: "using FSpriteCollider = CSpriteCollider", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CSpriteCollider</code> を使う。" }
      ]
    },
    {
      name: "CMeshCollider",
      kind: "クラス", header: "collision/MeshCollider.h",
      summary: "3D の三角形メッシュから当たり判定を作るクラス。内部で <t>BVH</t>(三角形を空間で階層分けした木)を組み、<t>レイキャスト</t>や <t>AABB</t> との重なり判定を高速に行う。",
      when: "床・壁・地形などのメッシュに対して、視線・弾・マウスのレイがどこに当たるかを調べたい時。レンダリング用メッシュをそのまま当たり判定に流用できる。",
      sample: "acs::CMeshCollider col;\ncol.BuildFromMesh(*Primitive::MakeCube(2.0f));\nacs::FRay3 ray{ {0,5,0}, {0,-1,0} };\nacs::FRayHit3 h = col.Raycast(ray);\nif (h.hit) Place(h.point, h.normal);  // 当たった位置と面の向き",
      members: [
        { sig: "TResult<void> BuildFromMesh(const AMeshAsset& mesh)", ret: "成功/失敗(<t>Result</t>)", desc: "レンダリング用メッシュ(<code>AMeshAsset</code>)の<b>頂点位置だけ</b>を使って当たり判定を構築する。", when: "描画に使っているメッシュをそのまま衝突にも使いたい時。" },
        { sig: "TResult<void> BuildFromTriangles(const FVec3* positions, u32 vertex_count, const u32* indices, u32 index_count)", ret: "成功/失敗", desc: "生の頂点位置配列 + 三角形<t>インデックス</t>列(3 個 1 組)から構築する。<code>BuildConvexHull3</code> の出力をそのまま渡せる。", sample: "col.BuildFromTriangles(hv.Data(), hv.Size(), hi.Data(), hi.Size());" },
        { sig: "FRayHit3 Raycast(const FRay3& ray, f32 t_max = 3.4028235e38f) const", ret: "ヒット情報", desc: "レイを飛ばし<b>最も近い</b>当たりを返す。結果の <code>hit</code>(当たったか)・<code>t</code>(距離)・<code>point</code>(位置)・<code>normal</code>(三角形の面法線)を見る。当たらなければ <code>hit=false</code>。", when: "マウスピッキング・着弾点・視線判定など。" },
        { sig: "bool OverlapsAabb(const FAabb3& box) const", ret: "重なるなら true", desc: "指定した <t>AABB</t> がメッシュ(の三角形)と重なるかを <t>BVH</t> で素早く判定する。<t>ブロードフェーズ</t>のふるい分けに。" },
        { sig: "FAabb3 Bounds() const", ret: "外接箱", desc: "メッシュ全体を囲む 3D の<t>AABB</t>。" },
        { sig: "u32 TriangleCount() const / u32 NodeCount() const", ret: "三角形数 / ノード数", desc: "保持する三角形の数と、組み上がった <t>BVH</t> のノード数。主にデバッグ・規模確認用。" },
        { sig: "void Clear()", desc: "構築済みの三角形と <t>BVH</t> をすべて捨てて空に戻す。" },
        { sig: "using FMeshCollider = CMeshCollider", desc: "旧名を使う既存コード向けの互換別名。新しいコードでは <code>CMeshCollider</code> を使う。" }
      ]
    },
    {
      name: "BuildConvexHull3",
      kind: "関数", header: "collision/ConvexHull3.h",
      summary: "3D の<b>点群</b>(メッシュ頂点など)から<t>凸包</t>を作り、三角形メッシュ(頂点配列 + <t>インデックス</t>列)として出力する関数。凹んだメッシュを軽い「凸の当たり判定」に近似するのに使う。",
      when: "複雑なメッシュをそのまま当たり判定にすると重い時に、その外形を包む凸の形だけ取り出して軽量化したい時。出力はそのまま <code>CMeshCollider::BuildFromTriangles</code> に渡せる。",
      sample: "acs::TArray&lt;acs::FVec3&gt; hv; acs::TArray&lt;acs::u32&gt; hi;\nacs::BuildConvexHull3(points, count, hv, hi);\n// hv = 凸包頂点, hi = 三角形インデックス(3 個 1 組)\nfMeshCol.BuildFromTriangles(hv.Data(), hv.Size(), hi.Data(), hi.Size());",
      members: [
        { sig: "TResult<void> BuildConvexHull3(const FVec3* points, u32 count, TArray<FVec3>& out_verts, TArray<u32>& out_indices)", ret: "成功/失敗(<t>Result</t>)", desc: "<code>points</code>/<code>count</code> から<t>凸包</t>を構築し、<code>out_verts</code>(凸包の頂点)と <code>out_indices</code>(三角形、3 個 1 組)を埋める。アルゴリズムは <t>増分凸包</t>(初期四面体に点を 1 つずつ足していく)。点が 4 個未満・すべて同一平面など退化した場合はエラーを返す。" }
      ]
    }
  ]
});

Object.assign(ACS_REF.glossary, {
  "凸包": "与えた点をすべて包む、へこみのない最小の形(輪ゴムで囲んだような外形)。当たり判定で扱いやすい。",
  "輪郭": "形の縁(ふち)をなぞった線。<t>凸包</t>と違いへこみ(凹み)にも沿う。",
  "AABB": "軸に平行な辺で作る、対象を囲む最小の長方形/直方体(Axis-Aligned Bounding Box)。粗い当たり判定が速い。",
  "BVH": "図形を空間的にまとめて階層の木にした構造(Bounding Volume Hierarchy)。<t>レイキャスト</t>や重なり判定を高速化する。",
  "レイキャスト": "ある点から一方向に半直線(レイ)を飛ばし、最初にぶつかる面の位置・距離・向きを調べること。",
  "レイ交差法": "判定点から外へ線を引き、形の縁を奇数回またげば内側と判断する点の内外判定。",
  "ブロードフェーズ": "本格的な衝突計算の前に、明らかに当たらない組を <t>AABB</t> 等でざっくり除外する前段。",
  "増分凸包": "点を 1 個ずつ足しながら<t>凸包</t>を更新していく構築アルゴリズム(incremental hull)。"
});
