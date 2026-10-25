#!/system/bin/sh
SKIPUNZIP=0
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
mkdir -p "$MODPATH/bin" "$MODPATH/in" "$MODPATH/out"
if [ -f "$MODPATH/bin/execd" ]; then
    set_perm "$MODPATH/bin/execd" 0 0 0755
fi
chmod 700 "$MODPATH/in" "$MODPATH/out"
ui_print "- 安装完成"
ui_print "- 写入 in/cmd.sh 即执行，结果在 out/result.txt"
