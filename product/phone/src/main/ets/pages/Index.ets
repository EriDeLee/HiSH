import { Index as MainPage } from 'hish_main';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

@Entry
@Component
struct Index {
  onPageShow(): void {
    this.setNavigationIndicator(false);
  }

  onPageHide(): void {
    this.setNavigationIndicator(true);
  }

  private setNavigationIndicator(visible: boolean): void {
    const main = AppStorage.get<window.Window>('mainWindow');
    if (main === undefined) { return; }
    main.setSpecificSystemBarEnabled('navigationIndicator', visible).catch((error: Error) => {
      hilog.warn(0, 'HiSH', 'Navigation indicator update failed: %{public}s', error.message);
    });
  }

  build() {
    Column() {
      MainPage()
    }
    .width('100%')
    .height(LayoutPolicy.matchParent)
    .ignoreLayoutSafeArea([LayoutSafeAreaType.SYSTEM], [LayoutSafeAreaEdge.BOTTOM])
    .backgroundColor(Color.Black)
  }
}
