import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import hssReceiver from 'libhss_receiver.so';

const DOMAIN = 0x4853;

export default class EntryAbility extends UIAbility {
  private async configureSystemBars(mainWindow: window.Window): Promise<void> {
    try {
      await mainWindow.setSpecificSystemBarEnabled('status', true);
      await mainWindow.setSpecificSystemBarEnabled('navigation', false);
      await mainWindow.setSpecificSystemBarEnabled('navigationIndicator', false);
    } catch (error) {
      hilog.warn(DOMAIN, 'HSS', 'Failed to restore the status bar: %{public}s', JSON.stringify(error));
    }
  }

  onCreate(_want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, 'HSS', 'Receiver ability created');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    try {
      const mainWindow: window.Window = windowStage.getMainWindowSync();
      void this.configureSystemBars(mainWindow);
    } catch (error) {
      hilog.warn(DOMAIN, 'HSS', 'Main window unavailable: %{public}s', JSON.stringify(error));
    }
    windowStage.loadContent('pages/Index', (error) => {
      if (error.code) {
        hilog.error(DOMAIN, 'HSS', 'Failed to load Index: %{public}s', JSON.stringify(error));
      }
    });
  }

  onForeground(): void {
    hssReceiver.onAppForeground();
    hilog.info(DOMAIN, 'HSS', 'Receiver ability entered foreground');
  }

  onBackground(): void {
    hssReceiver.onAppBackground();
    hilog.info(DOMAIN, 'HSS', 'Receiver ability entered background');
  }
}
