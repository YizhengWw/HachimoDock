# cJSON host-test dependency

These cJSON 1.7.19 sources come from the `espressif/cjson`
1.7.19~2 dependency pinned in `firmware/dependencies.lock` and
`firmware/dependencies.v3.lock`. Upstream: https://github.com/DaveGamble/cJSON

Only `cJSON.c`, `cJSON.h`, and the MIT `LICENSE` are vendored here, with trailing
whitespace normalized and no functional changes. They allow
host runtime tests to compile the actual firmware code on a clean checkout,
without downloading ESP-IDF or depending on a developer's PlatformIO cache.
They are test-only inputs and are not added to desktop or firmware builds.

Preserve the license and source copyright notices when updating these files.
