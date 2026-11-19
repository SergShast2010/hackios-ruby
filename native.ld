SECTIONS {
 . = 0xfffffff007472368;
  .text : { __nt_text_start = .; *(.text.start) *(.text*) *(.rodata*) __nt_text_end = .; }
  .switch 0xfffffff0074729f0 : { __nt_switch_start = .; *(.switch) __nt_switch_end = .; }
  .extra 0xfffffff007472c14 : { __nt_extra_start = .; *(.extra) __nt_extra_end = .; }
  __nt_switch_limit = 0xfffffff007472bf0;
  __nt_extra_limit = 0xfffffff007472f98;
  ASSERT(__nt_text_end <= 0xfffffff0074729ec, "native .text overlaps loader_jump")
  ASSERT(__nt_switch_end <= __nt_switch_limit, "native .switch overflows cave")
  ASSERT(__nt_extra_end <= __nt_extra_limit, "native .extra overflows cave")
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
XnuCurrentProcessor = 0xfffffff007451934;
XnuIdleThreadCreate = 0xfffffff0073099d4;
XnuSchedStartup = 0xfffffff007309aec;
XnuThreadBlockReason = 0xfffffff007303980;
XnuAssertWait = 0xfffffff0073017c8;
