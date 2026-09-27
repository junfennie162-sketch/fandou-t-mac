import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import tmac from 'libtmac_hap.so';

@Entry
@Component
struct Index {
  @State status: string = '就绪';
  @State detail: string = '先「加载模型」，再「生成」。模型需预先推到设备（见 README）。';
  @State modelPath: string = '/data/local/tmp/llm-tmac/model.gguf';
  @State sandboxInfo: string = '';
  @State prompt: string = 'The capital of France is';
  @State nPredict: string = '16';
  @State threads: string = '4';
  @State useMmap: boolean = false;
  @State busy: boolean = false;
  @State loadInfo: string = '';
  @State output: string = '';

  aboutToAppear(): void {
    try {
      this.loadInfo = tmac.nativeVersion();
      hilog.info(0x0000, 'LUTSA', 'nativeVersion: %{public}s', this.loadInfo);
    } catch (e) {
      this.loadInfo = `native 模块加载失败: ${e}`;
    }
    // The app sandbox is the only place this process may read the model from on HarmonyOS 7
    // (/data/local/tmp is blocked by SELinux). chmod it so `hdc file send` can push the file in,
    // and default the model path to it.
    const fd = this.filesDir();
    if (fd.length > 0) {
      this.modelPath = `${fd}/model.gguf`;
      try {
        this.sandboxInfo = tmac.prepareSandbox(fd);
        hilog.info(0x0000, 'LUTSA', 'sandbox: %{public}s', this.sandboxInfo);
      } catch (e) {
        this.sandboxInfo = `prepareSandbox 异常: ${e}`;
      }
    }
  }

  private filesDir(): string {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      return ctx.filesDir;
    } catch (e) {
      return '';
    }
  }

  private num(s: string, dflt: number): number {
    const v = Number.parseInt(s);
    return Number.isNaN(v) ? dflt : v;
  }

  build() {
    Column({ space: 10 }) {
      Text('LUT-SA · T-MAC')
        .fontSize(26)
        .fontWeight(FontWeight.Bold)
        .fontColor('#4FA8E8')
      Text('查找表低比特 LLM 推理 · 鸿蒙原生（ArkTS + NAPI）')
        .fontSize(12)
        .fontColor('#9AA4B5')
      Text(this.loadInfo)
        .fontSize(11)
        .fontColor('#6B7A90')
      Text(this.sandboxInfo)
        .fontSize(10)
        .fontColor('#5A6A80')
        .maxLines(2)
        .textOverflow({ overflow: TextOverflow.Ellipsis })

      // ---- model path ----
      TextInput({ text: this.modelPath })
        .fontSize(12)
        .fontFamily('monospace')
        .onChange((v: string) => { this.modelPath = v; })

      // ---- params ----
      Row({ space: 10 }) {
        Text('线程').fontSize(12).fontColor('#9AA4B5')
        TextInput({ text: this.threads })
          .width(48).fontSize(12)
          .onChange((v: string) => { this.threads = v; })
        Text('生成长度').fontSize(12).fontColor('#9AA4B5')
        TextInput({ text: this.nPredict })
          .width(56).fontSize(12)
          .onChange((v: string) => { this.nPredict = v; })
        Text('mmap').fontSize(12).fontColor('#9AA4B5')
        Toggle({ type: ToggleType.Switch, isOn: this.useMmap })
          .onChange((v: boolean) => { this.useMmap = v; })
      }
      .width('92%')

      // ---- buttons ----
      Row({ space: 8 }) {
        Button('加载模型')
          .height(42).layoutWeight(1).enabled(!this.busy)
          .onClick(() => {
            this.busy = true;
            this.status = '⏳ 加载中（首次含内核变换，可能几十秒）…';
            this.detail = '';
            tmac.loadModelAsync(this.modelPath, this.filesDir(), this.num(this.threads, 4), 512, this.useMmap)
              .then((r: string) => {
                this.status = r.startsWith('OK') ? '✅ 模型已加载' : '❌ 加载失败';
                this.detail = r;
                hilog.info(0x0000, 'LUTSA', 'loadModel: %{public}s', r);
              })
              .catch((e: Object) => {
                this.status = '❌ 加载异常';
                this.detail = `loadModelAsync 异常: ${e}`;
              })
              .finally(() => { this.busy = false; });
          })

        Button('生成')
          .height(42).layoutWeight(1).enabled(!this.busy)
          .onClick(() => {
            this.busy = true;
            this.status = '⏳ 推理中…';
            this.output = '';
            tmac.generateAsync(this.prompt, this.num(this.nPredict, 16), 0.8, 40)
              .then((r: string) => {
                this.status = '✅ 生成完成';
                this.output = r;
                hilog.info(0x0000, 'LUTSA', 'generate done, %{public}d chars', r.length);
              })
              .catch((e: Object) => {
                this.status = '❌ 推理异常';
                this.output = `generateAsync 异常: ${e}`;
              })
              .finally(() => { this.busy = false; });
          })
      }
      .width('92%')

      Row({ space: 8 }) {
        Button('内核自测')
          .height(38).layoutWeight(1).enabled(!this.busy)
          .onClick(() => {
            const r: string = tmac.selfTest();
            this.status = r.includes('PASS') ? '✅ 内核自测通过' : '❌ 内核自测失败';
            this.detail = r;
          })
        Button('内核基准')
          .height(38).layoutWeight(1).enabled(!this.busy)
          .onClick(() => {
            this.detail = tmac.bench(200);
            this.status = '⏱ 基准完成';
          })
        Button('释放')
          .height(38).layoutWeight(1).enabled(!this.busy)
          .onClick(() => {
            this.detail = tmac.release();
            this.status = '已释放模型';
          })
      }
      .width('92%')

      // ---- prompt ----
      TextInput({ text: this.prompt, placeholder: '输入 prompt…' })
        .fontSize(13)
        .onChange((v: string) => { this.prompt = v; })

      Text(this.status)
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
        .fontColor('#E8EEF7')

      Scroll() {
        Text(this.output.length > 0 ? this.output : this.detail)
          .fontSize(12)
          .fontFamily('monospace')
          .textAlign(TextAlign.Start)
          .fontColor('#CFE0F5')
      }
      .width('92%')
      .layoutWeight(1)
      .padding(10)
      .backgroundColor('#1E2738')
      .borderRadius(8)
    }
    .width('100%')
    .height('100%')
    .padding({ top: 14 })
    .justifyContent(FlexAlign.Start)
    .backgroundColor('#141B2E')
  }
}
