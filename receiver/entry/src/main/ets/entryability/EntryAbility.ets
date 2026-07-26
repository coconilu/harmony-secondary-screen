import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import hssReceiver from 'libhss_receiver.so';
import { PLAYBACK_ACTIVE_EVENT } from '../shared/ReceiverEvents';

const DOMAIN = 0x4853;

export default class EntryAbility extends UIAbility {
  private mainWindow: window.Window | undefined = undefined;
  private playbackActive: boolean = false;
  private foreground: boolean = false;
  private keepScreenOn: boolean | undefined = undefined;
  private keepScreenOnUpdatePending: boolean = false;
  private keepScreenOnRetryTimer: number = -1;
  private keepScreenOnRetryCount: number = 0;
  private readonly playbackActiveListener = (active: Object): void => {
    this.playbackActive = Boolean(active);
    this.scheduleKeepScreenOnUpdate(true);
  };

  private clearKeepScreenOnRetry(): void {
    if (this.keepScreenOnRetryTimer >= 0) {
      clearTimeout(this.keepScreenOnRetryTimer);
      this.keepScreenOnRetryTimer = -1;
    }
  }

  private scheduleKeepScreenOnUpdate(resetRetry: boolean = false): void {
    if (resetRetry) {
      this.clearKeepScreenOnRetry();
      this.keepScreenOnRetryCount = 0;
    }
    if (!this.keepScreenOnUpdatePending) {
      void this.applyKeepScreenOn();
    }
  }

  private async applyKeepScreenOn(): Promise<void> {
    this.keepScreenOnUpdatePending = true;
    let failed: boolean = false;
    try {
      while (this.mainWindow !== undefined) {
        const target: boolean = this.foreground && this.playbackActive;
        if (this.keepScreenOn === target) {
          break;
        }
        const targetWindow: window.Window = this.mainWindow;
        await targetWindow.setWindowKeepScreenOn(target);
        if (this.mainWindow !== targetWindow) {
          await targetWindow.setWindowKeepScreenOn(false);
          continue;
        }
        this.keepScreenOn = target;
      }
    } catch (error) {
      failed = true;
      this.keepScreenOn = undefined;
      hilog.warn(DOMAIN, 'HSS', 'Failed to update keep-screen-on: %{public}s',
        JSON.stringify(error));
    } finally {
      this.keepScreenOnUpdatePending = false;
      if (!failed) {
        this.keepScreenOnRetryCount = 0;
      } else if (this.mainWindow !== undefined && this.keepScreenOnRetryCount < 4) {
        const delayMs: number = 250 * (1 << this.keepScreenOnRetryCount);
        this.keepScreenOnRetryCount++;
        this.keepScreenOnRetryTimer = setTimeout(() => {
          this.keepScreenOnRetryTimer = -1;
          this.scheduleKeepScreenOnUpdate();
        }, delayMs);
      }
    }
  }

  private releaseWindow(targetWindow: window.Window | undefined): void {
    this.clearKeepScreenOnRetry();
    this.keepScreenOnRetryCount = 0;
    this.mainWindow = undefined;
    this.keepScreenOn = undefined;
    if (targetWindow !== undefined) {
      void targetWindow.setWindowKeepScreenOn(false).catch((error: Object) => {
        hilog.warn(DOMAIN, 'HSS', 'Failed to release keep-screen-on: %{public}s',
          JSON.stringify(error));
      });
    }
  }

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
    this.context.eventHub.on(PLAYBACK_ACTIVE_EVENT, this.playbackActiveListener);
    hilog.info(DOMAIN, 'HSS', 'Receiver ability created');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    try {
      const mainWindow: window.Window = windowStage.getMainWindowSync();
      this.mainWindow = mainWindow;
      this.keepScreenOn = undefined;
      void this.configureSystemBars(mainWindow);
      this.scheduleKeepScreenOnUpdate(true);
    } catch (error) {
      hilog.warn(DOMAIN, 'HSS', 'Main window unavailable: %{public}s', JSON.stringify(error));
    }
    windowStage.loadContent('pages/Index', (error) => {
      if (error.code) {
        hilog.error(DOMAIN, 'HSS', 'Failed to load Index: %{public}s', JSON.stringify(error));
      }
    });
  }

  onWindowStageDestroy(): void {
    this.releaseWindow(this.mainWindow);
  }

  onDestroy(): void {
    this.context.eventHub.off(PLAYBACK_ACTIVE_EVENT, this.playbackActiveListener);
    this.releaseWindow(this.mainWindow);
  }

  onForeground(): void {
    this.foreground = true;
    hssReceiver.onAppForeground();
    this.scheduleKeepScreenOnUpdate(true);
    hilog.info(DOMAIN, 'HSS', 'Receiver ability entered foreground');
  }

  onBackground(): void {
    this.foreground = false;
    this.scheduleKeepScreenOnUpdate(true);
    hssReceiver.onAppBackground();
    hilog.info(DOMAIN, 'HSS', 'Receiver ability entered background');
  }
}
