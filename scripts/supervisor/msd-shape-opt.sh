#!/bin/bash
# Supervisor wrapper for the MSD shape-optimization web interface on a Vast.ai base-image instance
# (same pattern as /opt/supervisor-scripts/tensorboard.sh). Installed by scripts/install_vast_service.sh.
#
# utils/cleanup_generic.sh is deliberately NOT sourced: it kills every descendant process on exit, which would
# abort optimization jobs whenever the web service restarts. Jobs run in their own sessions, survive a restart of
# the web server, and are re-attached by it when it starts again.
utils=/opt/supervisor-scripts/utils
. "${utils}/logging.sh"
. "${utils}/environment.sh"
. "${utils}/exit_serverless.sh"
. "${utils}/exit_portal.sh" "MSD Shape Optimization"

MSD_ROOT="${MSD_ROOT:-/workspace/Projects/msd_shape_opt}"
cd "${MSD_ROOT}"
pty "${MSD_ROOT}/scripts/run_web.sh" --host 127.0.0.1 --port "${MSD_PORT:-17860}" 2>&1
