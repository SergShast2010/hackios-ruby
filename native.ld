SECTIONS {
 . = 0xfffffff007472368;
 .text : { *(.text.start) *(.text*) *(.rodata*) }
 .switch 0xfffffff0074729f0 : { *(.switch) }
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
XnuCurrentProcessor = 0xfffffff007451934;
XnuIdleThreadCreate = 0xfffffff0073099d4;
XnuSchedStartup = 0xfffffff007309aec;
XnuThreadBlockReason = 0xfffffff007303980;
XnuAssertWait = 0xfffffff0073017c8;
