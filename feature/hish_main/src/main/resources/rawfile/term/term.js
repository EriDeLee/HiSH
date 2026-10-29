// Initialize xterm.js
// The Terminal instance is created lazily in initialize() (see createTerminal),
// because renderer options depend on native preferences (ligatures / italic).
var term = null;
var nativeSelection = null;

// Tracks the currently selected font family so user fonts can re-apply it
// once their FontFace finishes loading asynchronously.
var currentFontFamily = 'Default';

// Initialize Addons (constructed upfront; attached to term in createTerminal)
const fitAddon = new FitAddon.FitAddon();

// Initialize exports for ArkTS to call
window.exports = {};

// Clipboard bytes are decoded once; process output stays binary for xterm's streaming decoder.
const decoder = new TextDecoder('utf-8');

function setupImeGuard() {
    if (!term.textarea) {
        return;
    }
    const textarea = term.textarea;
    textarea.setAttribute('autocomplete', 'off');
    textarea.setAttribute('autocorrect', 'off');
    textarea.setAttribute('spellcheck', 'false');
    textarea.setAttribute('autocapitalize', 'off');
}

// Detects which monospace fonts are actually available in the WebView.
// Uses Canvas measureText: compares "CandidateFont, monospace" against
// pure "monospace" baseline. If a font doesn't exist, the browser falls
// back to monospace and the widths match → it gets filtered out.
function detectAvailableMonospaceFonts() {
    var testNarrow = 'iiiiilllll';
    var testWide = 'WWWWWmmmmm';
    var baseFont = 'monospace';

    // Comprehensive list of monospace fonts commonly available across
    // HarmonyOS / Android / Linux systems
    var candidates = [
        'Droid Sans Mono',
        'Noto Sans Mono',
        'Roboto Mono',
        'Source Code Pro',
        'Cascadia Code',
        'Fira Code',
        'JetBrains Mono',
        'Courier New',
        'Courier',
        'Consolas',
        'Menlo',
        'DejaVu Sans Mono',
        'Liberation Mono',
        'Ubuntu Mono',
        'Monaco',
    ];

    var canvas = document.createElement('canvas');
    var ctx = canvas.getContext('2d');
    var testFontSize = '16px';

    function measure(fontStack, text) {
        ctx.font = testFontSize + ' ' + fontStack;
        return ctx.measureText(text).width;
    }

    // Baseline with pure monospace (the browser's default monospace font)
    var monoNarrow = measure(baseFont, testNarrow);
    var monoWide = measure(baseFont, testWide);

    return candidates.filter(function(font) {
        // Measure with candidate + monospace fallback.
        // If the candidate font does NOT exist, the browser falls back
        // to monospace → same metrics as baseline → detected as unavailable.
        // If it DOES exist → different metrics → detected as available.
        var fontStack = '"' + font + '", ' + baseFont;
        var narrow = measure(fontStack, testNarrow);
        var wide = measure(fontStack, testWide);

        // Font is distinct from default monospace (it actually exists and differs)
        var narrowDiff = Math.abs(narrow - monoNarrow);
        var wideDiff = Math.abs(wide - monoWide);
        var isDistinct = narrowDiff > 0.5 || wideDiff > 0.5;

        // Font is monospace: narrow and wide chars have equal width
        // (1px tolerance for sub-pixel rendering differences)
        var monoDiff = Math.abs(narrow - wide);
        var isMonospace = monoDiff <= 1;

        return isDistinct && isMonospace;
    });
}

// Maps user-friendly font family name to CSS font-family stack.
// 'Default' uses the system monospace fallback chain;
// named fonts include a monospace fallback for safety.
function mapFontFamily(family) {
    if (!family || family === 'Default') {
        return 'monospace';
    }
    return JSON.stringify(family) + ', monospace';
}

let fitFrame = null;
let terminalObserver = null;
function scheduleFit() {
    if (fitFrame !== null) return;
    fitFrame = requestAnimationFrame(() => {
        fitFrame = null;
        const container = document.getElementById('terminal-container');
        if (!term || !container || container.clientWidth < 1 || container.clientHeight < 1) return;
        try { fitAddon.fit(); } catch (error) { console.warn('Terminal fit failed', error); }
    });
}

// Function to convert "binary string" (where charCode < 256) to Uint8Array
// ArkTS sends data as a string where each char is a byte.
function strToUint8Array(str) {
    const buf = new Uint8Array(str.length);
    for (let i = 0; i < str.length; i++) {
        buf[i] = str.charCodeAt(i);
    }
    return buf;
}

// Creates the xterm.js Terminal instance.
// NOTE: This xterm.js build ships with the DOM renderer only (no
// canvas/WebGL/rendererType option), so ligature and italic suppression
// are both handled purely via CSS classes on <body> — see term.css.
function createTerminal() {
    var ligatures = false;
    var italic = true;
    try {
        if (window.native && native.getLigatures) {
            ligatures = !!native.getLigatures();
        }
        if (window.native && native.getItalic) {
            italic = !!native.getItalic();
        }
    } catch (e) {
        console.warn('Failed to read renderer prefs', e);
    }

    if (!ligatures) {
        document.body.classList.add('ligatures-off');
    }
    if (!italic) {
        document.body.classList.add('italics-off');
    }

    var options = {
        convertEol: true, // Pipe mode has no PTY output discipline to supply CR.
        cursorBlink: false,
        allowProposedApi: true, // Needed for some addons
        allowTransparency: true, // User preference: Transparency supported
        fontFamily: mapFontFamily('Default'),
        fontSize: 14, // Default, will be overridden by native.getFontSize()
        theme: {
            background: 'transparent',
            foreground: HiSHAppearance.tokens.lightInk,
            cursor: HiSHAppearance.tokens.lightInk
        },
        screenReaderMode: false, // Disabled to fix touch scrolling issues (was conflicting with native selection)
        scrollback: 3000, // [Optimization] Limit scrollback to 3000 lines (Ring Buffer) to prevent memory overflow
    };

    term = new Terminal(options);
    term.loadAddon(fitAddon);

    // Link clicks
    term.options.linkHandler = {
        activate: (e, text) => {
            if (native && native.openLink) native.openLink(text);
        }
    };

    // Setup global term object for debugging if needed
    window.term = term;
}

// Main initialization function
window.onload = function () {
    // Retry mechanism for native object injection
    let retryCount = 0;
    const maxRetries = 10;

    function initialize() {
        // Check if native is ready
        if (!window.native && retryCount < maxRetries) {
            console.log(`Waiting for native object... (${retryCount + 1}/${maxRetries})`);
            retryCount++;
            setTimeout(initialize, 200);
            return;
        }

        // Initial fit with a slight delay to ensure container is ready
        requestAnimationFrame(() => {
            // 0. Create terminal and open it
            try {
                createTerminal();
                term.open(document.getElementById('terminal'));
                setupImeGuard();
            } catch (e) {
                console.error("Failed to create terminal", e);
                return;
            }

            // 1. Setup event listeners and sync preferences FIRST
            //    确保 term.onResize 在 fitAddon.fit() 之前设置好，
            //    这样首次 fit 的 resize 事件才能触发 native.resize() 更新原生终端尺寸
            try {
                syncPrefs();
                setupEventListeners();
            } catch (e) {
                console.error("Error during setup", e);
            }

            // 2. Detect available monospace fonts
            //    Preload bundled fonts first to ensure they're detected
            try {
                // Bundled fonts — always available, loaded via fonts.css @font-face
                var bundledFonts = ['JetBrains Mono', 'Fira Code', 'Source Code Pro'];
                var preloadPromises = [];
                if (document.fonts && document.fonts.load) {
                    for (var b = 0; b < bundledFonts.length; b++) {
                        preloadPromises.push(
                            document.fonts.load('14px "' + bundledFonts[b] + '"')
                                .catch(function() { /* ignore individual load failures */ })
                        );
                    }
                }
                // Run detection after preload (with timeout fallback)
                var doDetection = function() {
                    var availableFonts = detectAvailableMonospaceFonts();
                    // Ensure bundled fonts are always present (their woff2 files are baked in)
                    for (var k = 0; k < bundledFonts.length; k++) {
                        if (availableFonts.indexOf(bundledFonts[k]) === -1) {
                            availableFonts.push(bundledFonts[k]);
                        }
                    }
                    console.log('Detected monospace fonts: ' + availableFonts.join(', '));
                    if (native.onAvailableFontsDetected) {
                        native.onAvailableFontsDetected(JSON.stringify(availableFonts));
                    }
                    scheduleFit();
                };
                if (preloadPromises.length > 0) {
                    // Wait up to 2s for fonts to load, then run detection
                    Promise.race([
                        Promise.all(preloadPromises),
                        new Promise(function(r) { setTimeout(r, 2000); })
                    ]).then(doDetection);
                } else {
                    doDetection();
                }
            } catch (e) {
                console.warn('Font detection failed', e);
            }

            // 3. Fit Terminal (now onResize listener is ready to capture this)
            try {
                fitAddon.fit();
                console.log(`Terminal size after fit: ${term.cols}x${term.rows}`);

                // If fit failed (size 0), force a default size
                if (term.cols < 2 || term.rows < 2) {
                    term.resize(80, 24);
                    console.log("Forced resize to 80x24");
                }
            } catch (e) {
                console.error("Fit failed", e);
                term.resize(80, 24); // Fallback
            }

            // 5. Initialize and Start Shell
            try {
                // Notify native that we are ready
                if (window.native && native.load) {
                    native.load();
                } else {
                    console.error("Native object still missing after retries");
                    term.writeln('\r\nError: Native bridge failed to initialize.\r\n');
                }
            } catch (e) {
                console.error("Error during init/load", e);
            }

        });
    }

    // Start initialization
    initialize();
};

function syncPrefs() {
    try {
        if (native.getFontSize) {
            const fs = native.getFontSize();
            if (fs) term.options.fontSize = fs;
        }
        if (native.getFontFamily) {
            const ff = native.getFontFamily();
            if (ff) exports.setFontFamily(ff);
        }
        if (native.getCursorBlink) {
            const blink = native.getCursorBlink();
            if (typeof blink === 'boolean') term.options.cursorBlink = blink;
        }
        if (native.getCursorShape) {
            const shape = native.getCursorShape();
            if (shape) exports.setCursorShape(shape); // Use our adapter logic
        }
        if (native.getBackgroundColor) {
            const bg = native.getBackgroundColor();
            if (bg) exports.setBackgroundColor(bg);
        }
        if (native.getBackgroundImage) {
            const img = native.getBackgroundImage();
            if (img) exports.setBackgroundImage(img);
        }
        if (native.getBackgroundBlur) {
            const blur = native.getBackgroundBlur();
            if (blur) exports.setBackgroundBlur(blur);
        }
        if (native.getUserFonts) {
            const fontsJson = native.getUserFonts();
            if (fontsJson) registerUserFonts(fontsJson);
        }
    } catch (e) {
        console.warn("Failed to sync prefs", e);
    }
}

// Registers user-uploaded fonts (JSON array of {name, dataUrl}) via FontFace.
function registerUserFonts(json) {
    try {
        var fonts = JSON.parse(json);
        for (var i = 0; i < fonts.length; i++) {
            exports.addUserFont(fonts[i].name, fonts[i].dataUrl);
        }
    } catch (e) {
        console.warn('registerUserFonts failed', e);
    }
}
exports.registerUserFonts = registerUserFonts;

function setupEventListeners() {
    // 1. Unicode input -> native process (ArkTS encodes UTF-8 once)
    term.onData(data => {
        // Pass Unicode directly to the native bridge.
        if (native && native.sendInput) {
            native.sendInput(data);
        }
    });

    // 2. Resize -> Notify Native
    term.onResize(size => {
        if (native && native.resize) {
            native.resize(size.cols, size.rows);
        }
    });

    nativeSelection = HiSHNativeSelection.attach(term);

    // 4. Handle Window Resize
    window.addEventListener('resize', scheduleFit);
    if (window.visualViewport) window.visualViewport.addEventListener('resize', scheduleFit);
    if (typeof ResizeObserver !== 'undefined') {
        terminalObserver = new ResizeObserver(scheduleFit);
        terminalObserver.observe(document.getElementById('terminal-container'));
    }
    if (document.fonts && document.fonts.ready) document.fonts.ready.then(scheduleFit);
}

// Raw process output; never reinterpret backslash sequences printed by a command.
exports.writeBase64 = (base64Data, applicationMode) => {
    try {
        const uint8 = strToUint8Array(atob(base64Data));
        term.write(uint8, () => {
            if (term.modes.applicationCursorKeysMode !== applicationMode && native && native.setApplicationMode) {
                native.setApplicationMode(term.modes.applicationCursorKeysMode);
            }
        });
    } catch (e) {
        console.error("exports.writeBase64 failed", e);
    }
};

exports.pasteBase64 = (base64Data) => {
    if (typeof nativeSelection !== 'undefined' && nativeSelection) nativeSelection.clear();
    term.paste(decoder.decode(strToUint8Array(atob(base64Data))));
};

exports.copy = () => {
    const selected = window.getSelection();
    return selected && !selected.isCollapsed ? selected.toString() : term.getSelection();
};

exports.setFontSize = (size) => {
    if (!Number.isFinite(size) || size <= 0) return;
    term.options.fontSize = size;
    scheduleFit();
};

exports.setFontFamily = (family) => {
    currentFontFamily = family || 'Default';
    var newFamily = mapFontFamily(family);
    term.options.fontFamily = newFamily;

    // Resize to current dimensions to force xterm.js to
    // re-layout with the new font metrics, then re-fit.
    if (term.cols > 0 && term.rows > 0) {
        term.resize(term.cols, term.rows);
    }
    scheduleFit();
};

exports.setCursorShape = (shape) => {
    // hterm: BLOCK, BEAM, UNDERLINE
    // xterm: block, bar, underline
    switch (shape) {
        case 'BEAM': term.options.cursorStyle = 'bar'; break;
        case 'UNDERLINE': term.options.cursorStyle = 'underline'; break;
        default: term.options.cursorStyle = 'block'; break;
    }
};

exports.setCursorBlink = (blink) => {
    term.options.cursorBlink = blink;
};

// Ligature / italic toggles: this build always uses the DOM renderer,
// so both are pure CSS switches applied live without reloading.
exports.setLigatures = (enabled) => {
    document.body.classList.toggle('ligatures-off', !enabled);
};

exports.setItalic = (enabled) => {
    document.body.classList.toggle('italics-off', !enabled);
};

// --- Terminal appearance: background color / image / gaussian blur ---

exports.setBackgroundColor = (color) => {
    const appearance = HiSHAppearance.background(color);
    if (!appearance) return;
    document.body.style.backgroundColor = appearance.css;
    document.documentElement.style.setProperty('--terminal-ink', appearance.ink);
    term.options.theme = appearance.theme;
};

exports.setBackgroundImage = (dataUrl) => {
    var bg = document.getElementById('terminal-bg');
    if (!bg) return;
    bg.style.backgroundImage = dataUrl ? 'url("' + dataUrl + '")' : 'none';
};

exports.setBackgroundBlur = (px) => {
    var bg = document.getElementById('terminal-bg');
    if (!bg) return;
    var radius = parseInt(px, 10) || 0;
    if (radius > 0) {
        bg.style.filter = 'blur(' + radius + 'px)';
        // Slightly upscale so the blurred edges stay covered
        bg.style.transform = 'scale(' + (1 + radius / 100) + ')';
    } else {
        bg.style.filter = 'none';
        bg.style.transform = 'none';
    }
};

// Registers a user-uploaded font into the document via the FontFace API.
// dataUrl is a base64 data URL of the font file (ttf/otf/woff/woff2).
exports.addUserFont = (name, dataUrl) => {
    if (!name || !dataUrl || typeof FontFace === 'undefined') return;
    try {
        var face = new FontFace(name, 'url("' + dataUrl + '")');
        face.load().then(function (loaded) {
            document.fonts.add(loaded);
            console.log('User font loaded: ' + name);
            // Re-apply in case this font is the currently selected one
            if (currentFontFamily === name) {
                exports.setFontFamily(name);
            }
        }).catch(function (e) {
            console.error('User font load failed: ' + name, e);
        });
    } catch (e) {
        console.error('addUserFont failed: ' + name, e);
    }
};

exports.setFocused = (focus) => {
    if (focus) {
        setupImeGuard();
        term.focus();
    } else {
        term.blur();
    }
};
exports.setNativeLineMode = (convertEol) => {
    term.options.convertEol = convertEol;
};

// Mock hterm.openUrl for compatibility if something calls it directly
window.hterm = {
    openUrl: (url) => {
        if (native && native.openLink) native.openLink(url);
    }
};

exports.selectAll = () => {
    if (nativeSelection) nativeSelection.selectAll();
};

exports.clearSelection = () => {
    if (nativeSelection) nativeSelection.clear();
};

// Clears the whole terminal buffer (viewport + scrollback)
exports.clearScreen = () => {
    if (nativeSelection) nativeSelection.clear();
    term.clear();
};
