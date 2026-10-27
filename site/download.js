// Recommends a download: the file card ([data-asset]) that suits the
// visitor's system is copied into the recommendation box. The cards' links,
// names and sizes were filled in from the latest release when the site was
// built (tools/fill-download-pages.py), so nothing is fetched here.
(function () {
    "use strict";

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

    // `architecture` is "x86" or "arm" where the browser tells it
    // (Chromium), null elsewhere. Windows and Linux builds are for x86-64
    // only; a Mac is taken for Apple silicon unless it is said to be Intel.
    function recommend(key, architecture) {
        var note = null;
        if (key === "macos-dmg") {
            if (architecture === "x86") {
                key = "macos-intel-dmg";
            } else if (architecture !== "arm") {
                note = "arch-note-mac";
            }
        } else if (key && architecture === "arm") {
            note = key === "windows-installer" ? "arch-note-arm-windows" : "arch-note-arm-linux";
        }
        showRecommendation(key);
        var element = note && document.getElementById(note);
        if (element) {
            element.hidden = false;
        }
    }

    var key = detectAsset();
    if (key && navigator.userAgentData && navigator.userAgentData.getHighEntropyValues) {
        navigator.userAgentData.getHighEntropyValues(["architecture"]).then(function (values) {
            recommend(key, values.architecture || null);
        }, function () {
            recommend(key, null);
        });
    } else {
        recommend(key, null);
    }
})();
