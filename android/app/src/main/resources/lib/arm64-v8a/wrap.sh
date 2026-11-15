#!/system/bin/sh
# Debuggable builds only: reserve the guest image address range before ART starts (see src/runtime/aot_reserve.c)
HERE="$(cd "$(dirname "$0")" && pwd)"
export LD_PRELOAD="$HERE/libaotreserve.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec "$@"
