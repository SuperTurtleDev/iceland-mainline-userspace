#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
exec bash "$HERE/../../pencil/build.sh" "${DEB_OUT:?set by build.sh}"
