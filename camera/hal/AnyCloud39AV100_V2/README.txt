AnyCloud39AV100_V2 camera HAL (Anyka PDK 2.x)
=========================================

API (unchanged vs AnyCloud39AV200 / nova tree)
----------------------------------------------
  hal_camera_api.h + camera_hw.c own the device/stream/AI contract. This directory only
  implements those hooks using the PDK 2.x APIs (ak_vi / ak_vpss / ak_svp2), same entry points as AV200.

Implementation reference (PDK samples, not product scripts)
-----------------------------------------------------------
  VI pipeline:  platform/sample/vi/ak_vi_sample.c  (start_vi: open, load_sensor_cfg,
                get_sensor_resolution, set_dev_attr, set_chn_attr_ex, enable, get_frame)
  Multi-CIS:    platform/sample/multi_CISs/ak_multi_CISs_sample.c
  SVP2 / NPU:   platform/sample/svp2/ak_svp2_sample.c  (ak_npu_init, ak_svp2_create_chn, conf path)

Runtime INI
-----------
  /etc/config/hal_camera_config.ini — keys compatible with AV200 HAL for camera_hw.
  KM01W PDB: camera0:isp_config_path -> /etc/isp_os05a20_mipi_2lane_km01a.conf (from board
  rootfs/utils/etc/config/, deployed by build_rootfs process_utils).
  If INI is missing, HAL uses the same path as ISP_CONFIG_PATH_DEFAULT in hal_camera_config.c.
  isp_gc5603_mipi_2lane_av200.conf is only the AnyCloud39AV200 HAL fallback, not AV100_V2.

  Optional [ai_detect_subN] / [ai_modelN] sections: fill using ak_svp2_sample.c and on-device
  model binaries; do not assume anypi_nova board file layout.

Chip notes
----------
  HAL_CAMERA_HAS_IPU = 0; [ipu] in INI is ignored on this chip.

Known issues (PDK, not HAL logic bugs)
--------------------------------------
  1. Second open after ak_vi_close (new process): stock ak_vi_sample also gets no frames.
     Close path matches sample: disable_chn -> disable_dev -> ak_vi_close.
     See hardware/tests/docs/PDK_VI_REOPEN_ISSUE.md and tests/board/pdk_reopen_experiments.sh.

  2. Main-only stream (app uses ch0 only): dev_attr needs sub_max; PDK enable_dev may start
     ch0+ch1. hal_camera_sync_platform_vi_after_channels registers placeholder ch1;
     hal_camera_vi_quiet_placeholder_ch1 disables ch1 after enable_dev (app never enables ch1).

  3. Stop order: shared camera_hw marks DISABLE -> hal_camera_quiesce_channel (release,
     optional HAL_CAMERA_QUIESCE_PRE_DELAY_MS in hal_camera_chip_config.h) -> disable_chn
     -> join -> hal_camera_disable_channel. channel_stopping suppresses get_frame fail logs.
      Pending frames are released only while the VI channel is still enabled to avoid
      ak_vi_release_frame() warnings during shutdown races.
     NONE does not ak_vi_disable_dev; full close on cdev_close -> hal_camera_disable_device.

  4. Env: HAL_CAMERA_VI_3DNR_KB=0 skips ak_vi_set_dev_3DNR_memsize (PDK default).
