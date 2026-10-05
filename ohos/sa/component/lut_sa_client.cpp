// lut_sa_client.cpp —— LUT-SA 客户端：通过 samgr 找到 SA_ID=6901，逐项调用 ILutSa 的 IPC 接口
//
// 为什么需要它：SA 被 samgr「登记」只证明它注册了；**能从另一个进程跨 IPC 调通**，
// 才证明它作为「系统能力」真的可用（这也是 hidumper -s 6901 之外更硬的证据）。
//
// 用法：lut_sa_client [模型路径]    （不带参数时只做不需要模型的几项）
// 输出：stdout（由开机取证服务重定向到文件再打到串口）
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "iservice_registry.h"
#include "if_system_ability_manager.h"
#include "iremote_object.h"

#include "ilut_sa.h"
#include "lut_sa_proxy.h"

using namespace OHOS;
using namespace OHOS::LutSa;

namespace {
constexpr int32_t kLutSaId = 6901;

// 返回 0 表示调用成功（ErrCode 为 0）
int Call(const char *tag, ErrCode err, const std::string &value)
{
    printf("  [%s] ErrCode=%d", tag, static_cast<int>(err));
    if (!value.empty()) {
        printf("  -> %s", value.c_str());
    }
    printf("\n");
    return err == 0 ? 0 : 1;
}
}  // namespace

static int RunStress(sptr<ILutSa> proxy)
{
    printf("=====LUTSA-STRESS-START=====\n");
    int bad = 0;
    std::string r;

    int ok_cnt = 0;
    for (int i = 0; i < 3; ++i) {
        r.clear();
        if (proxy->NativeVersion(r) == 0) { ++ok_cnt; }
    }
    printf("  [repeat] NativeVersion x3 -> ok %d/3\n", ok_cnt);
    if (ok_cnt != 3) { ++bad; }

    for (int i = 0; i < 2; ++i) {
        r.clear();
        ErrCode e = proxy->SelfTest(r);
        printf("  [repeat] SelfTest #%d rc=%d\n", i + 1, static_cast<int>(e));
        if (e != 0) { ++bad; }
    }

    r.clear();
    ErrCode eg = proxy->Generate("hello", 4, 0.8, 40, r);
    printf("  [generate-without-load] rc=%d (expect non-zero)\n", static_cast<int>(eg));
    if (eg == 0) { ++bad; }

    ErrCode el = proxy->LoadModel("/data/local/tmp/definitely-not-exist.gguf", 1, 128);
    printf("  [load-bad-path] rc=%d (expect non-zero)\n", static_cast<int>(el));
    if (el == 0) { ++bad; }

    r.clear();
    ErrCode er = proxy->Release(r);
    printf("  [release] rc=%d\n", static_cast<int>(er));
    r.clear();
    ErrCode em = proxy->GetMetrics(r);
    printf("  [metrics-after-release] rc=%d -> %s\n", static_cast<int>(em), r.c_str());

    r.clear();
    ErrCode ef = proxy->SelfTest(r);
    const bool alive = (ef == 0);
    printf("  [alive-after-abuse] SelfTest rc=%d %s\n", static_cast<int>(ef), alive ? "PASS" : "FAIL");
    if (!alive) { ++bad; }

    printf("  ---- robustness summary: bad=%d ----\n", bad);
    printf("=====LUTSA-STRESS-END=====\n");
    return bad;
}
int main(int argc, char **argv)
{
    // stdout 重定向到文件时默认是块缓冲：客户端一旦卡在某个 IPC 调用上，已打印的行全留在缓冲区里
    // （STA-3 踩过：文件里只剩 START，看不出卡在哪一步）→ 改行缓冲，每行都立刻落盘
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    const char *modelPath = (argc > 1 && std::strcmp(argv[1], "--action") != 0) ? argv[1] : "";

    printf("=====LUTSA-CLIENT-START=====\n");
    printf("  client pid=%d argc=%d\n", static_cast<int>(getpid()), argc);

    // 1) 拿到 samgr
    auto samgr = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgr == nullptr) {
        printf("  [samgr] NULL —— 拿不到 SystemAbilityManager\n");
        printf("=====LUTSA-CLIENT-END (rc=2)=====\n");
        return 2;
    }
    printf("  [samgr] ok\n");

    // 2) 按 SA_ID=6901 取远端对象（这一步会真正跨进程去 samgr 查表）
    sptr<IRemoteObject> object = samgr->GetSystemAbility(kLutSaId);
    if (object == nullptr) {
        printf("  [GetSystemAbility(%d)] NULL —— samgr 里没有该 SA\n", kLutSaId);
        printf("=====LUTSA-CLIENT-END (rc=3)=====\n");
        return 3;
    }
    printf("  [GetSystemAbility(%d)] ok, remote=%p\n", kLutSaId, object.GetRefPtr());

    // 3) 转成 ILutSa 代理
    sptr<ILutSa> proxy = iface_cast<ILutSa>(object);
    if (proxy == nullptr) {
        printf("  [iface_cast] NULL —— 描述符不匹配\n");
        printf("=====LUTSA-CLIENT-END (rc=4)=====\n");
        return 4;
    }
    printf("  [iface_cast] ok —— IPC 代理已建立\n");

    if (argc > 1 && std::strcmp(argv[1], "--stress") == 0) {
        const int bad = RunStress(proxy);
        printf("=====LUTSA-CLIENT-END (rc=%d)=====\n", bad == 0 ? 0 : 1);
        return bad == 0 ? 0 : 1;
    }

    // S6-1：系统级动作执行 —— lut_sa_client --action <action> <arg>
    if (argc > 2 && std::strcmp(argv[1], "--action") == 0) {
        const std::string action = argv[2];
        const std::string arg = (argc > 3) ? argv[3] : "";
        printf("  [ExecuteAction(%s, %s)]\n", action.c_str(), arg.c_str());
        std::string ar;
        ErrCode ea = proxy->ExecuteAction(action, arg, ar);
        printf("  [ExecuteAction] ErrCode=%d  -> %s\n", static_cast<int>(ea), ar.c_str());
        std::string mr;
        ErrCode em = proxy->GetMetrics(mr);
        if (em == 0) {
            const size_t q = mr.find("last_action:");
            if (q != std::string::npos) {
                printf("  [metrics] %s\n", mr.substr(q, 200).c_str());
            }
        }
        printf("=====LUTSA-CLIENT-END (rc=%d)=====\n", ea == 0 ? 0 : 1);
        return ea == 0 ? 0 : 1;
    }

    // S6-2：自然语言意图 —— lut_sa_client --intent "打开设置"
    // 给了模型路径就先 LoadModel（否则引擎是 unloaded，source 必然是 keyword —— 模型路径就测不到）
    if (argc > 2 && std::strcmp(argv[1], "--intent") == 0) {
        const std::string utt = argv[2];
        const bool with_model = (argc > 3);
        if (with_model) {
            ErrCode el = proxy->LoadModel(argv[3], 4, 512);
            printf("  [LoadModel(%s)] ErrCode=%d\n", argv[3], static_cast<int>(el));
        }
        printf("  [ExecuteIntent(%s)]\n", utt.c_str());
        std::string ir;
        ErrCode ei = proxy->ExecuteIntent(utt, ir);
        printf("  [ExecuteIntent] ErrCode=%d  -> %s\n", static_cast<int>(ei), ir.c_str());
        if (with_model) {
            std::string rr;
            ErrCode er = proxy->Release(rr);
            printf("  [Release] ErrCode=%d\n", static_cast<int>(er));
        }
        printf("=====LUTSA-CLIENT-END (rc=%d)=====\n", ei == 0 ? 0 : 1);
        return ei == 0 ? 0 : 1;
    }

    // 只加载不释放（用于验证"引擎是否在多次 IPC 调用之间存活"）
    if (argc > 2 && std::strcmp(argv[1], "--load-keep") == 0) {
        ErrCode el = proxy->LoadModel(argv[2], 4, 512);
        printf("  [--load-keep %s] ErrCode=%d\n", argv[2], static_cast<int>(el));
        printf("=====LUTSA-CLIENT-END (rc=%d)=====\n", el == 0 ? 0 : 1);
        return el == 0 ? 0 : 1;
    }

    // 只打指标（顺带看 engine= 字段 —— 判断上一次 --load-keep 之后引擎还在不在）
    if (argc > 1 && std::strcmp(argv[1], "--metrics") == 0) {
        std::string mr;
        ErrCode em = proxy->GetMetrics(mr);
        printf("  [--metrics] ErrCode=%d  -> %s\n", static_cast<int>(em), mr.c_str());
        printf("=====LUTSA-CLIENT-END (rc=%d)=====\n", em == 0 ? 0 : 1);
        return em == 0 ? 0 : 1;
    }

    // 快模式：只做 加载 → 指标 → 释放（不生成），给取证脚本反复验证用，省时间
    if (argc > 2 && std::strcmp(argv[1], "--load") == 0) {
        const std::string path = argv[2];
        printf("  [--load %s]\n", path.c_str());
        ErrCode el = proxy->LoadModel(path, 4, 512);
        printf("  [LoadModel] ErrCode=%d\n", static_cast<int>(el));
        std::string mr;
        ErrCode em = proxy->GetMetrics(mr);
        if (em == 0) {
            const size_t q = mr.find("quota:");
            if (q != std::string::npos) {
                printf("  [metrics] %s\n", mr.substr(q, 160).c_str());
            }
        }
        ErrCode er = proxy->Release(mr);
        printf("  [Release] ErrCode=%d\n", static_cast<int>(er));
        printf("=====LUTSA-CLIENT-END (rc=%d)=====\n", el == 0 ? 0 : 1);
        return el == 0 ? 0 : 1;
    }

    int fail = 0;
    std::string r;

    // 4) 逐项调用（不需要模型的三项先来）
    // 注意：必须先把返回值取出来再传给 Call —— 参数求值顺序未定义，
    // 直接写 Call("x", proxy->X(r), r) 可能先拷贝空的 r，把结果串吃掉
    r.clear(); ErrCode e0 = proxy->NativeVersion(r); fail += Call("NativeVersion", e0, r);
    r.clear(); ErrCode e1 = proxy->SelfTest(r);      fail += Call("SelfTest", e1, r);
    r.clear(); ErrCode e2 = proxy->GetMetrics(r);    fail += Call("GetMetrics", e2, r);

    // 5) 需要模型路径的两项：有参数就带，没有就跳（无模型时预期返回错误码，但足以证明 IPC 通）
    if (modelPath[0] != '\0') {
        const auto t0 = std::chrono::steady_clock::now();
        ErrCode e = proxy->LoadModel(modelPath, 4, 512);
        const auto t1 = std::chrono::steady_clock::now();
        printf("  [LoadModel(%s)] ErrCode=%d (%.0f ms)\n", modelPath, static_cast<int>(e),
               std::chrono::duration<double, std::milli>(t1 - t0).count());
        if (e != 0) {
            fail += 1;
        }

        // STA-3 真推理三连：
        //  #1 采样（temp=0.8）+ #2 同 prompt 再来一次 → 文本必须完全一致（固定种子 + 每次清 KV）
        //  #3 换 prompt + 贪心（temp=0）→ 文本必须不同（排除"返回常量串"这种假通过）
        r.clear();
        ErrCode eg = proxy->Generate("The capital of France is", 24, 0.8, 40, r);
        fail += Call("Generate#1(sampling)", eg, r);
        const std::string g1 = r;

        r.clear();
        ErrCode eg2 = proxy->Generate("The capital of France is", 24, 0.8, 40, r);
        fail += Call("Generate#2(同 prompt 复现)", eg2, r);
        const bool same = (!g1.empty() && g1 == r);
        printf("  [determinism] 两次同 prompt 输出一致: %s\n", same ? "yes" : "no");
        if (!same) {
            fail += 1;
        }

        r.clear();
        ErrCode eg3 = proxy->Generate("2 + 2 =", 16, 0.0, 40, r);
        fail += Call("Generate#3(贪心/换 prompt)", eg3, r);
        const bool differ = (!r.empty() && r != g1);
        printf("  [non-constant] 换 prompt 输出不同: %s\n", differ ? "yes" : "no");
        if (!differ) {
            fail += 1;
        }

        r.clear(); ErrCode em = proxy->GetMetrics(r);  fail += Call("GetMetrics(引擎态)", em, r);
        r.clear(); ErrCode er = proxy->Release(r);     fail += Call("Release", er, r);
    } else {
        printf("  [LoadModel/Generate/Release] 跳过（未给模型路径）\n");
    }

    printf("  ---- 调用汇总：失败项 %d ----\n", fail);
    printf("=====LUTSA-CLIENT-END (rc=0)=====\n");
    return 0;
}
