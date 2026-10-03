/**
 * EngineClient.ets —— 跨进程调用 LUT 引擎的**客户端**（任何应用照抄这段即可接进来）
 *
 * 流程：
 *   1) connectServiceExtensionAbility(want)  → 系统拉起/连上 LutEngineExtension（独立进程）
 *   2) onConnect 回调里拿到 rpc.RemoteObject
 *   3) MessageSequence 发请求：writeString(method) + writeString(argsJson)，回读 writeString 结果
 *
 * 为什么这样做：零售版 HarmonyOS 不允许第三方注册 SystemAbility，但
 * ServiceExtensionAbility + RPC 是**允许**的「系统服务化」形态：
 * 引擎在独立进程里持有模型与会话内存，UI 进程只发请求、不占内存。
 */
import { bundleManager, common, Want } from '@kit.AbilityKit';
import { rpc } from '@kit.IPCKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { EngineMethod } from './LutEngineExtension';

const TAG: string = 'LUTSA_CLI';
const DOMAIN: number = 0x0000;
/** 扩展的 descriptor：与服务端 LutEngineRemote 构造时用的一致 */
const ENGINE_DESCRIPTOR: string = 'com.fandou.lutsa.LutEngine';

/** 服务端应答的统一格式 */
interface EngineReply {
  ok: boolean;
  data?: string;
  err?: string;
}

export class EngineClient {
  private ctx: common.UIAbilityContext;
  private remote: rpc.IRemoteObject | null = null;
  private connected: boolean = false;
  /** 连接 id（connectServiceExtensionAbility 的返回值，断开时要用） */
  private connId: number = -1;

  constructor(ctx: common.UIAbilityContext) {
    this.ctx = ctx;
  }

  isConnected(): boolean {
    return this.connected;
  }

  /** 连上扩展（幂等）：连上后 remote 才可用 */
  async connect(): Promise<string> {
    if (this.connected) {
      return '已连接';
    }
    const want: Want = {
      bundleName: 'com.fandou.lutsa',
      abilityName: 'LutEngineExtension'
    };
    try {
      const self = this;
      // 注意：connectServiceExtensionAbility 是**同步返回**连接 id（不是 Promise）
      this.connId = this.ctx.connectServiceExtensionAbility(want, {
        onConnect: (elementName: bundleManager.ElementName, remote: rpc.IRemoteObject) => {
          self.remote = remote;
          self.connected = true;
          hilog.info(DOMAIN, TAG, '已连上引擎扩展: %{public}s', JSON.stringify(elementName));
        },
        onDisconnect: (elementName: bundleManager.ElementName) => {
          self.remote = null;
          self.connected = false;
          hilog.info(DOMAIN, TAG, '引擎扩展断开: %{public}s', JSON.stringify(elementName));
        },
        onFailed: (code: number) => {
          self.remote = null;
          self.connected = false;
          hilog.error(DOMAIN, TAG, '连接引擎扩展失败, code=%{public}d', code);
        }
      });
      return `连接已发起, connId=${this.connId}（remote ${this.remote === null ? '尚未就绪' : '已就绪'}）`;
    } catch (e) {
      const err = e as BusinessError;
      return `连接异常: code=${err.code} msg=${err.message}`;
    }
  }

  /** 断开（引擎进程若无人连接会被系统回收，等于把内存还给系统） */
  async disconnect(): Promise<string> {
    if (this.connId < 0) {
      return '未连接';
    }
    try {
      await this.ctx.disconnectServiceExtensionAbility(this.connId);
      this.remote = null;
      this.connected = false;
      this.connId = -1;
      return '已断开';
    } catch (e) {
      return `断开异常: ${(e as BusinessError).message}`;
    }
  }

  /**
   * 发一次跨进程请求。
   * @param method EngineMethod 里的方法名
   * @param args   JSON 字符串（无参数传 ''）
   */
  async call(method: string, args: string = ''): Promise<string> {
    const remote = this.remote;
    if (remote === null) {
      return `未连接（先 connect()）。方法=${method}`;
    }
    const data: rpc.MessageSequence = rpc.MessageSequence.create();
    const reply: rpc.MessageSequence = rpc.MessageSequence.create();
    try {
      data.writeString(method);
      data.writeString(args);
      const option: rpc.MessageOption = new rpc.MessageOption();
      await remote.sendMessageRequest(1, data, reply, option);
      const raw: string = reply.readString();
      const parsed: EngineReply = JSON.parse(raw) as EngineReply;
      if (parsed.ok) {
        return parsed.data ?? '(空)';
      }
      return `引擎返回错误: ${parsed.err ?? '未知'}`;
    } catch (e) {
      return `RPC 异常(${method}): ${(e as BusinessError).message}`;
    } finally {
      data.reclaim();
      reply.reclaim();
    }
  }

  /** 便捷封装：版本 / 自检 / 基准 */
  async nativeVersion(): Promise<string> {
    return this.call(EngineMethod.VERSION);
  }

  async selfTest(): Promise<string> {
    return this.call(EngineMethod.SELFTEST);
  }

  async bench(steps: number = 200): Promise<string> {
    return this.call(EngineMethod.BENCH, `${steps}`);
  }

  async loadModel(modelPath: string, filesDir: string, threads: number, nCtx: number,
                  useMmap: boolean): Promise<string> {
    return this.call(EngineMethod.LOAD_MODEL,
      JSON.stringify({ modelPath: modelPath, filesDir: filesDir, threads: threads, nCtx: nCtx, useMmap: useMmap }));
  }

  async generate(prompt: string, nPredict: number, temp: number, topK: number): Promise<string> {
    return this.call(EngineMethod.GENERATE,
      JSON.stringify({ prompt: prompt, nPredict: nPredict, temp: temp, topK: topK }));
  }

  async release(): Promise<string> {
    return this.call(EngineMethod.RELEASE);
  }
}
