#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetches OpenDMR (GPL-2.0-or-later / GPL-3.0-or-later, the AMBE+2 encoder of
# OP25 by KA1RBI and Pavel Yazev's fixed point IMBE analysis) for the
# optional DMR encoder of the C62. As for mbelib, the AMBE+2 algorithm is
# covered by patents in some countries: check that using it is lawful where
# you are before fetching it.
#
# Usage: fetch_opendmr.sh [west workspace]
# The workspace defaults to the folder holding the OpenRTX checkout. The next
# build finds OpenDMR in <workspace>/modules/lib/opendmr (CMake option
# C62_DMR_ENC, AUTO by default).
set -e
TOP=${1:-$(cd "$(dirname "$0")/../../.." && pwd)}
DEST=$TOP/modules/lib/opendmr
REPO=https://github.com/MW0MWZ/OpenDMR
COMMIT=d28164b39ba4d91ad5948ff22707937f8944f70f # version tested with OpenRTX

if [ ! -d "$TOP/.west" ]; then
    echo "$TOP is not a west workspace (no .west folder): pass its path" >&2
    exit 1
fi
if [ -d "$DEST/.git" ]; then
    echo "OpenDMR already present in $DEST"
else
    git clone -q "$REPO" "$DEST"
fi
git -C "$DEST" checkout -q "$COMMIT"
echo "OpenDMR ready in $DEST: the next build has the DMR encoder."
