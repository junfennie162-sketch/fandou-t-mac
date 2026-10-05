#ifndef OHOS_LUTSA_ILUTSA_H
#define OHOS_LUTSA_ILUTSA_H

#include <cstdint>
#include <iremote_broker.h>
#include <string_ex.h>

namespace OHOS {
namespace LutSa {

enum class ILutSaIpcCode {
    COMMAND_NATIVE_VERSION = MIN_TRANSACTION_ID,
    COMMAND_LOAD_MODEL,
    COMMAND_GENERATE,
    COMMAND_GET_METRICS,
    COMMAND_SELF_TEST,
    COMMAND_RELEASE,
    COMMAND_EXECUTE_ACTION,
    COMMAND_EXECUTE_INTENT,
    // S7-1b：感知（世界→Agent）与执行（Agent→世界）是两个方向，各自独立的方法 ——
    // 不复用 ExecuteAction；以后的 ReadCamera/ReadAudio/ReadSensor 按同一条线走。
    COMMAND_READ_SCREEN,
};

class ILutSa : public IRemoteBroker {
public:
    DECLARE_INTERFACE_DESCRIPTOR(u"OHOS.LutSa.ILutSa");

    virtual ErrCode NativeVersion(
        std::string& funcResult) = 0;

    virtual ErrCode LoadModel(
        const std::string& path,
        int32_t threads,
        int32_t nCtx) = 0;

    virtual ErrCode Generate(
        const std::string& prompt,
        int32_t nPredict,
        double temp,
        int32_t topK,
        std::string& funcResult) = 0;

    virtual ErrCode GetMetrics(
        std::string& funcResult) = 0;

    virtual ErrCode SelfTest(
        std::string& funcResult) = 0;

    virtual ErrCode Release(
        std::string& funcResult) = 0;

    virtual ErrCode ExecuteAction(
        const std::string& action,
        const std::string& arg,
        std::string& funcResult) = 0;

    virtual ErrCode ExecuteIntent(
        const std::string& utterance,
        std::string& funcResult) = 0;

    // S7-1b：读当前屏幕（无障碍元素树）—— 感知方向，独立方法。
    // maxNodes <= 0 时用默认上限；返回有界 JSON（窗口列表 + 文本/可点元素 + 计数 + 采集耗时）。
    // 隐私：屏幕原文默认不写日志；返回内容只给通过准入的调用方。
    virtual ErrCode ReadScreen(
        int32_t maxNodes,
        std::string& funcResult) = 0;
protected:
    const int VECTOR_MAX_SIZE = 102400;
    const int LIST_MAX_SIZE = 102400;
    const int SET_MAX_SIZE = 102400;
    const int MAP_MAX_SIZE = 102400;
};
} // namespace LutSa
} // namespace OHOS
#endif // OHOS_LUTSA_ILUTSA_H

