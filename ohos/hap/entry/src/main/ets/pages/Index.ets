import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { picker, fileIo as fs } from '@kit.CoreFileKit';
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

  private doLoad(): void {
    if (this.busy) {
      return;
    }
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
  }

  // Product-grade model import: system file picker -> copy into the app sandbox -> load.
  // No shell/hdc needed, works on any device regardless of sandbox policy.
  private async pickAndImportModel(): Promise<void> {
    if (this.busy) {
      return;
    }
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const doc = new picker.DocumentViewPicker(ctx);
      const uris = await doc.select({ maxSelectNumber: 1, fileSuffixFilters: ['.gguf'] } as picker.DocumentSelectOptions);
      if (!uris || uris.length === 0) {
        this.status = '已取消选择';
        return;
      }
      this.busy = true;
      this.status = '⏳ 正在把模型拷进应用沙箱…（请保持应用在前台）';
      const dest = `${ctx.filesDir}/model.gguf`;
      const srcFile = fs.openSync(uris[0], fs.OpenMode.READ_ONLY);
      const dstFile = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      const sizeMb = Math.round(fs.statSync(srcFile.fd).size / 1024 / 1024);
      await this.copyWithGuard(srcFile.fd, dstFile.fd);
      fs.closeSync(srcFile);
      fs.closeSync(dstFile);
      this.modelPath = dest;
      this.status = `✅ 模型已导入（${sizeMb} MB）`;
      this.detail = `source: ${uris[0]}\ndest:   ${dest}`;
      hilog.info(0x0000, 'LUTSA', 'imported model, %{public}d MB -> %{public}s', sizeMb, dest);
      this.busy = false;
      this.doLoad();   // chain straight into loading
    } catch (e) {
      this.busy = false;
      this.status = '❌ 导入失败';
      this.detail = `文件选择/拷贝异常: ${e}`;
      hilog.error(0x0000, 'LUTSA', 'import failed: %{public}s', JSON.stringify(e));
    }
  }

  private num(s: string, dflt: number): number {
    const v = Number.parseInt(s);
    return Number.isNaN(v) ? dflt : v;
  }

  // Big file copies must not run on the JS thread (a synchronous 1 GB copy freezes the UI and the
  // system kills the app: appfreeze / THREAD_BLOCK_6S). Copy on a NAPI worker thread instead, and
  // hold a transient task so the process survives going to the background.
  private async copyWithGuard(srcFd: number, dstFd: number): Promise<string> {
    let requestId = -1;
    try {
      const info = backgroundTaskManager.requestSuspendDelay('model-copy', () => {
        hilog.warn(0x0000, 'LUTSA', 'suspend delay expired during copy');
      });
      requestId = info.requestId;
    } catch (e) {
      hilog.warn(0x0000, 'LUTSA', 'requestSuspendDelay failed: %{public}s', JSON.stringify(e));
    }
    try {
      return await tmac.copyFdAsync(srcFd, dstFd);
    } finally {
      if (requestId >= 0) {
        try {
          backgroundTaskManager.cancelSuspendDelay(requestId);
        } catch (e) {
          hilog.warn(0x0000, 'LUTSA', 'cancelSuspendDelay failed');
        }
      }
    }
  }

  // Export the sandbox model to phone storage via the system SAVE dialog.
  // Real feature (share/backup the converted GGUF) and it also makes the picker-import flow
  // testable on an emulator whose user storage cannot be written from outside.
  private async exportModel(): Promise<void> {
    if (this.busy) {
      return;
    }
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const src = `${ctx.filesDir}/model.gguf`;
      const st = fs.statSync(src);
      if (st.size <= 0) {
        this.status = '⚠️ 沙箱里没有模型可导出';
        return;
      }
      const doc = new picker.DocumentViewPicker(ctx);
      // unique name: the system save dialog refuses to overwrite an existing file
      const fileName = `bitnet-3b-tmac-${Date.now()}.gguf`;
      const uris = await doc.save({ newFileNames: [fileName] } as picker.DocumentSaveOptions);
      if (!uris || uris.length === 0) {
        this.status = '已取消导出';
        return;
      }
      this.busy = true;
      this.status = `⏳ 正在导出模型（${Math.round(st.size / 1024 / 1024)} MB，请保持应用在前台）…`;
      const srcFile = fs.openSync(src, fs.OpenMode.READ_ONLY);
      const dstFile = fs.openSync(uris[0], fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      await this.copyWithGuard(srcFile.fd, dstFile.fd);
      fs.closeSync(srcFile);
      fs.closeSync(dstFile);
      this.status = '✅ 模型已导出';
      this.detail = `dest: ${uris[0]}\n现在可以用「📂 选择模型文件」把它选回来（验证导入链路）`;
      hilog.info(0x0000, 'LUTSA', 'exported model to %{public}s', uris[0]);
    } catch (e) {
      this.status = '❌ 导出失败';
      this.detail = `导出异常: ${e}`;
      hilog.error(0x0000, 'LUTSA', 'export failed: %{public}s', JSON.stringify(e));
    } finally {
      this.busy = false;
    }
  }

  private doGenerate(): void {
    if (this.busy) {
      return;
    }
    if (this.prompt.length === 0) {
      this.status = '⚠️ 请先输入提示词';
      return;
    }
    this.busy = true;
    this.status = '⏳ 推理中…';
    this.output = '';
    const asked = this.prompt;
    tmac.generateAsync(asked, this.num(this.nPredict, 16), 0.8, 40)
      .then((r: string) => {
        this.status = '✅ 生成完成';
        this.output = `提示词: ${asked}\n────\n${r}`;
        hilog.info(0x0000, 'LUTSA', 'generate done, %{public}d chars', r.length);
      })
      .catch((e: Object) => {
        this.status = '❌ 推理异常';
        this.output = `generateAsync 异常: ${e}`;
      })
      .finally(() => { this.busy = false; });
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
      Text('📁 模型路径')
        .fontSize(11).fontColor('#7A8AA0').width('92%').textAlign(TextAlign.Start)
      TextInput({ text: this.modelPath })
        .width('92%').height(44).fontSize(12).fontFamily('monospace')
        .fontColor('#CFE0F5').backgroundColor('#1E2738')
        .borderRadius(8).border({ width: 1, color: '#33405A' })
        .padding({ left: 12, right: 12 })
        .onChange((v: string) => { this.modelPath = v; })

      // ---- params ----
      Row({ space: 10 }) {
        Text('线程').fontSize(12).fontColor('#9AA4B5')
        TextInput({ text: this.threads })
          .width(52).height(40).fontSize(12).textAlign(TextAlign.Center)
          .fontColor('#E8EEF7').backgroundColor('#1E2738').borderRadius(8)
          .border({ width: 1, color: '#33405A' })
          .onChange((v: string) => { this.threads = v; })
        Text('生成长度').fontSize(12).fontColor('#9AA4B5')
        TextInput({ text: this.nPredict })
          .width(60).height(40).fontSize(12).textAlign(TextAlign.Center)
          .fontColor('#E8EEF7').backgroundColor('#1E2738').borderRadius(8)
          .border({ width: 1, color: '#33405A' })
          .onChange((v: string) => { this.nPredict = v; })
        Text('mmap').fontSize(12).fontColor('#9AA4B5')
        Toggle({ type: ToggleType.Switch, isOn: this.useMmap })
          .onChange((v: boolean) => { this.useMmap = v; })
      }
      .width('92%')

      // ---- buttons ----
      Row({ space: 8 }) {
        Button('📂 选择模型文件')
          .height(42).layoutWeight(1).enabled(!this.busy)
          .onClick(() => { this.pickAndImportModel(); })
        Button('📤 导出到手机')
          .height(42).layoutWeight(1).enabled(!this.busy)
          .onClick(() => { this.exportModel(); })
      }
      .width('92%')

      Row({ space: 8 }) {
        Button('加载模型')
          .height(42).layoutWeight(1).enabled(!this.busy)
          .onClick(() => { this.doLoad(); })
        Button('生成')
          .height(42).layoutWeight(1).enabled(!this.busy)
          .onClick(() => { this.doGenerate(); })
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

      // ---- prompt (the main interactive box) ----
      Text('✍️ 提示词（输入后回车，或点「生成」）')
        .fontSize(11).fontColor('#7A8AA0').width('92%').textAlign(TextAlign.Start)
      TextInput({ text: this.prompt, placeholder: '在这里输入提示词…' })
        .width('92%').height(50).fontSize(15)
        .fontColor('#FFFFFF').placeholderColor('#5A6A80')
        .backgroundColor('#243044').borderRadius(8)
        .border({ width: 1.5, color: '#4FA8E8' })
        .padding({ left: 12, right: 12 })
        .enterKeyType(EnterKeyType.Go)
        .onChange((v: string) => { this.prompt = v; })
        .onSubmit(() => { this.doGenerate(); })
      Text('base 模型：按你的提示词续写（不是问答）；英文语料为主，中文续写质量一般')
        .fontSize(10)
        .fontColor('#5A6A80')

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
