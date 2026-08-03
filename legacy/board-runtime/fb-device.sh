#!/bin/sh
# Resolve the board runtime framebuffer from sysfs metadata.

fb_device_exists() {
    path="$1"
    if [ -c "$path" ]; then
        return 0
    fi
    # Tests use regular files under a fake /dev root.
    if [ "${PET_CLAW_FB_DEV_ROOT:-/dev}" != "/dev" ] && [ -e "$path" ]; then
        return 0
    fi
    return 1
}

fb_read_sysfs() {
    path="$1"
    if [ -f "$path" ]; then
        tr -d '\r\n' < "$path" 2>/dev/null
    fi
}

fb_score_candidate() {
    name="$1"
    size="$2"
    bpp="$3"
    score=0

    case "$name" in
        fb_ili9341) score=$((score + 100));;
        *ili9341*|*ILI9341*) score=$((score + 80));;
        *fbtft*|*FBTFT*|*spi*|*SPI*) score=$((score + 60));;
        *BCM*|*DRM*|*drm*|*simple*) score=$((score - 40));;
    esac

    case "$size" in
        320,240|240,320) score=$((score + 30));;
    esac

    case "$bpp" in
        16) score=$((score + 20));;
        32) score=$((score + 5));;
    esac

    echo "$score"
}

fb_resolve_device() {
    requested="${1:-${PET_CLAW_FB_DEV:-auto}}"
    sysfs_root="${PET_CLAW_FB_SYSFS_ROOT:-/sys/class/graphics}"
    dev_root="${PET_CLAW_FB_DEV_ROOT:-/dev}"

    case "$requested" in
        ""|auto|AUTO) ;;
        *)
            if fb_device_exists "$requested"; then
                echo "$requested"
                return 0
            fi
            ;;
    esac

    best_dev=""
    best_score=-9999

    for fb_path in "$sysfs_root"/fb[0-9]*; do
        [ -d "$fb_path" ] || continue
        fb_name="${fb_path##*/}"
        fb_num="${fb_name#fb}"
        case "$fb_num" in
            ""|*[!0-9]*) continue;;
        esac

        dev_path="$dev_root/fb$fb_num"
        fb_device_exists "$dev_path" || continue

        name="$(fb_read_sysfs "$fb_path/name")"
        size="$(fb_read_sysfs "$fb_path/virtual_size")"
        bpp="$(fb_read_sysfs "$fb_path/bits_per_pixel")"
        score="$(fb_score_candidate "$name" "$size" "$bpp")"

        if [ "$score" -gt "$best_score" ]; then
            best_score="$score"
            best_dev="$dev_path"
        fi
    done

    if [ -n "$best_dev" ]; then
        echo "$best_dev"
        return 0
    fi

    if fb_device_exists "$dev_root/fb0"; then
        echo "$dev_root/fb0"
        return 0
    fi

    return 1
}

fb_device_number_from_path() {
    case "${1:-}" in
        */fb*) echo "${1##*/fb}";;
        *) echo "0";;
    esac
}
