#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# Inherit from rhodep device
$(call inherit-product, device/motorola/rhodep/device.mk)

# Inherit Halcyon configuration.
ifeq ($(TARGET_BUILD_VARIANT),userdebug)
WITH_ADB_INSECURE := true
endif
$(call inherit-product, vendor/halcyon/config/common.mk)

ifeq ($(TARGET_BUILD_VARIANT),userdebug)
PRODUCT_NOT_DEBUGGABLE_IN_USERDEBUG :=
PRODUCT_SYSTEM_PROPERTIES += persist.sys.usb.config=adb
endif

PRODUCT_NAME := halcyon_rhodep
PRODUCT_DEVICE := rhodep
PRODUCT_MANUFACTURER := motorola
PRODUCT_BRAND := motorola
PRODUCT_MODEL := moto g82 5G

PRODUCT_GMS_CLIENTID_BASE := android-motorola

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc="rhodep_g-user 13 T1SUS33.1-124-6-16 5406e-b418a2 release-keys" \
    BuildFingerprint=motorola/rhodep_g/rhodep:13/T1SUS33.1-124-6-16/5406e-b418a2:user/release-keys \
    DeviceProduct=rhodep_g

TARGET_BOOT_ANIMATION_RES := 1080
