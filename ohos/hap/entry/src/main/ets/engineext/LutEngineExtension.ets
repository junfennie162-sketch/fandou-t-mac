/**
 * LutEngineExtension.ets —— 把 LUT 推理引擎做成「可用别的应用拉起的系统服务组件」
 *
 * 为什么是 ServiceExtensionAbility 而不是 SystemAbility（SA）：
 *   零售版 HarmonyOS 不允许第三方注册 SA（实测六条锁：非 root / SELinux Enforcing /
 *   系统分区只读 / profile·init 目录不可访问 / `hdc target mount` 要求 debug 镜像 /
 *   两套 SDK 均无 samgr 头文件）。能落地的「系统服务化」形态就是 ServiceExtensionAbility：
 *   · 通过 module.json5 的 "process" 字段跑在**独立进程**里（引擎内存与会话不占 UI 进程）
 *   · "exported": true 后，别的应用可用 connectServiceExtensionAbility + Want 连上来
 *   · 连接后拿到 rpc.RemoteObject，用 MessageSequence 跨进程发请求
 *
 * 本文件是**服务端**（AppServiceExtensionAbility，等价于老接口 ServiceExtensionAbility）：
 * 把 native（libtmac_hap.so）的同步/异步接口包装成
 *   request  = writeString(method) + writeString(argsJson)
 *   response = writeString(resultJson)
 *
 * 客户端见 EngineClient.ets。
 */
// 本 SDK（HarmonyOS NEXT 系）里的后端服务扩展是 AppServiceExtensionAbility；
// 老接口 ServiceExtensionAbility 已不在 @kit.AbilityKit 中导出。
import { AppServiceExtensionAbility, Want } from '@kit.AbilityKit';
import { process } from '@kit.ArkTS';
import { rpc } from '@kit.IPCKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import tmac from 'libtmac_hap.so';

const TAG: string = 'LUTSA_EXT';
const DOMAIN: number = 0x0000;
const DESCRIPTOR: string = 'com.fandou.lutsa.LutEngine';

/** 请求方法名（客户端 EngineClient 里用同一套常量） */
export class EngineMethod {
  static readonly PING: string = 'ping';
  static readonly VERSION: string = 'nativeVersion';
  static readonly SELFTEST: string = 'selfTest';
  static readonly BENCH: string = 'bench';
  static readonly LOAD_MODEL: string = 'loadModel';
  static readonly GENERATE: string = 'generate';
  static readonly RELEASE: string = 'release';
  static readonly SANDBOX: string = 'prepareSandbox';
}

/** loadModel 的参数（用 JSON 传，避免 rpc 里塞复杂类型） */
interface LoadModelArgs {
  modelPath: string;
  filesDir: string;
  threads: number;
  nCtx: number;
  useMmap: boolean;
}

interface GenerateArgs {
  prompt: string;
  nPredict: number;
  temp: number;
  topK: number;
}

/**
 * 服务端 RemoteObject：每个跨进程请求进来，按 method 分派到 native 引擎。
 * 快方法同步处理；loadModel / generate 走 native 的 *Async，返回 Promise（不阻塞 IPC 线程）。
 */
class LutEngineRemote extends rpc.RemoteObject {
  constructor(descriptor: string) {
    super(descriptor);
  }

  /** 把结果按统一格式回给调用方：{"ok":true,"data":...} 或 {"ok":false,"err":"..."} */
  private static replyOk(reply: rpc.MessageSequence, data: string): void {
    reply.writeString(JSON.stringify({ ok: true, data: data }));
  }

  private static replyErr(reply: rpc.MessageSequence, err: string): void {
    reply.writeString(JSON.stringify({ ok: false, err: err }));
  }

  onRemoteMessageRequest(code: number, data: rpc.MessageSequence, reply: rpc.MessageSequence,
                         option: rpc.MessageOption): boolean | Promise<boolean> {
    let method: string = '';
    let args: string = '';
    try {
      method = data.readString();
      args = data.readString();
    } catch (e) {
      LutEngineRemote.replyErr(reply, `读取请求失败: ${(e as BusinessError).message}`);
      return true;
    }
    hilog.info(DOMAIN, TAG, '收到跨进程请求: %{public}s args=%{public}s', method, args);

    // ---- 快方法：同步回 ----
    try {
      if (method === EngineMethod.PING) {
        LutEngineRemote.replyOk(reply, `pong pid=${process.pid ?? -1}`);
        return true;
      }
      if (method === EngineMethod.VERSION) {
        LutEngineRemote.replyOk(reply, tmac.nativeVersion());
        return true;
      }
      if (method === EngineMethod.SELFTEST) {
        LutEngineRemote.replyOk(reply, tmac.selfTest());
        return true;
      }
      if (method === EngineMethod.BENCH) {
        let steps: number = Number.parseInt(args, 10);
        if (Number.isNaN(steps) || steps <= 0) {
          steps = 200;
        }
        LutEngineRemote.replyOk(reply, tmac.bench(steps));
        return true;
      }
      if (method === EngineMethod.RELEASE) {
        LutEngineRemote.replyOk(reply, tmac.release());
        return true;
      }
      if (method === EngineMethod.SANDBOX) {
        LutEngineRemote.replyOk(reply, tmac.prepareSandbox(args));
        return true;
      }
    } catch (e) {
      LutEngineRemote.replyErr(reply, `${method} 异常: ${(e as BusinessError).message}`);
      return true;
    }

    // ---- 慢方法：异步回（loadModel / generate 在设备上要秒到分钟级）----
    if (method === EngineMethod.LOAD_MODEL) {
      return new Promise<boolean>((resolve: (v: boolean) => void) => {
        try {
          const a: LoadModelArgs = JSON.parse(args) as LoadModelArgs;
          tmac.loadModelAsync(a.modelPath, a.filesDir, a.threads, a.nCtx, a.useMmap)
            .then((r: string) => {
              LutEngineRemote.replyOk(reply, r);
              resolve(true);
            })
            .catch((err: BusinessError) => {
              LutEngineRemote.replyErr(reply, `loadModel 失败: ${err.message}`);
              resolve(true);
            });
        } catch (e) {
          LutEngineRemote.replyErr(reply, `loadModel 参数解析失败: ${(e as BusinessError).message}`);
          resolve(true);
        }
      });
    }

    if (method === EngineMethod.GENERATE) {
      return new Promise<boolean>((resolve: (v: boolean) => void) => {
        try {
          const a: GenerateArgs = JSON.parse(args) as GenerateArgs;
          tmac.generateAsync(a.prompt, a.nPredict, a.temp, a.topK)
            .then((r: string) => {
              LutEngineRemote.replyOk(reply, r);
              resolve(true);
            })
            .catch((err: BusinessError) => {
              LutEngineRemote.replyErr(reply, `generate 失败: ${err.message}`);
              resolve(true);
            });
        } catch (e) {
          LutEngineRemote.replyErr(reply, `generate 参数解析失败: ${(e as BusinessError).message}`);
          resolve(true);
        }
      });
    }

    LutEngineRemote.replyErr(reply, `未知方法: ${method}`);
    return true;
  }
}

/** ServiceExtensionAbility：系统按 module.json5 的配置拉起它（独立进程 lut_engine） */
export default class LutEngineExtension extends AppServiceExtensionAbility {
  onCreate(want: Want): void {
    hilog.info(DOMAIN, TAG, 'LutEngineExtension onCreate, want=%{public}s', JSON.stringify(want));
  }

  onConnect(want: Want): rpc.RemoteObject {
    hilog.info(DOMAIN, TAG, 'LutEngineExtension onConnect: %{public}s', JSON.stringify(want));
    return new LutEngineRemote(DESCRIPTOR);
  }

  onDisconnect(want: Want): void {
    hilog.info(DOMAIN, TAG, 'LutEngineExtension onDisconnect');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'LutEngineExtension onDestroy');
  }
}
