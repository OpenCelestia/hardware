#ifndef __CHIP_AK3918AV130_SERIES
#define __CHIP_AK3918AV130_SERIES 1
#endif

/*
 * Sky39AV130 motion HAL. Calls match platform/sample/md (ak_mrd_* in
 * platform/dep_lib). The HAL matrix is 64 columns; libak_mrd uses MD_SIZE.
 */
#include "hal_motion_detect_api.h"

#include <string.h>

#include "ak_mrd.h"

static void *s_mrd;
static int s_day_night;

int hal_motion_detect_init(void)
{
    if (s_mrd == NULL)
        s_mrd = ak_mrd_init();
    return s_mrd != NULL ? 0 : -1;
}

int hal_motion_detect_deinit(void)
{
    if (s_mrd != NULL)
        ak_mrd_destroy(s_mrd);
    s_mrd = NULL;
    return 0;
}

int hal_motion_detect_get_day_night_status(void)
{
    return s_day_night;
}

int hal_motion_detect_set_day_night_status(int status)
{
    s_day_night = status;
    if (s_mrd != NULL)
        (void)ak_mrd_set_mode(s_mrd, (unsigned char)status);
    return 0;
}

int hal_motion_detect_get_vsize(int *vsize)
{
    if (vsize == NULL)
        return -1;
    *vsize = MD_SIZE;
    return 0;
}

static int mira_mrd_run(void *handle, unsigned short md_stat[][64], int vsize, void *boxes)
{
    unsigned short local[MD_SIZE][MD_SIZE];
    int rows;
    int i;
    int j;

    if (handle == NULL || md_stat == NULL || boxes == NULL || vsize <= 0)
        return -1;
    rows = vsize > MD_SIZE ? MD_SIZE : vsize;
    memset(local, 0, sizeof(local));
    for (i = 0; i < rows; i++) {
        for (j = 0; j < MD_SIZE; j++)
            local[i][j] = md_stat[i][j];
    }
    return ak_mrd(handle, local, rows, (MRD_RECTANGLE *)boxes);
}

int hal_motion_detect_process_mrd(unsigned short md_stat[][64], int vsize,
                                  void *boxes, int max_boxes)
{
    (void)max_boxes;
    if (s_mrd == NULL && hal_motion_detect_init() != 0)
        return -1;
    return mira_mrd_run(s_mrd, md_stat, vsize, boxes);
}

void *hal_mrd_init(void)
{
    return ak_mrd_init();
}

int hal_mrd_destroy(void *handle)
{
    return ak_mrd_destroy(handle);
}

int hal_mrd_set_filters(void *handle, unsigned short flt_big, unsigned short flt_small)
{
    return ak_mrd_set_filters(handle, flt_big, flt_small);
}

int hal_mrd_set_motion_region_size(void *handle, int size_min, int size_max)
{
    return ak_mrd_set_motion_region_size(handle, size_min, size_max);
}

int hal_mrd_set_mode(void *handle, unsigned char mode)
{
    return ak_mrd_set_mode(handle, mode);
}

int hal_mrd(void *handle, unsigned short md_stat[][64], int len, void *boxes)
{
    return mira_mrd_run(handle, md_stat, len, boxes);
}
