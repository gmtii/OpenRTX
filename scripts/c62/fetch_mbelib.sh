#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetches mbelib (ISC licence) for the optional DMR voice of the C62 DMR
# receive mode. The AMBE+2 algorithm is covered by patents in some
# countries, so mbelib is not part of OpenRTX or of its west manifest: check
# that using it is lawful where you are before fetching it.
#
# Usage: fetch_mbelib.sh [west workspace]
# The workspace defaults to the folder holding the OpenRTX checkout. The next
# build finds mbelib in <workspace>/modules/lib/mbelib and enables DMR voice
# (CMake option C62_DMR_VOICE, AUTO by default).
set -e
TOP=${1:-$(cd "$(dirname "$0")/../../.." && pwd)}
DEST=$TOP/modules/lib/mbelib
REPO=https://github.com/lwvmobile/mbelib
COMMIT=34adf9f054bc5650ace162a4917dcbc2cfa6102e # version tested with OpenRTX

if [ ! -d "$TOP/.west" ]; then
    echo "$TOP is not a west workspace (no .west folder): pass its path" >&2
    exit 1
fi
if [ -d "$DEST/.git" ]; then
    echo "mbelib already present in $DEST"
else
    git clone -q "$REPO" "$DEST"
fi
git -C "$DEST" checkout -q "$COMMIT"
echo "mbelib ready in $DEST: the next build has DMR voice."
