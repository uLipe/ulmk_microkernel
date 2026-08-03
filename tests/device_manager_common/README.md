# device_manager_common

In-tree copies of the **display** and **input** class contracts used by
`device_manager_unit` and `sdk_suite/device_manager`.

Policy headers live in `ulmk_apps/ulmk_device_classes/`. Kernel CI does not
check out that repo, so these fixtures keep the suite hermetic. Prefer the
apps tree when present (dev.py mounts `/ulmk_apps`); fall back here otherwise.

Keep in sync with `ulmk_apps/ulmk_device_classes/include/ulmk_device_{display,input}.h`.
