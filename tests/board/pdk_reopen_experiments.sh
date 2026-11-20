#!/bin/sh
# PDK reopen isolation: run on board after building/flashing.
# Confirms whether driver reload fixes "2nd ak_vi_sample no frames".
# Usage: sh pdk_reopen_experiments.sh [fullstack|sensor|help]

set -e

VI_SAMPLE="${VI_SAMPLE:-/usr/sbin/ak_vi_sample}"
OS05_SCRIPT="${OS05_SCRIPT:-/usr/sbin/os05a20.sh}"
SENSOR_KO="${SENSOR_KO:-/usr/modules/sensor_os05a20.ko}"
SENSOR_ARGS="SENSOR_I2C_ADDR=0x36 MIPI_LANES_SWAP=0x858d10 check_id=1"

run_vi_sample_once() {
    tag="$1"
    echo "========== $tag: ak_vi_sample (short capture) =========="
    # Adjust args to match your board (resolution / frame count from sample help).
    "$VI_SAMPLE" -h 2>/dev/null || true
    "$VI_SAMPLE" || return 1
    echo "========== $tag: done =========="
}

exp_fullstack() {
    echo "### Experiment 1: full driver stack reload between two runs ###"
    run_vi_sample_once "run1" || exit 1
    echo "--- os05a20.sh uninstall + install ---"
    "$OS05_SCRIPT" uninstall
    "$OS05_SCRIPT" install
    sleep 1
    run_vi_sample_once "run2_after_fullstack_reload" || {
        echo "FAIL: run2 still no frames after full stack reload"
        exit 1
    }
    echo "PASS: run2 OK after full stack reload -> PDK close does not reset drivers"
}

exp_sensor() {
    echo "### Experiment 2: sensor module reload only ###"
    run_vi_sample_once "run1" || exit 1
    echo "--- rmmod sensor_os05a20.ko; insmod ---"
    rmmod sensor_os05a20.ko 2>/dev/null || true
    insmod "$SENSOR_KO" $SENSOR_ARGS
    sleep 1
    run_vi_sample_once "run2_after_sensor_reload" || {
        echo "FAIL: run2 still no frames after sensor-only reload"
        exit 1
    }
    echo "PASS: run2 OK after sensor reload -> sensor not reset on ak_vi_close"
}

usage() {
    echo "Usage: $0 [fullstack|sensor|help]"
    echo "  fullstack  run1 -> os05a20 uninstall/install -> run2"
    echo "  sensor     run1 -> rmmod/insmod sensor_os05a20.ko -> run2"
}

case "${1:-help}" in
    fullstack) exp_fullstack ;;
    sensor)    exp_sensor ;;
    help|*)    usage ;;
esac
