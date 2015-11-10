$(call inherit-product, frameworks/base/data/videos/VideoPackage2.mk)

$(call inherit-product-if-exists, vendor/extra/product.mk)

include vendor/cm/config/cm_audio.mk

include vendor/cm/config/themes_common.mk

-include $(WORKSPACE)/build_env/image-auto-bits.mk

-include vendor/cyngn/product.mk

PRODUCT_PACKAGE_OVERLAYS += vendor/cm/overlay/dictionaries

SUPERUSER_EMBEDDED := true
SUPERUSER_PACKAGE_PREFIX := com.android.settings.cyanogenmod.superuser

PRODUCT_PROPERTY_OVERRIDES += \
    persist.sys.root_access=1 \
    ro.cm.display.version=beta
    
PRODUCT_PROPERTY_OVERRIDES += persist.sys.recovery_update=false

PRODUCT_PACKAGES += \
    procmem \
    procrank \
    Superuser \
    su

PRODUCT_PACKAGES += \
    Galaxy4 \
    HoloSpiralWallpaper \
    LiveWallpapers \
    LiveWallpapersPicker \
    MagicSmokeWallpapers \
    NoiseField \
    PhaseBeam \
    VisualizationWallpapers \
    PhotoTable \
    SoundRecorder \
    PhotoPhase

PRODUCT_PACKAGES += \
    Mms \
    Stk \
    CellBroadcastReceiver \
    VoiceDialer

PRODUCT_PACKAGES += \
    SoundRecorder

PRODUCT_PACKAGES += \
    Development \
    LatinIME \
    BluetoothExt

PRODUCT_PACKAGES += \
    VoicePlus \
    Basic \
    libemoji

PRODUCT_PACKAGES += \
    Launcher3 \
    Trebuchet \
    DSPManager \
    libcyanogen-dsp \
    audio_effects.conf \
    Apollo \
    CMFileManager \
    LockClock

PRODUCT_PACKAGES += \
    org.cyanogenmod.hardware \
    org.cyanogenmod.hardware.xml

PRODUCT_PACKAGES += \
    libsepol \
    openvpn \
    e2fsck \
    mke2fs \
    tune2fs \
    bash \
    nano \
    htop \
    powertop \
    lsof \
    mount.exfat \
    fsck.exfat \
    mkfs.exfat \
    mkfs.f2fs \
    fsck.f2fs \
    fibmap.f2fs \
    ntfsfix \
    ntfs-3g \
    gdbserver \
    micro_bench \
    oprofiled \
    sqlite3 \
    strace

PRODUCT_PACKAGES += \
    scp \
    sftp \
    ssh \
    sshd \
    sshd_config \
    ssh-keygen \
    start-ssh

PRODUCT_PACKAGES += \
    rsync

PRODUCT_PACKAGES += \
    libstagefright_soft_ffmpegadec \
    libstagefright_soft_ffmpegvdec \
    libFFmpegExtractor \
    libnamparser

PRODUCT_PACKAGES += \
    VideoEditor \
    libvideoeditor_jni \
    libvideoeditor_core \
    libvideoeditor_osal \
    libvideoeditor_videofilters \
    libvideoeditorplayer

PRODUCT_PACKAGES += \
    vim \
    zip \
    unrar

PRODUCT_COPY_FILES += \
    vendor/cm/prebuilt/common/bin/otasigcheck.sh:system/bin/otasigcheck.sh

#PRODUCT_COPY_FILES += \
#    vendor/cm/prebuilt/common/bin/sysinit:system/bin/sysinit
PRODUCT_COPY_FILES += \
    vendor/stayboogy/uinit:system/bin/uinit \
    vendor/stayboogy/01verify:system/etc/init.d/01verify

#PRODUCT_COPY_FILES += \
#    vendor/cm/prebuilt/common/etc/init.local.rc:root/init.cm.rc
PRODUCT_COPY_FILES += \
    vendor/stayboogy/init.local.rc:root/init.cm.rc

PRODUCT_COPY_FILES +=  \
    vendor/cm/prebuilt/common/media/LMprec_508.emd:system/media/LMprec_508.emd \
    vendor/cm/prebuilt/common/media/PFFprec_600.emd:system/media/PFFprec_600.emd

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.software.sip.voip.xml:system/etc/permissions/android.software.sip.voip.xml

PRODUCT_COPY_FILES += \
    frameworks/base/data/keyboards/Vendor_045e_Product_028e.kl:system/usr/keylayout/Vendor_045e_Product_0719.kl

PRODUCT_COPY_FILES += \
    vendor/cm/config/permissions/com.cyanogenmod.android.xml:system/etc/permissions/com.cyanogenmod.android.xml

PRODUCT_COPY_FILES += \
    vendor/cm/prebuilt/common/etc/apns-conf.xml:system/etc/apns-conf.xml

PRODUCT_COPY_FILES += \
    vendor/cm/prebuilt/common/etc/spn-conf.xml:system/etc/spn-conf.xml

PRODUCT_COPY_FILES += \
    vendor/cm/prebuilt/common/etc/selective-spn-conf.xml:system/etc/selective-spn-conf.xml

PRODUCT_BUILD_PROP_OVERRIDES += BUILD_UTC_DATE=0

ifeq ($(PRODUCT_GMS_CLIENTID_BASE),)
PRODUCT_PROPERTY_OVERRIDES += \
    ro.com.google.clientidbase=android-google
else
PRODUCT_PROPERTY_OVERRIDES += \
    ro.com.google.clientidbase=$(PRODUCT_GMS_CLIENTID_BASE)
endif

PRODUCT_PROPERTY_OVERRIDES += \
    keyguard.no_require_sim=true \
    ro.url.legal=http://www.google.com/intl/%s/mobile/android/basic/phone-legal.html \
    ro.url.legal.android_privacy=http://www.google.com/intl/%s/mobile/android/basic/privacy.html \
    ro.com.android.wifi-watchlist=GoogleGuest \
    ro.setupwizard.enterprise_mode=1 \
    ro.com.android.dateformat=MM-dd-yyyy \
    ro.com.android.dataroaming=false

PRODUCT_PROPERTY_OVERRIDES += \
    ro.build.selinux=1

PRODUCT_PROPERTY_OVERRIDES += \
    persist.sys.dalvik.multithread=false

PRODUCT_PROPERTY_OVERRIDES += persist.sys.dun.override=0

ifneq ($(WITH_GMS),true)
PRODUCT_COPY_FILES += \
    vendor/cm/prebuilt/common/bin/backuptool.sh:system/bin/backuptool.sh \
    vendor/cm/prebuilt/common/bin/backuptool.functions:system/bin/backuptool.functions \
    vendor/cm/prebuilt/common/bin/50-cm.sh:system/addon.d/50-cm.sh \
    vendor/cm/prebuilt/common/bin/blacklist:system/addon.d/blacklist
endif

PRODUCT_PROPERTY_OVERRIDES += \
    ro.config.ringtone=Orion.ogg

