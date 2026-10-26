// Fills in the download page from the latest GitHub release: each file
// card ([data-asset]) gets the link, name and size of the asset its pattern
// matches, and the card that suits the visitor's system is copied into the
// recommendation box. Without JavaScript, or when the API cannot be
// reached (it allows 60 requests an hour per address), every link goes to
// the release page instead, so the page still works.
(function () {
    "use strict";

    var API = "https://api.github.com/repos/informationsea/tableskim/releases/latest";

    // Asset names as cmake/Packaging.cmake makes them.
    var PATTERNS = {
        "windows-installer": /^tableskim-[\d.]+-Windows-[^-]+\.exe$/,
        "windows-zip": /^tableskim-[\d.]+-Windows-[^-]+\.zip$/,
        "macos-dmg": /^tableskim-[\d.]+-Darwin-[^-]+\.dmg$/,
        "macos-zip": /^tableskim-[\d.]+-Darwin-[^-]+\.zip$/,
        "linux-deb": /^tableskim_[\d.]+_.+\.deb$/,
        "linux-rpm": /^tableskim-[\d.]+-.+\.rpm$/,
        "linux-zip": /^tableskim-[\d.]+-Linux-[^-]+\.zip$/,
        "linux-x11-zip": /^tableskim-[\d.]+-Linux-x11-[^-]+\.zip$/,
        "linux-tui-zip": /^tableskim-tui-[\d.]+-Linux-[^-]+\.zip$/
    };

    var page = document.documentElement;
    var lang = page.lang === "ja" ? "ja" : "en";

    function formatSize(bytes) {
        return (bytes / 1048576).toFixed(1) + " MB";
    }

    function formatDate(iso) {
        var date = new Date(iso);
        if (isNaN(date.getTime())) {
            return "";
        }
        return date.toLocaleDateString(lang === "ja" ? "ja-JP" : "en-US",
            { year: "numeric", month: "long", day: "numeric" });
    }

    // The asset that suits this system, or null (phones, unknown systems).
    function detectAsset() {
        var ua = navigator.userAgent || "";
        var platform = (navigator.userAgentData && navigator.userAgentData.platform) ||
            navigator.platform || "";
        if (/android|iphone|ipad|ipod/i.test(ua)) {
            return null;
        }
        // iPadOS reports itself as a Mac, but has touch points.
        if (/mac/i.test(platform) && navigator.maxTouchPoints > 1) {
            return null;
        }
        if (/win/i.test(platform) || /windows/i.test(ua)) {
            return "windows-installer";
        }
        if (/mac/i.test(platform) || /mac os x/i.test(ua)) {
            return "macos-dmg";
        }
        if (/linux|x11/i.test(platform) || /linux/i.test(ua)) {
            // Some browsers on Ubuntu name it; other distributions get the
            // ZIP, which runs on any of them.
            return /ubuntu/i.test(ua) ? "linux-deb" : "linux-zip";
        }
        return null;
    }

    function setText(selector, text) {
        var element = document.querySelector(selector);
        if (element) {
            element.textContent = text;
        }
    }

    function showRecommendation(key) {
        var box = document.getElementById("recommended");
        var card = key && document.querySelector('[data-asset="' + key + '"]');
        if (!box || !card) {
            return;
        }
        // Copied before it is marked, so the copy shows neither the badge
        // nor the highlight.
        box.querySelector(".recommended-card").appendChild(card.cloneNode(true));
        card.classList.add("recommended");
        box.hidden = false;
        var other = document.getElementById("no-recommendation");
        if (other) {
            other.hidden = true;
        }
    }

    function fillAssets(release) {
        var assets = release.assets || [];
        document.querySelectorAll("[data-asset]").forEach(function (card) {
            var pattern = PATTERNS[card.getAttribute("data-asset")];
            var asset = null;
            for (var i = 0; pattern && i < assets.length; i++) {
                if (pattern.test(assets[i].name)) {
                    asset = assets[i];
                    break;
                }
            }
            if (!asset) {
                card.classList.add("missing");
                return;
            }
            card.querySelector(".file-name").textContent = asset.name;
            card.querySelector(".file-size").textContent = formatSize(asset.size);
            var link = card.querySelector("a.button");
            link.href = asset.browser_download_url;
            link.setAttribute("download", "");
        });
        setText("#release-version", release.tag_name || "");
        setText("#release-date", formatDate(release.published_at));
        var notes = document.getElementById("release-notes");
        if (notes && release.html_url) {
            notes.href = release.html_url;
        }
        document.getElementById("release-info").hidden = false;
    }

    var key = detectAsset();

    fetch(API, { headers: { Accept: "application/vnd.github+json" } })
        .then(function (response) {
            if (!response.ok) {
                throw new Error("HTTP " + response.status);
            }
            return response.json();
        })
        .then(function (release) {
            fillAssets(release);
            showRecommendation(key);
        })
        .catch(function () {
            showRecommendation(key);
            var error = document.getElementById("release-error");
            if (error) {
                error.hidden = false;
            }
        });

    // Only Apple silicon and x86-64 Windows / Linux builds are made; say so
    // when the browser tells another architecture.
    if (navigator.userAgentData && navigator.userAgentData.getHighEntropyValues) {
        navigator.userAgentData.getHighEntropyValues(["architecture"]).then(function (values) {
            var note = null;
            if (key === "macos-dmg" && values.architecture === "x86") {
                note = "arch-note-intel-mac";
            } else if (key && key !== "macos-dmg" && values.architecture === "arm") {
                note = key === "windows-installer" ? "arch-note-arm-windows" : "arch-note-arm-linux";
            }
            var element = note && document.getElementById(note);
            if (element) {
                element.hidden = false;
            }
        }).catch(function () {});
    }
})();
