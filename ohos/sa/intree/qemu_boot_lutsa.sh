#!/usr/bin/env bash
# qemu_boot_lutsa.sh —— 启动 OH 7.0 x86_64 标准系统（QEMU）并采集证据
#
# 与官方 vendor/ohemu/qemu_x86_64_linux_full/qemu_run.sh 的参数一致，额外做到：
#   1) 串口日志落盘（-serial file:…）→ 可用 grep 验证 LutSa 的 OnStart / Publish 日志
#   2) QEMU monitor 挂 unix socket → 可用 `screendump /tmp/oh-screen.png` 抓系统界面截图
#   3) 端口转发补上 hdc 的 8710（官方只转 5555）
#
# 用法:
#   bash qemu_boot_lutsa.sh                 # 前台启动（日志 /src/oh-serial.log）
#   bash qemu_boot_lutsa.sh daemon          # 后台启动
#   QEMU_DISPLAY=gtk bash qemu_boot_lutsa.sh  # 走 WSLg 出窗口（默认 headless）
set -uo pipefail

IMG=${OHOS_IMG:-/src/ohos/out/x86_64_virt/packages/phone/images}
LOG=${OH_LOG:-/src/oh-serial.log}
MON=${OH_MON:-/tmp/oh-qemu-mon.sock}
DISPLAY_TYPE=${QEMU_DISPLAY:-none}

if [ ! -f "$IMG/bzImage" ]; then
  echo "❌ 未找到镜像：$IMG/bzImage（先编译 qemu_x86_64_linux_full）"
  exit 1
fi

if [ -r /dev/kvm ]; then
  MACHINE="q35,accel=kvm"; ACCEL=""
else
  MACHINE="q35"; ACCEL="-accel tcg,thread=multi"
fi

case "$DISPLAY_TYPE" in
  gtk) DISPLAY_ARGS=(-device virtio-gpu-pci -display gtk,gl=off) ;;
  vnc) DISPLAY_ARGS=(-device virtio-gpu-pci,xres=800,yres=500 -vnc :21) ;;
  *)   DISPLAY_ARGS=(-device virtio-gpu-pci -display none) ;;   # headless：仍可 screendump
esac

BOOTARGS="console=ttyS0,115200 sn=0023456789 init=/bin/init hardware=virt root=/dev/ram0 rw ip=dhcp ohos.boot.hardware=virt ohos.required_mount.system=/dev/block/vdb@/usr@ext4@ro,barrier=1@wait,required ohos.required_mount.vendor=/dev/block/vdc@/vendor@ext4@ro,barrier=1@wait,required ohos.required_mount.sys_prod=/dev/block/vdd@/sys_prod@ext4@rw,barrier=1@wait,required ohos.required_mount.chip_prod=/dev/block/vde@/chip_prod@ext4@rw,barrier=1@wait,required ohos.required_mount.data=/dev/block/vdf@/data@ext4@nosuid,nodev,noatime,barrier=1,data=ordered,noauto_da_alloc@wait,reservedsize=104857600"

rm -f "$MON"
[ "$DISPLAY_TYPE" = "gtk" ] || : > "$LOG"

CMD=(qemu-system-x86_64
  -machine "$MACHINE" ${ACCEL:-} -cpu max -smp 4 -m 4096
  -kernel "$IMG/bzImage" -initrd "$IMG/ramdisk.img"
  "${DISPLAY_ARGS[@]}"
  -monitor "unix:$MON,server,nowait"
  -serial "file:$LOG"
  -device virtio-mouse-pci -device virtio-keyboard-pci
  -netdev user,id=net0,hostfwd=tcp::8710-:8710,hostfwd=tcp::5555-:5555
  -device virtio-net-pci,netdev=net0)

for d in updater system vendor sys_prod chip_prod userdata; do
  CMD+=(-drive "if=none,file=$IMG/$d.img,format=raw,id=$d" -device "virtio-blk-pci,drive=$d,serial=$d")
done
CMD+=(-append "$BOOTARGS")

echo "== QEMU 启动（$MACHINE${ACCEL:+ / tcg}） =="
echo "   串口日志 : $LOG"
echo "   monitor  : $MON   （抓图: echo 'screendump /tmp/oh-screen.png' | socat - UNIX-CONNECT:$MON）"
echo "   hdc      : 8710 已转发（hdc tconn 127.0.0.1:8710）"

if [ "${1:-}" = "daemon" ]; then
  nohup "${CMD[@]}" >/dev/null 2>&1 &
  echo "   已后台启动 pid=$!"
else
  exec "${CMD[@]}"
fi
