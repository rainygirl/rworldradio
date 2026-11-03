#!/bin/sh
# Installs R World Radio into /Applications (or ~/Applications with --user).
#
# The bundle is self-contained: the dataset and the CA store live inside it under
# Contents/Resources, so uninstalling is dragging it to the Trash. Run
# `make bundle` first.

set -e

here=$(cd "$(dirname "$0")" && pwd)
app="$here/build/RWorldRadio.app"
destination=/Applications

usage() {
    cat <<EOF
usage: install.sh [--user] [--prefix DIR] [--uninstall]

  --user        install into ~/Applications instead of /Applications
  --prefix DIR  install into DIR
  --uninstall   remove an installed copy
EOF
}

mode=install

while [ $# -gt 0 ]; do
    case "$1" in
        --user)      destination="$HOME/Applications" ;;
        --prefix)    shift; destination="$1" ;;
        --uninstall) mode=uninstall ;;
        -h|--help)   usage; exit 0 ;;
        *)           echo "unknown option: $1" >&2; usage; exit 1 ;;
    esac
    shift
done

target="$destination/RWorldRadio.app"

if [ "$mode" = uninstall ]; then
    if [ -d "$target" ]; then
        rm -rf "$target"
        echo "removed $target"
    else
        echo "nothing installed at $target"
    fi
    exit 0
fi

if [ ! -d "$app" ]; then
    echo "$app does not exist - run 'make bundle' first" >&2
    exit 1
fi

# A bundle with no dataset would launch and then say it found nothing, which is a
# confusing way to discover the build was incomplete.
if [ ! -f "$app/Contents/Resources/stations.dat" ]; then
    echo "$app has no stations.dat - run 'make dataset' then 'make bundle'" >&2
    exit 1
fi

mkdir -p "$destination"
rm -rf "$target"
cp -R "$app" "$target"

# The Finder caches bundle metadata aggressively; touching the bundle is what
# makes a reinstalled copy pick up a changed icon or version on Tiger.
touch "$target"

echo "installed $target"
echo
echo "The dataset and CA store are inside the bundle. To uninstall:"
echo "  ./install.sh --uninstall"
