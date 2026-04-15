SUMMARY = "misc-modules kernel modules from ldd3"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module update-rc.d

INITSCRIPT_NAME = "misc-modules-init"
INITSCRIPT_PARAMS = "defaults 98"

SRC_URI = "git://git@github.com/cu-ecen-aeld/assignment-7-KirSpaceB.git;protocol=ssh;branch=main"
SRC_URI += "file://misc-modules-init"

PV = "1.0+git${SRCPV}"
SRCREV = "af0b8cff9000d4d5bf06423aca2fd5bfa5c45e83"

S = "${WORKDIR}/git"
MODULES_MODULE_SYMVERS_LOCATION = "misc-modules"

EXTRA_OEMAKE += " -C ${STAGING_KERNEL_DIR} M=${S}/misc-modules EXTRA_CFLAGS=-I${S}/include"

do_install () {
    module_do_install
    install -d ${D}${sysconfdir}/init.d
    install -m 0755 ${WORKDIR}/misc-modules-init ${D}${sysconfdir}/init.d/misc-modules-init
}

FILES:${PN} += "${sysconfdir}/init.d/misc-modules-init"

RPROVIDES:${PN} += "kernel-module-hello kernel-module-faulty"
