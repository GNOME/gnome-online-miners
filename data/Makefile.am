servicedir = $(datadir)/dbus-1/services
service_DATA =

if BUILD_FACEBOOK

service_DATA += org.gnome.OnlineMiners.Facebook.service
org.gnome.OnlineMiners.Facebook.service: org.gnome.OnlineMiners.Facebook.service.in Makefile
	$(AM_V_GEN)	\
		[ -d $(@D) ] || $(mkdir_p) $(@D) ; \
		sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@.tmp && mv $@.tmp $@

endif # BUILD_FACEBOOK

if BUILD_FLICKR

service_DATA += org.gnome.OnlineMiners.Flickr.service
org.gnome.OnlineMiners.Flickr.service: org.gnome.OnlineMiners.Flickr.service.in Makefile
	$(AM_V_GEN)	\
		[ -d $(@D) ] || $(mkdir_p) $(@D) ; \
		sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@.tmp && mv $@.tmp $@

endif # BUILD_FLICKR

if BUILD_GOOGLE

service_DATA += org.gnome.OnlineMiners.GData.service
org.gnome.OnlineMiners.GData.service: org.gnome.OnlineMiners.GData.service.in Makefile
	$(AM_V_GEN)	\
		[ -d $(@D) ] || $(mkdir_p) $(@D) ; \
		sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@.tmp && mv $@.tmp $@

endif # BUILD_GOOGLE

if BUILD_MEDIA_SERVER

service_DATA += org.gnome.OnlineMiners.MediaServer.service
org.gnome.OnlineMiners.MediaServer.service: org.gnome.OnlineMiners.MediaServer.service.in Makefile
	$(AM_V_GEN)	\
		[ -d $(@D) ] || $(mkdir_p) $(@D) ; \
		sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@.tmp && mv $@.tmp $@

endif # BUILD_MEDIA_SERVER

if BUILD_OWNCLOUD

service_DATA += org.gnome.OnlineMiners.Owncloud.service
org.gnome.OnlineMiners.Owncloud.service: org.gnome.OnlineMiners.Owncloud.service.in Makefile
	$(AM_V_GEN)	\
		[ -d $(@D) ] || $(mkdir_p) $(@D) ; \
		sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@.tmp && mv $@.tmp $@

endif # BUILD_OWNCLOUD

if BUILD_WINDOWS_LIVE

service_DATA += org.gnome.OnlineMiners.Zpj.service
org.gnome.OnlineMiners.Zpj.service: org.gnome.OnlineMiners.Zpj.service.in Makefile
	$(AM_V_GEN)	\
		[ -d $(@D) ] || $(mkdir_p) $(@D) ; \
		sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@.tmp && mv $@.tmp $@

endif # BUILD_WINDOWS_LIVE

service_in_files = \
    org.gnome.OnlineMiners.Facebook.service.in \
    org.gnome.OnlineMiners.Flickr.service.in \
    org.gnome.OnlineMiners.GData.service.in \
    org.gnome.OnlineMiners.MediaServer.service.in \
    org.gnome.OnlineMiners.Owncloud.service.in \
    org.gnome.OnlineMiners.Zpj.service.in \
    $(NULL)

EXTRA_DIST= \
    $(service_in_files) \
    $(NULL)

CLEANFILES = \
    $(service_DATA) \
    $(NULL)

-include $(top_srcdir)/git.mk
