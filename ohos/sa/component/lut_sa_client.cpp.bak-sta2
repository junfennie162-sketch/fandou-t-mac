// lut_sa_client.cpp —— LUT-SA 客户端：通过 samgr 找到 SA_ID=6901，逐项调用 ILutSa 的 IPC 接口
//
// 为什么需要它：SA 被 samgr「登记」只证明它注册了；**能从另一个进程跨 IPC 调通**，
// 才证明它作为「系统能力」真的可用（这也是 hidumper -s 6901 之外更硬的证据）。
//
// 用法：lut_sa_client [模型路径]    （不带参数时只做不需要模型的几项）
// 输出：stdout（由开机取证服务重定向到文件再打到串口）
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

int main(int argc, char **argv)
{
    const char *modelPath = (argc > 1) ? argv[1] : "";

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
        ErrCode e = proxy->LoadModel(modelPath, 4, 512);
        printf("  [LoadModel(%s)] ErrCode=%d\n", modelPath, static_cast<int>(e));
        if (e != 0) {
            fail += 1;
        }
        r.clear();
        ErrCode eg = proxy->Generate("The capital of France is", 8, 0.8, 40, r);
        fail += Call("Generate", eg, r);
        r.clear(); ErrCode er = proxy->Release(r); fail += Call("Release", er, r);
    } else {
        printf("  [LoadModel/Generate/Release] 跳过（未给模型路径）\n");
    }

    printf("  ---- 调用汇总：失败项 %d ----\n", fail);
    printf("=====LUTSA-CLIENT-END (rc=0)=====\n");
    return 0;
}
