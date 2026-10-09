/** @file km-init.c

  km-init -- userspace keymaster/keymint TA bootstrap for the OnePlus Pad 4
  (Qualcomm SM8850 "kaanapali", project iceland) mainline Linux port.

  The ESP boot chain (stock ABL -> BOOTAA64.EFI) deliberately skips ABL's
  BootLinux() verified-boot path, so nothing ever tells the keymaster TA in
  QTEE what the boot state is -- no SET_ROT, no SET_BOOT_STATE, no VBH, no
  milestone.  This daemon fills that slot from Linux userspace, reporting
  the byte-exact values a locked, stock, GREEN boot would have produced:

    root object -> registerAsClient(credentials) -> IClientEnv
    IClientEnv.open(3)          -> IAppLoader
    IAppLoader.loadFromBuffer() -> IAppController   [stock keymaster.img]
    IAppController.getAppObject()                   [the TA's IOpener]
    IOpener.open(150 = CKMHal)  -> IKMHal
    IKMHal.sendCmd():
        0x200 GET_VERSION   handshake, require Major >= 2
        0x201 SET_ROT       SHA256(avb_public_key || is_unlocked)
        0x208 SET_BOOT_STATE{IsUnlocked=0, PublicKey=SHA256(avb_public_key),
                             Color=GREEN, SystemVersion, SystemSecurityLevel}
        0x211 SET_VBH       SHA256 over the stock vbmeta chain
        0x204 MILESTONE_CALL  seal this boot's state

  Object ops/UIDs mirror QcomModulePkg/Include/Library/SmciInvokeUtils.h
  and the quic/quic-teec tests; the KM wire structs mirror QcomModulePkg/
  Library/avb/KeymasterClient.{c,h} verbatim (uefi.lnx.6.0.r49-rel).

  After the sequence the process stays alive holding the object references
  so the TA stays resident for later clients (Waydroid's keymint HAL) --
  systemd stops it with SIGTERM at shutdown.  No keymaster app-level
  (0x100-range) commands are ever sent; nothing irreversible (no ARB fuse,
  no tamper fuse, no RPMB writes) is reachable from this sequence.

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <qcomtee_object.h>
#include <qcomtee_object_types.h>

#include "sha256.h"

/* ------------------------------------------------------------------ */
/* Object interface constants (see file header for provenance)         */
/* ------------------------------------------------------------------ */

#define ICLIENTENV_OP_REGISTER_AS_CLIENT	2
#define ICLIENTENV_OP_OPEN			0
#define IAPPLOADER_UID				3
#define IAPPLOADER_OP_LOAD_FROM_BUFFER		0
#define IAPPCONTROLLER_OP_GETAPPOBJECT		2
#define IOPENER_OP_OPEN			0
#define CKMHAL_UID				150
#define IKMHAL_OP_SENDCMD			0

/* ------------------------------------------------------------------ */
/* keymaster "utils" wire protocol (KEYMASTER_UTILS_CMD_ID = 0x200)    */
/* ------------------------------------------------------------------ */

#define KM_UTILS_CMD_ID			0x200U

#define KM_GET_VERSION			(KM_UTILS_CMD_ID + 0)
#define KM_SET_ROT			(KM_UTILS_CMD_ID + 1)
#define KM_MILESTONE_CALL			(KM_UTILS_CMD_ID + 4)
#define KM_SET_BOOT_STATE			(KM_UTILS_CMD_ID + 8)
#define KM_SET_VBH				(KM_UTILS_CMD_ID + 17)

struct km_get_version_req {
	uint32_t cmd;
} __attribute__((packed));

struct km_get_version_rsp {
	int32_t status;
	uint32_t major, minor, app_major, app_minor;
} __attribute__((packed));

struct km_status_rsp {
	int32_t status;
} __attribute__((packed));

/* offsets must match ABL's KMSetRotReq exactly (EDK2 UINT32 = 4 bytes) */
struct km_set_rot_req {
	uint32_t cmd;			/* 0 */
	uint32_t rot_offset;		/* 4 */
	uint32_t rot_size;		/* 8 */
	uint8_t rot_digest[32];		/* 12 */
} __attribute__((packed));

struct km_boot_state {
	uint32_t is_unlocked;		/* 0 */
	uint8_t public_key[32];		/* 4: SHA256(raw avb public key) */
	uint32_t color;			/* 36: GREEN=0 ORANGE=1 YELLOW=2 RED=3 */
	uint32_t system_version;	/* 40: (maj<<14)|(min<<7)|sub */
	uint32_t system_security_level;	/* 44: (day<<11)|((y-2000)<<4)|month */
} __attribute__((packed));

struct km_set_boot_state_req {
	uint32_t cmd;			/* 0 */
	uint32_t version;		/* 4: ABL sends 0 */
	uint32_t offset;		/* 8: offsetof(boot_state) = 16 */
	uint32_t size;			/* 12: sizeof(boot_state) = 48 */
	struct km_boot_state bs;	/* 16 */
} __attribute__((packed));

struct km_set_vbh_req {
	uint32_t cmd;
	uint8_t vbh[32];
} __attribute__((packed));

struct km_milestone_req {
	uint32_t cmd;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

#define MAX_PUBKEY 2048

struct config {
	uint8_t pubkey[MAX_PUBKEY];
	size_t pubkey_len;
	uint8_t vbh[32];
	uint32_t os_version;
	uint32_t sec_patch;
	uint32_t color;
	uint32_t is_unlocked;
	int send_milestone;
};

struct derived {
	uint8_t boot_key[32];	/* SHA256(pubkey): attestation verifiedBootKey */
	uint8_t rot[32];	/* SHA256(pubkey || is_unlocked) */
};

static struct {
	const char *config_path;
	const char *ta_path;
	const char *device;
	int selftest;
	int oneshot;
	int no_milestone;
	int verbose;
} opt = {
	.config_path = "/etc/km-init/values.conf",
	.ta_path = "/usr/lib/km-init/keymaster.img",
	.device = NULL,
	.selftest = 0,
	.oneshot = 0,
	.no_milestone = 0,
	.verbose = 0,
};

static void logf_(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("[km-init] ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void dbg(const char *fmt, ...)
{
	va_list ap;

	if (!opt.verbose)
		return;
	va_start(ap, fmt);
	fputs("[km-init:d] ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void hexdump(const char *tag, const uint8_t *p, size_t len)
{
	printf("%s (%zu bytes):", tag, len);
	for (size_t i = 0; i < len; i++) {
		if (i % 32 == 0)
			printf("\n    ");
		printf("%02x", p[i]);
	}
	printf("\n");
}

static int parse_hex(uint8_t *out, size_t out_max, size_t *out_len,
		     const char *in)
{
	size_t n = 0;

	if (!in)
		return -1;
	while (*in == ' ' || *in == '\t')
		in++;
	if (in[0] == '0' && (in[1] == 'x' || in[1] == 'X'))
		in += 2;

	while (*in) {
		if (*in == ' ' || *in == '\t') {
			in++;
			continue;
		}
		int hi, lo;
		char c;

		c = *in++;
		hi = (c >= '0' && c <= '9') ? c - '0' :
		     (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
		     (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		c = *in++;
		lo = (c >= '0' && c <= '9') ? c - '0' :
		     (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
		     (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if (hi < 0 || lo < 0 || (c == '\0'))
			return -1;
		if (n >= out_max)
			return -1;
		out[n++] = (uint8_t)((hi << 4) | lo);
	}
	*out_len = n;
	return (n > 0) ? 0 : -1;
}

static int parse_u32(uint32_t *out, const char *in)
{
	char *end;
	unsigned long v;

	if (!in || !*in)
		return -1;
	errno = 0;
	v = strtoul(in, &end, 0);
	if (errno || *end)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	return s;
}

static int load_config(struct config *cfg, const char *path)
{
	FILE *f;
	char line[8192];
	int seen_pubkey = 0, seen_vbh = 0, seen_ver = 0, seen_patch = 0;

	memset(cfg, 0, sizeof(*cfg));
	cfg->send_milestone = 1;

	f = fopen(path, "r");
	if (!f) {
		logf_("cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *key, *val, *hash, *eq;

		hash = strchr(line, '#');
		if (hash)
			*hash = '\0';
		{
			char *nl = strchr(line, '\n');

			if (nl)
				*nl = '\0';
		}
		eq = strchr(line, '=');
		if (!eq) {
			if (*trim(line))
				goto bad;	/* non-empty, no '=' */
			continue;
		}
		*eq = '\0';
		key = trim(line);
		val = trim(eq + 1);
		if (!*key || !*val)
			continue;

		if (!strcmp(key, "avb_public_key")) {
			size_t n;

			if (parse_hex(cfg->pubkey, MAX_PUBKEY, &n, val))
				goto bad;
			cfg->pubkey_len = n;
			seen_pubkey = 1;
		} else if (!strcmp(key, "vbmeta_digest")) {
			size_t n;

			if (parse_hex(cfg->vbh, sizeof(cfg->vbh), &n, val) ||
			    n != 32)
				goto bad;
			seen_vbh = 1;
		} else if (!strcmp(key, "os_version_packed")) {
			if (parse_u32(&cfg->os_version, val))
				goto bad;
			seen_ver = 1;
		} else if (!strcmp(key, "security_patch_packed")) {
			if (parse_u32(&cfg->sec_patch, val))
				goto bad;
			seen_patch = 1;
		} else if (!strcmp(key, "color")) {
			if (parse_u32(&cfg->color, val))
				goto bad;
		} else if (!strcmp(key, "is_unlocked")) {
			if (parse_u32(&cfg->is_unlocked, val))
				goto bad;
		} else if (!strcmp(key, "send_milestone")) {
			uint32_t v;

			if (parse_u32(&v, val))
				goto bad;
			cfg->send_milestone = !!v;
		} else {
			goto bad;
		}
		continue;
bad:
		logf_("bad line in %s: %s", path, line);
		fclose(f);
		return -1;
	}
	fclose(f);

	if (!seen_pubkey || !seen_vbh || !seen_ver || !seen_patch) {
		logf_("%s: missing required key(s) (need avb_public_key, "
		      "vbmeta_digest, os_version_packed, security_patch_packed)",
		      path);
		return -1;
	}
	if (cfg->is_unlocked) {
		logf_("%s: this daemon only reports a locked/GREEN state; "
		      "is_unlocked must be 0", path);
		return -1;
	}
	return 0;
}

static void derive(const struct config *cfg, struct derived *d)
{
	struct sha256_ctx c;

	sha256_init(&c);
	sha256_update(&c, cfg->pubkey, cfg->pubkey_len);
	sha256_final(&c, d->boot_key);

	/* KeymasterClient.c: ROT = SHA256(PublicKey || IsUnlocked), where
	 * EDK2 BOOLEAN is one byte and GREEN means IsUnlocked = FALSE = 0 */
	sha256_init(&c);
	sha256_update(&c, cfg->pubkey, cfg->pubkey_len);
	{
		uint8_t unlocked = 0;

		sha256_update(&c, &unlocked, 1);
	}
	sha256_final(&c, d->rot);
}

static int dump_config(const struct config *cfg, const struct derived *d)
{
	printf("km-init derived report (nothing sent to the TEE):\n");
	printf("  avb_public_key_len  : %zu\n", cfg->pubkey_len);
	hexdump("  avb_public_key      ", cfg->pubkey, cfg->pubkey_len);
	hexdump("  verified_boot_key   ", d->boot_key, 32);
	hexdump("  rot_digest          ", d->rot, 32);
	hexdump("  vbh                 ", cfg->vbh, 32);
	printf("  color               : %u (0=GREEN)\n", cfg->color);
	printf("  is_unlocked         : %u\n", cfg->is_unlocked);
	printf("  system_version      : %u\n", cfg->os_version);
	printf("  security_patch      : %u\n", cfg->sec_patch);
	printf("  send_milestone      : %d\n",
	       cfg->send_milestone && !opt.no_milestone);
	return 0;
}

/* ------------------------------------------------------------------ */
/* libqcomtee plumbing                                                */
/* ------------------------------------------------------------------ */

/* Implemented in libqcomtee's objects/credentials_obj.c; declared here in
 * addition to the headers so the build is robust to header reshuffles. */
int qcomtee_object_credentials_init(struct qcomtee_object *root,
				    struct qcomtee_object **object);

static struct qcomtee_object *g_root;
static volatile sig_atomic_t g_stop;

/* libqcomtee calls back into us through this trampoline for
 * TEE_IOC_SUPPL_RECV / TEE_IOC_SUPPL_SEND; both take a pointer.  The
 * async-cancel window mirrors the quic-teec tests so the supplicant
 * thread can be torn down while parked in the ioctl. */
static int tee_call(int fd, unsigned long req, ...)
{
	void *arg;
	va_list ap;
	int oldtype, rc;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);
	rc = (int)ioctl(fd, (int)req, arg);
	pthread_setcanceltype(oldtype, NULL);
	return rc;
}

static void *supplicant(void *unused)
{
	(void)unused;
	while (!g_stop)
		qcomtee_object_process_one(g_root);
	return NULL;
}

static int qinv(struct qcomtee_object *o, qcomtee_op_t op,
		struct qcomtee_param *p, int n)
{
	qcomtee_result_t result;
	int ret;

	ret = qcomtee_object_invoke(o, op, p, n, &result);
	if (ret < 0) {
		dbg("invoke transport error: %d (%s)", ret, strerror(-ret));
		return ret;
	}
	if (result != 0)
		dbg("invoke object result: %d", (int)result);
	return (int)result;
}

struct session {
	struct qcomtee_object *root, *creds, *client_env, *app_loader,
			      *controller, *app_obj, *km;
	pthread_t thr;
	int thr_started;
};

static void session_release(struct session *s)
{
	if (s->thr_started) {
		pthread_cancel(s->thr);
		pthread_join(s->thr, NULL);
		s->thr_started = 0;
	}
	if (s->km && s->km != s->app_obj)
		qcomtee_object_refs_dec(s->km);
	if (s->app_obj)
		qcomtee_object_refs_dec(s->app_obj);
	if (s->controller)
		qcomtee_object_refs_dec(s->controller);
	if (s->app_loader)
		qcomtee_object_refs_dec(s->app_loader);
	if (s->client_env)
		qcomtee_object_refs_dec(s->client_env);
	if (s->creds)
		qcomtee_object_refs_dec(s->creds);
	if (s->root)
		qcomtee_object_refs_dec(s->root);
	memset(s, 0, sizeof(*s));
}

/* root -> registerAsClient -> open(IAppLoader) -> loadFromBuffer ->
 * getAppObject -> open(CKMHal).  Mirrors quic-teec tests/ta_load.c plus
 * ABL's SmciInvokeUtils.h sequence. */
static int session_open(struct session *s, const char *device,
			const void *ta_img, size_t ta_len)
{
	struct qcomtee_param p[2];
	uint32_t uid;
	int rc;

	memset(s, 0, sizeof(*s));

	s->root = qcomtee_object_root_init(device, tee_call, NULL, NULL);
	if (!s->root || s->root == QCOMTEE_OBJECT_NULL) {
		dbg("root_init failed on %s", device);
		return -1;
	}
	if (pthread_create(&s->thr, NULL, supplicant, NULL) == 0)
		s->thr_started = 1;

	/* credentials callback object; QTEE reads (uid, time) CBOR from it
	 * while registerAsClient() runs -- that is why the supplicant thread
	 * must already be live. */
	rc = qcomtee_object_credentials_init(s->root, &s->creds);
	if (rc || !s->creds) {
		logf_("credentials object init failed (%d)", rc);
		goto fail;
	}

	memset(p, 0, sizeof(p));
	p[0].attr = QCOMTEE_OBJREF_INPUT;
	p[0].object = s->creds;
	p[1].attr = QCOMTEE_OBJREF_OUTPUT;
	rc = qinv(s->root, ICLIENTENV_OP_REGISTER_AS_CLIENT, p, 2);
	if (rc || !p[1].object) {
		logf_("registerAsClient failed on %s (rc=%d)", device, rc);
		goto fail;
	}
	s->client_env = p[1].object;

	uid = IAPPLOADER_UID;
	memset(p, 0, sizeof(p));
	p[0].attr = QCOMTEE_UBUF_INPUT;
	p[0].ubuf.addr = &uid;
	p[0].ubuf.size = sizeof(uid);
	p[1].attr = QCOMTEE_OBJREF_OUTPUT;
	rc = qinv(s->client_env, ICLIENTENV_OP_OPEN, p, 2);
	if (rc || !p[1].object) {
		logf_("IClientEnv.open(%u IAppLoader) failed (rc=%d)", uid, rc);
		goto fail;
	}
	s->app_loader = p[1].object;

	memset(p, 0, sizeof(p));
	p[0].attr = QCOMTEE_UBUF_INPUT;
	p[0].ubuf.addr = (void *)ta_img;
	p[0].ubuf.size = ta_len;
	p[1].attr = QCOMTEE_OBJREF_OUTPUT;
	rc = qinv(s->app_loader, IAPPLOADER_OP_LOAD_FROM_BUFFER, p, 2);
	if (rc || !p[1].object) {
		logf_("IAppLoader.loadFromBuffer failed (rc=%d) -- is the TA "
		      "image the stock-signed keymaster.img?", rc);
		goto fail;
	}
	s->controller = p[1].object;

	memset(p, 0, sizeof(p));
	p[0].attr = QCOMTEE_OBJREF_OUTPUT;
	rc = qinv(s->controller, IAPPCONTROLLER_OP_GETAPPOBJECT, p, 1);
	if (rc || !p[0].object) {
		logf_("IAppController.getAppObject failed (rc=%d)", rc);
		goto fail;
	}
	s->app_obj = p[0].object;

	/* ABL opens CKMHal(150) off the app object; if the loader handed us
	 * the KM object directly, fall back to using it as-is. */
	uid = CKMHAL_UID;
	memset(p, 0, sizeof(p));
	p[0].attr = QCOMTEE_UBUF_INPUT;
	p[0].ubuf.addr = &uid;
	p[0].ubuf.size = sizeof(uid);
	p[1].attr = QCOMTEE_OBJREF_OUTPUT;
	rc = qinv(s->app_obj, IOPENER_OP_OPEN, p, 2);
	if (rc || !p[1].object) {
		logf_("IOpener.open(%u CKMHal) failed (rc=%d); continuing with "
		      "the app object as IKMHal", uid, rc);
		s->km = s->app_obj;
	} else {
		s->km = p[1].object;
	}
	return 0;
fail:
	session_release(s);
	return -1;
}

static int km_send(struct qcomtee_object *km, const void *req, size_t req_len,
		   void *rsp, size_t rsp_len)
{
	struct qcomtee_param p[2];
	int rc;

	memset(p, 0, sizeof(p));
	p[0].attr = QCOMTEE_UBUF_INPUT;
	p[0].ubuf.addr = (void *)req;
	p[0].ubuf.size = req_len;
	p[1].attr = QCOMTEE_UBUF_OUTPUT;
	p[1].ubuf.addr = rsp;
	p[1].ubuf.size = rsp_len;
	rc = qinv(km, IKMHAL_OP_SENDCMD, p, 2);
	if (rc == 0)
		dbg("sendCmd out size %zu", (size_t)p[1].ubuf.size);
	return rc;
}

/* ------------------------------------------------------------------ */
/* the forged ABL notification sequence                                */
/* ------------------------------------------------------------------ */

static int run_sequence(struct session *s, const struct config *cfg,
			const struct derived *d)
{
	struct km_get_version_req gv_req = { .cmd = KM_GET_VERSION };
	struct km_get_version_rsp gv_rsp;
	struct km_set_rot_req rot_req;
	struct km_status_rsp st_rsp;
	struct km_set_boot_state_req bs_req;
	struct km_set_vbh_req vbh_req;
	struct km_milestone_req ms_req;
	int rc;

	rc = km_send(s->km, &gv_req, sizeof(gv_req), &gv_rsp, sizeof(gv_rsp));
	if (rc || gv_rsp.status != 0) {
		logf_("GET_VERSION failed (rc=%d status=%d)", rc,
		      gv_rsp.status);
		return -1;
	}
	logf_("keymaster TA version %u.%u (app %u.%u)", gv_rsp.major,
	      gv_rsp.minor, gv_rsp.app_major, gv_rsp.app_minor);
	if (gv_rsp.major < 2) {
		logf_("keymaster Major < 2, refusing (mirrors ABL check)");
		return -1;
	}

	rot_req.cmd = KM_SET_ROT;
	rot_req.rot_offset = offsetof(struct km_set_rot_req, rot_digest);
	rot_req.rot_size = sizeof(rot_req.rot_digest);
	memcpy(rot_req.rot_digest, d->rot, 32);
	memset(&st_rsp, 0, sizeof(st_rsp));
	rc = km_send(s->km, &rot_req, sizeof(rot_req), &st_rsp, sizeof(st_rsp));
	if (rc || st_rsp.status != 0) {
		logf_("SET_ROT failed (rc=%d status=%d)", rc, st_rsp.status);
		return -1;
	}
	logf_("SET_ROT ok");

	memset(&bs_req, 0, sizeof(bs_req));
	bs_req.cmd = KM_SET_BOOT_STATE;
	bs_req.version = 0;
	bs_req.offset = offsetof(struct km_set_boot_state_req, bs);
	bs_req.size = sizeof(bs_req.bs);
	bs_req.bs.is_unlocked = cfg->is_unlocked;
	memcpy(bs_req.bs.public_key, d->boot_key, 32);
	bs_req.bs.color = cfg->color;
	bs_req.bs.system_version = cfg->os_version;
	bs_req.bs.system_security_level = cfg->sec_patch;
	rc = km_send(s->km, &bs_req, sizeof(bs_req), &st_rsp, sizeof(st_rsp));
	if (rc || st_rsp.status != 0) {
		logf_("SET_BOOT_STATE failed (rc=%d status=%d)", rc,
		      st_rsp.status);
		return -1;
	}
	logf_("SET_BOOT_STATE ok (color=%u locked=%u)", cfg->color,
	      cfg->is_unlocked);

	memset(&vbh_req, 0, sizeof(vbh_req));
	vbh_req.cmd = KM_SET_VBH;
	memcpy(vbh_req.vbh, cfg->vbh, 32);
	rc = km_send(s->km, &vbh_req, sizeof(vbh_req), &st_rsp, sizeof(st_rsp));
	if (rc || st_rsp.status != 0) {
		logf_("SET_VBH failed (rc=%d status=%d)", rc, st_rsp.status);
		return -1;
	}
	logf_("SET_VBH ok");

	if (cfg->send_milestone && !opt.no_milestone) {
		ms_req.cmd = KM_MILESTONE_CALL;
		rc = km_send(s->km, &ms_req, sizeof(ms_req), &st_rsp,
			     sizeof(st_rsp));
		if (rc || st_rsp.status != 0) {
			logf_("MILESTONE_CALL failed (rc=%d status=%d)", rc,
			      st_rsp.status);
			return -1;
		}
		logf_("MILESTONE_CALL ok (boot state sealed for this boot)");
	} else {
		logf_("MILESTONE_CALL skipped (--no-milestone / config)");
	}
	return 0;
}

/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static int try_device(const char *device, struct session *s,
		      const void *ta_img, size_t ta_len,
		      const struct config *cfg, const struct derived *d)
{
	logf_("trying TEE device %s", device);
	if (session_open(s, device, ta_img, ta_len))
		return -1;
	if (run_sequence(s, cfg, d)) {
		session_release(s);
		return -1;
	}
	logf_("boot state reported on %s (GREEN/locked)", device);
	return 0;
}

static void *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	long sz;
	void *buf;

	if (!f) {
		logf_("cannot open %s: %s", path, strerror(errno));
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET)) {
		logf_("cannot size %s", path);
		fclose(f);
		return NULL;
	}
	buf = malloc(sz ? (size_t)sz : 1);
	if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		logf_("cannot read %s", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t)sz;
	return buf;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-t] [-v] [--oneshot] [--no-milestone]\n"
		"          [--config PATH] [--ta PATH] [--device PATH]\n"
		"  -t             parse config, print derived values, exit\n"
		"  -v             verbose object-level debug\n"
		"  --oneshot      exit after the sequence instead of holding\n"
		"  --no-milestone skip the KEYMASTER_MILESTONE_CALL\n",
		argv0);
}

int main(int argc, char **argv)
{
	struct config cfg;
	struct derived d;
	struct session s;
	void *ta_img;
	size_t ta_len = 0;
	char devbuf[32];
	int i, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--self-test"))
			opt.selftest = 1;
		else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			opt.verbose = 1;
		else if (!strcmp(argv[i], "--oneshot"))
			opt.oneshot = 1;
		else if (!strcmp(argv[i], "--no-milestone"))
			opt.no_milestone = 1;
		else if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) &&
			 i + 1 < argc)
			opt.config_path = argv[++i];
		else if (!strcmp(argv[i], "--ta") && i + 1 < argc)
			opt.ta_path = argv[++i];
		else if (!strcmp(argv[i], "--device") && i + 1 < argc)
			opt.device = argv[++i];
		else {
			usage(argv[0]);
			return 1;
		}
	}

	if (load_config(&cfg, opt.config_path))
		return 1;
	derive(&cfg, &d);
	if (opt.selftest)
		return dump_config(&cfg, &d);

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	ta_img = read_file(opt.ta_path, &ta_len);
	if (!ta_img)
		return 1;
	logf_("TA image %s (%zu bytes)", opt.ta_path, ta_len);

	if (opt.device) {
		rc = try_device(opt.device, &s, ta_img, ta_len, &cfg, &d);
	} else {
		rc = -1;
		for (i = 0; i < 8 && rc; i++) {
			snprintf(devbuf, sizeof(devbuf), "/dev/tee%d", i);
			if (access(devbuf, F_OK) != 0)
				continue;
			rc = try_device(devbuf, &s, ta_img, ta_len, &cfg, &d);
		}
	}
	if (rc) {
		logf_("ERROR: no usable TEE device / sequence failed");
		free(ta_img);
		return 1;
	}

	if (opt.oneshot) {
		session_release(&s);
		free(ta_img);
		logf_("oneshot mode: exiting");
		return 0;
	}

	/* Hold the references (and thus the loaded TA + reported state) until
	 * shutdown; Waydroid's keymint HAL attaches to the same TA later. */
	logf_("holding TA + object references; SIGTERM to exit");
	while (!g_stop)
		pause();
	logf_("shutting down");
	session_release(&s);
	free(ta_img);
	return 0;
}
