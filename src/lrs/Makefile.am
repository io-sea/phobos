AM_CFLAGS= $(CC_OPT)

sbin_PROGRAMS=phobosd

noinst_LTLIBRARIES=libpho_lrs.la

unitdir=`pkg-config systemd --variable=systemdsystemunitdir`
unit_files=phobosd.service
unit_DATA=$(unit_files)
EXTRA_DIST=$(unit_files)

phobosd_SOURCES=lrs.c \
	lrs_cfg.h lrs_cfg.c \
	lrs_sched.h lrs_sched.c \
	lrs_device.h lrs_device.c \
	lrs_thread.h lrs_thread.c
phobosd_CFLAGS=$(AM_CFLAGS)
phobosd_LDADD=../dss/libpho_dss.la ../cfg/libpho_cfg.la ../ldm/libpho_ldm.la \
	      ../module-loader/libpho_module_loader.la ../io/libpho_io.la \
	      ../serializer/libpho_serializer.la ../communication/libpho_comm.la \
	      ../common/libpho_common.la
phobosd_LDFLAGS=-Wl,-rpath=$(libdir) -Wl,-rpath=$(pkglibdir)

libpho_lrs_la_SOURCES=lrs_cfg.c lrs_sched.c lrs_device.c lrs_thread.c
