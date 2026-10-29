import { UIAbility, ConfigurationConstant, bundleManager, Want, AbilityConstant } from '@kit.AbilityKit';
import { KeyboardAvoidMode, window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import preference from '@ohos.data.preferences';
import appOption, { UserFont } from '../model/appOption';

interface TerminalPreference {
  key: string;
  value: string | number | boolean;
}

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
  }

  async onWindowStageCreate(stage: window.WindowStage): Promise<void> {
    const app = this.context.getApplicationContext();
    AppStorage.setOrCreate(appOption.appFilesDir, app.filesDir);
    AppStorage.setOrCreate(appOption.appTempDir, app.tempDir);
    AppStorage.setOrCreate(appOption.bundleInfo,
      await bundleManager.getBundleInfoForSelf(bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION));
    const prefs = await preference.getPreferences(app, appOption.preferenceName);
    const defaults: TerminalPreference[] = [
      { key: appOption.keepScreenOn, value: false },
      { key: appOption.showVirtKey, value: true },
      { key: appOption.terminalFontSize, value: deviceInfo.deviceType === 'phone' ? 10 : 12 },
      { key: appOption.terminalFontFamily, value: 'Default' },
      { key: appOption.terminalCursorShape, value: 'BLOCK' },
      { key: appOption.terminalCursorBlink, value: false },
      { key: appOption.terminalLigatures, value: false },
      { key: appOption.terminalItalic, value: true },
      { key: appOption.terminalBackgroundColor, value: '#000000' },
      { key: appOption.terminalBackgroundImage, value: '' },
      { key: appOption.terminalBackgroundBlur, value: 0 }
    ];
    for (const item of defaults) {
      AppStorage.setOrCreate(item.key, await prefs.get(item.key, item.value));
    }
    let fonts: UserFont[] = [];
    try {
      fonts = JSON.parse(await prefs.get(appOption.terminalUserFonts, '[]') as string) as UserFont[];
    } catch (err) {
      hilog.warn(0, 'HiSH', 'Invalid terminal font preferences');
    }
    AppStorage.setOrCreate(appOption.terminalUserFonts, fonts);
    const main = stage.getMainWindowSync();
    AppStorage.setOrCreate('mainWindow', main);
    stage.loadContent('pages/Index', (error) => {
      if (error.code !== 0) {
        hilog.error(0, 'HiSH', 'loadContent failed: %{public}d', error.code);
        return;
      }
      main.getUIContext().setKeyboardAvoidMode(KeyboardAvoidMode.RESIZE);
      main.setWindowKeepScreenOn(AppStorage.get<boolean>(appOption.keepScreenOn) ?? false);
    });
  }
}
