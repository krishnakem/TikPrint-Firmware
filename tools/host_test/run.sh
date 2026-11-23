#!/bin/sh
# Try connector code on a computer (see README.txt).
#   run.sh rules
#   run.sh ticket <app> <reply.json> [out.png] [font] [size 0-2]
set -e
fw=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/tikprint_host_test
mkdir -p "$out"
cd "$fw"   # everything below is named from the firmware folder, so that spaces in the path above it do not matter
here=tools/host_test
cjson=managed_components/espressif__cjson/cJSON
flags="-std=gnu11 -O1 -g -Wall -I$here/shims -Imain -I$cjson"
drawing="main/conn_ticket.c main/ticket_draw.c $cjson/cJSON.c $here/fonts.S"
apps="main/conn_todoist.c"

case "$1" in
rules)
    cc $flags -Dtime=test_time "-DCONNECTORS_TEST_APPS=&conn_test_a, &conn_test_b," \
        -o "$out/test_rules" $here/test_rules.c main/connectors.c $drawing $apps
    rm -f "$out/nvs.tmp"
    TZ=America/Detroit "$out/test_rules" before "$out/nvs.tmp" && TZ=America/Detroit "$out/test_rules" after "$out/nvs.tmp"
    rm -f "$out/nvs.tmp"
    ;;
ticket)
    png=${4:-$out/ticket.png}
    cc $flags -o "$out/test_ticket" $here/test_ticket.c $drawing $apps
    "$out/test_ticket" "$2" "$3" "$out/ticket.pbm" "${5:-Inter}" "${6:-1}"
    python3 $here/pbm_to_png.py "$out/ticket.pbm" "$png"
    echo "ticket drawn: $png"
    ;;
*)
    sed -n '2,5p' "$0"; exit 2 ;;
esac
