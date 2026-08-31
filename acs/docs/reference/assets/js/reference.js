// SPDX-License-Identifier: MIT

(function () {
  "use strict";

  var root = document.documentElement;
  root.classList.add("js");
  var themeButton = document.querySelector("[data-theme-toggle]");
  var storedTheme = null;
  try {
    storedTheme = window.localStorage.getItem("acs-reference-theme");
  } catch (_) {
    storedTheme = null;
  }

  function applyTheme(theme) {
    if (theme === "light" || theme === "dark") {
      root.dataset.theme = theme;
    } else {
      root.removeAttribute("data-theme");
      theme = "system";
    }
    if (themeButton) {
      var labels = {
        system: "配色: 端末設定。押すと明るい配色へ切り替えます",
        light: "配色: 明るい。押すと暗い配色へ切り替えます",
        dark: "配色: 暗い。押すと端末設定へ戻します",
      };
      var marks = { system: "◐", light: "☀", dark: "☾" };
      themeButton.setAttribute("aria-label", labels[theme]);
      themeButton.title = labels[theme];
      themeButton.textContent = marks[theme];
      themeButton.dataset.themeState = theme;
    }
  }

  applyTheme(storedTheme || "system");
  if (themeButton) {
    themeButton.addEventListener("click", function () {
      var current = themeButton.dataset.themeState || "system";
      var next = current === "system" ? "light" : current === "light" ? "dark" : "system";
      try {
        if (next === "system") {
          window.localStorage.removeItem("acs-reference-theme");
        } else {
          window.localStorage.setItem("acs-reference-theme", next);
        }
      } catch (_) {
        // 保存できない環境でも、そのページ内の切り替えは維持する。
      }
      applyTheme(next);
    });
  }

  var navToggle = document.querySelector("[data-nav-toggle]");
  var drawer = document.querySelector("[data-mobile-drawer]");
  var backdrop = document.querySelector("[data-drawer-backdrop]");
  var siteHeader = document.querySelector(".site-header");
  var pageLayout = document.querySelector(".layout");
  var lastFocused = null;

  function drawerFocusable() {
    if (!drawer) return [];
    return Array.prototype.slice.call(
      drawer.querySelectorAll('a[href], button:not([disabled]), input:not([disabled]), [tabindex]:not([tabindex="-1"])'),
    ).filter(function (element) {
      return !element.hidden && element.getClientRects().length > 0;
    });
  }

  function setPageInert(value) {
    [siteHeader, pageLayout].forEach(function (element) {
      if (!element) return;
      if (value) element.setAttribute("inert", "");
      else element.removeAttribute("inert");
    });
  }

  function openDrawer() {
    if (!navToggle || !drawer || !backdrop) return;
    lastFocused = document.activeElement;
    drawer.hidden = false;
    backdrop.hidden = false;
    drawer.removeAttribute("inert");
    navToggle.setAttribute("aria-expanded", "true");
    navToggle.setAttribute("aria-label", "ナビゲーションを閉じる");
    setPageInert(true);
    document.body.classList.add("nav-open");
    var focusable = drawerFocusable();
    if (focusable.length) focusable[0].focus();
  }

  function closeDrawer(restoreFocus) {
    if (!navToggle || !drawer || !backdrop) return;
    drawer.setAttribute("inert", "");
    navToggle.setAttribute("aria-expanded", "false");
    navToggle.setAttribute("aria-label", "ナビゲーションを開く");
    setPageInert(false);
    drawer.hidden = true;
    backdrop.hidden = true;
    document.body.classList.remove("nav-open");
    if (restoreFocus && lastFocused && typeof lastFocused.focus === "function") {
      lastFocused.focus();
    }
  }

  if (navToggle && drawer && backdrop) {
    navToggle.addEventListener("click", function () {
      if (drawer.hidden) openDrawer(); else closeDrawer(true);
    });
    backdrop.addEventListener("click", function () { closeDrawer(true); });
    drawer.addEventListener("click", function (event) {
      if (event.target.closest("[data-nav-close]")) {
        closeDrawer(true);
        return;
      }
      if (event.target.closest("a[href]")) closeDrawer(false);
    });
    drawer.addEventListener("keydown", function (event) {
      if (event.key !== "Tab") return;
      var focusable = drawerFocusable();
      if (!focusable.length) return;
      var first = focusable[0];
      var last = focusable[focusable.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    });
    var desktopNavigation = window.matchMedia("(min-width: 1200px)");
    var closeDrawerForDesktop = function (event) {
      if (event.matches && !drawer.hidden) {
        var focusWasInsideDrawer = drawer.contains(document.activeElement);
        closeDrawer(false);
        if (focusWasInsideDrawer) {
          var visibleFallback = document.querySelector(".site-header .brand");
          if (visibleFallback) visibleFallback.focus();
        }
      }
    };
    if (typeof desktopNavigation.addEventListener === "function") {
      desktopNavigation.addEventListener("change", closeDrawerForDesktop);
    } else if (typeof desktopNavigation.addListener === "function") {
      desktopNavigation.addListener(closeDrawerForDesktop);
    }
  }

  function setTermOpen(wrapper, open, pinned) {
    if (!wrapper) return;
    wrapper.classList.toggle("is-open", open);
    var button = wrapper.querySelector(".term-trigger");
    if (!button) return;
    button.dataset.termPinned = pinned ? "true" : "false";
  }

  function closeTerms(except) {
    document.querySelectorAll(".term-wrap.is-open").forEach(function (wrapper) {
      if (wrapper === except) return;
      setTermOpen(wrapper, false, false);
    });
  }

  document.querySelectorAll(".term-trigger").forEach(function (button) {
    button.addEventListener("focus", function () {
      var wrapper = button.closest(".term-wrap");
      if (!wrapper || button.dataset.termPinned === "true") return;
      wrapper.classList.remove("is-suppressed");
      closeTerms(wrapper);
      setTermOpen(wrapper, true, false);
    });
    button.addEventListener("blur", function () {
      var wrapper = button.closest(".term-wrap");
      if (!wrapper || button.dataset.termPinned === "true") return;
      setTermOpen(wrapper, false, false);
    });
    button.addEventListener("click", function (event) {
      event.stopPropagation();
      var wrapper = button.closest(".term-wrap");
      if (!wrapper) return;
      wrapper.classList.remove("is-suppressed");
      var willOpen = button.dataset.termPinned !== "true";
      closeTerms(wrapper);
      setTermOpen(wrapper, willOpen, willOpen);
    });
    var wrapper = button.closest(".term-wrap");
    if (wrapper) {
      wrapper.addEventListener("pointerleave", function () {
        wrapper.classList.remove("is-suppressed");
      });
    }
  });

  document.addEventListener("click", function () { closeTerms(null); });
  document.addEventListener("keydown", function (event) {
    if (event.key === "Escape") {
      closeTerms(null);
      document.querySelectorAll(".term-wrap").forEach(function (wrapper) {
        if (wrapper.matches(":hover")) wrapper.classList.add("is-suppressed");
      });
      if (drawer && !drawer.hidden) closeDrawer(true);
    }
    if (event.key === "/" && !event.ctrlKey && !event.metaKey && !event.altKey) {
      var active = document.activeElement;
      var editing = active && /^(INPUT|TEXTAREA|SELECT)$/.test(active.tagName);
      if (!editing) {
        var search = document.querySelector("[data-reference-search-input]") ||
          document.querySelector('.site-search input[name="q"]');
        if (search) {
          event.preventDefault();
          search.focus();
        }
      }
    }
  });

  function normalize(value) {
    return String(value || "")
      .normalize("NFKC")
      .toLocaleLowerCase("ja")
      .replace(/\s+/g, " ")
      .trim();
  }

  function escapeHtml(value) {
    return String(value || "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#39;");
  }

  function expandRecord(record) {
    if (!Array.isArray(record)) return record;
    var contexts = Array.isArray(window.ACS_SEARCH_CONTEXTS) ? window.ACS_SEARCH_CONTEXTS : [];
    return {
      title: record[0] || "",
      qualified: record[1] || "",
      signature: record[2] || "",
      text: record[3] || "",
      context: contexts[record[4]] || "",
      url: record[5] || "",
    };
  }

  function normalizeRecord(compactRecord) {
    var record = expandRecord(compactRecord);
    return {
      record: record,
      title: normalize(record.title),
      qualified: normalize(record.qualified || ""),
      signature: normalize(record.signature || ""),
      body: normalize(record.text || ""),
      context: normalize(record.context || ""),
    };
  }

  function tokenFieldScore(value, token, weight) {
    if (value === token) return weight;
    if (value.indexOf(token) === 0) return weight + 2;
    if (value.indexOf(token) >= 0) return weight + 5;
    return Number.POSITIVE_INFINITY;
  }

  function scoreRecord(entry, tokens, query) {
    if (entry.title === query || entry.qualified === query) return 0;
    var score = 0;
    for (var index = 0; index < tokens.length; index += 1) {
      var token = tokens[index];
      var tokenScore = Math.min(
        tokenFieldScore(entry.title, token, 0),
        tokenFieldScore(entry.qualified, token, 4),
        tokenFieldScore(entry.signature, token, 8),
        tokenFieldScore(entry.context, token, 16),
        tokenFieldScore(entry.body, token, 24),
      );
      if (!Number.isFinite(tokenScore)) return Number.POSITIVE_INFINITY;
      score += tokenScore;
    }
    return score;
  }

  var searchForm = document.querySelector("[data-reference-search]");
  var searchInput = document.querySelector("[data-reference-search-input]");
  var searchStatus = document.querySelector("[data-search-status]");
  var searchResults = document.querySelector("[data-search-results]");
  var rawSearchIndex = Array.isArray(window.ACS_SEARCH_INDEX) ? window.ACS_SEARCH_INDEX : [];
  var normalizedSearchIndex = null;

  function ensureNormalizedSearchIndex() {
    if (!normalizedSearchIndex) normalizedSearchIndex = rawSearchIndex.map(normalizeRecord);
    return normalizedSearchIndex;
  }

  if (rawSearchIndex.length && "requestIdleCallback" in window) {
    window.requestIdleCallback(ensureNormalizedSearchIndex, { timeout: 1200 });
  }

  function renderSearch(queryValue) {
    if (!searchStatus || !searchResults) return;
    var query = normalize(queryValue);
    searchResults.replaceChildren();
    if (!query) {
      searchStatus.textContent = "型名、関数名、機能名、用語、症状を入力してください。";
      return;
    }
    var tokens = query.split(" ").filter(Boolean);
    var matches = [];
    var searchIndex = ensureNormalizedSearchIndex();
    searchIndex.forEach(function (entry, order) {
      var score = scoreRecord(entry, tokens, query);
      if (Number.isFinite(score)) matches.push({ entry: entry, score: score, order: order });
    });
    matches.sort(function (left, right) {
      return left.score - right.score ||
        left.entry.title.localeCompare(right.entry.title, "ja") ||
        left.order - right.order;
    });

    var batchSize = 100;
    var shownCount = 0;

    function appendBatch() {
      var previousButton = searchResults.querySelector(".search-more");
      var restoreMoreFocus = previousButton && document.activeElement === previousButton;
      if (previousButton) previousButton.remove();
      var nextCount = Math.min(matches.length, shownCount + batchSize);
      var firstAdded = null;
      matches.slice(shownCount, nextCount).forEach(function (entry) {
        var record = entry.entry.record;
        var link = document.createElement("a");
        link.className = "result-card";
        link.href = record.url;
        link.innerHTML =
          '<span><span class="identifier">' + escapeHtml(record.qualified || record.title) + '</span>' +
          (record.signature ? '<span class="signature-summary">' + escapeHtml(record.signature) + '</span>' : "") +
          '<span class="summary">' + escapeHtml(record.text) + '</span></span>' +
          '<span class="search-context">' + escapeHtml(record.context) + "</span>";
        searchResults.appendChild(link);
        if (!firstAdded) firstAdded = link;
      });
      shownCount = nextCount;
      searchStatus.textContent = shownCount < matches.length
        ? `全 ${matches.length} 件中 ${shownCount} 件を表示しています。`
        : `${matches.length} 件見つかりました。`;
      if (shownCount < matches.length) {
        var moreButton = document.createElement("button");
        moreButton.className = "search-more";
        moreButton.type = "button";
        moreButton.textContent = `さらに ${Math.min(batchSize, matches.length - shownCount)} 件を表示`;
        moreButton.addEventListener("click", appendBatch);
        searchResults.appendChild(moreButton);
        if (restoreMoreFocus) moreButton.focus();
      } else if (restoreMoreFocus && firstAdded) {
        firstAdded.focus();
      }
    }

    appendBatch();
  }

  if (searchForm && searchInput) {
    var searchTimer = null;
    var queryParameters = new URLSearchParams(window.location.search);
    var initialQuery = queryParameters.get("q") || "";
    searchInput.value = initialQuery;
    renderSearch(initialQuery);
    searchForm.addEventListener("submit", function (event) {
      event.preventDefault();
      var query = searchInput.value;
      var url = new URL(window.location.href);
      if (query) url.searchParams.set("q", query); else url.searchParams.delete("q");
      window.history.replaceState(null, "", url);
      renderSearch(query);
    });
    searchInput.addEventListener("input", function () {
      window.clearTimeout(searchTimer);
      searchTimer = window.setTimeout(function () { renderSearch(searchInput.value); }, 120);
    });
  }
})();
