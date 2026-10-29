/* Hallmark · component: terminal appearance · existing HiSH system
 * pre-emit critique: P4 H4 E4 S5 R5 V3. No decorative motion or new font.
 */
(function (root) {
    'use strict';
    const tokens = Object.freeze({
        base: '#000000', lightInk: '#FFFFFF', darkInk: '#000000',
        lightSelection: 'rgba(255,255,255,0.25)', darkSelection: 'rgba(0,0,0,0.20)'
    });
    function channel(value) {
        value /= 255;
        return value <= 0.04045 ? value / 12.92 : Math.pow((value + 0.055) / 1.055, 2.4);
    }
    // ArkUI accepts #AARRGGBB; CSS accepts #RRGGBBAA. Normalize explicitly and
    // choose the better-contrast text against the solid fill over the black base.
    function background(value) {
        if (typeof value !== 'string' || !/^#(?:[a-f\d]{6}|[a-f\d]{8})$/i.test(value)) return null;
        const hasAlpha = value.length === 9;
        const alpha = hasAlpha ? parseInt(value.slice(1, 3), 16) / 255 : 1;
        const rgb = hasAlpha ? value.slice(3) : value.slice(1);
        const channels = [0, 2, 4].map(offset => parseInt(rgb.slice(offset, offset + 2), 16));
        const visible = channels.map(c => Math.round(c * alpha));
        const luminance = channel(visible[0]) * 0.2126 + channel(visible[1]) * 0.7152 + channel(visible[2]) * 0.0722;
        const darkText = (luminance + 0.05) / 0.05 >= 1.05 / (luminance + 0.05);
        const ink = darkText ? tokens.darkInk : tokens.lightInk;
        return {
            css: 'rgb(' + visible.join(',') + ')',
            theme: { background: 'transparent', foreground: ink, cursor: ink,
                cursorAccent: 'rgb(' + visible.join(',') + ')',
                selectionBackground: darkText ? tokens.darkSelection : tokens.lightSelection },
            ink, contrast: darkText ? (luminance + 0.05) / 0.05 : 1.05 / (luminance + 0.05)
        };
    }
    root.HiSHAppearance = Object.freeze({ tokens, background });
})(window);
