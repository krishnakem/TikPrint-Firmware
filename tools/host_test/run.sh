#!/bin/sh
# Try board code on a computer (see README.txt).
#   run.sh code
#   run.sh qr [folder for the pictures]
#   run.sh ticket <app> <reply.json> [out.png] [font] [size 0-2]
set -e
fw=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/tikprint_host_test
mkdir -p "$out"
cd "$fw"   # everything below is named from the firmware folder, so that spaces in the path above it do not matter
here=tools/host_test
ref=$here/reference   # connector ticket code that left the firmware: host-only, kept for the cloud's drift test
cjson=managed_components/espressif__cjson/cJSON
flags="-std=gnu11 -O1 -g -Wall -I$here/shims -I$ref -Imain -I$cjson"
drawing="$ref/conn_ticket.c main/ticket_draw.c $cjson/cJSON.c $here/fonts.S"
apps="$ref/conn_todoist.c"

case "$1" in
code)
    cc $flags -o "$out/test_code" $here/test_code.c main/paper_code.c
    "$out/test_code"
    ;;
qr)
    qrlib=managed_components/espressif__qrcode
    cc $flags -I$qrlib/include -I$qrlib -o "$out/test_qr" $here/test_qr.c main/qr_ticket.c main/ticket_draw.c \
        $qrlib/esp_qrcode_main.c $qrlib/esp_qrcode_wrapper.c $qrlib/qrcodegen.c $here/fonts.S
    "$out/test_qr" "${2:-$out}"
    for f in "${2:-$out}"/qr_*.pbm; do python3 $here/pbm_to_png.py "$f" "${f%.pbm}.png"; done
    echo "QR tickets drawn: ${2:-$out}/qr_*.png"
    ;;
ticket)
    png=${4:-$out/ticket.png}
    cc $flags -o "$out/test_ticket" $here/test_ticket.c $drawing $apps
    "$out/test_ticket" "$2" "$3" "$out/ticket.pbm" "${5:-Inter}" "${6:-1}"
    python3 $here/pbm_to_png.py "$out/ticket.pbm" "$png"
    echo "ticket drawn: $png"
    ;;
*)
    sed -n '2,4p' "$0"; exit 2 ;;
esac
