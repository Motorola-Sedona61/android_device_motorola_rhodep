#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

from camera.gpu_layout import fix_gpu_layout

from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.fixups_lib import (
    lib_fixups,
    lib_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

namespace_imports = [
    'device/motorola/rhodep',
    'device/motorola/sm6375-common',
    'hardware/motorola',
    'hardware/qcom-caf/common/libqti-perfd-client',
    'hardware/qcom-caf/sm6375-6.1',
    'vendor/motorola/sm6375-common',
]

lib_fixups: lib_fixups_user_type = {
    **lib_fixups,
}

blob_fixups: blob_fixups_user_type = {
    'vendor/lib64/camera/components/com.qti.node.gpu.so': blob_fixup()
        .call(fix_gpu_layout),
    'vendor/lib64/libBSTSWAD.so': blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_lock')
        .clear_symbol_version('AHardwareBuffer_lockPlanes')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),
    (
        'vendor/lib64/camera/components/com.qti.node.dewarp.so',
        'vendor/lib64/camera/components/com.vidhance.node.ica.so',
        'vendor/lib64/camera/components/com.vidhance.node.processing.so'
    ): blob_fixup()
        .replace_needed('libui.so', 'libui-v34.so'),
    'vendor/lib64/hw/camera.qcom.so': blob_fixup()
        .replace_needed('libsnsapi.so', 'libsnsapi-stock.so'),
    # Linux 6.1 has no /dev/ion; the common shim serves these from DMA-BUF heaps.
    (
        'vendor/lib64/libacdbloader.so',
        'vendor/lib64/libets_teeclient_v2.so',
    ): blob_fixup()
        .replace_needed('libion.so', 'libion_qsecom_shim.so'),
    'vendor/lib64/libmot_chi_desktop_helper.so': blob_fixup()
        .add_needed('libgui_shim_vendor.so'),
}  # fmt: skip

module = ExtractUtilsModule(
    'rhodep',
    'motorola',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
    add_firmware_proprietary_file=False,
)

if __name__ == '__main__':
    utils = ExtractUtils.device_with_common(
        module, 'sm6375-common', module.vendor
    )
    utils.run()
