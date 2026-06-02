/* ACS リファレンス — レンダラ (依存なし)。
   データは window.ACS_REF = { modules:[], glossary:{}, guide:[], troubleshooting:[] } から読む。
   各 data/<module>.js が ACS_REF.modules.push({...}) と Object.assign(ACS_REF.glossary,{...}) を行う。 */
(function () {
  "use strict";
  var REF = window.ACS_REF || { modules: [], glossary: {}, guide: [], troubleshooting: [] };
  // 同じ id のモジュールを 1 つに統合(分割生成された gameframework 等を 1 セクションに)
  (function () {
    var byId = {}, merged = [];
    REF.modules.forEach(function (m) {
      if (byId[m.id]) byId[m.id].types = (byId[m.id].types || []).concat(m.types || []);
      else { byId[m.id] = m; merged.push(m); }
    });
    REF.modules = merged;
  })();
  REF.modules.sort(function (a, b) { return (a.order || 0) - (b.order || 0) || a.title.localeCompare(b.title); });

  // 全 type を平坦化（検索・ルーティング用）
  var ALL = [];
  REF.modules.forEach(function (m) { (m.types || []).forEach(function (t) { ALL.push({ mod: m, type: t }); }); });

  var $ = function (sel, el) { return (el || document).querySelector(sel); };
  var ce = function (tag, cls, html) { var e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; return e; };
  function stripTags(s) { return (s || "").replace(/<[^>]+>/g, ""); }
  function esc(s) { return (s || "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }

  // コードの簡易ハイライト（既にエスケープ済み文字列に対し、コメントだけ色付け）
  function hl(code) {
    return (code || "").split("\n").map(function (ln) {
      var i = ln.indexOf("//");
      if (i >= 0) return ln.slice(0, i) + '<span class="cm">' + ln.slice(i) + "</span>";
      return ln;
    }).join("\n");
  }

  // ---- サイドバー ----
  var sideEl, contentEl;
  function buildSidebar() {
    sideEl.innerHTML = "";
    var filt = ce("input", "filter"); filt.placeholder = "モジュール内を絞り込み…";
    filt.addEventListener("input", function () { applyFilter(filt.value.trim().toLowerCase()); });
    sideEl.appendChild(filt);
    REF.modules.forEach(function (m) {
      var mod = ce("div", "mod"); mod.dataset.id = m.id;
      var name = ce("button", "modname", esc(m.title) + '<span class="cnt">' + (m.types ? m.types.length : 0) + "</span>");
      name.addEventListener("click", function () { mod.classList.toggle("open"); });
      mod.appendChild(name);
      var types = ce("div", "types");
      (m.types || []).forEach(function (t) {
        var a = ce("a", null, esc(stripTags(t.name))); a.href = "#/" + m.id + "/" + encodeURIComponent(stripTags(t.name));
        a.dataset.key = (m.id + " " + stripTags(t.name) + " " + stripTags(t.summary || "")).toLowerCase();
        types.appendChild(a);
      });
      mod.appendChild(types);
      sideEl.appendChild(mod);
    });
  }
  function applyFilter(q) {
    sideEl.querySelectorAll(".mod").forEach(function (mod) {
      var any = false;
      mod.querySelectorAll(".types a").forEach(function (a) {
        var hit = !q || a.dataset.key.indexOf(q) >= 0;
        a.style.display = hit ? "" : "none"; if (hit) any = true;
      });
      mod.style.display = any ? "" : "none";
      if (q) mod.classList.add("open");
    });
  }

  // ---- type ページ ----
  function memberEl(mb) {
    var wrap = ce("div", "member");
    var sig = ce("button", "m-sig",
      '<span class="caret">▶</span><span class="sig">' + (mb.sig || "") + "</span>" +
      (mb.ret ? '<span class="ret">→ ' + esc(stripTags(mb.ret)) + "</span>" : ""));
    var body = ce("div", "m-body");
    var h = '<div class="desc">' + (mb.desc || "") + "</div>";
    if (mb.sample) h += '<pre class="code">' + hl(mb.sample) + "</pre>";
    if (mb.when) h += '<div class="when"><b>使う場面:</b> ' + mb.when + "</div>";
    body.innerHTML = h;
    sig.addEventListener("click", function () { wrap.classList.toggle("open"); });
    wrap.appendChild(sig); wrap.appendChild(body);
    return wrap;
  }
  function typeEl(mod, t) {
    var card = ce("div", "type");
    var head = ce("div", "t-head",
      '<div class="tname">' + esc(stripTags(t.name)) + "</div>" +
      '<div class="tmeta">' + (t.kind ? esc(t.kind) : "") +
      (t.header ? ' · <span class="hp">' + esc(t.header) + "</span>" : "") + "</div>");
    card.appendChild(head);
    var body = ce("div", "t-body");
    var h = "";
    if (t.summary) h += '<div class="kv"><div class="k">概要</div><div class="vv">' + t.summary + "</div></div>";
    if (t.when) h += '<div class="kv"><div class="k">こういう時に使う</div><div class="vv">' + t.when + "</div></div>";
    if (t.sample) h += '<div class="kv"><div class="k">サンプル</div><pre class="code">' + hl(t.sample) + "</pre></div>";
    body.innerHTML = h;
    if (t.members && t.members.length) {
      var hd = ce("div", "kv", '<div class="k">メンバー (' + t.members.length + ")</div>");
      body.appendChild(hd);
      var ms = ce("div", "members");
      t.members.forEach(function (mb) { ms.appendChild(memberEl(mb)); });
      body.appendChild(ms);
    }
    card.appendChild(body);
    return card;
  }

  // ---- ルーティング ----
  function setActiveNav(name) {
    document.querySelectorAll("header.top nav button").forEach(function (b) {
      b.classList.toggle("active", b.dataset.route === name);
    });
  }
  function markCurType(modId, typeName) {
    sideEl.querySelectorAll(".types a").forEach(function (a) { a.classList.remove("cur"); });
    var mod = sideEl.querySelector('.mod[data-id="' + modId + '"]');
    if (mod) {
      mod.classList.add("open");
      mod.querySelectorAll(".types a").forEach(function (a) {
        if (decodeURIComponent(a.href.split("/").pop()) === typeName) { a.classList.add("cur"); a.scrollIntoView({ block: "nearest" }); }
      });
    }
  }

  function route() {
    var hash = location.hash.replace(/^#\/?/, "");
    var parts = hash.split("/").map(decodeURIComponent);
    window.scrollTo(0, 0);
    if (!hash || parts[0] === "guide") return renderGuide();
    if (parts[0] === "trouble") return renderTrouble();
    if (parts[0] === "glossary") return renderGlossary();
    if (parts[0] === "search") return renderSearch(parts[1] || "");
    // module/type
    var modId = parts[0], typeName = parts[1];
    var mod = REF.modules.filter(function (m) { return m.id === modId; })[0];
    if (!mod) return renderGuide();
    setActiveNav("ref");
    contentEl.innerHTML = "";
    contentEl.appendChild(ce("div", "crumb", "リファレンス / " + esc(mod.title)));
    if (!typeName) {
      contentEl.appendChild(ce("h1", "page", esc(mod.title)));
      if (mod.blurb) contentEl.appendChild(ce("p", "blurb", mod.blurb));
      (mod.types || []).forEach(function (t) { contentEl.appendChild(typeEl(mod, t)); });
    } else {
      var t = (mod.types || []).filter(function (x) { return stripTags(x.name) === typeName; })[0];
      if (!t) { contentEl.appendChild(ce("p", "empty", "見つかりませんでした。")); return; }
      contentEl.appendChild(typeEl(mod, t));
      markCurType(modId, typeName);
    }
  }

  // ---- ガイド / トラブルシュート / 用語集 ----
  function renderDocBlocks(blocks) {
    var doc = ce("div", "doc");
    (blocks || []).forEach(function (b) {
      if (b.h2) doc.appendChild(ce("h2", null, esc(b.h2)));
      if (b.h3) doc.appendChild(ce("h3", null, esc(b.h3)));
      if (b.p) doc.appendChild(ce("p", null, b.p));
      if (b.note) doc.appendChild(ce("div", "note" + (b.kind ? " " + b.kind : ""), b.note));
      if (b.code) doc.appendChild(ce("pre", "code", hl(b.code)));
      if (b.ul) { var ul = ce("ul"); b.ul.forEach(function (li) { ul.appendChild(ce("li", null, li)); }); doc.appendChild(ul); }
    });
    return doc;
  }
  function renderGuide() {
    setActiveNav("guide"); contentEl.innerHTML = "";
    contentEl.appendChild(ce("h1", "page", "はじめに & 規約"));
    contentEl.appendChild(ce("p", "sectlead", "ACS を読む前に押さえると一気に分かりやすくなる共通ルール。専門用語は<t>ホバー</t>で説明が出ます。"));
    contentEl.appendChild(renderDocBlocks(REF.guide));
  }
  function renderTrouble() {
    setActiveNav("trouble"); contentEl.innerHTML = "";
    contentEl.appendChild(ce("h1", "page", "トラブルシューティング"));
    contentEl.appendChild(ce("p", "sectlead", "症状から引けるよくある詰まりどころと対処。上の検索でも引けます。"));
    var box = ce("div");
    var search = ce("input", "filter"); search.placeholder = "症状で絞り込み (例: リンク, クラッシュ, 真っ黒, ビルド)…"; search.style.maxWidth = "520px"; search.style.margin = "8px 0 16px";
    box.appendChild(search);
    var list = ce("div");
    function draw(q) {
      list.innerHTML = "";
      var n = 0;
      (REF.troubleshooting || []).forEach(function (qa) {
        var hay = (qa.q + " " + stripTags(qa.a) + " " + (qa.tags || []).join(" ")).toLowerCase();
        if (q && hay.indexOf(q) < 0) return;
        n++;
        var item = ce("div", "qa");
        var qb = ce("button", "q", '<span class="qm">Q.</span><span>' + esc(qa.q) + "</span>");
        var ab = ce("div", "a", qa.a + (qa.tags ? '<div class="tags">' + qa.tags.map(function (t) { return '<span class="tag">#' + esc(t) + "</span>"; }).join("") + "</div>" : ""));
        qb.addEventListener("click", function () { item.classList.toggle("open"); });
        item.appendChild(qb); item.appendChild(ab); list.appendChild(item);
      });
      if (!n) list.appendChild(ce("div", "empty", "該当なし。検索を変えてみてください。"));
    }
    search.addEventListener("input", function () { draw(search.value.trim().toLowerCase()); });
    draw(""); box.appendChild(list);
    contentEl.appendChild(box);
  }
  function renderGlossary() {
    setActiveNav("glossary"); contentEl.innerHTML = "";
    contentEl.appendChild(ce("h1", "page", "用語集"));
    contentEl.appendChild(ce("p", "sectlead", "本文中の<t>点線の語</t>はここに対応。" + Object.keys(REF.glossary).length + " 語。"));
    var keys = Object.keys(REF.glossary).sort(function (a, b) { return a.localeCompare(b, "ja"); });
    var gl = ce("div", "glist");
    keys.forEach(function (k) {
      gl.appendChild(ce("div", "gentry", '<div class="gt">' + esc(k) + '</div><div class="gd">' + REF.glossary[k] + "</div>"));
    });
    contentEl.appendChild(gl);
  }

  // ---- 検索 ----
  function renderSearch(q) {
    setActiveNav(null); contentEl.innerHTML = "";
    q = (q || "").trim();
    contentEl.appendChild(ce("h1", "page", "検索: " + esc(q)));
    if (!q) { contentEl.appendChild(ce("p", "empty", "キーワードを入力してください。")); return; }
    var lq = q.toLowerCase(), res = [];
    ALL.forEach(function (e) {
      var t = e.type;
      var hay = (stripTags(t.name) + " " + stripTags(t.summary || "") + " " + stripTags(t.when || "")).toLowerCase();
      if (hay.indexOf(lq) >= 0) res.push({ mod: e.mod, name: stripTags(t.name), ctx: stripTags(t.summary || ""), where: e.mod.title });
      (t.members || []).forEach(function (mb) {
        var mh = (stripTags(mb.sig || "") + " " + stripTags(mb.desc || "")).toLowerCase();
        if (mh.indexOf(lq) >= 0) res.push({ mod: e.mod, name: stripTags(t.name), member: stripTags(mb.sig || ""), ctx: stripTags(mb.desc || ""), where: e.mod.title + " / " + stripTags(t.name) });
      });
    });
    // トラブルシュートも検索
    (REF.troubleshooting || []).forEach(function (qa) {
      if ((qa.q + " " + stripTags(qa.a)).toLowerCase().indexOf(lq) >= 0)
        res.push({ trouble: true, name: qa.q, ctx: stripTags(qa.a).slice(0, 160), where: "トラブルシュート" });
    });
    if (!res.length) { contentEl.appendChild(ce("p", "empty", "ヒットなし。別の語で試してください。トラブルシュートや用語集も見てみてください。")); return; }
    contentEl.appendChild(ce("p", "blurb", res.length + " 件"));
    res.slice(0, 200).forEach(function (r) {
      var href = r.trouble ? "#/trouble" : "#/" + r.mod.id + "/" + encodeURIComponent(r.name);
      var label = esc(r.name) + (r.member ? ' <span class="mono" style="color:#8ef2b3">::' + esc(r.member) + "</span>" : "");
      var sr = ce("div", "sr",
        '<a href="' + href + '">' + label + "</a>" +
        '<div class="where">' + esc(r.where) + "</div>" +
        (r.ctx ? '<div class="ctx">' + esc(r.ctx) + "</div>" : ""));
      contentEl.appendChild(sr);
    });
  }

  // ---- ツールチップ (用語集ホバー) ----
  var tip;
  function showTip(el) {
    var key = el.getAttribute("data-term") || el.textContent.trim();
    var def = REF.glossary[key];
    if (!def) return;
    if (!tip) { tip = ce("div", "tip"); document.body.appendChild(tip); }
    tip.innerHTML = '<span class="tt">' + esc(key) + "</span>" + def;
    tip.style.display = "block";
    var r = el.getBoundingClientRect();
    var top = r.bottom + 8, left = Math.min(r.left, window.innerWidth - 360);
    if (top + tip.offsetHeight > window.innerHeight) top = r.top - tip.offsetHeight - 8;
    tip.style.top = Math.max(8, top) + "px"; tip.style.left = Math.max(8, left) + "px";
  }
  function hideTip() { if (tip) tip.style.display = "none"; }
  document.addEventListener("mouseover", function (e) {
    var el = e.target.closest && e.target.closest("t,.term"); if (el) showTip(el);
  });
  document.addEventListener("mouseout", function (e) {
    if (e.target.closest && e.target.closest("t,.term")) hideTip();
  });

  // ---- 起動 ----
  function boot() {
    sideEl = $("aside.side"); contentEl = $("main.content");
    buildSidebar();
    var box = $("#searchbox");
    box.addEventListener("keydown", function (e) {
      if (e.key === "Enter") { location.hash = "#/search/" + encodeURIComponent(box.value.trim()); }
    });
    document.querySelectorAll("header.top nav button").forEach(function (b) {
      b.addEventListener("click", function () {
        var r = b.dataset.route;
        location.hash = r === "ref" ? "#/" + (REF.modules[0] ? REF.modules[0].id : "") : "#/" + r;
      });
    });
    window.addEventListener("hashchange", route);
    route();
    // 統計をフッタへ
    var st = $("#stats");
    if (st) st.textContent = REF.modules.length + " モジュール · " + ALL.length + " 型 · " + Object.keys(REF.glossary).length + " 用語 · " + (REF.troubleshooting || []).length + " トラブル項目";
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot); else boot();
})();
