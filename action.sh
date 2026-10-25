#!/system/bin/sh
# KSU 模块页「执行」按钮：手动跑一次 in/cmd.sh
MODDIR=${0%/*}
IN="$MODDIR/in/cmd.sh"
OUT="$MODDIR/out/result.txt"
if [ ! -f "$IN" ]; then
    echo "no cmd file" > "$OUT"
    exit 0
fi
{
    echo "=== $(date) ==="
    sh "$IN" 2>&1
    echo "=== exit $? ==="
} > "$OUT.tmp" 2>&1
mv "$OUT.tmp" "$OUT"
rm -f "$IN"
