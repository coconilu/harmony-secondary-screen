@Entry
@Component
struct Index {
  @State hostAddress: string = '';
  @State statusText: string = '等待 Windows Host';
  @State isConnected: boolean = false;

  private connect(): void {
    if (this.hostAddress.trim().length === 0) {
      this.statusText = '请输入 Windows 主机 IP';
      return;
    }

    this.statusText = '原生传输模块将在 v0.1 里程碑中接入';
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

      Column() {
        Text('原生 AVCodec Surface')
          .fontSize(18)
          .fontColor('#AAB6C5')
        Text('等待接收 Windows 虚拟显示器画面')
          .fontSize(14)
          .fontColor('#718096')
          .margin({ top: 8 })
      }
      .width('100%')
      .layoutWeight(1)
      .justifyContent(FlexAlign.Center)
      .backgroundColor('#0B111A')
      .borderRadius(20)
      .border({ width: 1, color: '#2D3748' })

      Row({ space: 12 }) {
        TextInput({ placeholder: 'Windows 主机 IP，例如 192.168.1.20', text: this.hostAddress })
          .layoutWeight(1)
          .height(48)
          .backgroundColor('#202A38')
          .fontColor('#F7FAFC')
          .placeholderColor('#718096')
          .onChange((value: string) => {
            this.hostAddress = value;
          })

        Button('连接')
          .height(48)
          .fontColor('#07130C')
          .backgroundColor('#4ADE80')
          .onClick(() => this.connect())
      }
      .width('100%')

      Text(this.statusText)
        .width('100%')
        .fontSize(14)
        .fontColor('#AAB6C5')
    }
    .width('100%')
    .height('100%')
    .padding(28)
    .backgroundColor('#141C27')
  }
}
