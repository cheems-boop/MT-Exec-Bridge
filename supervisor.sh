#!/system/bin/sh
# 守护进程看门狗：execd 崩溃 2 秒后自动拉起（execd 内部 flock 保证全局单实例）
MODDIR=${0%/*}
while true; do
    if [ -x "$MODDIR/bin/execd" ]; then
        "$MODDIR/bin/execd"
    else
        sh "$MODDIR/polld.sh"
    fi
    echo "$(date +%s) | daemon exited, respawn in 2s" >> "$MODDIR/out/history.log"
    sleep 2
done
