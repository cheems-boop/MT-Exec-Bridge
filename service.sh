#!/system/bin/sh
# v2：supervisor 看门狗常驻；单实例由 execd 内部 flock 保证
MODDIR=${0%/*}
mkdir -p "$MODDIR/bin" "$MODDIR/in" "$MODDIR/out"
chmod 700 "$MODDIR/in" "$MODDIR/out"
chmod 755 "$MODDIR/bin/execd" 2>/dev/null

if [ ! -f "$MODDIR/in/env.sh" ]; then
cat > "$MODDIR/in/env.sh" <<'EOF'
# 每个任务执行前被 source；可在此持久 export 环境变量，例如：
# export LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib
# export PATH=/data/data/com.termux/files/usr/bin:$PATH
EOF
chmod 600 "$MODDIR/in/env.sh"
fi

nohup sh "$MODDIR/supervisor.sh" >/dev/null 2>&1 &
