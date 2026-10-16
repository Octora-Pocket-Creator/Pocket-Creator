#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local ld_gc_flag="-Wl,--gc-sections"
    local test_dir

    # The GNU linker collects unused sections with --gc-sections; ld64 on macOS
    # uses -dead_strip for the same purpose.
    if [[ "$(uname -s)" == "Darwin" ]]; then
        ld_gc_flag="-Wl,-dead_strip"
    fi

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/pockigo-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        -Icomponents/bsp/include \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        -Icomponents/bsp/include \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    for demo in audio ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" "${ld_gc_flag}" \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    # 小鸡快跑:游戏逻辑与像素美术/碰撞盒的一致性(纯 C,不需要 LVGL/ESP-IDF)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_chick_run_model.c main/chick_run_model.c \
        -o "${test_dir}/test_chick_run_model"
    "${test_dir}/test_chick_run_model"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_chick_run_sprites.c main/chick_run_sprites.c main/chick_run_model.c \
        -o "${test_dir}/test_chick_run_sprites"
    "${test_dir}/test_chick_run_sprites"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_chick_run_settings.c main/chick_run_settings.c main/chick_color.c \
        -o "${test_dir}/test_chick_run_settings"
    "${test_dir}/test_chick_run_settings"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_chick_run_layout.c main/chick_run_layout.c \
        -o "${test_dir}/test_chick_run_layout"
    "${test_dir}/test_chick_run_layout"
    # 方向快手:出题/计时/命数/分数规则(纯 C,不需要 LVGL/ESP-IDF)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_pocket_reflex_model.c main/pocket_reflex_model.c \
        -o "${test_dir}/test_pocket_reflex_model"
    "${test_dir}/test_pocket_reflex_model"
    # 番茄闹钟:25/5 循环、暂停冻结、重置、调时长、通知窗口(纯 C,不需要 ESP-IDF)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_pomodoro_model.c main/pocket_pomodoro_model.c \
        -o "${test_dir}/test_pomodoro_model"
    "${test_dir}/test_pomodoro_model"
    # 抖动掩码:rank 是排列、各占空比下都是蓝噪声(相邻点亮率低于独立随机)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_chick_dither.c main/chick_dither.c \
        -o "${test_dir}/test_chick_dither"
    "${test_dir}/test_chick_dither"
    # 渐变抖动:端点平坦、行平均严格执行理想渐变、相邻行最多差一个量化档。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_chick_gradient.c main/chick_gradient.c main/chick_dither.c \
        -o "${test_dir}/test_chick_gradient"
    "${test_dir}/test_chick_gradient"
    # 屏幕色温补偿:暖度 0 必须严格不改画面,查表与直接计算必须一致。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_chick_color.c main/chick_color.c \
        -o "${test_dir}/test_chick_color"
    "${test_dir}/test_chick_color"
    # 音频波形/包络:音符边界不能出现满幅硬切(爆音)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_rtttl_wave.c main/rtttl_wave.c \
        -o "${test_dir}/test_rtttl_wave"
    "${test_dir}/test_rtttl_wave"
    # 电量:采样防抖(去极值/IIR/滞回) + 分压还原 + 电压→电量曲线(纯逻辑,不需要 ESP-IDF/ADC)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/include \
        tests/test_bsp_battery_math.c components/bsp/src/bsp_battery_math.c \
        -o "${test_dir}/test_bsp_battery_math"
    "${test_dir}/test_bsp_battery_math"
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_chick_run_text.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_chick_run_audio.py

    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_pocketport_skills.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/pockigo-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/pockigo-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/PockiGo-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/PockiGo-full.bin" \
        "${repo_root}/build/PockiGo-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
