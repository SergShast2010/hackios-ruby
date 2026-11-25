/*
 * Network Block Device (HNBD v1) over USB CDC-ECM / virtio-net TCP.
 * Exposes host disk images as /dev/nbd0..1 (block) and /dev/rnbd0..1 (raw)
 * without touching the phone's UFS flash.
 */
#ifndef _SYS_HAKIOS_NBD_H_
#define _SYS_HAKIOS_NBD_H_

#include <stdbool.h>
#include <sys/types.h>

void hakios_nbd_init(void);
bool hakios_nbd_dev(dev_t dev);
void hakios_nbd_quiesce(void);

#endif /* _SYS_HAKIOS_NBD_H_ */
