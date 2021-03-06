# make sure both are distributed, regardless of which condition is taken
EXTRA_DIST = \
	mailwatch-plugin.c \
	mailwatch-plugin-4.2.c

if BUILD_42_PANEL_PLUGIN

plugindir = @XFCE4_PANEL_PLUGINSDIR@
plugin_LTLIBRARIES = libmailwatch.la

libmailwatch_la_LDFLAGS = \
	-avoid-version \
	-export-dynamic \
	-module

if HAVE_CYGWIN
  libmailwatch_la_LDFLAGS += \
	-no-undefined \
	-export-symbols $(datadir)/xfce4/devel/panel.def
endif

libmailwatch_la_SOURCES = \
	mailwatch-plugin-4.2.c

libmailwatch_la_CFLAGS = \
	-I$(top_srcdir)/libmailwatch-core \
	@GTHREAD_CFLAGS@ \
	@XFCE4_PANEL_CFLAGS@ \
	-DDATADIR=\"$(datadir)\" \
	-DLOCALEDIR=\"$(localedir)\"

libmailwatch_la_DEPENDENCIES = \
	$(top_builddir)/libmailwatch-core/libmailwatch-code.la

libmailwatch_la_LIBADD = \
	$(top_builddir)/libmailwatch-core/libmailwatch-core.la \
	@GTHREAD_LIBS@ \
	@XFCE4_PANEL_LIBS@ \
	@GNUTLS_LIBS@

else

plugindir = $(libexecdir)/xfce4/panel-plugins
plugin_PROGRAMS = xfce4-mailwatch-plugin

xfce4_mailwatch_plugin_SOURCES = \
	mailwatch-plugin.c

xfce4_mailwatch_plugin_CFLAGS = \
	-I$(top_srcdir)/libmailwatch-core \
	@GTHREAD_CFLAGS@ \
	@LIBXFCE4PANEL_CFLAGS@ \
	@LIBXFCEGUI4_CFLAGS@ \
	-DDATADIR=\"$(datadir)\" \
	-DLOCALEDIR=\"$(localedir)\"

xfce4_mailwatch_plugin_DEPENDENCIES = \
	$(top_builddir)/libmailwatch-core/libmailwatch-core.la

xfce4_mailwatch_plugin_LDADD = \
	$(top_builddir)/libmailwatch-core/libmailwatch-core.la \
	@GTHREAD_LIBS@ \
	@LIBXFCE4PANEL_LIBS@ \
	@LIBXFCEGUI4_LIBS@ \
	@GNUTLS_LIBS@

if HAVE_CYGWIN
  xfce4_mailwatch_plugin_LDFLAGS = \
	-no-undefined
endif

# .desktop file
#
# Some automake trickery here. Because we cannot use $(libexecdir) in the
# automake stage, we'll use sed to get the full path into the .desktop file.
# We also need to let intltool merge the translated fields, so we add an
# additional level of indirection: a <name>.desktop.in.in file.
# 
desktop_in_in_files = mailwatch.desktop.in.in
desktop_in_files = $(desktop_in_in_files:.desktop.in.in=.desktop.in)
	
desktopdir = $(datadir)/xfce4/panel-plugins
desktop_DATA = $(desktop_in_files:.desktop.in=.desktop)
@INTLTOOL_DESKTOP_RULE@
	
# get full path into .desktop file
%.desktop.in: %.desktop.in.in
	sed -e "s^@PLUGIN_PATH@^$(libexecdir)/xfce4/panel-plugins^" \
		$< > $@

EXTRA_DIST += $(desktop_in_in_files)

DISTCLEANFILES = \
	$(desktop_DATA) $(desktop_in_files)

endif
