#!/usr/bin/env sh
set -eu
sed -i 's/name: New Project/name: Edited in companion/' "$1"
