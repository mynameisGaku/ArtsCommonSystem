/* ===========================================================================
   ACS リファレンス — レンダラ（手書き・依存なし）
   ・各ページは <body data-page="..." data-module="..."> を持つ。
   ・data/*.js（手書き）が window.ACS_REF に push したデータを描画する。
   ・window.ACS_NAV（assets/nav.js）からヘッダ・サイドバーの目次を組む。
   生成器ではなく、ブラウザ実行時に描画するだけ。
   ========================================================================= */
(function () {
  "use strict";
  var REF = window.ACS_REF || { modules: [], glossary: {}, guide: [], troubleshooting: [] };
  var NAV = window.ACS_NAV || { docs: [], groups: [] };

  // 同一 id のモジュール（gameframework 等、複数ファイル）を 1 つに統合
  (function () {
    var byId = {}, merged = [];
    REF.modules.forEach(function (m) {
      if (byId[m.id]) {
        byId[m.id].types = (byId[m.id].types || []).concat(m.types || []);
      } else { byId[m.id] = m; merged.push(m); }
    });
    REF.modules = merged;
    // 型ごとに一意なアンカーを割り当て（同一スラグ衝突を回避: 2件目以降に -2,-3…）
    REF.modules.forEach(function (m) {
      var seen = {};
      (m.types || []).forEach(function (t) {
        var s = slug(t.name);
        seen[s] = (seen[s] || 0) + 1;
        t.__anchor = seen[s] === 1 ? s : s + "-" + seen[s];
      });
    });
  })();

  // 型名 → その型自身の概要/所在（ツールチップを文脈に合った説明にするため）。
  // 読み込まれているモジュールの型のみ（モジュールページは当該モジュール、index/検索/用語集は全型）。
  var TYPEINDEX = {};
  REF.modules.forEach(function (m) {
    (m.types || []).forEach(function (t) {
      var n = (t.name || "").replace(/<[^>]+>/g, "").trim();
      if (n && !TYPEINDEX[n]) TYPEINDEX[n] = { summary: t.summary || "", modId: m.id, anchor: t.__anchor, kind: t.kind || "" };
    });
  });

  var $ = function (s, el) { return (el || document).querySelector(s); };
  function ce(tag, cls, html) { var e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; return e; }
  function esc(s) { return (s || "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }
  function stripTags(s) { return (s || "").replace(/<[^>]+>/g, ""); }
  function slug(s) { return stripTags(s).trim().toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "") || "item"; }
  // 実体参照を1段デコード（&lt; → <）。&amp; は最後に。
  function decodeEnt(s) {
    return (s || "").replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&quot;/g, '"')
      .replace(/&#39;/g, "'").replace(/&apos;/g, "'").replace(/&amp;/g, "&");
  }
  // 識別子（型名・シグネチャ）の表示用: 実体参照/生<>のどちらで書かれていても正しく文字として出す。
  function dispCode(s) { return esc(decodeEnt(s || "")); }

  var SVG_CARET = '<svg class="caret" viewBox="0 0 10 10" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M3 1.5 L7 5 L3 8.5"/></svg>';
  var SVG_SEARCH = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><circle cx="7" cy="7" r="4.5"/><path d="M11 11 L15 15"/></svg>';

  // ---- コードハイライト（入力は既にHTMLエンティティ化済み。壊さない単一走査） ----
  var KW = {};
  ("return if else for while do switch case break continue class struct enum union const constexpr " +
   "auto void using namespace public private protected override final noexcept template typename static " +
   "virtual inline new delete true false nullptr sizeof this operator mutable explicit friend").split(" ")
    .forEach(function (k) { KW[k] = 1; });

  function hl(code) {
    var s = code || "", i = 0, n = s.length, out = "";
    while (i < n) {
      var c = s[i];
      if (c === "&") { // エンティティはそのまま通す
        var em = /^&(?:lt|gt|amp|quot|apos|#\d+|[a-zA-Z]+);/.exec(s.slice(i));
        if (em) { out += em[0]; i += em[0].length; continue; }
        out += "&"; i++; continue;
      }
      if (c === "/" && s[i + 1] === "/") { out += '<span class="cm">' + s.slice(i) + "</span>"; break; }
      if (c === '"' || c === "'") {
        var q = c, j = i + 1, str = c;
        while (j < n) {
          if (s[j] === "\\") { str += s[j] + (s[j + 1] || ""); j += 2; continue; }
          if (s[j] === "&") { var sm = /^&(?:lt|gt|amp|quot|apos|#\d+|[a-zA-Z]+);/.exec(s.slice(j)); if (sm) { str += sm[0]; j += sm[0].length; continue; } }
          str += s[j];
          if (s[j] === q) { j++; break; }
          j++;
        }
        out += '<span class="str">' + str + "</span>"; i = j; continue;
      }
      if (c >= "0" && c <= "9") {
        var nm = /^(?:0[xX][0-9a-fA-F]+|[0-9]+\.?[0-9]*)[fFuUlL]*/.exec(s.slice(i))[0];
        out += '<span class="num">' + nm + "</span>"; i += nm.length; continue;
      }
      if (/[A-Za-z_]/.test(c)) {
        var id = /^[A-Za-z_]\w*/.exec(s.slice(i))[0];
        out += KW[id] ? '<span class="kw">' + id + "</span>" : id;
        i += id.length; continue;
      }
      out += c; i++;
    }
    return out;
  }

  function codeBlock(text) {
    return '<pre class="code"><button class="copy" type="button">copy</button><span class="src">' + hl(text) + "</span></pre>";
  }

  // =========================================================================
  // chrome（ヘッダ / サイドバー / フッタ）
  // =========================================================================
  function buildHeader(page) {
    var h = ce("header", "top");
    h.appendChild(ce("div", "brand", '<span class="a">ACS</span><span class="b">reference</span>'));
    var nav = ce("nav");
    NAV.docs.forEach(function (d) {
      var a = ce("a", page === d.key ? "active" : null, esc(d.label));
      a.href = d.page; nav.appendChild(a);
    });
    h.appendChild(nav);
    var box = ce("div", "search",
      SVG_SEARCH + '<input id="searchbox" type="search" placeholder="検索…" autocomplete="off"><span class="kbd">/</span>');
    h.appendChild(box);
    return h;
  }

  function buildSidebar(activeId) {
    var aside = ce("aside", "side");
    var filt = ce("input", "sfilter"); filt.type = "search"; filt.placeholder = "モジュールを絞り込み…";
    filt.addEventListener("input", function () { filterSide(aside, filt.value.trim().toLowerCase()); });
    aside.appendChild(filt);

    NAV.groups.forEach(function (g) {
      var grp = ce("div", "grp");
      grp.appendChild(ce("div", "glabel", esc(g.label)));
      g.items.forEach(function (it) {
        var cur = it.id === activeId;
        var mod = REF.modules.filter(function (m) { return m.id === it.id; })[0];
        var cnt = mod && mod.types ? mod.types.length : "";
        var a = ce("a", "mlink" + (cur ? " cur" : ""),
          '<span class="ml">' + esc(it.label) + "</span>" + (cnt !== "" ? '<span class="mc">' + cnt + "</span>" : ""));
        a.href = it.page; a.dataset.key = (it.id + " " + it.label).toLowerCase();
        grp.appendChild(a);
        // 現在モジュールは型一覧（ページ内アンカー）を展開
        if (cur && mod) {
          var types = ce("div", "types");
          (mod.types || []).forEach(function (t) {
            var ta = ce("a", null, dispCode(t.name)); ta.href = "#" + (t.__anchor || slug(t.name));
            ta.dataset.anchor = (t.__anchor || slug(t.name));
            ta.title = decodeEnt(stripTags(t.name));
            types.appendChild(ta);
          });
          grp.appendChild(types);
        }
      });
      aside.appendChild(grp);
    });
    return aside;
  }
  function filterSide(aside, q) {
    aside.querySelectorAll(".grp").forEach(function (grp) {
      var any = false;
      grp.querySelectorAll("a.mlink").forEach(function (a) {
        var hit = !q || a.dataset.key.indexOf(q) >= 0;
        a.style.display = hit ? "" : "none"; if (hit) any = true;
      });
      var lbl = grp.querySelector(".glabel"); if (lbl) lbl.style.display = any ? "" : "none";
    });
  }

  function buildFooter(extra) {
    var allTypes = REF.modules.reduce(function (a, m) { return a + (m.types ? m.types.length : 0); }, 0);
    var f = ce("footer", "foot");
    f.appendChild(ce("div", "fstat",
      (extra ? extra + " · " : "") +
      NAV.groups.reduce(function (a, g) { return a + g.items.length; }, 0) + " modules · " +
      allTypes + " types · " + Object.keys(REF.glossary).length + " 用語"));
    f.appendChild(ce("div", null, "ACS — 日本のインディー開発者向け軽量 C++ ゲームフレームワーク (Windows / DirectX 12)。手書きリファレンス。"));
    return f;
  }

  // =========================================================================
  // 型カード / メンバ
  // =========================================================================
  function memberEl(mb) {
    var wrap = ce("div", "member");
    var sig = ce("button", "m-sig",
      SVG_CARET + '<span class="sig">' + dispCode(mb.sig) + "</span>" +
      (mb.ret ? '<span class="ret">→ ' + esc(stripTags(mb.ret)) + "</span>" : ""));
    var body = ce("div", "m-body");
    var h = '<div class="desc">' + (mb.desc || "") + "</div>";
    if (mb.sample) h += codeBlock(mb.sample);
    if (mb.when) h += '<div class="when"><b>使う場面:</b> ' + mb.when + "</div>";
    body.innerHTML = h;
    sig.addEventListener("click", function () { wrap.classList.toggle("open"); });
    wrap.appendChild(sig); wrap.appendChild(body);
    return wrap;
  }

  function typeEl(t) {
    var card = ce("div", "type"); card.id = t.__anchor || slug(t.name);
    card.appendChild(ce("div", "t-head",
      '<div class="tname">' + dispCode(t.name) + "</div>" +
      '<div class="tmeta">' + (t.kind ? '<span class="kind">' + esc(t.kind) + "</span>" : "") +
      (t.header ? ' · <span class="hp">' + esc(t.header) + "</span>" : "") + "</div>"));
    var body = ce("div", "t-body");
    var h = "";
    if (t.summary) h += '<div class="kv"><div class="k">概要</div><div class="vv">' + t.summary + "</div></div>";
    if (t.when) h += '<div class="kv"><div class="k">こういう時に使う</div><div class="vv">' + t.when + "</div></div>";
    if (t.sample) h += '<div class="kv"><div class="k">サンプル</div>' + codeBlock(t.sample) + "</div>";
    body.innerHTML = h;
    if (t.members && t.members.length) {
      body.appendChild(ce("div", "mhd", "メンバー (" + t.members.length + ")"));
      var ms = ce("div", "members");
      t.members.forEach(function (mb) { ms.appendChild(memberEl(mb)); });
      body.appendChild(ms);
    }
    card.appendChild(body);
    return card;
  }

  // =========================================================================
  // ページ描画
  // =========================================================================
  function renderModule(content, modId) {
    var mod = REF.modules.filter(function (m) { return m.id === modId; })[0];
    if (!mod) { content.appendChild(ce("p", "empty", "モジュールが見つかりません: " + esc(modId))); return; }
    document.title = stripTags(mod.title) + " — ACS リファレンス";
    content.appendChild(ce("div", "crumb", '<a href="index.html">reference</a> / ' + esc(modId)));
    content.appendChild(ce("div", "kicker", esc(modId)));
    content.appendChild(ce("h1", "page", esc(stripTags(mod.title).replace(/^[^—\-]*[—\-]\s*/, ""))));
    if (mod.blurb) content.appendChild(ce("p", "blurb", mod.blurb));
    (mod.types || []).forEach(function (t) { content.appendChild(typeEl(t)); });
    return mod;
  }

  function renderGuide(content) {
    document.title = "はじめに — ACS リファレンス";
    content.appendChild(ce("div", "kicker", "guide"));
    content.appendChild(ce("h1", "page", "はじめに & 規約"));
    content.appendChild(ce("p", "sectlead", "ACS を読む前に押さえると一気に分かりやすくなる共通ルール。専門用語は<t>ホバー</t>で説明が出ます。"));
    content.appendChild(renderDocBlocks(REF.guide));
  }
  function renderDocBlocks(blocks) {
    var doc = ce("div", "doc");
    (blocks || []).forEach(function (b) {
      if (b.h2) doc.appendChild(ce("h2", null, esc(b.h2)));
      if (b.h3) doc.appendChild(ce("h3", null, esc(b.h3)));
      if (b.p) doc.appendChild(ce("p", null, b.p));
      if (b.note) doc.appendChild(ce("div", "note" + (b.kind ? " " + b.kind : ""), b.note));
      if (b.code) { var w = ce("div"); w.innerHTML = codeBlock(b.code); doc.appendChild(w.firstChild); }
      if (b.ul) { var ul = ce("ul"); b.ul.forEach(function (li) { ul.appendChild(ce("li", null, li)); }); doc.appendChild(ul); }
    });
    return doc;
  }

  function renderTrouble(content) {
    document.title = "トラブルシュート — ACS リファレンス";
    content.appendChild(ce("div", "kicker", "troubleshooting"));
    content.appendChild(ce("h1", "page", "トラブルシューティング"));
    content.appendChild(ce("p", "sectlead", "症状から引けるよくある詰まりどころと対処。上の検索でも引けます。"));
    var search = ce("input", "sfilter"); search.type = "search";
    search.placeholder = "症状で絞り込み (例: 真っ黒, リンク, クラッシュ, ビルド)…";
    search.style.cssText = "width:100%;max-width:560px;margin:6px 0 18px;padding:8px 11px";
    content.appendChild(search);
    var list = ce("div"); content.appendChild(list);
    function draw(q) {
      list.innerHTML = ""; var n = 0;
      (REF.troubleshooting || []).forEach(function (qa) {
        var hay = (qa.q + " " + stripTags(qa.a) + " " + (qa.tags || []).join(" ")).toLowerCase();
        if (q && hay.indexOf(q) < 0) return; n++;
        var item = ce("div", "qa");
        var qb = ce("button", "q", '<span class="qm">Q</span><span>' + esc(qa.q) + "</span>");
        var ab = ce("div", "a", qa.a + (qa.tags ? '<div class="tags">' + qa.tags.map(function (t) { return '<span class="tag">#' + esc(t) + "</span>"; }).join("") + "</div>" : ""));
        qb.addEventListener("click", function () { item.classList.toggle("open"); });
        item.appendChild(qb); item.appendChild(ab); list.appendChild(item);
      });
      if (!n) list.appendChild(ce("div", "empty", "該当なし。検索語を変えてみてください。"));
    }
    search.addEventListener("input", function () { draw(search.value.trim().toLowerCase()); });
    draw("");
  }

  function renderGlossary(content) {
    document.title = "用語集 — ACS リファレンス";
    content.appendChild(ce("div", "kicker", "glossary"));
    content.appendChild(ce("h1", "page", "用語集"));
    var keys = Object.keys(REF.glossary).sort(function (a, b) { return a.localeCompare(b, "ja"); });
    content.appendChild(ce("p", "sectlead", "本文中の<t>点線の語</t>はここに対応。" + keys.length + " 語。"));
    var search = ce("input", "sfilter"); search.type = "search"; search.placeholder = "用語を絞り込み…";
    search.style.cssText = "width:100%;max-width:560px;margin:6px 0 18px;padding:8px 11px";
    content.appendChild(search);
    var gl = ce("div", "glist"); content.appendChild(gl);
    function draw(q) {
      gl.innerHTML = "";
      keys.forEach(function (k) {
        if (q && (k + " " + stripTags(REF.glossary[k])).toLowerCase().indexOf(q) < 0) return;
        gl.appendChild(ce("div", "gentry", '<div class="gt">' + esc(k) + '</div><div class="gd">' + REF.glossary[k] + "</div>"));
      });
    }
    search.addEventListener("input", function () { draw(search.value.trim().toLowerCase()); });
    draw("");
  }

  function moduleOfId(id) { for (var i = 0; i < NAV.groups.length; i++) { var it = NAV.groups[i].items.filter(function (x) { return x.id === id; })[0]; if (it) return it; } return null; }

  function renderSearch(content) {
    var q = (new URLSearchParams(location.search).get("q") || "").trim();
    document.title = (q ? "検索: " + q : "検索") + " — ACS リファレンス";
    content.appendChild(ce("div", "kicker", "search"));
    content.appendChild(ce("h1", "page", q ? "検索: " + esc(q) : "検索"));
    var box = $("#searchbox"); if (box) box.value = q;
    if (!q) { content.appendChild(ce("p", "empty", "上の検索窓にキーワードを入れて Enter。")); return; }
    var lq = q.toLowerCase(), res = [];
    REF.modules.forEach(function (m) {
      var it = moduleOfId(m.id); var page = it ? it.page : null;
      (m.types || []).forEach(function (t) {
        var hay = (stripTags(t.name) + " " + stripTags(t.summary || "") + " " + stripTags(t.when || "")).toLowerCase();
        if (hay.indexOf(lq) >= 0) res.push({ page: page, anchor: (t.__anchor || slug(t.name)), name: stripTags(t.name), ctx: stripTags(t.summary || ""), where: m.id });
        (t.members || []).forEach(function (mb) {
          if ((stripTags(mb.sig || "") + " " + stripTags(mb.desc || "")).toLowerCase().indexOf(lq) >= 0)
            res.push({ page: page, anchor: (t.__anchor || slug(t.name)), name: stripTags(t.name), member: stripTags(mb.sig || ""), ctx: stripTags(mb.desc || ""), where: m.id + " / " + stripTags(t.name) });
        });
      });
    });
    (REF.troubleshooting || []).forEach(function (qa) {
      if ((qa.q + " " + stripTags(qa.a)).toLowerCase().indexOf(lq) >= 0)
        res.push({ page: "troubleshooting.html", anchor: "", name: qa.q, ctx: stripTags(qa.a).slice(0, 160), where: "トラブルシュート" });
    });
    if (!res.length) { content.appendChild(ce("p", "empty", "ヒットなし。別の語で試してください。")); return; }
    content.appendChild(ce("p", "blurb", res.length + " 件"));
    res.slice(0, 300).forEach(function (r) {
      var href = r.page ? (r.page + (r.anchor ? "#" + r.anchor : "")) : "#";
      var label = dispCode(r.name) + (r.member ? ' <span class="mono" style="color:var(--green)">:: ' + dispCode(r.member) + "</span>" : "");
      content.appendChild(ce("div", "sr",
        '<a href="' + href + '">' + label + "</a>" +
        '<div class="where">' + esc(r.where) + "</div>" +
        (r.ctx ? '<div class="ctx">' + esc(r.ctx) + "</div>" : "")));
    });
  }

  // =========================================================================
  // 対話: ツールチップ / コピー / 検索送信 / スクロール追従
  // =========================================================================
  var tip;
  function showTip(el) {
    var key = el.getAttribute("data-term") || el.textContent.trim();
    var html;
    var ti = TYPEINDEX[key];
    if (ti && ti.summary) {
      // 文書化済みの型 → その型“自身”の概要を出す（＝その場に合った説明）
      var s = ti.summary;
      if (s.length > 240) { var cut = s.lastIndexOf("。", 240); s = (cut > 60 ? s.slice(0, cut + 1) : s.slice(0, 220)) + "…"; }
      html = s + '<div class="tmore">' + (ti.kind ? esc(ti.kind) + " · " : "") + esc(ti.modId) + " モジュール</div>";
    } else {
      var def = REF.glossary[key]; if (!def) return;   // 概念用語 → 用語集
      html = def;
    }
    if (!tip) { tip = ce("div", "tip"); document.body.appendChild(tip); }
    tip.innerHTML = '<span class="tt">' + esc(key) + "</span>" + html;
    tip.style.display = "block";
    var r = el.getBoundingClientRect();
    var top = r.bottom + 8, left = Math.min(r.left, window.innerWidth - 356);
    if (top + tip.offsetHeight > window.innerHeight) top = r.top - tip.offsetHeight - 8;
    tip.style.top = Math.max(8, top) + "px"; tip.style.left = Math.max(8, left) + "px";
  }
  function hideTip() { if (tip) tip.style.display = "none"; }

  function wireInteractions() {
    document.addEventListener("mouseover", function (e) { var el = e.target.closest && e.target.closest("t,.term"); if (el) showTip(el); });
    document.addEventListener("mouseout", function (e) { if (e.target.closest && e.target.closest("t,.term")) hideTip(); });
    document.addEventListener("click", function (e) {
      var btn = e.target.closest && e.target.closest(".copy");
      if (btn) {
        var src = btn.parentNode.querySelector(".src");
        var txt = src ? src.innerText : "";
        navigator.clipboard && navigator.clipboard.writeText(txt);
        btn.textContent = "copied"; btn.classList.add("done");
        setTimeout(function () { btn.textContent = "copy"; btn.classList.remove("done"); }, 1200);
      }
    });
    var box = $("#searchbox");
    if (box) box.addEventListener("keydown", function (e) {
      if (e.key === "Enter") { var v = box.value.trim(); if (v) location.href = "search.html?q=" + encodeURIComponent(v); }
    });
    document.addEventListener("keydown", function (e) {
      if (e.key === "/" && document.activeElement !== box && !/input|textarea/i.test((document.activeElement || {}).tagName || "")) {
        e.preventDefault(); if (box) box.focus();
      }
    });
  }

  // 現在表示中の型をサイドバーで強調（スクロール追従）
  function wireScrollSpy(aside) {
    var anchors = [].slice.call(aside.querySelectorAll(".types a"));
    if (!anchors.length) return;
    var targets = anchors.map(function (a) { return document.getElementById(a.dataset.anchor); }).filter(Boolean);
    function spy() {
      var cur = null, mid = 120;
      for (var i = 0; i < targets.length; i++) { if (targets[i].getBoundingClientRect().top <= mid) cur = targets[i].id; }
      anchors.forEach(function (a) { a.classList.toggle("cur", a.dataset.anchor === cur); });
    }
    window.addEventListener("scroll", spy, { passive: true }); spy();
  }

  // =========================================================================
  // 起動
  // =========================================================================
  function boot() {
    var page = document.body.dataset.page || "module";
    var modId = document.body.dataset.module || "";
    document.body.appendChild(buildHeader(page === "module" ? null : page));

    var wrap = ce("div", "wrap");
    var aside = buildSidebar(page === "module" ? modId : null);
    var content = ce("main", "content");
    wrap.appendChild(aside); wrap.appendChild(content);
    document.body.appendChild(wrap);

    var extra = "";
    if (page === "module") { var m = renderModule(content, modId); if (m) extra = (m.types ? m.types.length : 0) + " types on this page"; }
    else if (page === "guide") renderGuide(content);
    else if (page === "trouble") renderTrouble(content);
    else if (page === "glossary") renderGlossary(content);
    else if (page === "search") renderSearch(content);
    else if (page === "home") renderHome(content);

    document.body.appendChild(buildFooter(extra));
    wireInteractions();
    if (page === "module") wireScrollSpy(aside);
    // ページ内アンカーへ（リロード/ディープリンク時は即着地。scroll-margin-top で補正）
    if (location.hash) {
      var t = document.getElementById(decodeURIComponent(location.hash.slice(1)));
      if (t) t.scrollIntoView({ block: "start", behavior: "instant" });
    }
  }

  // index（ホーム）
  function renderHome(content) {
    document.title = "ACS リファレンス";
    var allTypes = REF.modules.reduce(function (a, m) { return a + (m.types ? m.types.length : 0); }, 0);
    var mods = NAV.groups.reduce(function (a, g) { return a + g.items.length; }, 0);
    var hero = ce("div", "hero");
    hero.innerHTML =
      '<div class="kicker">ACS C++ game framework</div>' +
      "<h1>ACS リファレンス</h1>" +
      '<p class="lead">日本のインディー開発者向けの軽量 C++ ゲームフレームワーク。Windows / <t>DirectX 12</t> 上で動き、<t>STL</t> を使わず独自の小さな部品で構成されています。全クラス・全機能を手書きで解説します。</p>' +
      '<div class="stat"><b>' + mods + "</b> modules · <b>" + allTypes + "</b> types · <b>" + Object.keys(REF.glossary).length + "</b> 用語</div>";
    content.appendChild(hero);

    content.appendChild(ce("div", "secthead", "まず読む"));
    var quick = ce("div", "modgrid");
    [["guide.html", "はじめに & 規約", "名前空間・命名・Result・Y-down・ビルドの共通ルール"],
     ["easy.html", "easy で最短ゲーム", "クラス継承なし、関数呼び出しだけで 2D ゲームを書く"],
     ["troubleshooting.html", "トラブルシュート", "真っ黒・クラッシュ・リンクエラーなど症状から引く"],
     ["glossary.html", "用語集", "本文の専門用語をまとめて確認"]].forEach(function (q) {
      var a = ce("a", "modcard"); a.href = q[0];
      a.innerHTML = '<div class="mh">' + esc(q[1]) + '</div><div class="md">' + esc(q[2]) + "</div>";
      quick.appendChild(a);
    });
    content.appendChild(quick);

    NAV.groups.forEach(function (g) {
      content.appendChild(ce("div", "secthead", g.label));
      var grid = ce("div", "modgrid");
      g.items.forEach(function (it) {
        var mod = REF.modules.filter(function (m) { return m.id === it.id; })[0];
        var blurb = mod ? stripTags(mod.blurb || "").slice(0, 90) : "";
        var tc = mod && mod.types ? mod.types.length : 0;
        var a = ce("a", "modcard"); a.href = it.page;
        a.innerHTML = '<div class="mh">' + esc(it.label) + '</div><div class="md">' + esc(blurb) + (blurb ? "…" : "") +
          '</div><div class="mm">' + tc + " types</div>";
        grid.appendChild(a);
      });
      content.appendChild(grid);
    });
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot); else boot();
})();
