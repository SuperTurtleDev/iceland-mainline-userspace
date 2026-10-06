#!/usr/bin/env bash
# Called by ../../build.sh before the rootfs stages so WORK/debs holds a
# deb matching the current kernel build. Delegates to the fastcharge
# builder; DEB_OUT (WORK/debs) becomes its output directory.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec bash "${HERE}/../../fastcharge/build.sh" "${DEB_OUT:?set by build.sh}"
