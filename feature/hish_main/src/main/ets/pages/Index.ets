import { CustomContentDialog } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import WebTerminal from '../components/WebTerminal';
import SettingsContent from '../components/SettingsContent';

@Component
export struct Index {
  private isPc: boolean = deviceInfo.deviceType === '2in1';
  private settings: CustomDialogController = new CustomDialogController({
    builder: CustomContentDialog({
      primaryTitle: $r('app.string.Setting_title'),
      contentBuilder: () => { this.buildSettings(); }
    }),
    width: '80%'
  });

  @Builder
  private buildSettings() {
    SettingsContent()
  }

  build() {
    Column() {
      if (this.isPc) {
        Row() {
          Image($r('app.media.startIcon')).width(20)
          Text($r('app.string.EntryAbility_label')).fontColor(Color.White).margin({ left: 8 })
          Blank()
          Button({ buttonStyle: ButtonStyleMode.TEXTUAL }) {
            Image($r('app.media.gearshape')).width(20)
          }
          .width(44)
          .height(44)
          .accessibilityText($r('app.string.menu_settings'))
          .onClick(() => { this.settings.open(); })
        }
        .height(48)
        .padding({ left: 16, right: 16 })
      }
      WebTerminal({ virtKeyEnabled: !this.isPc }).layoutWeight(1)
    }
    .width('100%')
    .height('100%')
  }
}
