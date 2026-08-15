#!/usr/bin/env bash
# tty-check.sh — 在真机上跑 luminaria-tty，退出后生成一份可以直接交出去的日志报告。
#
# 用途是 TODO 第 0 项：整条 DRM 路径只能在真显示器上确认，而"确认了什么"必须留下证据。
# 脚本本身不判断画面对不对（那要人眼），它做的是：把运行环境记下来、给混成器的输出打上
# 时间戳、按需拉起一个客户端、退出后把日志里能证明的事情逐条勾出来。
#
#   ./scripts/tty-check.sh                  # 自动挑一个客户端
#   ./scripts/tty-check.sh --no-client      # 只跑混成器，自己开客户端
#   ./scripts/tty-check.sh --client foot    # 指定客户端
#   ./scripts/tty-check.sh --device /dev/dri/card1
#   ./scripts/tty-check.sh --bin ./build/.../luminaria-tty   # 换一个构建
#
# 必须从一个空闲 VT 上跑（Ctrl+Alt+F3 登录，桌面停掉）。混成器里按 Esc 退出。
set -uo pipefail

# ------------------------------------------------------------------ 参数
CLIENT=""
NO_CLIENT=0
DEVICE=""
OUT_ROOT=""
BIN=""
REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --client) CLIENT="${2:-}"; shift 2 ;;
        --no-client) NO_CLIENT=1; shift ;;
        --device) DEVICE="${2:-}"; shift 2 ;;
        --out) OUT_ROOT="${2:-}"; shift 2 ;;
        --bin) BIN="${2:-}"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "未知参数: $1（--help 看用法）" >&2; exit 2 ;;
    esac
done

if [[ -z "$BIN" ]]; then
    BIN="$REPO/build/linux/x86_64/debug/luminaria-tty"
    [[ -x "$BIN" ]] || BIN="$REPO/build/linux/x86_64/release/luminaria-tty"
fi
if [[ ! -x "$BIN" ]]; then
    echo "找不到 luminaria-tty，先构建：xmake f -y --toolchain=clang && xmake build luminaria-tty" >&2
    exit 1
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="${OUT_ROOT:-$REPO/logs}/tty-check-$STAMP"
mkdir -p "$OUT" || exit 1
RUN_LOG="$OUT/run.log"
CLIENT_LOG="$OUT/client.log"
SYS_TXT="$OUT/system.txt"
REPORT="$OUT/report.md"

# ------------------------------------------------------------------ 运行前环境
{
    echo "date: $(date -Is)"
    echo "host: $(uname -n)"
    echo "kernel: $(uname -sr)"
    echo "binary: $BIN"
    echo "built: $(date -Ir -r "$BIN" 2>/dev/null)"
    echo "git: $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null) $(git -C "$REPO" status --porcelain 2>/dev/null | wc -l) 个未提交改动"
    echo "tty: $(tty 2>/dev/null)"
    echo "session: XDG_SESSION_ID=${XDG_SESSION_ID:-} XDG_SEAT=${XDG_SEAT:-} XDG_VTNR=${XDG_VTNR:-}"
    echo "env: WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-} DISPLAY=${DISPLAY:-}"
    echo "groups: $(id -nG)"
    echo
    echo "--- GPU ---"
    lspci -nn 2>/dev/null | grep -Ei 'vga|3d|display' || echo "（没有 lspci）"
    echo
    echo "--- DRM connector ---"
    for c in /sys/class/drm/card*-*; do
        [[ -e "$c/status" ]] || continue
        printf '%s: %s\n' "$(basename "$c")" "$(cat "$c/status" 2>/dev/null)"
        [[ -s "$c/modes" ]] && sed 's/^/    /' "$c/modes" | head -5
    done
    echo
    echo "--- 输入设备 ---"
    grep -E '^N: Name=' /proc/bus/input/devices 2>/dev/null | sed 's/^N: //' || echo "（读不到 /proc/bus/input/devices）"
    echo
    echo "--- Vulkan ---"
    if command -v vulkaninfo >/dev/null; then
        vulkaninfo --summary 2>/dev/null | sed -n '1,40p'
    else
        echo "（没有 vulkaninfo）"
    fi
    echo
    echo "--- loginctl ---"
    loginctl session-status 2>/dev/null | head -12 || echo "（没有 loginctl）"
} >"$SYS_TXT" 2>&1

# 内核日志：dmesg 通常要权限，拿不到就算了，报告里会写明。
DMESG_BEFORE=""
if sudo -n true 2>/dev/null && sudo -n dmesg >/dev/null 2>&1; then
    DMESG_BEFORE="$(sudo -n dmesg | wc -l)"
elif dmesg >/dev/null 2>&1; then
    DMESG_BEFORE="$(dmesg | wc -l)"
fi

# ------------------------------------------------------------------ 客户端
# 顺序有讲究：shm 的最简单（只验证纹理上屏），EGL/dmabuf 的才走零拷贝导入，
# 能全屏的才可能触发直出扫描。
pick_client() {
    local c
    for c in weston-simple-shm foot kitty alacritty konsole weston-terminal \
             gtk4-widget-factory gtk4-demo vkcube-wayland vkcube; do
        if command -v "$c" >/dev/null; then echo "$c"; return; fi
    done
}
if [[ $NO_CLIENT -eq 0 && -z "$CLIENT" ]]; then
    CLIENT="$(pick_client)"
fi

# ------------------------------------------------------------------ 运行前提示
cat <<EOF

  luminaria-tty 真机验证 —— 日志写到
      $OUT

  接下来屏幕会被混成器接管。请按顺序做这几件事，每一项在日志里都留得下痕迹：

    1. 看背景色出来（深蓝灰 #1a1a21），指针能动
    2. ${CLIENT:-（未启动客户端，自己开一个）} 会自动连上 —— 确认窗口画出来了
    3. 指针移到窗口上，滚一下滚轮，按几下 Shift / 字母键
    4. 有第二台显示器的话，插拔一次
    5. Ctrl+Alt+F1 切走，再 Ctrl+Alt+F$(fgconsole 2>/dev/null || echo N) 切回来
    6. 按 Esc 退出

  卡死的话：切到别的 VT，pkill luminaria-tty。

EOF
if [[ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]]; then
    echo "  警告：检测到 WAYLAND_DISPLAY/DISPLAY —— 你在桌面里，不是空闲 VT。"
    echo "        这样跑会抢不到 DRM master，也会偷走桌面的输入。"
    echo
fi
read -rp "  准备好了按回车开始…" _ || true

# ------------------------------------------------------------------ 跑
# 客户端在混成器打印出 socket 名之后才启动 —— 从日志里读那一行，比 sleep 可靠。
if [[ -n "$CLIENT" ]]; then
    (
        for _ in $(seq 1 100); do
            sock="$(grep -m1 -o 'WAYLAND_DISPLAY=[^ ]*' "$RUN_LOG" 2>/dev/null | head -1 | cut -d= -f2)"
            if [[ -n "$sock" ]]; then
                export WAYLAND_DISPLAY="$sock"
                export QT_QPA_PLATFORM=wayland GDK_BACKEND=wayland SDL_VIDEODRIVER=wayland
                export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
                echo "=== $CLIENT on WAYLAND_DISPLAY=$sock ===" >"$CLIENT_LOG"
                exec setsid $CLIENT >>"$CLIENT_LOG" 2>&1
            fi
            sleep 0.1
        done
        echo "=== 混成器没有打印 WAYLAND_DISPLAY，客户端没启动 ===" >"$CLIENT_LOG"
    ) &
    CLIENT_WAITER=$!
fi

: >"$RUN_LOG"
(
    if [[ -n "$DEVICE" ]]; then "$BIN" "$DEVICE"; else "$BIN"; fi
    echo $? >"$OUT/exit-code"
) 2>&1 | while IFS= read -r line; do
    printf '%(%H:%M:%S)T %s\n' -1 "$line"
done | tee "$RUN_LOG"

EXIT_CODE="$(cat "$OUT/exit-code" 2>/dev/null || echo "?")"

# 客户端可能还活着（混成器退出后它会掉线，但不一定自己退）。
if [[ -n "${CLIENT_WAITER:-}" ]]; then
    kill "$CLIENT_WAITER" 2>/dev/null
fi
if [[ -n "$CLIENT" ]]; then
    pkill -f "$(basename "$CLIENT")" 2>/dev/null
fi

DMESG_AFTER=""
if [[ -n "$DMESG_BEFORE" ]]; then
    if sudo -n true 2>/dev/null; then
        sudo -n dmesg | tail -n "+$((DMESG_BEFORE + 1))" >"$OUT/dmesg-new.txt" 2>/dev/null
    else
        dmesg | tail -n "+$((DMESG_BEFORE + 1))" >"$OUT/dmesg-new.txt" 2>/dev/null
    fi
    DMESG_AFTER="$OUT/dmesg-new.txt"
fi

# ------------------------------------------------------------------ 分析
# grep -c 在没匹配时既打印 0 又返回 1，所以这里不能用 `|| echo 0` —— 那会打印两个 0，
# 后面的算术比较就炸了。
has() { grep -qF -- "$1" "$RUN_LOG" 2>/dev/null; }
count_of() { local n; n="$(grep -cF -- "$1" "$RUN_LOG" 2>/dev/null)"; echo "${n:-0}"; }
count_re() { local n; n="$(grep -cE -- "$1" "$RUN_LOG" 2>/dev/null)"; echo "${n:-0}"; }

# frames/s 行拆出每一路的峰值，直出扫描有没有真的发生就看 scanout 那一列。
read -r MAX_COMPOSITED MAX_SCANOUT MAX_UNCHANGED MAX_FALLBACK STAT_LINES <<<"$(
    awk '
        /frames\/s/ {
            n++
            for (i = 1; i <= NF; i++) {
                split($i, kv, "=")
                if (kv[1] == "composited" && kv[2] + 0 > c) c = kv[2] + 0
                if (kv[1] == "scanout"    && kv[2] + 0 > s) s = kv[2] + 0
                if (kv[1] == "unchanged"  && kv[2] + 0 > u) u = kv[2] + 0
                if (kv[1] == "fallback"   && kv[2] + 0 > f) f = kv[2] + 0
            }
        }
        END { printf "%d %d %d %d %d\n", c, s, u, f, n }
    ' "$RUN_LOG" 2>/dev/null
)"

verdict() { # verdict <条件为真的判定> <否则的判定> <条件…>
    if "${@:3}"; then echo "$1"; else echo "$2"; fi
}
row() { printf '| %s | %s | %s |\n' "$1" "$2" "$3" >>"$REPORT"; }

{
    echo "# luminaria-tty 真机验证 — $STAMP"
    echo
    echo "- 主机 \`$(uname -n)\`，内核 \`$(uname -sr)\`"
    echo "- 提交 \`$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null)\`，退出码 \`$EXIT_CODE\`"
    echo "- 客户端 \`${CLIENT:-（无）}\`"
    echo "- 环境明细见 \`system.txt\`，完整日志见 \`run.log\`"
    echo
    echo "## 逐项结论"
    echo
    echo "| 检查项 | 结果 | 依据 |"
    echo "|---|---|---|"
} >"$REPORT"

if has "session = libseat"; then
    row "libseat 会话" "有" "session = libseat"
else
    row "libseat 会话" "**无**（VT 切换不安全）" "$(grep -m1 'session = none' "$RUN_LOG" | sed 's/^[0-9:]* //')"
fi

if grep -q 'luminaria-tty: output [0-9]' "$RUN_LOG"; then
    row "atomic 模式设置 + 翻页" "OK" "$(grep -m1 'luminaria-tty: output [0-9]' "$RUN_LOG" | sed 's/^[0-9:]* //')"
else
    row "atomic 模式设置 + 翻页" "**没发生**" "日志里没有 output 行"
fi

OUTPUT_COUNT="$(count_re 'luminaria-tty: output [0-9]')"
if [[ "$OUTPUT_COUNT" -gt 1 ]] || has "output removed"; then
    row "多输出 / 热插拔" "有事件（$OUTPUT_COUNT 次 output，$(count_of 'output removed') 次移除）" "见 run.log"
else
    row "多输出 / 热插拔" "未验证" "只有一台显示器，且没有插拔"
fi

# 下面两项都以"有没有 output 行"为前提：一台显示器都没起来的时候，说"没有硬件光标平面"
# 是在报告一件根本没发生过的事。
if [[ "$OUTPUT_COUNT" -eq 0 ]]; then
    row "硬件光标平面" "未验证" "没有输出"
    row "GPU dmabuf 扫描输出" "未验证" "没有输出"
elif grep -q 'cursor plane yes' "$RUN_LOG"; then
    row "硬件光标平面" "有" "cursor plane yes"
else
    row "硬件光标平面" "无 → 走合成路径" "cursor plane no (composited)"
fi

if [[ "$OUTPUT_COUNT" -gt 0 ]]; then
    if has "falling back to CPU read-back scanout"; then
        row "GPU dmabuf 扫描输出" "**降级为 CPU 回读**" "falling back to CPU read-back scanout"
    else
        row "GPU dmabuf 扫描输出" "OK（没有降级）" "没有 fallback 日志"
    fi
fi

if grep -q 'input devices:' "$RUN_LOG"; then
    row "libinput 设备" "OK" "$(grep -m1 'input devices:' "$RUN_LOG" | sed 's/^[0-9:]* //')"
else
    row "libinput 设备" "**没有设备**" "没有 input devices 行"
fi

if has "window mapped"; then
    row "客户端窗口" "OK（$(count_of 'window mapped') 个）" "$(grep -m1 'window mapped' "$RUN_LOG" | sed 's/^[0-9:]* //')"
else
    row "客户端窗口" "**未验证**" "没有客户端连上来，纹理合成这条路没走过"
fi

if [[ "${MAX_SCANOUT:-0}" -gt 0 ]]; then
    row "直出扫描" "OK（峰值 ${MAX_SCANOUT}/s）" "frames/s scanout>0"
else
    row "直出扫描" "未触发" "需要一个全屏、未旋转、格式匹配的客户端"
fi

if [[ "${MAX_COMPOSITED:-0}" -gt 0 ]]; then
    row "GPU 合成" "OK（峰值 ${MAX_COMPOSITED}/s）" "frames/s composited>0"
else
    row "GPU 合成" "未触发" "画面没有变化过？"
fi

if [[ "${MAX_UNCHANGED:-0}" -gt 0 ]]; then
    row "空闲态翻页" "有（峰值 ${MAX_UNCHANGED}/s）" "TODO 第 2 步要消灭的就是这个数"
else
    row "空闲态翻页" "0" "静止时没有提交"
fi

if has "session inactive" && has "session active — VT"; then
    row "VT 切换" "OK（切走 $(count_of 'session inactive') 次，切回 $(count_of 'session active — VT') 次）" "session inactive/active"
elif has "session inactive"; then
    row "VT 切换" "**切走了没切回来**" "只有 session inactive"
else
    row "VT 切换" "未验证" "整个运行期间没切过 VT"
fi

if [[ "$EXIT_CODE" == "0" ]]; then
    row "退出" "干净（Esc）" "exit 0"
else
    row "退出" "**异常退出 $EXIT_CODE**" "见下面的错误行"
fi

{
    echo
    echo "## 错误行"
    echo
    ERRORS="$(grep -nE 'submit:|drm:|scanout:|vulkan:|input:|failed|rejected|error' "$RUN_LOG" | grep -v 'cursor plane' || true)"
    if [[ -n "$ERRORS" ]]; then
        echo '```'
        echo "$ERRORS"
        echo '```'
    else
        echo "没有。"
    fi

    echo
    echo "## 帧统计"
    echo
    if [[ "${STAT_LINES:-0}" -gt 0 ]]; then
        echo "共 $STAT_LINES 条 frames/s 行，峰值 composited=$MAX_COMPOSITED scanout=$MAX_SCANOUT unchanged=$MAX_UNCHANGED fallback=$MAX_FALLBACK。"
        echo
        echo '```'
        grep 'frames/s' "$RUN_LOG" | head -20
        [[ "$STAT_LINES" -gt 20 ]] && echo "…（还有 $((STAT_LINES - 20)) 行，见 run.log）"
        echo '```'
    else
        echo "一条都没有 —— 混成器没跑到第一秒，或者一帧都没提交。"
    fi

    if [[ -s "${DMESG_AFTER:-/dev/null}" ]]; then
        echo
        echo "## 运行期间的内核消息"
        echo
        echo '```'
        grep -iE 'drm|i915|amdgpu|nouveau|nvidia' "$DMESG_AFTER" | tail -30
        echo '```'
    fi

    if [[ -s "$CLIENT_LOG" ]]; then
        echo
        echo "## 客户端输出"
        echo
        echo '```'
        tail -20 "$CLIENT_LOG"
        echo '```'
    fi

    echo
    echo "## 日志开头"
    echo
    echo '```'
    head -40 "$RUN_LOG"
    echo '```'
    echo
    echo "## 日志结尾"
    echo
    echo '```'
    tail -30 "$RUN_LOG"
    echo '```'
} >>"$REPORT"

echo
echo "  报告：$REPORT"
echo "  原始日志：$RUN_LOG"
echo
sed -n '/## 逐项结论/,/^## 错误行/p' "$REPORT" | head -n -2
