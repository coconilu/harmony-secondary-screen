import hssReceiver, { ReceiverStatus } from 'libhss_receiver.so';

@Entry
@Component
struct Index {
  @State listenAddress: string = '';
  @State pairingCode: string = '------';
  @State pairedAddress: string = '';
  @State statusText: string = '请输入平板当前的 Wi-Fi IPv4';
  @State isListening: boolean = false;
  @State isConnected: boolean = false;
  @State framesDecoded: number = 0;
  @State framesDropped: number = 0;
  private statusTimer: number = -1;

  aboutToAppear(): void {
    this.statusTimer = setInterval(() => {
      const status: ReceiverStatus = hssReceiver.getStatus();
      this.statusText = status.detail;
      this.isListening = status.listening;
      this.isConnected = status.connected;
      this.pairingCode = status.pairingCode.length > 0 ? status.pairingCode : '已使用';
      this.pairedAddress = status.pairedAddress;
      this.framesDecoded = status.framesDecoded;
      this.framesDropped = status.framesDropped;
    }, 250);
  }

  aboutToDisappear(): void {
    if (this.statusTimer >= 0) {
      clearInterval(this.statusTimer);
      this.statusTimer = -1;
    }
    hssReceiver.stopReceiver();
  }

  private startReceiver(): void {
    if (this.listenAddress.trim().length === 0) {
      this.statusText = '请输入平板当前的 Wi-Fi IPv4';
      return;
    }
    if (!hssReceiver.startReceiver(this.listenAddress.trim())) {
      this.statusText = '该地址不是平板当前启用的 Wi-Fi IPv4';
    }
  }

  private stopReceiver(): void {
    hssReceiver.stopReceiver();
  }

  build() {
    Column({ space: 20 }) {
      Row() {
        Column({ space: 4 }) {
          Text('网页伴随屏')
            .fontSize(28)
            .fontWeight(FontWeight.Bold)
            .fontColor('#F7FAFC')
          Text('HarmonyOS 原生接收端 · 仅可信局域网 · 无音频')
            .fontSize(15)
            .fontColor('#AAB6C5')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Text(this.isConnected ? '正在显示' : this.isListening ? '等待配对' : '未启动')
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
        TextInput({ placeholder: '平板 Wi-Fi IPv4，例如 192.168.1.30', text: this.listenAddress })
          .layoutWeight(1)
          .height(48)
          .backgroundColor('#202A38')
          .fontColor('#F7FAFC')
          .placeholderColor('#718096')
          .onChange((value: string) => {
            this.listenAddress = value;
          })

        Text(`配对码 ${this.pairingCode}`)
          .width(150)
          .fontSize(18)
          .fontColor('#F7FAFC')

        Button(this.isListening || this.isConnected ? '停止接收' : '开始接收')
          .height(48)
          .fontColor('#07130C')
          .backgroundColor('#4ADE80')
          .onClick(() => {
            if (this.isListening || this.isConnected) {
              this.stopReceiver();
            } else {
              this.startReceiver();
            }
          })
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

        Text(this.pairedAddress.length > 0 ? `发送端 ${this.pairedAddress}` : '尚未配对')
          .fontSize(13)
          .fontColor('#718096')
      }
        .width('100%')
    }
    .width('100%')
    .height('100%')
    .padding(28)
    .backgroundColor('#141C27')
  }
}
