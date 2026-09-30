// LUT-SA SystemAbility 绑定：把业务内核注册进 samgr（SA_ID=6901）。
// 仅在定义了 TMAC_SA_SAMGR_BINDING 时编译（该宏需要 OH 平台源码树的头文件；
// 应用 SDK 不提供 samgr/safwk 头文件，因此默认关闭，qemu 冒烟走不带绑定的路径）。
#ifdef TMAC_SA_SAMGR_BINDING

#ifndef LUT_SA_ABILITY_H
#define LUT_SA_ABILITY_H

#include "system_ability.h"

namespace tmac_sa {

class LutSystemAbility : public SystemAbility {
    DECLARE_SYSTEM_ABILITY(LutSystemAbility);

public:
    DISALLOW_COPY_AND_MOVE(LutSystemAbility);
    LutSystemAbility(int32_t saId, bool runOnCreate) : SystemAbility(saId, runOnCreate) {}

    // 生命周期：OnStart 里 Publish 自身（samgr 登记）；OnStop 里回收会话
    void OnStart() override;
    void OnStop() override;

    // hidumper -s 6901 的输出来源
    int32_t Dump(int fd, const std::vector<std::u16string> &args) override;
};

}  // namespace tmac_sa

#endif  // LUT_SA_ABILITY_H

#endif  // TMAC_SA_SAMGR_BINDING
