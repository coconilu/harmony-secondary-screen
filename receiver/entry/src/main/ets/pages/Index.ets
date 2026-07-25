import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import { mdns } from '@kit.NetworkKit';
import { scanBarcode, scanCore } from '@kit.ScanKit';
import hssReceiver, { PairingRecord, ReceiverStatus } from 'libhss_receiver.so';

const PREFS_NAME: string = 'trusted_edge';
const DEVICE_ID_KEY: string = 'deviceId';
const SENDER_ID_KEY: string = 'senderId';
const CREDENTIAL_KEY: string = 'credential';
const PAIRING_VERSION_KEY: string = 'pairingVersion';
const DIRECT_PORT: number = 44000;

@Entry
@Component
struct Index {
  @State listenAddress: string = '';
  @State shortCode: string = '';
  @State pairedAddress: string = '';
  @State deviceId: string = '';
  @State statusText: string = '正在检查当前 Wi-Fi';
  @State discoveryText: string = '尚未注册局域网服务';
  @State isListening: boolean = false;
  @State isConnected: boolean = false;
  @State isPaired: boolean = false;
  @State framesDecoded: number = 0;
  @State framesDropped: number = 0;
  @State receivedFrames: number = 0;
  private statusTimer: number = -1;
  private store: preferences.Preferences | undefined = undefined;
  private savedPairingVersion: number = 0;
  private savedDeviceId: string = '';
  private registeredService: mdns.LocalServiceInfo | undefined = undefined;

  private abilityContext(): common.UIAbilityContext {
    const context: common.Context | undefined = this.getUIContext().getHostContext();
    if (context === undefined) {
      throw new Error('Receiver ability context is unavailable');
    }
    return context as common.UIAbilityContext;
  }

  async aboutToAppear(): Promise<void> {
    await this.restoreTrust();
    const addresses: string[] = hssReceiver.getWifiAddresses();
    if (addresses.length === 1) {
      this.listenAddress = addresses[0];
      this.statusText = `已检测到 Wi-Fi ${addresses[0]}，点击开始接收以确认`;
    } else if (addresses.length > 1) {
      this.listenAddress = addresses[0];
      this.statusText = '检测到多个 Wi-Fi IPv4，请确认后开始接收';
    } else {
      this.statusText = '没有可用的私网 Wi-Fi IPv4；不会绑定 VPN、蜂窝或公网地址';
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
    this.stopReceiver();
  }

  private async restoreTrust(): Promise<void> {
    try {
      this.store = await preferences.getPreferences(this.abilityContext(), PREFS_NAME);
      const deviceId: string = String(await this.store.get(DEVICE_ID_KEY, ''));
      const senderId: string = String(await this.store.get(SENDER_ID_KEY, ''));
      const credential: string = String(await this.store.get(CREDENTIAL_KEY, ''));
      this.savedPairingVersion = Number(await this.store.get(PAIRING_VERSION_KEY, 0));
      this.savedDeviceId = deviceId;
      if (deviceId.length > 0 &&
        !hssReceiver.configureTrust(deviceId, senderId, credential, this.savedPairingVersion)) {
        this.statusText = '本地配对凭据无效，请忘记设备后重新配对';
      }
    } catch (error) {
      this.store = undefined;
      this.statusText = '无法读取应用沙箱中的配对凭据';
    }
  }

  private refreshStatus(): void {
    const status: ReceiverStatus = hssReceiver.getStatus();
    this.statusText = status.detail;
    this.isListening = status.listening;
    this.isConnected = status.connected;
    this.isPaired = status.paired;
    this.pairedAddress = status.pairedAddress;
    this.deviceId = status.deviceId;
    this.framesDecoded = status.framesDecoded;
    this.framesDropped = status.framesDropped;
    this.receivedFrames = status.receivedFrames;
    const pairing: PairingRecord = hssReceiver.getPairing();
    if ((pairing.version > this.savedPairingVersion ||
      (pairing.deviceId.length > 0 && pairing.deviceId !== this.savedDeviceId)) &&
      this.store !== undefined) {
      void this.persistPairing(pairing);
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
      this.statusText = '无法保存配对凭据；请勿关闭 Receiver，并重试配对';
    }
  }

  private async startReceiver(): Promise<void> {
    if (this.listenAddress.trim().length === 0) {
      this.statusText = '请选择平板当前的私网 Wi-Fi IPv4';
      return;
    }
    if (!hssReceiver.startReceiver(this.listenAddress.trim())) {
      this.statusText = '只允许绑定平板当前启用的私网 Wi-Fi IPv4';
      return;
    }
    await this.registerMdnsService();
  }

  private stopReceiver(): void {
    hssReceiver.stopReceiver();
    void this.unregisterMdnsService();
  }

  private async registerMdnsService(): Promise<void> {
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
      this.registeredService = await mdns.addLocalService(this.abilityContext(), service);
      this.discoveryText = '已注册 DNS-SD 服务；若 .local 解析失败，请在扩展输入上方 Wi-Fi IPv4';
    } catch (error) {
      this.registeredService = undefined;
      this.discoveryText = `DNS-SD 注册失败，请在扩展手动输入 ${this.listenAddress}`;
    }
  }

  private async unregisterMdnsService(): Promise<void> {
    if (this.registeredService === undefined) {
      return;
    }
    const service: mdns.LocalServiceInfo = this.registeredService;
    this.registeredService = undefined;
    try {
      await mdns.removeLocalService(this.abilityContext(), service);
    } catch (error) {
      // Receiver socket has already stopped; stale DNS-SD cleanup is best effort.
    }
    this.discoveryText = '尚未注册局域网服务';
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
      if (version !== 3 || !hssReceiver.authorizeQr(sessionId, token, expiresAt)) {
        this.statusText = '二维码格式无效或已经过期';
      }
    } catch (error) {
      this.statusText = '扫码未完成；可输入扩展显示的六位一次性短码';
    }
  }

  private authorizeShortCode(): void {
    if (!hssReceiver.authorizeShortCode(this.shortCode.trim())) {
      this.statusText = '请输入扩展当前显示的六位一次性短码';
      return;
    }
    this.shortCode = '';
  }

  private async forgetDevice(): Promise<void> {
    hssReceiver.forgetDevice();
    const pairing: PairingRecord = hssReceiver.getPairing();
    await this.persistPairing(pairing);
    this.statusText = '已忘记电脑，需要重新扫码配对';
  }

  build() {
    Column({ space: 16 }) {
      Row() {
        Column({ space: 4 }) {
          Text('网页伴随屏')
            .fontSize(28)
            .fontWeight(FontWeight.Bold)
            .fontColor('#F7FAFC')
          Text('HarmonyOS 原生接收端 · 可信局域网 · 无音频')
            .fontSize(15)
            .fontColor('#AAB6C5')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Text(this.isConnected ? '正在显示' : this.isListening ? '等待 Edge' : '未启动')
          .fontSize(14)
          .fontColor(this.isConnected ? '#4ADE80' : '#FBBF24')
      }
      .width('100%')

      XComponent({
        id: 'hssVideoSurface',
        type: XComponentType.SURFACE,
        libraryname: 'hss_receiver'
      })
      .width('100%')
      .layoutWeight(1)
      .backgroundColor('#0B111A')
      .borderRadius(20)
      .border({ width: 1, color: '#2D3748' })

      Row({ space: 10 }) {
        TextInput({ placeholder: '当前 Wi-Fi 私网 IPv4', text: this.listenAddress })
          .layoutWeight(1)
          .height(46)
          .backgroundColor('#202A38')
          .fontColor('#F7FAFC')
          .placeholderColor('#718096')
          .onChange((value: string) => {
            this.listenAddress = value;
          })

        Button(this.isListening || this.isConnected ? '停止接收' : '确认并开始接收')
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
      }
      .width('100%')

      Row({ space: 10 }) {
        Button('扫码配对')
          .height(44)
          .enabled(this.isListening)
          .onClick(() => {
            void this.scanPairingQr();
          })

        TextInput({ placeholder: '摄像头不可用时输入六位短码', text: this.shortCode })
          .layoutWeight(1)
          .height(44)
          .maxLength(6)
          .type(InputType.Number)
          .onChange((value: string) => {
            this.shortCode = value;
          })

        Button('授权短码')
          .height(44)
          .enabled(this.isListening)
          .onClick(() => {
            this.authorizeShortCode();
          })

        Button('忘记电脑')
          .height(44)
          .enabled(this.isPaired)
          .backgroundColor('#334155')
          .onClick(() => {
            void this.forgetDevice();
          })
      }
      .width('100%')

      Column({ space: 4 }) {
        Text(this.statusText)
          .fontSize(14)
          .fontColor('#AAB6C5')
        Text(this.discoveryText)
          .fontSize(13)
          .fontColor('#718096')
        Text(`接收 ${this.receivedFrames} · 解码 ${this.framesDecoded} · 丢弃 ${this.framesDropped}`)
          .fontSize(13)
          .fontColor('#718096')
        Text(this.pairedAddress.length > 0
          ? `发送端 ${this.pairedAddress} · 设备 ${this.deviceId.slice(0, 8)}`
          : this.isPaired ? '已保存可信电脑，等待连接' : '尚未配对')
          .fontSize(13)
          .fontColor('#718096')
      }
      .alignItems(HorizontalAlign.Start)
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .padding(24)
    .backgroundColor('#141C27')
  }
}
