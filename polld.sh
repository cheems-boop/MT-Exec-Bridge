#!/system/bin/sh
# shell 兼容轮询 v2（execd 缺失时的降级方案）：in/*.sh 按名排序逐个执行，固定 timeout 600
MODDIR=${0%/*}
IN="$MODDIR/in"; OUT="$MODDIR/out"
while true; do
    for f in "$IN"/*.sh; do
        [ -f "$f" ] || continue
        b=${f##*/}
        [ "$b" = "env.sh" ] && continue
        T=$(date +%s)
        {
            echo "=== $T | job: $b (polld) ==="
            timeout 600 sh "$f" 2>&1
            echo "=== exit $? | polld ==="
        } > "$OUT/.result.tmp" 2>&1
        mv "$OUT/.result.tmp" "$OUT/result.txt"
        rm -f "$f"
        echo "$T | $b | polld" >> "$OUT/history.log"
    done
    sleep 0.5
done
