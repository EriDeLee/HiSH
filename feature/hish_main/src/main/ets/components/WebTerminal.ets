import util from '@ohos.util';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { NativeTerminalSession } from '../native/NativeTerminalSession';
import { webview } from '@kit.ArkWeb';
import { deviceInfo } from '@kit.BasicServicesKit'
import appOption, { CursorShape, savePreference, UserFont, UserFontData } from '../model/appOption';
import { common } from '@kit.AbilityKit';
import { SymbolGlyphModifier } from '@kit.ArkUI';
import fileToDataUrl from '../lib/fileToDataUrl';
import { ImmersiveButtonModifier } from '../lib/immersiveButton';
import {
  applyVirtualKeyModifiers,
  FunctionKeySequences,
  getVirtualKeyMetrics,
  NormalKeySequences
} from '../lib/terminalVirtualKeys';

const DOMAIN = 0x0000;

const VKeyMetrics = getVirtualKeyMetrics(deviceInfo.deviceType)
const VKeyMargin = VKeyMetrics.margin;
const VKeyPadding = VKeyMetrics.padding;
const VKeyHeight = VKeyMetrics.height;
const VKeyWidth = VKeyMetrics.width;
const VKeyFontSize = VKeyMetrics.fontSize;
const VKeyRadius = VKeyMetrics.radius;
const VKeyActiveTextColor = '#80DEEA';
const VKeyLabelStyle: LabelStyle = {
  maxLines: 1,
  minFontSize: VKeyFontSize - 1,
  maxFontSize: VKeyFontSize,
  heightAdaptivePolicy: TextHeightAdaptivePolicy.MIN_FONT_SIZE_FIRST
};

@Component
export default struct WebTerminal {
  @Require virtKeyEnabled: boolean = true
  @State ctrlPressed: boolean = false
  @State shiftPressed: boolean = false
  @State altPressed: boolean = false
  @State fnPressed: boolean = false
  @Watch('onFontSizeChanged') @StorageProp(appOption.terminalFontSize) fontSize: number | null = null
  @Watch('onFontFamilyChanged') @StorageProp(appOption.terminalFontFamily) fontFamily: string = 'Default'
  @Watch('onCursorShapeChanged') @StorageProp(appOption.terminalCursorShape) cursorShape: CursorShape | null = null
  @Watch('onCursorBlinkChanged') @StorageProp(appOption.terminalCursorBlink) cursorBlink: boolean | null = null
  @Watch('saveShowVirtKey') @StorageProp(appOption.showVirtKey) showVirtKey: boolean = true
  @Watch('onLigaturesChanged') @StorageProp(appOption.terminalLigatures) termLigatures: boolean = false
  @Watch('onItalicChanged') @StorageProp(appOption.terminalItalic) termItalic: boolean = true
  @Watch('onBgColorChanged') @StorageProp(appOption.terminalBackgroundColor) termBgColor: string = '#000000'
  @Watch('onBgImageChanged') @StorageProp(appOption.terminalBackgroundImage) termBgImage: string = ''
  @Watch('onBgBlurChanged') @StorageProp(appOption.terminalBackgroundBlur) termBgBlur: number = 0
  @Watch('onUserFontsChanged') @StorageProp(appOption.terminalUserFonts) termUserFonts: UserFont[] = []
  webviewController: WebviewController = new webview.WebviewController()
  applicationMode: boolean = false
  immersiveButton: ImmersiveButtonModifier = new ImmersiveButtonModifier()

  /**
   * @param appCode code used in application mode
   */
  @Builder
  buildVKey(name: string, value: string, appCode?: string, weight: number = 1) {
    Button(name, { buttonStyle: ButtonStyleMode.TEXTUAL })
      .height(VKeyHeight)
      .layoutWeight(weight)
      .borderRadius(VKeyRadius)
      .fontSize(VKeyFontSize)
      .labelStyle(VKeyLabelStyle)
      .fontWeight(FontWeight.Normal)
      .fontColor($r('sys.color.white'))
      .backgroundColor(Color.Black)
      .margin(VKeyMargin)
      .padding({ left: VKeyPadding, right: VKeyPadding })
      .attributeModifier(this.immersiveButton)
      .accessibilityText(name)
      .onClick(() => {
        if (this.applicationMode && appCode) {
          this.sendInput(appCode);
        } else {
          this.sendInput(value)
        }
      })
  }

  @Builder
  toggleCtrlKey(name: string, action: () => void) {
    Button(name, { buttonStyle: ButtonStyleMode.TEXTUAL })
    .height(VKeyHeight)
    .layoutWeight(1)
    .borderRadius(VKeyRadius)
    .margin(VKeyMargin)
    .fontSize(VKeyFontSize)
    .labelStyle(VKeyLabelStyle)
    .fontWeight(FontWeight.Normal)
    .fontColor(this.ctrlPressed ? VKeyActiveTextColor : Color.White)
    .backgroundColor(this.ctrlPressed ? '#2C2C2E' : Color.Black)
    .stateEffect(true)
    .accessibilityText('Control')
    .padding(VKeyPadding)
    .attributeModifier(this.immersiveButton)
    .onClick(() => action())
  }

  @Builder
  toggleShiftKey(name: string, action: () => void) {
    Button(name, { buttonStyle: ButtonStyleMode.TEXTUAL })
    .height(VKeyHeight)
    .layoutWeight(1.15)
    .borderRadius(VKeyRadius)
    .margin(VKeyMargin)
    .fontSize(VKeyFontSize)
    .labelStyle(VKeyLabelStyle)
    .fontWeight(FontWeight.Normal)
    .fontColor(this.shiftPressed ? VKeyActiveTextColor : Color.White)
    .backgroundColor(this.shiftPressed ? '#2C2C2E' : Color.Black)
    .stateEffect(true)
    .accessibilityText('Shift')
    .padding(VKeyPadding)
    .attributeModifier(this.immersiveButton)
    .onClick(() => action())
  }

  @Builder
  toggleFnKey(name: string, action: () => void) {
    Button(name, { buttonStyle: ButtonStyleMode.TEXTUAL })
    .height(VKeyHeight)
    .layoutWeight(1)
    .borderRadius(VKeyRadius)
    .margin(VKeyMargin)
    .fontSize(VKeyFontSize)
    .labelStyle(VKeyLabelStyle)
    .fontWeight(FontWeight.Normal)
    .fontColor(this.fnPressed ? VKeyActiveTextColor : Color.White)
    .backgroundColor(this.fnPressed ? '#2C2C2E' : Color.Black)
    .stateEffect(true)
    .accessibilityText('功能键')
    .padding(VKeyPadding)
    .attributeModifier(this.immersiveButton)
    .onClick(() => action())
  }

  @Builder
  toggleAltKey(name: string, action: () => void) {
    Button(name, { buttonStyle: ButtonStyleMode.TEXTUAL })
    .height(VKeyHeight)
    .layoutWeight(1)
    .borderRadius(VKeyRadius)
    .margin(VKeyMargin)
    .fontSize(VKeyFontSize)
    .labelStyle(VKeyLabelStyle)
    .fontWeight(FontWeight.Normal)
    .fontColor(this.altPressed ? VKeyActiveTextColor : Color.White)
    .backgroundColor(this.altPressed ? '#2C2C2E' : Color.Black)
    .stateEffect(true)
    .accessibilityText('Alt')
    .padding(VKeyPadding)
    .attributeModifier(this.immersiveButton)
    .onClick(() => action())
  }

  @Builder
  settingsButton() {
    Button({ buttonStyle: ButtonStyleMode.TEXTUAL }) {
      Image($r('app.media.gearshape')).width(18).height(18).padding(0)
    }
    .height(VKeyHeight)
    .width(VKeyWidth)
    .borderRadius(VKeyRadius)
    .margin({ left: 8, right: 4 })
    .padding(0)
    .backgroundColor(Color.Black)
    .attributeModifier(this.immersiveButton)
    .id('terminal-menu')
    .accessibilityText($r('app.string.menu_settings'))
    .bindMenu(this.settingsButtonOptions())
  }

  private settingsButtonOptions(): MenuElement[] {
    const options: MenuElement[] = [{
      value: '重新启动终端',
      action: () => { void this.confirmRestart(); }
    }, {
      value: $r('app.string.menu_clear_screen'),
      action: () => { this.webviewController.runJavaScript('exports.clearScreen()'); }
    }, {
      value: $r('app.string.menu_settings'),
      symbolIcon: new SymbolGlyphModifier($r('sys.symbol.gearshape')).fontSize($r('sys.float.Title_S')),
      action: () => this.getUIContext().getRouter().pushUrl({ url: 'pages/SettingsPage' })
    }];
    return options;
  }

  private async confirmRestart(): Promise<void> {
    try {
      const result = await this.getUIContext().getPromptAction().showDialog({
        title: '重新启动终端',
        message: '这会结束当前终端和其中的程序。未保存的内容可能丢失。',
        buttons: [{ text: '取消', color: '#FFFFFF' }, { text: '重新启动', color: '#FF453A' }]
      });
      if (result.index === 1) { await this.startNativeSession('shell'); }
    } catch (error) {
      hilog.warn(DOMAIN, 'HiSH', 'Restart dialog dismissed');
    }
  }

  @Builder
  hideVKeyButton() {
    Button('▼', { buttonStyle: ButtonStyleMode.TEXTUAL })
      .height(VKeyHeight)
      .width(VKeyWidth)
      .borderRadius(VKeyRadius)
      .margin({ left: 8, right: 4 })
      .padding({ left: VKeyPadding, right: VKeyPadding })
      .fontColor($r('sys.color.white'))
      .backgroundColor(Color.Black)
      .fontSize(VKeyFontSize)
      .accessibilityText('收起扩展键盘')
      .attributeModifier(this.immersiveButton)
      .onClick(() => {
        this.showVirtKey = false
      })
  }

  build() {
    Column() {
      Web({
        src: $rawfile('term/term.html'),
        controller: this.webviewController
      })
        .javaScriptProxy({
          object: this,
          name: 'native',
          methodList: ['sendInput', 'resize', 'load',
            'getFontSize', 'getFontFamily',
            'getCursorShape', 'getCursorBlink',
            'getLigatures', 'getItalic',
            'getBackgroundColor', 'getBackgroundImage', 'getBackgroundBlur',
            'getUserFonts',
            'setApplicationMode',
            'onAvailableFontsDetected'],
          controller: this.webviewController,
          asyncMethodList: [],
          permission: `{
          "javascriptProxyPermission": {
            "urlPermissionList": [
              {
                "scheme": "resource",
                "host": "rawfile",
                "port": "",
                "path": ""
              }
            ]
          }
        }`
        })
        .backgroundColor('#000')
        .onLoadIntercept((event) => {
          if (!event) { return true; }
          const url = event.data.getRequestUrl().toLowerCase();
          return !(url.startsWith('resource://rawfile/') || url.startsWith('resource:/rawfile/'));
        })
        .layoutWeight(1)
        .editMenuOptions({
          onCreateMenu: (items: TextMenuItem[]): TextMenuItem[] => {
            return items.filter((item: TextMenuItem) => !item.id.equals(TextMenuItemId.CUT));
          },
          onMenuItemClick: (): boolean => false
        })
      if (this.virtKeyEnabled) {
        if (this.showVirtKey) {
          this.buildVirtualKeys()
        } else {
          this.buildShowVirtualKeysButton()
        }
      }
    }
    .height('100%')

  }

  @Builder
  buildShowVirtualKeysButton() {
    Column() {
      Button('▲', { buttonStyle: ButtonStyleMode.TEXTUAL })
        .width(120)
        .height(16)
        .fontSize(16)
        .borderRadius(VKeyRadius)
        .fontColor('#444')
        .accessibilityText('展开扩展键盘')
        .padding({ left: VKeyPadding, right: VKeyPadding })
        .attributeModifier(this.immersiveButton)
        .onClick(() => {
          this.showVirtKey = true
        })
    }
    .width('100%')
  }

  @Builder
  buildVirtualKeys() {
    Column() {
      if (this.fnPressed) {
        Row() {
          this.buildVKey('ESC', NormalKeySequences.ESC);
          this.toggleFnKey('Fn', () => this.toggleFn());
          this.buildVKey('F1', FunctionKeySequences.F1, undefined, 1.15);
          this.buildVKey('F2', FunctionKeySequences.F2);
          this.buildVKey('F3', FunctionKeySequences.F3, undefined, 1.15);
          this.buildVKey('F4', FunctionKeySequences.F4);
          this.buildVKey('F5', FunctionKeySequences.F5);
          this.buildVKey('F6', FunctionKeySequences.F6, undefined, 1.1);
          this.settingsButton();
        }
        .width('100%')

        Row() {
          this.buildVKey('TAB', NormalKeySequences.TAB);
          this.toggleCtrlKey('CTRL', () => this.toggleCtrl());
          this.buildVKey('F7', FunctionKeySequences.F7, undefined, 1.15);
          this.buildVKey('F8', FunctionKeySequences.F8);
          this.buildVKey('F9', FunctionKeySequences.F9, undefined, 1.15);
          this.buildVKey('F10', FunctionKeySequences.F10);
          this.buildVKey('F11', FunctionKeySequences.F11);
          this.buildVKey('F12', FunctionKeySequences.F12, undefined, 1.1);
          this.hideVKeyButton();
        }
        .width('100%')
      } else {
        Row() {
          this.buildVKey('ESC', NormalKeySequences.ESC);
          this.toggleFnKey('Fn', () => this.toggleFn());
          this.buildVKey('/', '/', '/', 1.15);
          this.buildVKey('—', '-', '-');
          this.buildVKey('HOME', NormalKeySequences.HOME, undefined, 1.15);
          this.buildVKey('↑', NormalKeySequences.UP, NormalKeySequences.UP_APP);
          this.buildVKey('END', NormalKeySequences.END);
          this.buildVKey('PGUP', NormalKeySequences.PGUP, undefined, 1.1);
          this.settingsButton();
        }
        .width('100%')

        Row() {
          this.buildVKey('TAB', NormalKeySequences.TAB);
          this.toggleCtrlKey('CTRL', () => this.toggleCtrl());
          this.toggleShiftKey('SHIFT', () => this.toggleShift());
          this.toggleAltKey('ALT', () => this.toggleAlt());
          this.buildVKey('←', NormalKeySequences.LEFT, NormalKeySequences.LEFT_APP, 1.15);
          this.buildVKey('↓', NormalKeySequences.DOWN, NormalKeySequences.DOWN_APP);
          this.buildVKey('→', NormalKeySequences.RIGHT, NormalKeySequences.RIGHT_APP);
          this.buildVKey('PGDN', NormalKeySequences.PGDN, undefined, 1.1);
          this.hideVKeyButton();
        }
        .width('100%')
      }
    }
    .width('100%')
    .backgroundColor(Color.Black)
  }

  async saveShowVirtKey() {
    await savePreference(appOption.showVirtKey, this.showVirtKey, this.getUIContext())
    if (!this.showVirtKey) {
      this.getUIContext().getPromptAction().showToast({
        message: $r('app.string.hide_virtual_key_toast'),
        duration: 5000
      })
    }
  }

  private toggleCtrl(): void {
    this.ctrlPressed = !this.ctrlPressed;
  }

  private toggleShift(): void {
    this.shiftPressed = !this.shiftPressed;
  }

  private toggleFn(): void {
    this.fnPressed = !this.fnPressed;
  }

  private toggleAlt(): void {
    this.altPressed = !this.altPressed;
  }
  //  ============== for javascript in ArkWeb ================

  sendInput(data: string): void {


    const modifierResult = applyVirtualKeyModifiers(data, this.ctrlPressed, this.shiftPressed, this.altPressed)
    data = modifierResult.data

    if (modifierResult.shouldResetCtrl) {
      this.toggleCtrl()
    }
    if (modifierResult.shouldResetShift) {
      this.toggleShift()
    }
    if (modifierResult.shouldResetAlt) {
      this.toggleAlt()
    }
    const session = this.nativeSession;
    if (session !== null) {
      session.send(data);
    }

    // Ctrl/Shift/Alt 为一次性修饰键；Fn 保持功能键页，手动切回。
  }

  private nativeSession: NativeTerminalSession | null = null;
  private sessionRevision: number = 0;
  private terminalColumns: number = 80;
  private terminalRows: number = 24;

  async load(): Promise<void> {
    this.webviewController.runJavaScript('exports.setFocused(true)');
    await this.startNativeSession('shell');
  }

  private async startNativeSession(mode: string): Promise<void> {
    const revision = ++this.sessionRevision;
    const old = this.nativeSession;
    this.nativeSession = null;
    if (old !== null) { old.close(); }
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const root = context.getApplicationContext().filesDir + '/native';
    try {
      const session = await NativeTerminalSession.open(root, this.terminalColumns, this.terminalRows,
        mode, (data: ArrayBuffer): void => {
          if (revision === this.sessionRevision) { this.onData(data); }
        }, (): void => {
          if (revision === this.sessionRevision) {
            this.writeStatus('终端已结束，可从设置菜单重新启动。');
          }
        }, (convertEol: boolean): void => {
          if (revision === this.sessionRevision) {
            this.webviewController.runJavaScript('exports.setNativeLineMode(' + convertEol + ')')
              .catch((error: Error): void => { hilog.warn(DOMAIN, 'HiSH', 'Terminal line-mode update failed'); });
          }
        });
      if (revision !== this.sessionRevision) { session.close(); return; }
      this.nativeSession = session;
    } catch (err) {
      if (revision === this.sessionRevision) {
        this.writeStatus('原生后端启动失败: ' + (err as Error).message);
      }
    }
  }

  private writeStatus(message: string): void {
    this.onData(util.TextEncoder.create('utf-8').encodeInto('\r\n' + message + '\r\n').buffer as ArrayBuffer);
  }

  resize(width: number, height: number): void {
    this.terminalColumns = width;
    this.terminalRows = height;
    this.nativeSession?.resize(width, height);
  }

  aboutToDisappear(): void {
    this.sessionRevision += 1;
    this.nativeSession?.close();
    this.nativeSession = null;
    if (this.flushTimeoutID !== undefined) { clearTimeout(this.flushTimeoutID); }
    this.pendingChunks = [];
  }

  // 性能优化：输出缓冲区
  private pendingChunks: Uint8Array[] = [];
  private flushTimeoutID: number | undefined = undefined;
  private readonly FLUSH_INTERVAL = 16; // 60FPS (16.6ms)

  private onData(ab: ArrayBuffer) {
    try {
      // 将收到的数据块加入队列
      this.pendingChunks.push(new Uint8Array(ab));

      // 如果没有正在等待的 Flush 任务，则调度一个
      if (this.flushTimeoutID === undefined) {
        this.flushTimeoutID = setTimeout(() => {
          this.flushData();
        }, this.FLUSH_INTERVAL);
      }
    } catch (e) {
      hilog.error(DOMAIN, 'WebTerminal', 'onData failed: %{public}s', JSON.stringify(e));
    }
  }

  private flushData() {
    this.flushTimeoutID = undefined;

    if (this.pendingChunks.length === 0) return;

    try {
      // 1. 计算总长度
      let totalLength = 0;
      for (const chunk of this.pendingChunks) {
        totalLength += chunk.length;
      }

      // 2. 合并所有 Chunk
      const merged = new Uint8Array(totalLength);
      let offset = 0;
      for (const chunk of this.pendingChunks) {
        merged.set(chunk, offset);
        offset += chunk.length;
      }

      // 清空缓冲区
      this.pendingChunks = [];

      // 4. 发送合并后的数据
      const base64Helper = new util.Base64Helper();
      const base64 = base64Helper.encodeToStringSync(merged);
      this.webviewController.runJavaScript(`exports.writeBase64("${base64}", ${this.applicationMode})`);

    } catch (e) {
      hilog.error(DOMAIN, 'WebTerminal', 'flushData failed: %{public}s', JSON.stringify(e));
      // 出错时也要清空缓冲区，避免积压
      this.pendingChunks = [];
    }
  }

  getFontSize(): number | null {
    return this.fontSize
  }

  getFontFamily(): string {
    return this.fontFamily
  }

  getCursorShape(): string | null {
    return this.cursorShape
  }

  getCursorBlink(): boolean | null {
    return this.cursorBlink
  }

  getDeviceType(): string {
    return deviceInfo.deviceType;
  }

  onFontSizeChanged() {
    this.webviewController.runJavaScript(`exports.setFontSize(${this.fontSize})`)
  }

  onFontFamilyChanged() {
    // Read directly from AppStorage to ensure we get the updated value
    const family = AppStorage.get<string>(appOption.terminalFontFamily) ?? 'Default';
    hilog.info(DOMAIN, 'WebTerminal', 'onFontFamilyChanged: %{public}s', family);
    // Use JSON.stringify to safely escape any special characters in font name
    this.webviewController.runJavaScript(`exports.setFontFamily(${JSON.stringify(family)})`)
      .catch((err: Error) => {
        hilog.error(DOMAIN, 'WebTerminal', 'setFontFamily failed: %{public}s', JSON.stringify(err));
      })
  }

  onCursorShapeChanged() {
    this.webviewController.runJavaScript(`exports.setCursorShape('${this.cursorShape}')`)
  }

  onCursorBlinkChanged() {
    this.webviewController.runJavaScript(`exports.setCursorBlink(${this.cursorBlink})`)
  }

  getLigatures(): boolean {
    return this.termLigatures
  }

  getItalic(): boolean {
    return this.termItalic
  }

  getBackgroundColor(): string {
    return this.termBgColor
  }

  getBackgroundBlur(): number {
    return this.termBgBlur
  }

  getBackgroundImage(): string {
    return this.backgroundImageDataUrl()
  }

  getUserFonts(): string {
    const fonts = AppStorage.get<UserFont[]>(appOption.terminalUserFonts) ?? []
    const list: UserFontData[] = []
    for (const f of fonts) {
      const data: UserFontData = { name: f.name, dataUrl: fileToDataUrl(f.file) }
      if (data.dataUrl.length > 0) {
        list.push(data)
      }
    }
    return JSON.stringify(list)
  }

  private backgroundImageDataUrl(): string {
    const fileName = AppStorage.get<string>(appOption.terminalBackgroundImage) ?? ''
    if (fileName.length === 0) {
      return ''
    }
    const filesDir = AppStorage.get<string>(appOption.appFilesDir) ?? ''
    return fileToDataUrl(`${filesDir}/terminal_bg/${fileName}`)
  }

  onBgColorChanged() {
    this.webviewController.runJavaScript(`exports.setBackgroundColor(${JSON.stringify(this.termBgColor)})`)
  }

  onBgImageChanged() {
    this.webviewController.runJavaScript(`exports.setBackgroundImage(${JSON.stringify(this.backgroundImageDataUrl())})`)
  }

  onBgBlurChanged() {
    this.webviewController.runJavaScript(`exports.setBackgroundBlur(${this.termBgBlur})`)
  }

  onUserFontsChanged() {
    const json = this.getUserFonts()
    this.webviewController.runJavaScript(`exports.registerUserFonts(${JSON.stringify(json)})`)
  }

  onLigaturesChanged() {
    this.webviewController.runJavaScript(`exports.setLigatures(${this.termLigatures})`)
  }

  onItalicChanged() {
    this.webviewController.runJavaScript(`exports.setItalic(${this.termItalic})`)
  }

  setApplicationMode(mode: boolean) {
    this.applicationMode = mode
    hilog.info(DOMAIN, 'WebTerminal', 'set applicationMode to %{public}s', this.applicationMode);
  }

  // Called from WebView when monospace font detection completes.
  // Receives a JSON array of available monospace font names.
  onAvailableFontsDetected(json: string): void {
    try {
      const fonts: string[] = JSON.parse(json);
      hilog.info(DOMAIN, 'WebTerminal', 'Available monospace fonts: %{public}s', fonts.join(', '));
      AppStorage.setOrCreate(appOption.availableFontFamilies, fonts);
    } catch (e) {
      hilog.error(DOMAIN, 'WebTerminal', 'Failed to parse available fonts: %{public}s', JSON.stringify(e));
    }
  }
}
