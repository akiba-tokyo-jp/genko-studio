#!/bin/sh
# Genko Studio (C++ 版) をこの利用者だけに入れる（管理者の権限は要らない）。
#   ./install.sh           … ~/.local/opt/genko-studio-<版> に入れ、~/.local/bin/genko-studio とメニューの項目を作る
# 原稿・設定・旧版（Python 版）には触れない。genko:// のリンクやファイルの関連付けは、アプリの中で利用者が選んだときだけ登録する。
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
version=$(basename "$here" | sed -n 's/^genko-studio-\(.*\)-linux-x86_64$/\1/p')
[ -n "$version" ] || version=dev
data=${XDG_DATA_HOME:-$HOME/.local/share}
target=$HOME/.local/opt/genko-studio-$version
if [ -e "$target" ]; then
  echo "すでに入っています: $target（先に uninstall.sh で外します）" >&2
  exit 1
fi
mkdir -p "$HOME/.local/opt" "$HOME/.local/bin" "$data/applications"
cp -R "$here" "$target"
ln -sf "$target/bin/genko" "$HOME/.local/bin/genko-studio"
sed "s|@GENKO@|$target/bin/genko|g" "$here/share/applications/genko-studio.desktop" > "$data/applications/genko-studio-$version.desktop"
echo "入れました: $target"
echo "起動: genko-studio（~/.local/bin が PATH にないときは $target/bin/genko）"
