#!/system/bin/sh
# lut_evidence.sh —— 开机一次性取证：跑 SA 客户端并打出关键诊断，结果整体打到串口
#
# 两个通道，互为备份：
#   ① 串口直接打标记（ttyS0）—— 脚本卡在哪一步都能看出来（STA-3 踩过：脚本整段没输出，
#      分不清是"没跑"还是"卡住了"）
#   ② 全量结果落 /data/local/tmp/lut_evidence.txt，最后 cat 到串口；
#      即使脚本没跑完，宿主机也能在 QEMU 停机后从 userdata 镜像里把文件捞出来
echo "#####LUT-EV-BEGIN#####" > /dev/ttyS0
sleep 30
echo "#####LUT-EV-STEP-1-sleep-done#####" > /dev/ttyS0

F=/data/local/tmp/lut_evidence.txt
mkdir -p /data/local/tmp 2>/dev/null

{
  echo "#####LUT-EV-START#####"
  date

  echo "--- [0] SELinux 状态与进程上下文（诊断为什么 get 被拒）---"
  echo "enforce=$(cat /sys/fs/selinux/enforce 2>&1)"
  echo "self context: $(cat /proc/self/attr/current 2>&1)"
  # 注意：guest 的 toybox 里没有 awk/tr，只能用 sed（这里踩过：awk: inaccessible or not found）
  for p in $(ps -ef 2>/dev/null | grep -E 'lut_sa|lut_sa_client' | sed -E 's/^[^ ]+[ ]+([0-9]+).*/\1/'); do
    echo "pid $p ($(cat /proc/$p/comm 2>/dev/null)): $(cat /proc/$p/attr/current 2>&1)"
  done

  echo "--- [0b] 模型就位与 SELinux 标签（SA 进程域要能读 gguf）---"
  M=/data/local/tmp/model.gguf
  if [ -f "$M" ]; then
    echo "path=$M size=$(stat -c %s "$M" 2>&1) magic=$(head -c 4 "$M" 2>/dev/null | od -An -c 2>&1)"
    echo "label-before: $(ls -lZ "$M" 2>&1 | head -1)"
    chmod 644 "$M" 2>/dev/null
    # 证据服务以 init 身份运行 → 能改标签。统一成 system_file（各域普遍持有 read/open/map）
    chcon u:object_r:system_file:s0 "$M" 2>&1 | head -2
    echo "label-after : $(ls -lZ "$M" 2>&1 | head -1)"
    head -c 8 "$M" > /dev/null 2>&1 && echo "self-read   : ok" || echo "self-read   : FAIL"
  else
    echo "  缺 $M（模型未注入镜像）"
  fi

  echo "--- [9] 系统级动作执行（S6-1b-2）：start_ability 参数变体一轮试完 ---"
  printf 'com.ohos.settings
' > /data/lut_sa/actions_allow.txt
  echo "[9a1] 只给 bundle（基线，已知 AMS=22）"
  /system/bin/lut_sa_client --action start_ability com.ohos.settings
  echo "[9a2] 显式 ability（入口 com.ohos.settings.MainAbility）→ 期望 AMS 受理"
  /system/bin/lut_sa_client --action start_ability com.ohos.settings/com.ohos.settings.MainAbility
  echo "[9a3] 显式 ability + userId=100"
  /system/bin/lut_sa_client --action start_ability com.ohos.settings/com.ohos.settings.MainAbility@100
  echo "[9a4] 显式 ability + userId=100 + module=phone（module 名取自 Settings.hap 的 module.json）"
  /system/bin/lut_sa_client --action start_ability 'com.ohos.settings/com.ohos.settings.MainAbility@100#phone'
  echo "[9b] 未授权目标（com.ohos.camera）→ 期望 201"
  /system/bin/lut_sa_client --action start_ability com.ohos.camera
  echo "[9c] 删表恢复（默认只放 settings）→ 期望受理（同 [9a4]）"
  rm -f /data/lut_sa/actions_allow.txt
  /system/bin/lut_sa_client --action start_ability 'com.ohos.settings/com.ohos.settings.MainAbility@100#phone'

  echo "--- [2] ps ---"
  ps -ef 2>&1 | grep -i lut

  echo "--- [3] 装机文件 ---"
  ls -la /system/lib64/libtmac_sa.z.so /system/bin/lut_sa_client /system/profile/lut_sa.json 2>&1

  echo "--- [4] service_contexts 里的 6901 ---"
  grep -a 6901 /system/etc/selinux/targeted/contexts/service_contexts 2>&1

  echo "--- [5a] hilog 里的 [LutSa]（SA 自己的日志）---"
  hilog -x 2>&1 | grep -a '\[LutSa\]' | head -15
  echo "--- [5b] hilog 里的 SA_CLIENT/Selinux ---"
  hilog -x 2>&1 | grep -aiE 'SA_CLIENT|samgr_class' | head -12

  echo "--- [5c] SA 进程的 stderr/stdout（引擎断言/LOG(FATAL) 都打在这里）---"
  for f in /data/local/tmp/lut_sa_stderr.txt /data/local/tmp/lut_sa_stdout.txt; do
    echo "== $f ($(stat -c %s $f 2>/dev/null) 字节) 尾 25 行 =="
    tail -25 "$f" 2>&1
  done

  echo "--- [10] 自然语言 → 动作（S6-2）：source 字段如实标注 ---"
  echo "[10a] “打开设置” → 期望 ErrCode=0 且 bundle=com.ohos.settings（轻活服务里没加载模型 → source=keyword）"
  /system/bin/lut_sa_client --intent "打开设置"
  echo "[10b] “打开相机” → 期望拒绝（动作表没匹配）"
  /system/bin/lut_sa_client --intent "打开相机"
  echo "[10c] 自定义动作表把“打开设置”指向未授权 bundle(com.ohos.camera) → 期望 201（动作白名单仍然生效）"
  printf '打开设置=com.ohos.camera
' > /data/lut_sa/intents.txt
  /system/bin/lut_sa_client --intent "打开设置"
  rm -f /data/lut_sa/intents.txt
  echo "[10d] 删表回默认 → 期望再回到 0"
  /system/bin/lut_sa_client --intent "打开设置"
  echo "[10e] 内置表已含"看相机"→com.ohos.camera，但动作白名单默认没放它 → 期望 201（两道门）"
  /system/bin/lut_sa_client --intent "看相机"

  echo "--- [11] 动作白名单「正向授权」双证明（S6-4）：放行 + 收回 ---"
  printf '看相机=com.ohos.camera
' > /data/lut_sa/intents.txt
  printf 'com.ohos.settings
com.ohos.camera
' > /data/lut_sa/actions_allow.txt
  echo "[11a] 意图表 + 动作白名单都放 camera → 期望 ErrCode=0 且 JSON 里 bundle=com.ohos.camera"
  /system/bin/lut_sa_client --intent "看相机"
  echo "[11b] 收回动作白名单（只放 settings）→ 同一条说法应回到 201（对照）"
  printf 'com.ohos.settings
' > /data/lut_sa/actions_allow.txt
  /system/bin/lut_sa_client --intent "看相机"
  rm -f /data/lut_sa/intents.txt /data/lut_sa/actions_allow.txt

  echo "--- [12] S7-0 侦察：无障碍服务在本镜像里吗 ---"
  echo "  ps(accessib): $(ps -ef 2>/dev/null | grep -i accessib | grep -v grep | head -1)"
  echo "  profile     : $(grep -o '\"process\": *\"[a-z]*\"' /system/profile/accessibility.json 2>/dev/null | head -1)"
  echo "  libs        : $(ls /system/lib64/libaccessibleability*.z.so 2>/dev/null | wc -l) 个无障碍库"
  echo "  cfg         : $(ls /system/etc/init/accessibility.cfg 2>/dev/null | wc -l) 个 init cfg"

  echo "--- [13] S7-1a 探针：独立原生进程能不能拿到无障碍元素树（v2 走 AccessibilityUITestAbility）---"
  echo "  v1 教训：普通客户端 GetWindows/GetRoot 一律 4004=RET_ERR_NO_CONNECTION（必须有系统下发的 channel）"
  echo "  v2 路径：RegisterAbilityListener → Connect(userId) → 等 channel 回调 → GetWindows/GetRoot/GetChildren"
  echo "  权限来源：本服务 cfg 的 permission 字段（init 按它给进程设 native token，见 QEMU-DEPLOY FIX-83）"
  echo "  默认模式会把**所有窗口**都走一遍（实测：顶层桌面窗口 4 层内全是容器、没有文本）"
  echo "  每一步的真实返回值都是证据；退出码 0=有文本 1=有树无文本 2=句柄空 3=没连上 4=连上没树"
  echo "[13a] 默认（无参数，走所有窗口）"
  /system/bin/lut_a11y_dump
  echo "  exit=$?"
  echo "[13b] 只走指定窗口（上轮的顶层窗口 winId=6）"
  /system/bin/lut_a11y_dump 6
  echo "  exit=$?"
  echo "  注意：「没拿到」也是结论 —— 但要留下**真实错误码**，不许写成「大概不行」"

  echo "--- [14] S7-1b 读屏：SA 的 ReadScreen（感知方向，独立 IDL 方法）---"
  echo "  门：与其它方法同一套准入（默认档位 / allow_uids.txt 白名单）—— 先证拒绝、再证放行"
  echo "[14a] 写白名单只放 uid 12345（root 不在内）→ 期望 ErrCode=201（准入拒绝，且 SA 不回传结果串）"
  mkdir -p /data/lut_sa 2>/dev/null
  echo "12345" > /data/lut_sa/allow_uids.txt
  /system/bin/lut_sa_client --screen 0
  echo "[14b] 删掉白名单恢复默认档位 → 期望 ErrCode=0，且 JSON 里 ok=1 与 counts.withText>0"
  rm -f /data/lut_sa/allow_uids.txt
  /system/bin/lut_sa_client --screen 0
  echo "[14c] 有界性：上限 8 → 期望 ok=1 且 counts.nodes<=8（截断标记 truncated=1）"
  /system/bin/lut_sa_client --screen 8
  echo "[14d] 指标里的 screen 状态（只记连接状态，不含屏幕原文 —— 隐私纪律）"
  /system/bin/lut_sa_client --metrics

  echo "--- [15] 真机屏幕可视：真屏截图 + 同一时刻的 ReadScreen（配成一对，供叠框）---"
  echo "  用镜像里的 snapshot_display（foundation/window/window_manager/snapshot）；要 CAPTURE_SCREEN 权限"
  echo "[15a] 截屏 → /data/local/tmp/lut_screen.png"
  # 实测（evidence/62）：-f 的**文件名必须自带与 -t 一致的后缀**，
  # 否则报 "fileName /data/local/tmp/lut_screen invalid, suffix must be .png"（exit=255）
  /system/bin/snapshot_display -f /data/local/tmp/lut_screen.png -t png
  echo "  exit=$?"
  if [ ! -s /data/local/tmp/lut_screen.png ]; then
    echo "  png 方式失败 → 退回 jpeg"
    /system/bin/snapshot_display -f /data/local/tmp/lut_screen.jpeg -t jpeg
    echo "  exit=$?"
  fi
  ls -l /data/local/tmp/lut_screen.* 2>/dev/null
  echo "  截图时刻: $(date +%s)"
  echo "[15b] 紧接着读屏（与截图同一时刻的元素树）"
  /system/bin/lut_sa_client --screen 0
  echo "  读屏时刻: $(date +%s)"

  echo "--- [15c] S7-2-A0 进程内 MMI 注入（我们自己的进程，不是外部 uitest CLI）---"
  echo "  动机：uitest 的注入已被证明可用，但它是外部工具；这里验证 SA 同权限的进程能否自己注入"
  echo "  权限：INJECT_INPUT_EVENT 在 lut_evidence.cfg 的 permission_acls（S7-3 预留那条）；本段故意放在 [16] 之前"
  echo "[15c1] swipe 解锁 (512,600)->(512,200) 300ms —— 期望 CHANGED（锁屏窗口消失）"
  /system/bin/lut_mmi_probe swipe
  echo "  exit=$?"
  echo "[15c2] click 屏幕中心 (512,400) —— 看是否 CHANGED"
  /system/bin/lut_mmi_probe click
  echo "  exit=$?"
  echo "[15c3] back（KEYCODE_BACK）—— 看是否 CHANGED"
  /system/bin/lut_mmi_probe back
  echo "  exit=$?"

  echo "--- [24] S7-2-A Agent Loop（SA 进程内单步闭环；goal 从文件读，不新增 IPC）---"
  echo "  goal 文件在**启动前**由宿主写进 userdata：/data/lut_sa/agent_goal.txt"
  echo "  SA 在 OnStart 后延迟 15s 跑一次闭环，把 trace 写 /data/lut_sa/agent_trace.json + stdout"
  echo "[24a] 机器可读 trace（agent_trace.json）"
  cat /data/lut_sa/agent_trace.json 2>/dev/null || echo "  （trace 文件不存在——goal 未写入或 SA 未跑）"
  echo "[24b] 人读 trace（SA 的 stdout → rt_stdout.txt，取 agent 段）"
  grep -a -A 14 "S7-2-A Agent Loop" /data/lut_sa/rt_stdout.txt 2>/dev/null | tail -16
  echo "[24c] 注意：若 [24] 显示 PASS，则本轮的 [15c1]（用探针再解一次锁）应为 UNCHANGED ——"
  echo "       因为锁屏在 SA 跑闭环时就已经被划走了（两处证据互相印证）"

  echo "--- [16] S7-2-0 无障碍动作可行性（click/back/scroll/swipe + 前后快照 diff）---"
  echo "  纪律：动作数固定（每种一次）· 目标有界（前 200 节点）· 失败留真实 RetError；本段放最后（会改变界面）"
  echo "  判据：每个动作打 verdict=CHANGED（界面确有变化）或 UNCHANGED（发了但没动）+ ret"
  echo "[16a] click：点第一个可点元素（先打印它是什么，再前后 diff）"
  /system/bin/lut_a11y_dump --act click
  echo "  exit=$?"
  echo "[16b] back：对根元素发 ACTION_BACK(0x20000)"
  /system/bin/lut_a11y_dump --act back
  echo "  exit=$?"
  echo "[16c] scroll：对第一个可滚动元素发 ACTION_SCROLL_FORWARD(0x100)"
  /system/bin/lut_a11y_dump --act scroll
  echo "  exit=$?"
  echo "[16d] swipe：InjectGesture 上滑 (512,600)->(512,200) 300ms（解锁可行性）"
  /system/bin/lut_a11y_dump --act swipe
  echo "  exit=$?"
  echo "[16e] 动作后再看一次元素树（确认界面是否已变/是否离开锁屏）"
  /system/bin/lut_a11y_dump
  echo "  exit=$?"

  echo "--- [17] S7-2-0b 可交互界面：aa 拉起设置 + 采样 + 真实 click/back ---"
  echo "  动机：[16] 在锁屏上 click/back/scroll 都 ret=0 但界面无变化（不可交互界面），"
  echo "        swipe=4006(RET_ERR_NO_CAPABILITY)；所以先换到应用界面再测元素动作是否真能改变界面"
  echo "[17a] aa start（两种写法都试；这是路线B执行者在 guest 内的替身）"
  /system/bin/aa start -a MainAbility -b com.ohos.settings
  echo "  exit=$?"
  /system/bin/aa start -b com.ohos.settings -a EntryAbility
  echo "  exit=$?"
  sleep 3
  echo "[17b] 拉起后读屏（看窗口/文本是否已变成设置界面）"
  /system/bin/lut_sa_client --screen 0
  echo "[17c] 采样：设置界面元素树（真实文本，供规则表用，不猜）"
  /system/bin/lut_a11y_dump
  echo "  exit=$?"
  echo "[17d] 真实 click：优先点「带文本且可点」的元素 → 期望 CHANGED"
  /system/bin/lut_a11y_dump --act click
  echo "  exit=$?"
  echo "[17e] back：ACTION_BACK → 期望 CHANGED（回到上一页）"
  /system/bin/lut_a11y_dump --act back
  echo "  exit=$?"

  echo "--- [18] S7-2-0c 解锁与输入注入侦察（aa 成功但界面仍是锁屏 → 卡点在解锁）---"
  echo "  结论先用证据说话：先看设置进程是否已起、再看 guest 里有哪些注入工具、再试真解锁"
  echo "[18a] 进程诊断：设置/桌面/无障碍/uitest 相关进程"
  ps -ef 2>/dev/null | grep -iE "settings|launcher|accessib|uitest" | grep -v grep | head -8
  echo "[18b] 候选注入工具（镜像里有什么就用什么）"
  for t in uitest uinput wukong power-shell snapshot_display; do
    printf '  %-16s : %s
' "$t" "$(ls /system/bin/$t 2>/dev/null || echo '（无）')"
  done
  echo "[18c] uitest 用法（若可用则是标准输入注入路径）"
  /system/bin/uitest -h 2>&1 | head -12
  echo "[18d] 尝试用 uitest 注入上滑解锁（三种写法都试，打印真实返回）"
  /system/bin/uitest uiInput swipe 512 600 512 200 300; echo "  exit=$?"
  /system/bin/uitest -c uiInput -a swipe -x1 512 -y1 600 -x2 512 -y2 200 -t 300; echo "  exit=$?"
  /system/bin/uitest uiInput keyEvent 2; echo "  exit=$?"
  sleep 2
  echo "[18e] 解锁尝试后再读屏（若离开锁屏，文本里不会再有「上滑解锁」）"
  /system/bin/lut_sa_client --screen 0
  echo "[18f] 元素树（看是否已进入可交互界面）"
  /system/bin/lut_a11y_dump
  echo "  exit=$?"

  echo "--- [19] S7-2-0d 解锁后在可交互界面上的决定性测量（S7-2-0 收口）---"
  echo "  前提（[18] 已证）：uitest uiInput swipe 解锁成功（锁屏窗口消失、withText=0）"
  echo "[19a] 解锁状态下重新拉起设置（aa 之前被锁屏挡住）→ 读屏看是否进入设置界面"
  /system/bin/aa start -a MainAbility -b com.ohos.settings; echo "  exit=$?"
  sleep 3
  /system/bin/lut_sa_client --screen 0
  echo "[19b] 采样设置界面的真实元素文本（供规则表；不猜）"
  /system/bin/lut_a11y_dump
  echo "  exit=$?"
  echo "[19c] 元素动作（a11y）：点第一个「带文本且可点」的元素 → 期望 CHANGED"
  /system/bin/lut_a11y_dump --act click
  echo "  exit=$?"
  echo "[19d] 返回：a11y ACTION_BACK → 期望 CHANGED（回到上一页）"
  /system/bin/lut_a11y_dump --act back
  echo "  exit=$?"
  echo "[19e] 兜底路线：uitest 坐标点击（用刚刚读到的元素 box 中心点，坐标来自感知而非盲猜）"
  /system/bin/uitest uiInput click 512 300; echo "  exit=$?"
  sleep 2
  echo "[19f] 点击后再读屏（对比界面是否变化）"
  /system/bin/lut_sa_client --screen 0

  echo "--- [20] S7-2-0e 判别实验：把「为什么没效果」钉死（三个对照）---"
  echo "  已知：uitest swipe 解锁生效 ✓；a11y click/back ret=0 但无变化；aa 启动返回 0 但界面无变化"
  echo "[20a] 诊断 aa：设置进程到底有没有起来（启动前后各数一次）"
  echo "  before: settings 进程数=$(ps -ef 2>/dev/null | grep -i settings | grep -v grep | wc -l)"
  /system/bin/aa start -a MainAbility -b com.ohos.settings; echo "  aa exit=$?"
  sleep 2
  echo "  after : settings 进程数=$(ps -ef 2>/dev/null | grep -i settings | grep -v grep | wc -l)"
  ps -ef 2>/dev/null | grep -iE "settings|appspawn|launcher" | grep -v grep | head -5
  echo "[20b] 对照1：uitest 下拉通知栏（坐标 512,10 → 512,500）→ 期望 CHANGED（证明注入能改界面）"
  /system/bin/uitest uiInput swipe 512 10 512 500 300; echo "  exit=$?"
  sleep 2
  /system/bin/lut_sa_client --screen 0
  echo "[20c] 对照2：uitest 发 BACK 键（keyEvent 2）→ 期望 CHANGED（若生效=我们有「返回」手段）"
  /system/bin/uitest uiInput keyEvent 2; echo "  exit=$?"
  sleep 2
  /system/bin/lut_sa_client --screen 0
  echo "[20d] 对照3：uitest 点击屏幕中心（512,400）→ 看是否 CHANGED（坐标注入的通用性）"
  /system/bin/uitest uiInput click 512 400; echo "  exit=$?"
  sleep 2
  /system/bin/lut_a11y_dump
  echo "  exit=$?"

  echo "--- [21] S7-2-0f 执行通道定案：aa 正解 + 通知栏收起 + 设置界面采样 ---"
  echo "  [20a] 已证：aa start -a MainAbility 报 10104001（ability 不存在）——之前 exit=0 是 shell 退出码"
  echo "[21a] aa 正解：只给 bundle（由系统挑 entry ability）+ 查真实 ability 名对照"
  bm dump -n com.ohos.settings 2>/dev/null | grep -oE '"name": *"[^"]*Ability[^"]*"' | head -4
  /system/bin/aa start -b com.ohos.settings 2>&1 | head -6
  sleep 3
  echo "[21b] 拉起后读屏（期望：出现设置界面的窗口/文本）"
  /system/bin/lut_sa_client --screen 0
  echo "[21c] 采样设置界面元素树（真实文本 → 规则表输入，不猜）"
  /system/bin/lut_a11y_dump
  echo "  exit=$?"
  echo "[21d] 收起通知栏/回到桌面（坐标上滑）"
  /system/bin/uitest uiInput swipe 512 500 512 50 300; echo "  exit=$?"
  sleep 2
  /system/bin/lut_sa_client --screen 0

  echo "--- [22] S7-2-0g 真实包名：aa 失败的真因是包名不存在（10103601）---"
  echo "  动机：内置意图表里的 com.ohos.settings 是"猜测值"，aa 报 10103601 bundle 不存在"
  echo "[22a] 已安装 bundle 列表（前 20 个）+ 名称里含 settings/launcher 的"
  bm dump -a 2>/dev/null | head -20
  bm dump -a 2>/dev/null | grep -iE "settings|launcher|phone" | head -8
  echo "[22b] 系统里预装的 settings/launcher 目录（镜像侧对照）"
  ls /system/app 2>/dev/null | grep -iE "settings|launcher" | head -6
  echo "[22c] bm 用法确认（-n 参数到底要什么）"
  bm dump -h 2>&1 | head -10



  echo "--- [7] 调用方准入（S5-1）：默认档位 vs 白名单（同一进程、不重启 SA）---"
  rm -f /data/lut_sa/allow_uids.txt 2>/dev/null
  mkdir -p /data/lut_sa 2>/dev/null
  echo "[7a] 默认档位（root 属特权 uid）→ 期望 SelfTest 放行、失败项 0"
  /system/bin/lut_sa_client
  echo "[7b] 写白名单只放 uid 12345（root 不在内）→ 期望 SelfTest 返回 201、失败项 ≥1"
  echo "12345" > /data/lut_sa/allow_uids.txt
  /system/bin/lut_sa_client
  echo "[7c] 删掉白名单恢复默认档位 → 期望重新放行（证明策略每次调用都重读）"
  rm -f /data/lut_sa/allow_uids.txt
  /system/bin/lut_sa_client

  echo "--- [8] 配额（S5-2）：可加载模型大小上限 ---"
  echo "[8a] 写 model_mb=1（上限 1MB）+ 一次 966MB 模型的加载 → 期望 LoadModel 拒绝、SA 存活"
  echo "model_mb=1" > /data/lut_sa/quota.txt
  /system/bin/lut_sa_client --load "$M"
  echo "[8b] 删掉配额配置 → 期望重新加载成功（ErrCode=0）"
  rm -f /data/lut_sa/quota.txt
  /system/bin/lut_sa_client --load "$M"

  echo "#####LUT-EV-END#####"
} > $F 2>&1

cat $F > /dev/ttyS0 2>&1
