import hssReceiver, { ReceiverStatus } from 'libhss_receiver.so';

@Entry
@Component
struct Index {
  @State hostAddress: string = '';
  @State pairingCode: string = '';
  @State statusText: string = '等待 Windows Host';
  @State isConnected: boolean = false;
  @State inputMode: string = 'pointer';
  @State framesDecoded: number = 0;
  @State framesDropped: number = 0;
  private statusTimer: number = -1;

  aboutToAppear(): void {
    this.statusTimer = setInterval(() => {
      const status: ReceiverStatus = hssReceiver.getStatus();
      this.statusText = status.detail;
      this.isConnected = status.connected;
      this.framesDecoded = status.framesDecoded;
      this.framesDropped = status.framesDropped;
    }, 250);
  }

  aboutToDisappear(): void {
    if (this.statusTimer >= 0) {
      clearInterval(this.statusTimer);
      this.statusTimer = -1;
    }
    hssReceiver.disconnect();
  }

  private connect(): void {
    if (this.hostAddress.trim().length === 0) {
      this.statusText = '请输入 Windows 主机 IP';
      return;
    }
    if (!/^\d{6}$/.test(this.pairingCode)) {
      this.statusText = '请输入 Windows Host 显示的六位一次性配对码';
      return;
    }
    if (!hssReceiver.connect(this.hostAddress.trim(), this.pairingCode)) {
      this.statusText = '连接参数无效';
    }
  }

  private toggleInputMode(): void {
    this.inputMode = this.inputMode === 'pointer' ? 'scroll' : 'pointer';
    hssReceiver.setInputMode(this.inputMode === 'pointer' ? 'pointer' : 'scroll');
  }

  build() {
    Column({ space: 20 }) {
      Row() {
        Column({ space: 4 }) {
          Text('Harmony Secondary Screen')
            .fontSize(28)
            .fontWeight(FontWeight.Bold)
            .fontColor('#F7FAFC')
          Text('HarmonyOS 原生接收端 · Wi-Fi only')
            .fontSize(15)
            .fontColor('#AAB6C5')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Text(this.isConnected ? '已连接' : '未连接')
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
        TextInput({ placeholder: 'Windows 主机 IP，例如 192.168.1.20', text: this.hostAddress })
          .layoutWeight(1)
          .height(48)
          .backgroundColor('#202A38')
          .fontColor('#F7FAFC')
          .placeholderColor('#718096')
          .onChange((value: string) => {
            this.hostAddress = value;
          })

        TextInput({ placeholder: '六位配对码', text: this.pairingCode })
          .width(150)
          .height(48)
          .maxLength(6)
          .backgroundColor('#202A38')
          .fontColor('#F7FAFC')
          .placeholderColor('#718096')
          .onChange((value: string) => {
            this.pairingCode = value;
          })

        Button('连接')
          .height(48)
          .fontColor('#07130C')
          .backgroundColor('#4ADE80')
          .onClick(() => this.connect())
      }
      .width('100%')

      Row({ space: 12 }) {
        Text(this.statusText)
          .layoutWeight(1)
          .fontSize(14)
          .fontColor('#AAB6C5')

        Text(`解码 ${this.framesDecoded} · 丢弃 ${this.framesDropped}`)
          .fontSize(13)
          .fontColor('#718096')

        Button(this.inputMode === 'pointer' ? '指针模式' : '滚动模式')
          .height(38)
          .fontSize(13)
          .backgroundColor('#2D3748')
          .onClick(() => this.toggleInputMode())
      }
        .width('100%')
    }
    .width('100%')
    .height('100%')
    .padding(28)
    .backgroundColor('#141C27')
  }
}
