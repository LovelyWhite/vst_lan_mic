#!/usr/bin/env bash
# 将 VST3 / AudioUnit (.component) / VST2 (.vst) 插件安装到 macOS 标准插件目录。
#
# 用法:
#   ./install_vst_macos.sh [--user] <插件或目录>...
#
# 说明:
#   - 目录会被递归搜索其中的插件；同名插件保留修改时间最新的一个。
#   - 默认安装到系统目录（需要输入管理员密码），加 --user 安装到当前用户目录。
#
# 示例:
#   ./install_vst_macos.sh LAN_MIC_Send.vst3
#   ./install_vst_macos.sh --user ~/Downloads/lan_mic_return.vst3
#   ./install_vst_macos.sh ../vst3/build

set -euo pipefail

lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }

usage() {
    grep '^#' "$0" | sed -e 's/^# \{0,1\}//' -e '/^$/d'
}

SCOPE_USER=0
TARGETS=()
for arg in "$@"; do
    case "$arg" in
        --user|-u) SCOPE_USER=1 ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "未知选项: $arg" >&2; usage >&2; exit 1 ;;
        *) TARGETS+=("$arg") ;;
    esac
done

if [ "${#TARGETS[@]}" -eq 0 ]; then
    usage >&2
    exit 1
fi

PLUGINS=()
for t in "${TARGETS[@]}"; do
    if [ ! -e "$t" ]; then
        echo "错误: 找不到 $t" >&2
        exit 1
    fi
    case "$(lower "$t")" in
        *.vst3|*.component|*.vst)
            PLUGINS+=("$t")
            ;;
        *)
            if [ -d "$t" ]; then
                while IFS= read -r f; do
                    PLUGINS+=("$f")
                done < <(find "$t" \( -name '*.vst3' -o -name '*.component' -o -name '*.vst' \) | sort)
            else
                echo "跳过不支持的文件: $t" >&2
            fi
            ;;
    esac
done

if [ "${#PLUGINS[@]}" -eq 0 ]; then
    echo "错误: 未在给定路径中找到任何插件 (.vst3 / .component / .vst)" >&2
    exit 1
fi

# 同名插件去重，保留修改时间最新的一个（用于过滤 Debug/Release 等重复构建产物）
U_NAMES=(); U_PATHS=(); U_MTIME=()
for src in "${PLUGINS[@]}"; do
    name=$(basename "$src")
    mt=$(stat -f %m "$src")
    idx=-1
    for i in "${!U_NAMES[@]}"; do
        if [ "${U_NAMES[$i]}" = "$name" ]; then
            idx=$i
            break
        fi
    done
    if [ "$idx" -lt 0 ]; then
        U_NAMES+=("$name"); U_PATHS+=("$src"); U_MTIME+=("$mt")
    elif [ "$mt" -ge "${U_MTIME[$idx]}" ]; then
        U_PATHS[$idx]=$src; U_MTIME[$idx]=$mt
    fi
done

installed=0
for i in "${!U_NAMES[@]}"; do
    src="${U_PATHS[$i]}"
    name="${U_NAMES[$i]}"
    ext="${name##*.}"; ext=$(lower "$ext")

    # macOS 上插件均为 bundle 目录，必须包含 Contents
    if [ ! -d "$src/Contents" ]; then
        echo "警告: $name 缺少 Contents 目录，不是有效的 macOS 插件包，已跳过" >&2
        continue
    fi

    case "$ext" in
        vst3)      base="Library/Audio/Plug-Ins/VST3" ;;
        component) base="Library/Audio/Plug-Ins/Components" ;;
        vst)       base="Library/Audio/Plug-Ins/VST" ;;
    esac
    if [ "$SCOPE_USER" -eq 1 ]; then
        dest_dir="$HOME/$base"
    else
        dest_dir="/$base"
    fi

    SUDO=""
    if [ "$SCOPE_USER" -eq 0 ]; then
        if [ ! -d "$dest_dir" ]; then
            sudo mkdir -p "$dest_dir"
        fi
        if [ ! -w "$dest_dir" ]; then
            SUDO="sudo"
        fi
    else
        mkdir -p "$dest_dir"
    fi

    dest="$dest_dir/$name"
    $SUDO rm -rf "$dest"
    $SUDO ditto "$src" "$dest"

    # 移除下载隔离标记，避免 DAW 拒绝加载（best-effort）
    $SUDO xattr -dr com.apple.quarantine "$dest" 2>/dev/null || true

    echo "已安装: $name -> $dest"
    installed=$((installed + 1))
done

if [ "$installed" -eq 0 ]; then
    echo "错误: 没有插件被安装" >&2
    exit 1
fi

echo
echo "安装完成，共 $installed 个插件。请重启 DAW 或重新扫描插件。"
