SUMMARY = "AESD char driver kernel module"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

SRC_URI = "git://git@github.com/cu-ecen-aeld/assignment-9-KirSpaceB.git;protocol=ssh;branch=main"
PV = "1.0+git${SRCPV}"
SRCREV = "1ac74eba41ed82e52ae6e89ecec56516dbd03791"

S = "${WORKDIR}/git/aesd-char-driver"

MODULES_MODULE_SYMVERS_LOCATION = "aesd-char-driver"
EXTRA_OEMAKE += " -C ${STAGING_KERNEL_DIR} M=${S}"

do_install () {
    module_do_install
}

RPROVIDES:${PN} += "kernel-module-aesdchar"
