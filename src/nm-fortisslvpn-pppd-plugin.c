/* nm-fortisslvpn-service - SSLVPN integration with NetworkManager
 *
 * (C) 2015 Lubomir Rintel
 * (C) 2007 - 2008 Novell, Inc.
 * (C) 2008 - 2009 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#define ___CONFIG_H__
#include <config.h>

#include <pppd/pppd.h>
#include <pppd/fsm.h>
#include <pppd/ipcp.h>

#include "nm-default.h"

#include <sys/types.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <glib/gstdio.h>

#include "nm-fortisslvpn-pppd-service-dbus.h"
#include "nm-fortisslvpn-service.h"
#include "nm-ppp-status.h"

#include "nm-utils/nm-shared-utils.h"
#include "nm-utils/nm-vpn-plugin-macros.h"

/*****************************************************************************/

static struct {
	int log_level;
	const char *log_prefix_token;
	uid_t uid;
	gid_t gid;
	NMDBusFortisslvpnPpp *proxy;
} gl/*obal*/;

/*****************************************************************************/

#define _NMLOG(level, ...) \
    G_STMT_START { \
         if (gl.log_level >= (level)) { \
             g_printerr ("nm-fortisslvpn[%s] %-7s [helper-%ld] " _NM_UTILS_MACRO_FIRST (__VA_ARGS__) "\n", \
                         gl.log_prefix_token, \
                         nm_utils_syslog_to_str (level), \
                         (long) getpid () \
                         _NM_UTILS_MACRO_REST (__VA_ARGS__)); \
         } \
    } G_STMT_END

#define _LOGI(...) _NMLOG(LOG_NOTICE,  __VA_ARGS__)
#define _LOGW(...) _NMLOG(LOG_WARNING, __VA_ARGS__)
#define _LOGE(...) _NMLOG(LOG_ERR, __VA_ARGS__)

/*****************************************************************************/

int plugin_init (void);

char pppd_version[] = VERSION;

static void
chroot_sandbox (void)
{
	static int chrooted = 0;
	GError *error = NULL;
	int parentfd = -1;
	gchar *tmpdir;
	gchar *tmp;

	if (chrooted)
		return;
	chrooted = 1;

	tmpdir = g_dir_make_tmp (NULL, &error);
	if (tmpdir == NULL) {
		_LOGW ("Can't create a temporary directory name: %s", error->message);
		g_error_free (error);
		return;
	}

	tmp = g_path_get_dirname (tmpdir);
	g_printerr ("{%s} {%s}\n", tmpdir, tmp);
	parentfd = open (tmp, 0);
	if (parentfd == -1) {
		_LOGW ("Can't open '%s': %s", tmp, strerror (errno));
		goto cleanup;
	}
	g_clear_pointer (&tmp, g_free);

	if (chroot (tmpdir) == -1) {
		_LOGW ("Chroot to '%s' failed: %s", tmpdir, strerror (errno));
		goto cleanup;
	}

	tmp = g_path_get_basename (tmpdir);
	g_printerr ("{%s} {%s}\n", tmpdir, tmp);
	if (unlinkat (parentfd, tmp, AT_REMOVEDIR) == -1) {
		_LOGW ("Unlink of '%s' failed: %s", tmpdir, strerror (errno));
	}
	g_clear_pointer (&tmpdir, g_free);

cleanup:
	g_clear_pointer (&tmp, g_free);
	if (parentfd != -1)
		close (parentfd);
	if (tmpdir) {
		g_remove (tmpdir);
		g_free (tmpdir);
	}
}

static void
drop_privs (void)
{
	if (gl.uid == 0)
		return;
	if (setgroups(0, NULL))
		_LOGW ("setgroups() failed.");
	if (setgid(gl.gid) != 0)
		_LOGW ("setgid(%d) failed.", gl.gid);
	if (setuid(gl.uid) != 0)
		_LOGW ("setuid(%d) failed.", gl.uid);
	gl.uid = 0;
}

static void
nm_phasechange (void *data, int arg)
{
	NMPPPStatus ppp_status = NM_PPP_STATUS_UNKNOWN;
	char *ppp_phase;

	g_return_if_fail (NMDBUS_IS_FORTISSLVPN_PPP_PROXY (gl.proxy));

	switch (arg) {
	case PHASE_DEAD:
		ppp_status = NM_PPP_STATUS_DEAD;
		ppp_phase = "dead";
		break;
	case PHASE_INITIALIZE:
		ppp_status = NM_PPP_STATUS_INITIALIZE;
		ppp_phase = "initialize";
		break;
	case PHASE_SERIALCONN:
		ppp_status = NM_PPP_STATUS_SERIALCONN;
		ppp_phase = "serial connection";
		break;
	case PHASE_DORMANT:
		ppp_status = NM_PPP_STATUS_DORMANT;
		ppp_phase = "dormant";
		break;
	case PHASE_ESTABLISH:
		ppp_status = NM_PPP_STATUS_ESTABLISH;
		ppp_phase = "establish";
		break;
	case PHASE_AUTHENTICATE:
		ppp_status = NM_PPP_STATUS_AUTHENTICATE;
		ppp_phase = "authenticate";
		break;
	case PHASE_CALLBACK:
		ppp_status = NM_PPP_STATUS_CALLBACK;
		ppp_phase = "callback";
		break;
	case PHASE_NETWORK:
		ppp_status = NM_PPP_STATUS_NETWORK;
		ppp_phase = "network";
		break;
	case PHASE_RUNNING:
		ppp_status = NM_PPP_STATUS_RUNNING;
		ppp_phase = "running";
		break;
	case PHASE_TERMINATE:
		ppp_status = NM_PPP_STATUS_TERMINATE;
		ppp_phase = "terminate";
		break;
	case PHASE_DISCONNECT:
		ppp_status = NM_PPP_STATUS_DISCONNECT;
		ppp_phase = "disconnect";
		break;
	case PHASE_HOLDOFF:
		ppp_status = NM_PPP_STATUS_HOLDOFF;
		ppp_phase = "holdoff";
		break;
	case PHASE_MASTER:
		ppp_status = NM_PPP_STATUS_MASTER;
		ppp_phase = "master";
		break;

	default:
		ppp_phase = "unknown";
		break;
	}

	_LOGI ("phasechange: status %d / phase '%s'", ppp_status, ppp_phase);

	if (ppp_status > NM_PPP_STATUS_SERIALCONN)
		chroot_sandbox ();

	if (ppp_status > NM_PPP_STATUS_NETWORK)
		drop_privs ();

	if (ppp_status != NM_PPP_STATUS_UNKNOWN) {
		nmdbus_fortisslvpn_ppp_call_set_state (gl.proxy,
		                                       ppp_status,
		                                       NULL,
		                                       NULL, NULL);
	}
}

static GVariant *
get_ip4_routes (in_addr_t ouraddr)
{
	GVariantBuilder builder;
	GVariant *value;
	guint i;

	g_variant_builder_init (&builder, G_VARIANT_TYPE ("aau"));

	for (i = 0; i < 100; i++) {
		GVariantBuilder array;
		gchar *var;
		const gchar *str;
		in_addr_t dest, gateway;
		guint32 metric, prefix;

		var = g_strdup_printf ("VPN_ROUTE_DEST_%u", i);
		str = g_getenv (var);
		g_free (var);
		if (!str || !*str)
			break;
		dest = inet_addr (str);

		var = g_strdup_printf ("VPN_ROUTE_MASK_%u", i);
		str = g_getenv (var);
		g_free (var);
		if (!str || !*str)
			prefix = 32;
		else
			prefix = nm_utils_ip4_netmask_to_prefix (inet_addr (str));

		var = g_strdup_printf ("VPN_ROUTE_GATEWAY_%u", i);
		str = g_getenv (var);
		g_free (var);
		if (!str || !*str)
			gateway = ouraddr;
		else
			gateway = inet_addr (str);

		var = g_strdup_printf ("VPN_ROUTE_METRIC_%u", i);
		str = g_getenv (var);
		g_free (var);
		if (!str || !*str)
			metric = 0;
		else
			metric = atol (str);

		g_variant_builder_init (&array, G_VARIANT_TYPE ("au"));
		g_variant_builder_add_value (&array, g_variant_new_uint32 (dest));
		g_variant_builder_add_value (&array, g_variant_new_uint32 (prefix));
		g_variant_builder_add_value (&array, g_variant_new_uint32 (gateway));
		g_variant_builder_add_value (&array, g_variant_new_uint32 (metric));
		g_variant_builder_add_value (&builder, g_variant_builder_end (&array));
	}

	value = g_variant_builder_end (&builder);
	if (i > 0)
		return value;

	g_variant_unref (value);
	return NULL;
}

static void
nm_ip_up (void *data, int arg)
{
	guint32 pppd_made_up_address = htonl (0x0a404040 + ifunit);
	ipcp_options opts = ipcp_gotoptions[0];
	ipcp_options peer_opts = ipcp_hisoptions[0];
	GVariantBuilder builder;
	const gchar *str;
	GVariant *val;

	g_return_if_fail (NMDBUS_IS_FORTISSLVPN_PPP_PROXY (gl.proxy));

	_LOGI ("ip-up: event");

	if (!opts.ouraddr) {
		_LOGW ("ip-up: didn't receive an internal IP from pppd!");
		nm_phasechange (NULL, PHASE_DEAD);
		return;
	}

	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

	g_variant_builder_add (&builder, "{sv}",
	                       NM_VPN_PLUGIN_IP4_CONFIG_TUNDEV,
	                       g_variant_new_string (ifname));

	str = g_getenv ("VPN_GATEWAY");
	if (str) {
		g_variant_builder_add (&builder, "{sv}",
		                       NM_VPN_PLUGIN_IP4_CONFIG_EXT_GATEWAY,
		                       g_variant_new_uint32 (inet_addr (str)));
	} else
		_LOGW ("ip-up: no external gateway!");

	val = get_ip4_routes (opts.ouraddr);
	if (val)
		g_variant_builder_add (&builder, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_ROUTES, val);

	/* Prefer the peer options remote address first, _unless_ pppd made the
	 * address up, at which point prefer the local options remote address,
	 * and if that's not right, use the made-up address as a last resort.
	 */
	if (peer_opts.hisaddr && (peer_opts.hisaddr != pppd_made_up_address)) {
		g_variant_builder_add (&builder, "{sv}",
		                       NM_VPN_PLUGIN_IP4_CONFIG_PTP,
		                       g_variant_new_uint32 (peer_opts.hisaddr));
	} else if (opts.hisaddr) {
		g_variant_builder_add (&builder, "{sv}",
		                       NM_VPN_PLUGIN_IP4_CONFIG_PTP,
		                       g_variant_new_uint32 (opts.hisaddr));
	} else if (peer_opts.hisaddr == pppd_made_up_address) {
		/* As a last resort, use the made-up address */
		g_variant_builder_add (&builder, "{sv}",
		                       NM_VPN_PLUGIN_IP4_CONFIG_PTP,
		                       g_variant_new_uint32 (peer_opts.hisaddr));
	}

	g_variant_builder_add (&builder, "{sv}",
	                       NM_VPN_PLUGIN_IP4_CONFIG_ADDRESS,
	                       g_variant_new_uint32 (opts.ouraddr));

	g_variant_builder_add (&builder, "{sv}",
	                       NM_VPN_PLUGIN_IP4_CONFIG_PREFIX,
	                       g_variant_new_uint32 (32));

	if (opts.dnsaddr[0] || opts.dnsaddr[1]) {
		guint32 dns[2];
		int len = 0;

		if (opts.dnsaddr[0])
			dns[len++] = opts.dnsaddr[0];
		if (opts.dnsaddr[1])
			dns[len++] = opts.dnsaddr[1];

		g_variant_builder_add (&builder, "{sv}",
		                       NM_VPN_PLUGIN_IP4_CONFIG_DNS,
		                       g_variant_new_fixed_array (G_VARIANT_TYPE_UINT32,
		                                                  dns, len, sizeof (guint32)));
	}

	/* Default MTU to 1400, which is also what Windows XP/Vista use */
	g_variant_builder_add (&builder, "{sv}",
	                       NM_VPN_PLUGIN_IP4_CONFIG_MTU,
	                       g_variant_new_uint32 (1400));

	_LOGI ("ip-up: sending Ip4Config to NetworkManager-fortisslvpn");

	nmdbus_fortisslvpn_ppp_call_set_ip4_config (gl.proxy,
	                                            g_variant_builder_end (&builder),
	                                            NULL,
	                                            NULL, NULL);
}

static void
nm_exit_notify (void *data, int arg)
{
	g_return_if_fail (G_IS_DBUS_PROXY (gl.proxy));

	_LOGI ("exit: cleaning up");
	g_clear_object (&gl.proxy);
}

int
plugin_init (void)
{
	GError *error = NULL;
	const char *bus_name;
	struct passwd *pw;

	nm_g_type_init ();

	g_return_val_if_fail (!gl.proxy, -1);

	bus_name = getenv ("NM_DBUS_SERVICE_FORTISSLVPN");
	if (!bus_name)
		bus_name = NM_DBUS_SERVICE_FORTISSLVPN;

	gl.log_level = _nm_utils_ascii_str_to_int64 (getenv ("NM_VPN_LOG_LEVEL"),
	                                             10, 0, LOG_DEBUG,
	                                             LOG_NOTICE);
	gl.log_prefix_token = getenv ("NM_VPN_LOG_PREFIX_TOKEN") ?: "???";

	_LOGI ("initializing");

	pw = getpwnam("nm-fortisslvpn");
	if (!pw) {
		_LOGW ("No 'nm-fortisslvpn' user, falling back to nobody.");
		pw = getpwnam("nobody");
	}
	if (pw) {
		gl.uid = pw->pw_gid;
		gl.gid = pw->pw_uid;
	} else {
		_LOGW ("No 'nobody' user, will not drop privileges.");
		gl.uid = 0;
	}

	gl.proxy = nmdbus_fortisslvpn_ppp_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
	                                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
	                                                          bus_name,
	                                                          NM_DBUS_PATH_FORTISSLVPN_PPP,
	                                                          NULL, &error);

	if (!gl.proxy) {
		_LOGE ("couldn't create D-Bus proxy: %s", error->message);
		g_error_free (error);
		return -1;
	}

	add_notifier (&phasechange, nm_phasechange, NULL);
	add_notifier (&ip_up_notifier, nm_ip_up, NULL);
	add_notifier (&exitnotify, nm_exit_notify, NULL);
	return 0;
}
