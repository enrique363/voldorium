#!/usr/bin/env bash
# Builds GGPO (third_party/ggpo: pond3r/ggpo at 7ddadef, MIT, see its LICENSE) as a static library
# for the Windows executable: %LOCALAPPDATA%/sc-m1/ggpo/libggpo.a (tools/build.py --only ggpo).
# Two local changes, both marked "sc port": an MSVC-only extra qualification removed in
# network/udp_proto.h, and ggpo_set_prediction_window (lockstep outside fights, see m1/netplay.c).
set -euo pipefail
cd "$(dirname "$0")/../.."
python -I tools/build.py --only ggpo
