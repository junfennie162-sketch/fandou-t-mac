import { hilog } from '@kit.PerformanceAnalysisKit';
import tmac from 'libtmac_hap.so';

@Entry
@Component
struct Index {
  @State status: string = '待运行';
  @State detail: string = '点击下方按钮，在鸿蒙设备/模拟器上运行 T-MAC LUT 内核。';

  build() {
    Column({ space: 18 }) {
      Text('LUT-SA · T-MAC')
        .fontSize(30)
        .fontWeight(FontWeight.Bold)
        .fontColor('#4FA8E8')
      Text('比特级查找表 LLM 推理内核 · 鸿蒙原生验证')
        .fontSize(14)
        .fontColor('#9AA4B5')

      Button('运行内核自测')
        .width('80%')
        .height(48)
        .onClick(() => {
          try {
            const r: string = tmac.selfTest();
            this.status = r.includes('PASS') ? '✅ 内核自测通过' : '❌ 内核自测失败';
            this.detail = r;
            hilog.info(0x0000, 'LUTSA', 'selfTest: %{public}s', r);
          } catch (e) {
            this.status = '❌ 异常';
            this.detail = `native 调用异常: ${e}`;
            hilog.error(0x0000, 'LUTSA', 'selfTest exception: %{public}s', JSON.stringify(e));
          }
        })

      Button('性能测试（100 次调用）')
        .width('80%')
        .height(48)
        .onClick(() => {
          try {
            this.detail = tmac.bench(100);
            this.status = '⏱ 性能测试完成';
          } catch (e) {
            this.status = '❌ 异常';
            this.detail = `native 调用异常: ${e}`;
          }
        })

      Text(this.status)
        .fontSize(20)
        .fontWeight(FontWeight.Medium)

      Scroll() {
        Text(this.detail)
          .fontSize(13)
          .fontFamily('monospace')
          .textAlign(TextAlign.Start)
      }
      .width('90%')
      .height(220)
      .padding(12)
      .backgroundColor('#1E2738')
      .borderRadius(8)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .backgroundColor('#141B2E')
  }
}
