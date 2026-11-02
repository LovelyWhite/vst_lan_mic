#!/usr/bin/env bash
# Package the LAN MIC plugins as a macOS .pkg installer (double-click for the graphical wizard).
#
# Usage:
#   ./make_pkg.sh [--version X.Y.Z] [--sign "Developer ID Installer: ..."] \
#                 [-o output.pkg] [plugin-or-dir]...
#
# Notes:
#   - Without plugin arguments, .vst3 files are found under vst3/build*.
#   - Plugins install to /Library/Audio/Plug-Ins/VST3 (admin rights requested during install).
#   - Pass a Developer ID Installer certificate via --sign for App Store-style distribution;
#     unsigned is fine for local/internal use (Gatekeeper warns on first open).
#
# Examples:
#   ./make_pkg.sh                                  # auto-package from vst3/build*
#   ./make_pkg.sh ../vst3/build-universal          # explicit directory
#   ./make_pkg.sh --version 0.1.0 "LAN MIC Send.vst3" "LAN MIC Return.vst3"

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }

usage() {
    grep '^#' "$0" | sed -e 's/^# \{0,1\}//' -e '/^$/d'
}

IDENT="cn.suxitech.lanmic"
TITLE="LAN MIC Plugins"
VERSION=""
SIGN=""
OUTPUT=""
TARGETS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --version=*) VERSION="${1#*=}"; shift ;;
        --sign) SIGN="$2"; shift 2 ;;
        --ident) IDENT="$2"; shift 2 ;;
        -o|--output) OUTPUT="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "未知选项: $1" >&2; usage >&2; exit 1 ;;
        *) TARGETS+=("$1"); shift ;;
    esac
done

# Version: parsed from project(... VERSION ...) in vst3/CMakeLists.txt by default.
if [ -z "$VERSION" ]; then
    VERSION=$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' \
        "$REPO_ROOT/vst3/CMakeLists.txt" | head -n 1)
fi
[ -n "$VERSION" ] || VERSION="0.1.0"

# Default to searching vst3/build* when no plugins are given.
if [ "${#TARGETS[@]}" -eq 0 ]; then
    shopt -s nullglob
    for d in "$REPO_ROOT"/vst3/build*; do
        TARGETS+=("$d")
    done
    shopt -u nullglob
fi
if [ "${#TARGETS[@]}" -eq 0 ]; then
    echo "错误: 未指定插件，且未找到 vst3/build* 构建输出" >&2
    exit 1
fi

# Collect plugins: accept plugin paths directly, or directories to search recursively.
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

# Deduplicate plugins by name, keeping the newest by mtime.
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

# Staging area: dist/pkg/root mirrors the target filesystem layout.
STAGE="$REPO_ROOT/dist/pkg"
rm -rf "$STAGE"
mkdir -p "$STAGE/res" "$STAGE/scripts"

LIST_PATHS=()
i=0
while [ "$i" -lt "${#U_NAMES[@]}" ]; do
    src="${U_PATHS[$i]}"
    name="${U_NAMES[$i]}"

    # macOS plugins are bundle directories and must contain Contents.
    if [ ! -d "$src/Contents" ]; then
        echo "警告: $name 缺少 Contents 目录，不是有效的 macOS 插件包，已跳过" >&2
        i=$((i + 1))
        continue
    fi

    ext="${name##*.}"; ext=$(lower "$ext")
    case "$ext" in
        vst3)      sub="VST3" ;;
        component) sub="Components" ;;
        vst)       sub="VST" ;;
        *)         sub="" ;;
    esac
    if [ -z "$sub" ]; then
        i=$((i + 1))
        continue
    fi

    dest_rel="Library/Audio/Plug-Ins/$sub/$name"
    mkdir -p "$(dirname "$STAGE/root/$dest_rel")"
    ditto "$src" "$STAGE/root/$dest_rel"
    LIST_PATHS+=("/$dest_rel")
    echo "已暂存: $name"
    i=$((i + 1))
done

if [ "${#LIST_PATHS[@]}" -eq 0 ]; then
    echo "错误: 没有有效的插件可打包" >&2
    exit 1
fi

# postinstall: fix plugin ownership and permissions.
{
    echo '#!/bin/bash'
    for p in "${LIST_PATHS[@]}"; do
        echo "chown -R root:wheel \"$p\" || true"
        echo "chmod -R u+rwX,go+rX \"$p\" || true"
    done
    echo 'exit 0'
} > "$STAGE/scripts/postinstall"
chmod +x "$STAGE/scripts/postinstall"

# Generate distribution.xml (replacing placeholders).
sed -e "s/@IDENT@/$IDENT/g" -e "s/@TITLE@/$TITLE/g" \
    "$SCRIPT_DIR/pkg/distribution.xml" > "$STAGE/distribution.xml"
cp "$SCRIPT_DIR/pkg/welcome.txt" "$STAGE/res/welcome.txt"

# Build the component package.
COMPONENT="$STAGE/lanmic-component.pkg"
if [ -n "$SIGN" ]; then
    pkgbuild --root "$STAGE/root" --identifier "$IDENT" --version "$VERSION" \
        --install-location "/" --ownership recommended \
        --scripts "$STAGE/scripts" --sign "$SIGN" --timestamp "$COMPONENT"
else
    pkgbuild --root "$STAGE/root" --identifier "$IDENT" --version "$VERSION" \
        --install-location "/" --ownership recommended \
        --scripts "$STAGE/scripts" "$COMPONENT"
fi

# Build the product package.
if [ -z "$OUTPUT" ]; then
    OUTPUT="$REPO_ROOT/dist/LANMIC_${VERSION}.pkg"
fi
mkdir -p "$(dirname "$OUTPUT")"
if [ -n "$SIGN" ]; then
    productbuild --distribution "$STAGE/distribution.xml" --resources "$STAGE/res" \
        --package-path "$STAGE" --sign "$SIGN" --timestamp "$OUTPUT"
else
    productbuild --distribution "$STAGE/distribution.xml" --resources "$STAGE/res" \
        --package-path "$STAGE" "$OUTPUT"
fi

echo
echo "安装包已生成: $OUTPUT"
