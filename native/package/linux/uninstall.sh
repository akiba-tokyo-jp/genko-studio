#!/bin/sh
# install.sh で入れた Genko Studio (C++ 版) を外す。外すのはプログラムの入ったフォルダーとメニューの項目・起動の
# リンク（と、このプログラムを指している genko:// のリンクの登録）だけで、原稿（.genko のフォルダー）・設定・
# 復旧用のコピー・旧版（Python 版）は残す。
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
version=$(basename "$here" | sed -n 's/^genko-studio-\(.*\)-linux-x86_64$/\1/p')
[ -n "$version" ] || version=$(basename "$here" | sed -n 's/^genko-studio-//p')
[ -n "$version" ] || version=dev
data=${XDG_DATA_HOME:-$HOME/.local/share}
target=$HOME/.local/opt/genko-studio-$version
rm -f "$data/applications/genko-studio-$version.desktop"
if [ "$(readlink "$HOME/.local/bin/genko-studio" 2>/dev/null || true)" = "$target/bin/genko" ]; then
  rm -f "$HOME/.local/bin/genko-studio"
fi
# (アプリの中で登録した genko:// のリンクが、外すプログラムを指しているときだけ外す。旧版のものは残す)
links="$data/applications/genko-links.desktop"
if [ -f "$links" ] && grep -qF "$target/bin/genko" "$links"; then
  rm -f "$links"
  echo "genko:// のリンクの登録も外しました（旧版で使うときは、旧版でもう一度登録します）"
fi
case "$target" in
  "$HOME"/.local/opt/genko-studio-*) rm -rf "$target" ;;
  *) echo "外す場所が想定と違います: $target" >&2; exit 1 ;;
esac
echo "外しました: $target（原稿と設定は残しています）"
