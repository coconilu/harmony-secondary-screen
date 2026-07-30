import { common, ConfigurationConstant } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import { mdns } from '@kit.NetworkKit';
import { scanBarcode, scanCore } from '@kit.ScanKit';
import { window } from '@kit.ArkUI';
import hssReceiver, { PairingRecord, ReceiverStatus } from 'libhss_receiver.so';
import { PLAYBACK_ACTIVE_EVENT } from '../shared/ReceiverEvents';

const PREFS_NAME: string = 'trusted_edge';
const DEVICE_ID_KEY: string = 'deviceId';
const SENDER_ID_KEY: string = 'senderId';
const CREDENTIAL_KEY: string = 'credential';
const PAIRING_VERSION_KEY: string = 'pairingVersion';
const THEME_MODE_KEY: string = 'themeMode';
const DIRECT_PROTOCOL_VERSION: number = 5;
const DIRECT_PORT: number = 44000;
const FULLSCREEN_CONTROLS_TIMEOUT_MS: number = 5000;

@Entry
@Component
struct Index {
  @State listenAddress: string = '';
  @State shortCode: string = '';
  @State pairedAddress: string = '';
  @State deviceId: string = '';
  @State statusText: string = '正在检查当前 Wi-Fi';
  @State discoveryText: string = '尚未准备自动连接';
  @State isListening: boolean = false;
  @State isConnected: boolean = false;
  @State isPaired: boolean = false;
  @State framesDecoded: number = 0;
  @State framesDropped: number = 0;
  @State receivedFrames: number = 0;
  @State isFullscreen: boolean = false;
  @State fullscreenControlsVisible: boolean = false;
  @State themeMode: string = 'system';
  private statusTimer: number = -1;
  private fullscreenControlsTimer: number = -1;
  private store: preferences.Preferences | undefined = undefined;
  private savedPairingVersion: number = 0;
  private savedDeviceId: string = '';
  private registeredService: mdns.LocalServiceInfo | undefined = undefined;
  private dnsSdRegistered: boolean = false;
  private receivingRequested: boolean = false;
  private discoveryGeneration: number = 0;
  private receiverWindow: window.Window | undefined = undefined;
  private playbackActive: boolean = false;

  private abilityContext(): common.UIAbilityContext {
    const context: common.Context | undefined = this.getUIContext().getHostContext();
    if (context === undefined) {
      throw new Error('应用窗口上下文不可用');
    }
    return context as common.UIAbilityContext;
  }

  async aboutToAppear(): Promise<void> {
    await this.restoreSystemBars();
    await this.restoreTrust();
    const addresses: string[] = hssReceiver.getWifiAddresses();
    if (addresses.length === 1) {
      this.listenAddress = addresses[0];
      this.statusText = `已找到当前 Wi-Fi 地址 ${addresses[0]}`;
    } else if (addresses.length > 1) {
      this.listenAddress = addresses[0];
      this.statusText = '找到多个 Wi-Fi 地址，请确认平板当前使用的地址';
    } else {
      this.statusText = '未找到可用的 Wi-Fi 地址，请先把平板连接到 Wi-Fi';
    }
    this.statusTimer = setInterval(() => {
      this.refreshStatus();
    }, 250);
  }

  aboutToDisappear(): void {
    if (this.statusTimer >= 0) {
      clearInterval(this.statusTimer);
      this.statusTimer = -1;
    }
    this.hideFullscreenControls();
    void this.restoreSystemBars();
    this.updatePlaybackActive(false);
    this.stopReceiver();
  }

  onBackPress(): boolean {
    if (!this.isFullscreen) {
      return false;
    }
    void this.setFullscreen(false);
    return true;
  }

  private async restoreTrust(): Promise<void> {
    try {
      this.store = await preferences.getPreferences(this.abilityContext(), PREFS_NAME);
      const deviceId: string = String(await this.store.get(DEVICE_ID_KEY, ''));
      const senderId: string = String(await this.store.get(SENDER_ID_KEY, ''));
      const credential: string = String(await this.store.get(CREDENTIAL_KEY, ''));
      const storedThemeMode: string = String(await this.store.get(THEME_MODE_KEY, 'system'));
      this.themeMode =
        storedThemeMode === 'light' || storedThemeMode === 'dark' ? storedThemeMode : 'system';
      this.applyThemeColorMode();
      this.savedPairingVersion = Number(await this.store.get(PAIRING_VERSION_KEY, 0));
      this.savedDeviceId = deviceId;
      if (deviceId.length > 0 &&
        !hssReceiver.configureTrust(deviceId, senderId, credential, this.savedPairingVersion)) {
        this.statusText = '已保存的电脑连接信息无效，请移除电脑后重新连接';
      }
    } catch (error) {
      this.store = undefined;
      this.themeMode = 'system';
      this.applyThemeColorMode();
      this.statusText = '无法读取已保存的电脑连接信息';
    }
  }

  private refreshStatus(): void {
    const status: ReceiverStatus = hssReceiver.getStatus();
    const wasListening: boolean = this.isListening;
    this.statusText = status.detail;
    this.isListening = status.listening;
    this.isConnected = status.connected;
    this.isPaired = status.paired;
    this.pairedAddress = status.pairedAddress;
    this.deviceId = status.deviceId;
    this.framesDecoded = status.framesDecoded;
    this.framesDropped = status.framesDropped;
    this.receivedFrames = status.receivedFrames;
    this.updatePlaybackActive(status.connected);
    if ((wasListening || status.state === 'error') &&
      !status.listening && this.receivingRequested) {
      this.receivingRequested = false;
      this.discoveryGeneration += 1;
      void this.unregisterMdnsService();
    }
    this.refreshDiscoveryText(status);
    const pairing: PairingRecord = hssReceiver.getPairing();
    if ((pairing.version > this.savedPairingVersion ||
      (pairing.deviceId.length > 0 && pairing.deviceId !== this.savedDeviceId)) &&
      this.store !== undefined) {
      void this.persistPairing(pairing);
    }
  }

  private updatePlaybackActive(active: boolean): void {
    if (this.playbackActive === active) {
      return;
    }
    this.playbackActive = active;
    this.abilityContext().eventHub.emit(PLAYBACK_ACTIVE_EVENT, active);
  }

  private themeSelectionIndex(): number {
    if (this.themeMode === 'light') {
      return 1;
    }
    if (this.themeMode === 'dark') {
      return 2;
    }
    return 0;
  }

  private themeSelectionLabel(): string {
    if (this.themeMode === 'light') {
      return '浅色';
    }
    if (this.themeMode === 'dark') {
      return '深色';
    }
    return '跟随系统';
  }

  private applyThemeColorMode(): void {
    let colorMode: ConfigurationConstant.ColorMode =
      ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET;
    if (this.themeMode === 'light') {
      colorMode = ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT;
    } else if (this.themeMode === 'dark') {
      colorMode = ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    }
    try {
      this.abilityContext().setColorMode(colorMode);
    } catch (error) {
      this.statusText = '无法切换系统主题模式，请重试';
    }
  }

  private async selectTheme(index: number): Promise<void> {
    this.themeMode = index === 1 ? 'light' : index === 2 ? 'dark' : 'system';
    this.applyThemeColorMode();
    if (this.store !== undefined) {
      try {
        await this.store.put(THEME_MODE_KEY, this.themeMode);
        await this.store.flush();
      } catch (error) {
        this.statusText = '主题已切换，但暂时无法保存这个选择';
      }
    }
  }

  private clearFullscreenControlsTimer(): void {
    if (this.fullscreenControlsTimer >= 0) {
      clearTimeout(this.fullscreenControlsTimer);
      this.fullscreenControlsTimer = -1;
    }
  }

  private hideFullscreenControls(): void {
    this.clearFullscreenControlsTimer();
    this.fullscreenControlsVisible = false;
  }

  private showFullscreenControls(): void {
    this.clearFullscreenControlsTimer();
    this.fullscreenControlsVisible = true;
    this.fullscreenControlsTimer = setTimeout(() => {
      this.fullscreenControlsTimer = -1;
      this.fullscreenControlsVisible = false;
    }, FULLSCREEN_CONTROLS_TIMEOUT_MS);
  }

  private toggleFullscreenControls(): void {
    if (this.fullscreenControlsVisible) {
      this.hideFullscreenControls();
    } else {
      this.showFullscreenControls();
    }
  }

  private async setFullscreen(fullscreen: boolean): Promise<void> {
    try {
      if (this.receiverWindow === undefined) {
        this.receiverWindow = await window.getLastWindow(this.abilityContext());
      }
      await this.receiverWindow.setWindowLayoutFullScreen(fullscreen);
      await this.receiverWindow.setSpecificSystemBarEnabled('status', !fullscreen, true);
      await this.receiverWindow.setSpecificSystemBarEnabled('navigation', false, true);
      await this.receiverWindow.setSpecificSystemBarEnabled('navigationIndicator', false, true);
      this.isFullscreen = fullscreen;
      this.hideFullscreenControls();
    } catch (error) {
      this.receiverWindow = undefined;
      this.statusText = fullscreen ? '无法进入全屏，请重试' : '无法退出全屏，请使用系统返回手势重试';
    }
  }

  private async restoreSystemBars(): Promise<void> {
    try {
      const receiverWindow: window.Window =
        this.receiverWindow ?? await window.getLastWindow(this.abilityContext());
      await receiverWindow.setWindowLayoutFullScreen(false);
      await receiverWindow.setSpecificSystemBarEnabled('status', true);
      await receiverWindow.setSpecificSystemBarEnabled('navigation', false);
      await receiverWindow.setSpecificSystemBarEnabled('navigationIndicator', false);
    } catch (error) {
      // The window may already be unavailable while the page is disappearing.
    }
  }

  private async persistPairing(pairing: PairingRecord): Promise<void> {
    if (this.store === undefined) {
      return;
    }
    try {
      await this.store.put(DEVICE_ID_KEY, pairing.deviceId);
      await this.store.put(SENDER_ID_KEY, pairing.senderId);
      await this.store.put(CREDENTIAL_KEY, pairing.credential);
      await this.store.put(PAIRING_VERSION_KEY, pairing.version);
      await this.store.flush();
      this.savedPairingVersion = pairing.version;
      this.savedDeviceId = pairing.deviceId;
    } catch (error) {
      this.statusText = '无法保存连接信息；请保持应用打开并重试';
    }
  }

  private async startReceiver(): Promise<void> {
    if (this.listenAddress.trim().length === 0) {
      this.statusText = '请填写平板当前的 Wi-Fi 地址';
      return;
    }
    if (!hssReceiver.startReceiver(this.listenAddress.trim())) {
      this.statusText = '这个地址不是平板当前使用的 Wi-Fi 地址';
      return;
    }
    this.receivingRequested = true;
    await this.registerMdnsService();
  }

  private stopReceiver(): void {
    this.receivingRequested = false;
    this.discoveryGeneration += 1;
    this.updatePlaybackActive(false);
    hssReceiver.stopReceiver();
    void this.unregisterMdnsService();
  }

  private async registerMdnsService(): Promise<void> {
    const generation: number = ++this.discoveryGeneration;
    const service: mdns.LocalServiceInfo = {
      serviceType: '_hwc._tcp',
      serviceName: 'harmony-web-companion',
      port: DIRECT_PORT,
      host: {
        address: this.listenAddress,
        family: 1,
        port: DIRECT_PORT
      }
    };
    try {
      const registered: mdns.LocalServiceInfo =
        await mdns.addLocalService(this.abilityContext(), service);
      if (generation !== this.discoveryGeneration || !this.receivingRequested) {
        try {
          await mdns.removeLocalService(this.abilityContext(), registered);
        } catch (error) {
          // The receiver stopped while registration was in flight.
        }
        return;
      }
      this.registeredService = registered;
      this.dnsSdRegistered = true;
    } catch (error) {
      if (generation === this.discoveryGeneration) {
        this.registeredService = undefined;
        this.dnsSdRegistered = false;
      }
    }
  }

  private async unregisterMdnsService(): Promise<void> {
    this.dnsSdRegistered = false;
    if (this.registeredService === undefined) {
      this.discoveryText = '尚未准备自动连接';
      return;
    }
    const service: mdns.LocalServiceInfo = this.registeredService;
    this.registeredService = undefined;
    try {
      await mdns.removeLocalService(this.abilityContext(), service);
    } catch (error) {
      // Receiver socket has already stopped; stale DNS-SD cleanup is best effort.
    }
    this.discoveryText = '尚未准备自动连接';
  }

  private refreshDiscoveryText(status: ReceiverStatus): void {
    if (status.automaticAddressConflict) {
      this.discoveryText =
        `局域网内存在同名设备；请在电脑端输入本机地址 ${this.listenAddress}`;
      return;
    }
    if (status.automaticAddressPublished) {
      this.discoveryText = this.dnsSdRegistered
        ? '固定 .local 地址与设备服务均已发布'
        : '固定 .local 地址已发布；设备服务列表暂不可用';
      return;
    }
    if (status.automaticAddressFailed && status.listening) {
      this.discoveryText =
        `自动地址发布失败；请在电脑端输入本机地址 ${this.listenAddress}`;
      return;
    }
    if (status.listening || this.receivingRequested) {
      this.discoveryText = '正在检查固定 .local 地址是否可发布';
      return;
    }
    this.discoveryText = '尚未准备自动连接';
  }

  private async scanPairingQr(): Promise<void> {
    try {
      const result: scanBarcode.ScanResult = await scanBarcode.startScanForResult(this.abilityContext(), {
        scanTypes: [scanCore.ScanType.QR_CODE],
        enableMultiMode: false,
        enableAlbum: false
      });
      const value: Record<string, Object> = JSON.parse(result.originalValue) as Record<string, Object>;
      const version: number = Number(value['v']);
      const sessionId: string = String(value['sid']);
      const token: string = String(value['token']);
      const expiresAt: number = Number(value['exp']);
      if (version !== DIRECT_PROTOCOL_VERSION) {
        this.statusText = '电脑扩展与平板应用版本不兼容，请同时更新后重试';
        return;
      }
      if (!hssReceiver.authorizeQr(sessionId, token, expiresAt)) {
        this.statusText = '连接二维码无效或已经过期，请在电脑上刷新后重试';
      }
    } catch (error) {
      this.statusText = '扫码未完成；使用连接码时还需在扩展填写本机数字 IPv4';
    }
  }

  private authorizeShortCode(): void {
    if (!hssReceiver.authorizeShortCode(this.shortCode.trim())) {
      this.statusText = '请输入电脑上当前显示的 6 位连接码';
      return;
    }
    this.shortCode = '';
  }

  private async forgetDevice(): Promise<void> {
    hssReceiver.forgetDevice();
    const pairing: PairingRecord = hssReceiver.getPairing();
    await this.persistPairing(pairing);
    this.statusText = '已移除这台电脑，下次使用需要重新连接';
  }

  build() {
    if (this.isFullscreen) {
      Stack({ alignContent: Alignment.Center }) {
        XComponent({
          id: 'hssVideoSurface',
          type: XComponentType.SURFACE,
          libraryname: 'hss_receiver'
        })
          .width('100%')
          .aspectRatio(16 / 9)

        if (!this.isConnected) {
          Text(this.isListening ? '等待电脑发送画面' : '电脑画面已断开，请退出全屏后重新连接')
            .fontSize(16)
            .fontColor('#AAB6C5')
            .padding(14)
            .backgroundColor('#CC111A25')
            .borderRadius(12)
        }

        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('#01000000')
          .onClick(() => {
            this.toggleFullscreenControls();
          })

        if (this.fullscreenControlsVisible) {
          Column() {
            Row() {
              Blank()
              Button('退出全屏')
                .height(40)
                .fontColor('#F7FAFC')
                .backgroundColor('#CC182331')
                .onClick(() => {
                  void this.setFullscreen(false);
                })
            }
            .width('100%')
            Blank()
          }
          .width('100%')
          .height('100%')
          .padding(24)
        }
      }
      .width('100%')
      .height('100%')
      .backgroundColor('#000000')
    } else {
      Column({ space: 16 }) {
      Row() {
        Column({ space: 4 }) {
          Text('网页伴随屏')
            .fontSize(28)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('sys.color.ohos_id_color_text_primary'))
          Text('画面显示在平板，声音仍从电脑播放')
            .fontSize(15)
            .fontColor($r('sys.color.ohos_id_color_text_secondary'))
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Row({ space: 12 }) {
          Select([
            { value: '跟随系统' },
            { value: '浅色' },
            { value: '深色' }
          ])
            .selected(this.themeSelectionIndex())
            .value(this.themeSelectionLabel())
            .width(150)
            .height(40)
            .fontColor($r('sys.color.ohos_id_color_text_primary'))
            .backgroundColor($r('sys.color.ohos_id_color_sub_background'))
            .selectedOptionBgColor($r('sys.color.ohos_id_color_component_normal'))
            .selectedOptionFontColor($r('sys.color.ohos_id_color_text_primary'))
            .optionBgColor($r('sys.color.ohos_id_color_background'))
            .optionFontColor($r('sys.color.ohos_id_color_text_secondary'))
            .onSelect((index: number) => {
              void this.selectTheme(index);
            })

          Text(this.isConnected ? '正在播放' : this.isListening ? '已准备' : '未准备')
            .fontSize(14)
            .fontColor(this.isConnected ? '#22C55E' : '#D97706')
        }
      }
      .width('100%')
      .height(72)
      .alignItems(VerticalAlign.Center)

      Row({ space: 16 }) {
        Column({ space: 12 }) {
          Row() {
            Column({ space: 3 }) {
              Text('播放画面')
                .fontSize(20)
                .fontWeight(FontWeight.Bold)
                .fontColor($r('sys.color.ohos_id_color_text_primary'))
              Text(this.isConnected ? '来自电脑浏览器' : '连接后，电脑发送的画面会显示在这里')
                .fontSize(13)
                .fontColor($r('sys.color.ohos_id_color_text_secondary'))
            }
            .alignItems(HorizontalAlign.Start)
            .layoutWeight(1)

            Row({ space: 8 }) {
              Text(this.isConnected ? '正在播放' : '等待画面')
                .fontSize(13)
                .fontColor(this.isConnected ? '#22C55E' :
                  $r('sys.color.ohos_id_color_text_secondary'))

              Button('全屏播放')
                .height(36)
                .enabled(this.isListening || this.isConnected)
                .fontSize(13)
                .fontColor($r('sys.color.ohos_id_color_text_primary'))
                .backgroundColor($r('sys.color.ohos_id_color_component_normal'))
                .onClick(() => {
                  void this.setFullscreen(true);
                })
            }
          }
          .width('100%')

          Stack({ alignContent: Alignment.Center }) {
            XComponent({
              id: 'hssVideoSurface',
              type: XComponentType.SURFACE,
              libraryname: 'hss_receiver'
            })
            .width('100%')
            .height('100%')
            .borderRadius(16)
            .visibility(this.isConnected ? Visibility.Visible : Visibility.Hidden)

            if (!this.isConnected) {
              Column() {
                Text(this.isListening ? '等待电脑发送画面' : '请先在右侧准备接收')
                  .fontSize(16)
                  .fontColor($r('sys.color.ohos_id_color_text_tertiary'))
              }
              .width('100%')
              .height('100%')
              .justifyContent(FlexAlign.Center)
              .backgroundColor($r('sys.color.ohos_id_color_component_normal'))
              .zIndex(1)
            }
          }
          .width('100%')
          .aspectRatio(16 / 9)
          .backgroundColor(this.isConnected ? '#000000' :
            $r('sys.color.ohos_id_color_component_normal'))
          .borderRadius(16)
          .clip(true)
          .border({ width: 1, color: $r('sys.color.ohos_id_color_list_separator') })

          Text('画面会按原比例完整显示，空余区域会留黑边，不会拉伸或裁切。')
            .fontSize(13)
            .fontColor($r('sys.color.ohos_id_color_text_tertiary'))
        }
        .layoutWeight(1)
        .height('100%')
        .padding(18)
        .backgroundColor($r('sys.color.ohos_id_color_sub_background'))
        .borderRadius(20)
        .border({ width: 1, color: $r('sys.color.ohos_id_color_list_separator') })

        Column() {
          Scroll() {
            Column({ space: 14 }) {
          Text('连接电脑')
            .fontSize(20)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('sys.color.ohos_id_color_text_primary'))

          Text(this.isPaired
            ? '这台电脑已连接过，日常使用不需要再次扫码。'
            : '首次使用，先让电脑和平板建立连接。')
            .fontSize(14)
            .fontColor($r('sys.color.ohos_id_color_text_secondary'))
            .lineHeight(21)

          Text('平板 Wi-Fi 地址')
            .fontSize(13)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('sys.color.ohos_id_color_text_primary'))

          TextInput({ placeholder: '通常会自动填写', text: this.listenAddress })
            .width('100%')
            .height(46)
            .backgroundColor($r('sys.color.ohos_id_color_component_normal'))
            .fontColor($r('sys.color.ohos_id_color_text_primary'))
            .placeholderColor($r('sys.color.ohos_id_color_text_tertiary'))
            .onChange((value: string) => {
              this.listenAddress = value;
            })

          Text('通常无需修改；只有电脑找不到平板时才需要确认。')
            .fontSize(12)
            .fontColor($r('sys.color.ohos_id_color_text_tertiary'))

          Button(this.isListening || this.isConnected ? '停止接收画面' : '开始接收画面')
            .width('100%')
            .height(46)
            .fontColor('#07130C')
            .backgroundColor('#4ADE80')
            .onClick(() => {
              if (this.isListening || this.isConnected) {
                this.stopReceiver();
              } else {
                void this.startReceiver();
              }
            })

          Divider()
            .color($r('sys.color.ohos_id_color_list_separator'))

          if (!this.isPaired) {
            Text('首次连接')
              .fontSize(15)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('sys.color.ohos_id_color_text_primary'))

            Button('扫描电脑上的二维码')
              .width('100%')
              .height(44)
              .enabled(this.isListening)
              .onClick(() => {
                void this.scanPairingQr();
              })

            TextInput({ placeholder: '或输入电脑上的 6 位连接码', text: this.shortCode })
              .width('100%')
              .height(44)
              .maxLength(6)
              .type(InputType.Number)
              .onChange((value: string) => {
                this.shortCode = value;
              })

            Text('连接码模式还需在扩展“平板地址”中填写上方数字 IPv4。')
              .fontSize(12)
              .fontColor($r('sys.color.ohos_id_color_text_secondary'))

            Button('使用连接码')
              .width('100%')
              .height(44)
              .enabled(this.isListening)
              .fontColor($r('sys.color.ohos_id_color_text_primary'))
              .backgroundColor($r('sys.color.ohos_id_color_component_normal'))
              .onClick(() => {
                this.authorizeShortCode();
              })
          } else {
            Text('电脑已连接')
              .fontSize(15)
              .fontWeight(FontWeight.Medium)
              .fontColor('#22C55E')

            Text('在电脑浏览器中打开“网页伴随屏”，发送当前页面即可。')
              .fontSize(13)
              .fontColor($r('sys.color.ohos_id_color_text_secondary'))
              .lineHeight(20)

            Button('移除这台电脑')
              .width('100%')
              .height(44)
              .fontColor($r('sys.color.ohos_id_color_text_primary'))
              .backgroundColor($r('sys.color.ohos_id_color_component_normal'))
              .onClick(() => {
                void this.forgetDevice();
              })
          }

          Column({ space: 5 }) {
            Text('当前状态')
              .fontSize(12)
              .fontColor($r('sys.color.ohos_id_color_text_tertiary'))
            Text(this.statusText)
              .fontSize(13)
              .fontColor($r('sys.color.ohos_id_color_text_secondary'))
              .lineHeight(19)
            Text(this.discoveryText)
              .fontSize(12)
              .fontColor($r('sys.color.ohos_id_color_text_tertiary'))
              .lineHeight(18)
            }
            .alignItems(HorizontalAlign.Start)
            .width('100%')
            .padding(12)
            .backgroundColor($r('sys.color.ohos_id_color_component_normal'))
            .borderRadius(12)
            }
            .alignItems(HorizontalAlign.Start)
            .width('100%')
            .padding(18)
          }
          .width('100%')
          .layoutWeight(1)
          .scrollable(ScrollDirection.Vertical)
          .scrollBar(BarState.Auto)
        }
        .width(340)
        .height('100%')
        .backgroundColor($r('sys.color.ohos_id_color_sub_background'))
        .borderRadius(20)
        .border({ width: 1, color: $r('sys.color.ohos_id_color_list_separator') })
        .clip(true)
      }
      .width('100%')
      .layoutWeight(1)
      }
      .width('100%')
      .height('100%')
      .padding(24)
      .backgroundColor($r('sys.color.ohos_id_color_background'))
    }
  }
}
