/* The letters (data/ticket_fonts.bin) under the names the firmware's linker gives them. */
    .section __DATA,__const
    .globl _binary_ticket_fonts_bin_start
    .globl _binary_ticket_fonts_bin_end
_binary_ticket_fonts_bin_start:
    .incbin "data/ticket_fonts.bin"   /* run.sh compiles from the firmware folder */
_binary_ticket_fonts_bin_end:
    .byte 0
