/* ArkWeb supplies the selection, handles and editing menu. */
window.HiSHNativeSelection = {
    attach(term) {
        const element = term.element;
        const source = element.querySelector('.xterm-rows');
        // xterm recycles text nodes on focus/blur. Keep a stable DOM surface for
        // the system range while xterm continues processing shell output.
        const output = source.cloneNode(false);
        output.classList.add('hish-native-output');
        output.removeAttribute('aria-hidden');
        output.setAttribute('contenteditable', 'true');
        output.setAttribute('inputmode', 'none');
        output.setAttribute('spellcheck', 'false');
        output.setAttribute('autocorrect', 'off');
        output.setAttribute('aria-label', 'Terminal output');
        source.classList.add('hish-render-source');
        source.parentElement.appendChild(output);
        let touch = null;
        let pressing = false;
        let pendingFrame = 0;
        const inside = node => !!node && output.contains(node);
        function selected() {
            const selection = window.getSelection();
            return selection && !selection.isCollapsed &&
                (inside(selection.anchorNode) || inside(selection.focusNode));
        }
        function sync() {
            if (pressing || selected() || document.activeElement === output) return;
            output.style.cssText = source.style.cssText;
            for (let i = 0; i < source.children.length; i++) {
                const row = source.children[i];
                const old = output.children[i];
                if (!old) output.appendChild(row.cloneNode(true));
                else if (!old.isEqualNode(row)) old.replaceWith(row.cloneNode(true));
            }
            while (output.children.length > source.children.length) output.lastChild.remove();
        }
        function scheduleSync() {
            if (pendingFrame) return;
            pendingFrame = requestAnimationFrame(() => { pendingFrame = 0; sync(); });
        }
        function clear() {
            pressing = false;
            const selection = window.getSelection();
            if (selection && (inside(selection.anchorNode) || inside(selection.focusNode))) selection.removeAllRanges();
            if (document.activeElement === output) output.blur();
            scheduleSync();
        }
        output.addEventListener('paste', event => {
            const text = event.clipboardData?.getData('text/plain');
            if (typeof text !== 'string') return;
            event.preventDefault();
            event.stopImmediatePropagation();
            clear();
            term.paste(text);
            term.focus();
        }, true);
        // Never edit displayed output. Keyboard input still belongs to the shell.
        output.addEventListener('beforeinput', event => {
            event.preventDefault();
            if (event.isComposing) return;
            let data = event.data;
            if (event.inputType === 'insertParagraph' || event.inputType === 'insertLineBreak') data = '\r';
            if (event.inputType === 'deleteContentBackward') data = '\x7f';
            if (typeof data === 'string' && data) {
                clear();
                term.input(data, true);
                term.focus();
            }
        });
        // xterm's desktop handlers move its hidden textarea over the pressed
        // word and copy from xterm's separate (empty) selection.
        for (const name of ['mousedown', 'contextmenu', 'copy']) {
            element.addEventListener(name, event => {
                if (!inside(event.target)) return;
                event.stopImmediatePropagation();
                // Let ArkWeb finish its default copy before resuming live output.
                if (name === 'copy') setTimeout(clear, 0);
            }, true);
        }
        output.addEventListener('touchstart', event => {
            if (event.touches.length !== 1) { touch = null; return; }
            const point = event.touches[0];
            pressing = true;
            touch = { x: point.clientX, y: point.clientY, time: Date.now(), moved: false };
        }, { passive: true });
        output.addEventListener('touchmove', event => {
            if (!touch || event.touches.length !== 1) return;
            const point = event.touches[0];
            if (Math.hypot(point.clientX - touch.x, point.clientY - touch.y) > 10) {
                touch.moved = true;
                pressing = false;
                scheduleSync();
            }
        }, { passive: true });
        output.addEventListener('touchend', event => {
            const ended = touch;
            touch = null;
            pressing = false;
            if (ended && !ended.moved && Date.now() - ended.time < 300) {
                event.preventDefault();
                clear();
                term.focus();
            }
            scheduleSync();
        }, { passive: false });
        output.addEventListener('touchcancel', () => { pressing = false; touch = null; scheduleSync(); });
        document.addEventListener('selectionchange', scheduleSync);
        const render = term.onRender(scheduleSync);
        sync();
        return {
            clear,
            selectAll() {
                const range = document.createRange();
                range.selectNodeContents(output);
                const selection = window.getSelection();
                selection.removeAllRanges();
                selection.addRange(range);
            },
            dispose() {
                render.dispose();
                document.removeEventListener('selectionchange', scheduleSync);
                if (pendingFrame) cancelAnimationFrame(pendingFrame);
            }
        };
    }
};
