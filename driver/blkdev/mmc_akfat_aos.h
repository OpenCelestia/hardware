/* SDK libakfat32fs.a. Not a public module header. */
#ifndef HARDWARE_DRIVER_MMC_AKFAT_AOS_H
#define HARDWARE_DRIVER_MMC_AKFAT_AOS_H

#if !CONFIG_OS_LINUX

/* Quick format block device (e.g. /dev/mmcblk0p0). Returns 0 on success. */
int Driver_QuickFormat(unsigned char *pdev_name);

#endif /* !CONFIG_OS_LINUX */

#endif /* HARDWARE_DRIVER_MMC_AKFAT_AOS_H */
