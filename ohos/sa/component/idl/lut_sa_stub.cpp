#include "lut_sa_stub.h"

namespace OHOS {
namespace LutSa {

int32_t LutSaStub::OnRemoteRequest(
    uint32_t code,
    MessageParcel& data,
    MessageParcel& reply,
    MessageOption& option)
{
    std::u16string localDescriptor = GetDescriptor();
    std::u16string remoteDescriptor = data.ReadInterfaceToken();
    if (localDescriptor != remoteDescriptor) {
        return ERR_TRANSACTION_FAILED;
    }
    switch (static_cast<ILutSaIpcCode>(code)) {
        case ILutSaIpcCode::COMMAND_NATIVE_VERSION: {
            std::string result;
            ErrCode errCode = NativeVersion(result);
            if (!reply.WriteInt32(errCode)) {
                return ERR_INVALID_VALUE;
            }
            if (SUCCEEDED(errCode)) {
                if (!reply.WriteString16(Str8ToStr16(result))) {
                    return ERR_INVALID_DATA;
                }
            }
            return ERR_NONE;
        }
        case ILutSaIpcCode::COMMAND_LOAD_MODEL: {
            std::string path = Str16ToStr8(data.ReadString16());
            int32_t threads = data.ReadInt32();
            int32_t nCtx = data.ReadInt32();
            ErrCode errCode = LoadModel(path, threads, nCtx);
            if (!reply.WriteInt32(errCode)) {
                return ERR_INVALID_VALUE;
            }
            return ERR_NONE;
        }
        case ILutSaIpcCode::COMMAND_GENERATE: {
            std::string prompt = Str16ToStr8(data.ReadString16());
            int32_t nPredict = data.ReadInt32();
            double temp = data.ReadDouble();
            int32_t topK = data.ReadInt32();
            std::string result;
            ErrCode errCode = Generate(prompt, nPredict, temp, topK, result);
            if (!reply.WriteInt32(errCode)) {
                return ERR_INVALID_VALUE;
            }
            if (SUCCEEDED(errCode)) {
                if (!reply.WriteString16(Str8ToStr16(result))) {
                    return ERR_INVALID_DATA;
                }
            }
            return ERR_NONE;
        }
        case ILutSaIpcCode::COMMAND_GET_METRICS: {
            std::string result;
            ErrCode errCode = GetMetrics(result);
            if (!reply.WriteInt32(errCode)) {
                return ERR_INVALID_VALUE;
            }
            if (SUCCEEDED(errCode)) {
                if (!reply.WriteString16(Str8ToStr16(result))) {
                    return ERR_INVALID_DATA;
                }
            }
            return ERR_NONE;
        }
        case ILutSaIpcCode::COMMAND_SELF_TEST: {
            std::string result;
            ErrCode errCode = SelfTest(result);
            if (!reply.WriteInt32(errCode)) {
                return ERR_INVALID_VALUE;
            }
            if (SUCCEEDED(errCode)) {
                if (!reply.WriteString16(Str8ToStr16(result))) {
                    return ERR_INVALID_DATA;
                }
            }
            return ERR_NONE;
        }
        case ILutSaIpcCode::COMMAND_RELEASE: {
            std::string result;
            ErrCode errCode = Release(result);
            if (!reply.WriteInt32(errCode)) {
                return ERR_INVALID_VALUE;
            }
            if (SUCCEEDED(errCode)) {
                if (!reply.WriteString16(Str8ToStr16(result))) {
                    return ERR_INVALID_DATA;
                }
            }
            return ERR_NONE;
        }
        case ILutSaIpcCode::COMMAND_EXECUTE_ACTION: {
            std::string action = Str16ToStr8(data.ReadString16());
            std::string arg = Str16ToStr8(data.ReadString16());
            std::string result;
            ErrCode errCode = ExecuteAction(action, arg, result);
            if (!reply.WriteInt32(errCode)) {
                return ERR_INVALID_VALUE;
            }
            if (SUCCEEDED(errCode)) {
                if (!reply.WriteString16(Str8ToStr16(result))) {
                    return ERR_INVALID_DATA;
                }
            }
            return ERR_NONE;
        }
        case ILutSaIpcCode::COMMAND_EXECUTE_INTENT: {
            std::string utterance = Str16ToStr8(data.ReadString16());
            std::string result;
            ErrCode errCode = ExecuteIntent(utterance, result);
            if (!reply.WriteInt32(errCode)) {
                return ERR_INVALID_VALUE;
            }
            if (SUCCEEDED(errCode)) {
                if (!reply.WriteString16(Str8ToStr16(result))) {
                    return ERR_INVALID_DATA;
                }
            }
            return ERR_NONE;
        }
        // S7-1b：感知方向（读屏）—— 独立方法，参数只有节点上限
        case ILutSaIpcCode::COMMAND_READ_SCREEN: {
            int32_t maxNodes = data.ReadInt32();
            std::string result;
            ErrCode errCode = ReadScreen(maxNodes, result);
            if (!reply.WriteInt32(errCode)) {
                return ERR_INVALID_VALUE;
            }
            if (SUCCEEDED(errCode)) {
                if (!reply.WriteString16(Str8ToStr16(result))) {
                    return ERR_INVALID_DATA;
                }
            }
            return ERR_NONE;
        }
        default:
            return IPCObjectStub::OnRemoteRequest(code, data, reply, option);
    }

    return ERR_TRANSACTION_FAILED;
}
} // namespace LutSa
} // namespace OHOS
