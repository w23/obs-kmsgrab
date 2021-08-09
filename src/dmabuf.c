#include "drmsend.h"
#include "xcursor-xcb.h"

#include "drm-helpers.h"

#include <graphics/graphics.h>
#include <graphics/graphics-internal.h>

#include <obs-module.h>
#include <obs-nix-platform.h>
#include <util/platform.h>

#include <sys/wait.h>
#include <stdio.h>

#include "plugin-macros.generated.h"

// FIXME stringify errno

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

typedef struct {
	drmsend_response_t resp;
	int fb_fds[OBS_DRMSEND_MAX_FRAMEBUFFERS];
} dmabuf_source_fblist_t;

typedef struct {
	obs_source_t *source;

	xcb_connection_t *xcb;
	xcb_xcursor_t *cursor;
	gs_texture_t *texture;

	dmabuf_source_fblist_t fbs;
	int active_fb;

	bool show_cursor;
} dmabuf_source_t;

static const char send_binary_name[] = "linux-kmsgrab-send";
static const size_t send_binary_len = sizeof(send_binary_name) - 1;
static const char socket_filename[] = "/obs-kmsgrab-send.sock";
static const int socket_filename_len = sizeof(socket_filename) - 1;

static void set_visible(obs_properties_t *ppts, const char *name, bool visible)
{
	obs_property_t *p = obs_properties_get(ppts, name);
	obs_property_set_visible(p, visible);
}


static int dmabuf_source_receive_framebuffers(const char *dri_filename, dmabuf_source_fblist_t *list)
{
	blog(LOG_DEBUG, "dmabuf_source_receive_framebuffers");

	int retval = 0;
	int sockfd = -1;

	/* Get socket filename */
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	{
		const char *const module_path =
			obs_get_module_data_path(obs_current_module());
		assert(module_path);
		if (!os_file_exists(module_path)) {
			if (MKDIR_ERROR == os_mkdir(module_path)) {
				blog(LOG_ERROR, "Unable to create directory %s",
				     module_path);
				return 0;
			}
		}

		const int module_path_len = strlen(module_path);
		if (module_path_len + socket_filename_len + 1 >=
		    (int)sizeof(addr.sun_path)) {
			blog(LOG_ERROR, "Socket filename is too long, max %d",
			     (int)sizeof(addr.sun_path));
			return 0;
		}
		memcpy(addr.sun_path, module_path, module_path_len);
		memcpy(addr.sun_path + module_path_len, socket_filename,
		       socket_filename_len);

		blog(LOG_DEBUG, "Will bind socket to %s", addr.sun_path);
	}

	/* Find linux-kmsgrab-send */
	char *drmsend_filename = NULL;
	{
		const char* plugin_path = obs_get_module_binary_path(obs_current_module());
		const char* plugin_path_last_sep = strrchr(plugin_path, '/');
		if (!plugin_path_last_sep)
			plugin_path_last_sep = plugin_path;
		else
			plugin_path_last_sep += 1;

		const ssize_t prefix_len = plugin_path_last_sep - plugin_path;
		const ssize_t full_len = prefix_len;
		const ssize_t drmsend_filename_len = full_len + send_binary_len + 1;
		drmsend_filename = malloc(drmsend_filename_len);
		memcpy(drmsend_filename, plugin_path, prefix_len);
		memcpy(drmsend_filename + prefix_len, send_binary_name, send_binary_len + 1);

		if (!os_file_exists(drmsend_filename)) {
			blog(LOG_ERROR, "%s doesn't exist", drmsend_filename);
			goto filename_cleanup;
		}

		blog(LOG_DEBUG, "Will execute obs-kmsgrab-send from %s",
		     drmsend_filename);
	}

	/* 1. create and listen on unix socket */
	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

	unlink(addr.sun_path);
	if (-1 == bind(sockfd, (const struct sockaddr *)&addr, sizeof(addr))) {
		blog(LOG_ERROR, "Cannot bind unix socket to %s: %d",
		     addr.sun_path, errno);
		goto socket_cleanup;
	}

	if (-1 == listen(sockfd, 1)) {
		blog(LOG_ERROR, "Cannot listen on unix socket bound to %s: %d",
		     addr.sun_path, errno);
		goto socket_cleanup;
	}

	/* 2. run obs-kmsgrab-send utility */
	const pid_t drmsend_pid = fork();
	if (drmsend_pid == -1) {
		blog(LOG_ERROR, "Cannot fork(): %d", errno);
		goto socket_cleanup;
	} else if (drmsend_pid == 0) {
#ifdef USE_PKEXEC
		const char pkexec[] = "pkexec";
		execlp(pkexec, pkexec, drmsend_filename, dri_filename,
		       addr.sun_path, NULL);
		fprintf(stderr, "Cannot execlp(%s, %s): %d\n", pkexec, drmsend_filename,
			errno);
#else
		execlp(drmsend_filename, drmsend_filename, dri_filename,
		       addr.sun_path, NULL);
		fprintf(stderr, "Cannot execlp(%s): %d\n", drmsend_filename,
			errno);
#endif
		exit(-1);
	}

	blog(LOG_DEBUG, "Forked obs-kmsgrab-send to pid %d", drmsend_pid);

	/* 3. select() on unix socket w/ timeout */
	// FIXME updating timeout w/ time left is linux-specific, other unices might not do that
	struct timeval timeout;
	timeout.tv_sec = 5;
	timeout.tv_usec = 0;
	for (;;) {
		fd_set set;
		FD_ZERO(&set);
		FD_SET(sockfd, &set);
		const int maxfd = sockfd;
		const int nfds = select(maxfd + 1, &set, NULL, NULL, &timeout);
		if (nfds > 0) {
			if (FD_ISSET(sockfd, &set))
				break;
		}

		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			blog(LOG_ERROR, "Cannot select(): %d", errno);
			goto child_cleanup;
		}

		if (nfds == 0) {
			blog(LOG_ERROR, "Waiting for drmsend timed out");
			goto child_cleanup;
		}
	}

	blog(LOG_DEBUG, "Ready to accept");

	/* 4. accept() and receive data */
	int connfd = accept(sockfd, NULL, NULL);
	if (connfd < 0) {
		blog(LOG_ERROR, "Cannot accept unix socket: %d", errno);
		goto child_cleanup;
	}

	blog(LOG_DEBUG, "Receiving message from obs-kmsgrab-send");

	for (;;) {
		struct msghdr msg = {0};

		struct iovec io = {
			.iov_base = &list->resp,
			.iov_len = sizeof(list->resp),
		};
		msg.msg_iov = &io;
		msg.msg_iovlen = 1;

		char cmsg_buf[CMSG_SPACE(sizeof(int) *
					 OBS_DRMSEND_MAX_FRAMEBUFFERS) * 4];
		msg.msg_control = cmsg_buf;
		msg.msg_controllen = sizeof(cmsg_buf);
		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = sizeof(cmsg_buf);

		// FIXME blocking, may hang if drmsend dies before sending anything
		const ssize_t recvd = recvmsg(connfd, &msg, 0);
		blog(LOG_DEBUG, "recvmsg = %d", (int)recvd);
		if (recvd <= 0) {
			blog(LOG_ERROR, "cannot recvmsg: %d", errno);
			break;
		}

		if (io.iov_len != sizeof(list->resp)) {
			blog(LOG_ERROR,
			     "Received metadata size mismatch: %d received, %d expected",
			     (int)io.iov_len, (int)sizeof(list->resp));
			break;
		}

		if (list->resp.tag != OBS_DRMSEND_TAG) {
			blog(LOG_ERROR,
			     "Received metadata tag mismatch: %#x received, %#x expected",
			     list->resp.tag, OBS_DRMSEND_TAG);
			break;
		}

		if (cmsg->cmsg_len !=
		    CMSG_LEN(sizeof(int) * list->resp.num_fds)) {
			blog(LOG_ERROR,
			     "Received fd size mismatch: %d received, %d expected",
			     (int)cmsg->cmsg_len,
			     (int)CMSG_LEN(sizeof(int) *
					   list->resp.num_fds));
			break;
		}

		// FIXME validate fb, e.g. assert(planes <= 4 && planes > 0)

		memcpy(list->fb_fds, CMSG_DATA(cmsg),
		       sizeof(int) * list->resp.num_fds);
		retval = 1;
		break;
	}
	close(connfd);

	if (retval) {
		blog(LOG_INFO,
		     "Received %d framebuffers:", list->resp.num_framebuffers);
		for (int i = 0; i < list->resp.num_framebuffers; ++i) {
			const drmsend_framebuffer_t *fb =
				list->resp.framebuffers + i;
			blog(LOG_INFO,
			     "Received width=%d height=%d planes=%u fourcc=%s(%#x) fd=%d",
			     fb->width, fb->height, fb->planes, getDrmFourccName(fb->fourcc), fb->fourcc,
			     list->fb_fds[i]);
		}
	}

	// TODO consider using separate thread for waitpid() on drmsend_pid
	/* 5. waitpid() on obs-kmsgrab-send w/ timeout (poll) */
	int exited = 0;
child_cleanup:
	for (int i = 0; i < 10; ++i) {
		usleep(500 * 1000);
		int wstatus = 0;
		const pid_t p = waitpid(drmsend_pid, &wstatus, WNOHANG);
		if (p == drmsend_pid) {
			if (wstatus == 0 || WIFEXITED(wstatus)) {
				exited = 1;
				const int status = WEXITSTATUS(wstatus);
				if (status != 0)
					blog(LOG_ERROR, "%s returned %d",
					     drmsend_filename, status);
				break;
			}
		} else if (-1 == p) {
			const int err = errno;
			blog(LOG_ERROR, "Cannot waitpid() on drmsend: %d", err);
			if (err == ECHILD) {
				exited = 1;
				break;
			}
		}
	}

	if (!exited)
		blog(LOG_ERROR, "Couldn't wait for %s to exit, expect zombies",
		     drmsend_filename);

socket_cleanup:
	close(sockfd);
	unlink(addr.sun_path);

filename_cleanup:
	free(drmsend_filename);
	return retval;
}

static void dmabuf_source_close(dmabuf_source_t *ctx)
{
	blog(LOG_DEBUG, "dmabuf_source_close %p", ctx);
	ctx->active_fb = -1;
}

static void dmabuf_source_close_fds(dmabuf_source_t *ctx)
{
	for (int i = 0; i < ctx->fbs.resp.num_framebuffers; ++i) {
		const int fd = ctx->fbs.fb_fds[i];
		if (fd > 0)
			close(fd);
	}
}

static void dmabuf_source_open(dmabuf_source_t *ctx, uint32_t fb_id)
{
	blog(LOG_DEBUG, "dmabuf_source_open %p %#x", ctx, fb_id);
	assert(ctx->active_fb == -1);

	int index;
	for (index = 0; index < ctx->fbs.resp.num_framebuffers; ++index)
		if (fb_id == ctx->fbs.resp.framebuffers[index].fb_id)
			break;

	if (index == ctx->fbs.resp.num_framebuffers) {
		blog(LOG_ERROR, "Framebuffer id=%#x not found", fb_id);
		return;
	}

	blog(LOG_DEBUG, "Using framebuffer id=%#x (index=%d)", fb_id, index);

	const drmsend_framebuffer_t *fb = ctx->fbs.resp.framebuffers + index;

	blog(LOG_DEBUG, "%dx%d %d planes=%d", fb->width, fb->height,
	     ctx->fbs.fb_fds[index], fb->planes);

	// FIXME why is this needed?
	obs_enter_graphics();

	const uint64_t modifiers[4] = {fb->modifiers, fb->modifiers, fb->modifiers, fb->modifiers};
	const int fds[4] = {
		ctx->fbs.fb_fds[fb->fd_indexes[0]],
		ctx->fbs.fb_fds[fb->fd_indexes[1]],
		ctx->fbs.fb_fds[fb->fd_indexes[2]],
		ctx->fbs.fb_fds[fb->fd_indexes[3]]
	};
	ctx->texture = gs_texture_create_from_dmabuf(
			fb->width, fb->height,
			fb->fourcc,
			drmFourccToGs(fb->fourcc),
			fb->planes,
			fds,
			fb->pitches,
			fb->offsets,
			modifiers
	);

	obs_leave_graphics();

	if (!ctx->texture) {
		blog(LOG_ERROR, "Could not create texture from dmabuf source");
		return;
	}

	ctx->active_fb = index;
}

static void dmabuf_source_update(void *data, obs_data_t *settings)
{
	dmabuf_source_t *ctx = data;
	blog(LOG_DEBUG, "dmabuf_source_udpate", ctx);

	ctx->show_cursor = obs_data_get_bool(settings, "show_cursor");

	dmabuf_source_close_fds(ctx);
	dmabuf_source_close(ctx);
	dmabuf_source_open(ctx, obs_data_get_int(settings, "framebuffer"));
}

static void *dmabuf_source_create(obs_data_t *settings, obs_source_t *source)
{
	blog(LOG_DEBUG, "dmabuf_source_create");
	(void)settings;

	dmabuf_source_t *ctx = bzalloc(sizeof(dmabuf_source_t));
	ctx->source = source;
	ctx->active_fb = -1;

#define COUNTOF(a) (sizeof(a) / sizeof(*a))
	for (int i = 0; i < (int)COUNTOF(ctx->fbs.fb_fds); ++i) {
		ctx->fbs.fb_fds[i] = -1;
	}

	if (!dmabuf_source_receive_framebuffers(obs_data_get_string(settings, "dri_card"), &ctx->fbs)) {
		blog(LOG_ERROR, "Unable to enumerate DRM/KMS framebuffers");
		bfree(ctx);
		return NULL;
	}

	ctx->xcb = xcb_connect(NULL, NULL);
	if (!ctx->xcb || xcb_connection_has_error(ctx->xcb)) {
		blog(LOG_ERROR, "Unable to open X display, cursor will not be available");
	}

	ctx->cursor = xcb_xcursor_init(ctx->xcb);

	dmabuf_source_update(ctx, settings);
	return ctx;
}

static void dmabuf_source_destroy(void *data)
{
	dmabuf_source_t *ctx = data;
	blog(LOG_DEBUG, "dmabuf_source_destroy %p", ctx);

	if (ctx->texture)
		gs_texture_destroy(ctx->texture);

	dmabuf_source_close(ctx);
	dmabuf_source_close_fds(ctx);

	if (ctx->cursor)
		xcb_xcursor_destroy(ctx->cursor);

	if (ctx->xcb)
		xcb_disconnect(ctx->xcb);

	bfree(data);
}

static void dmabuf_source_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	dmabuf_source_t *ctx = data;

	if (!ctx->texture)
		return;
	if (!obs_source_showing(ctx->source))
		return;
	if (!ctx->cursor)
		return;

	xcb_xfixes_get_cursor_image_cookie_t cur_c =
		xcb_xfixes_get_cursor_image_unchecked(ctx->xcb);
	xcb_xfixes_get_cursor_image_reply_t *cur_r =
		xcb_xfixes_get_cursor_image_reply(ctx->xcb, cur_c, NULL);

	obs_enter_graphics();
	xcb_xcursor_update(ctx->cursor, cur_r);
	obs_leave_graphics();

	free(cur_r);
}

static void dmabuf_source_render(void *data, gs_effect_t *effect)
{
	const dmabuf_source_t *ctx = data;

	if (!ctx->texture)
		return;

	effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, ctx->texture);

	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite(ctx->texture, 0, 0, 0);
	}

	if (ctx->show_cursor && ctx->cursor) {
		while (gs_effect_loop(effect, "Draw")) {
			xcb_xcursor_render(ctx->cursor);
		}
	}
}

static bool dri_device_selected(void *data, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	blog(LOG_DEBUG, "dri_device_selected");
	dmabuf_source_t *ctx = data;

	obs_property_t *fb_list = obs_properties_get(props, "framebuffer");
	obs_property_list_clear(fb_list);

	if (!dmabuf_source_receive_framebuffers(obs_data_get_string(settings, "dri_card"), &ctx->fbs))
	{
		blog(LOG_ERROR, "Unable to enumerate DRM/KMS framebuffers");
		set_visible(props, "framebuffer", false);
		set_visible(props, "show_cursor", false);
		return false;
	}

	set_visible(props, "framebuffer", true);
	set_visible(props, "show_cursor", true);

	for (int i = 0; i < ctx->fbs.resp.num_framebuffers; ++i) {
		const drmsend_framebuffer_t *fb = ctx->fbs.resp.framebuffers + i;
		char buf[128];
		sprintf(buf, "%dx%d (%#x)", fb->width, fb->height, fb->fb_id);
		obs_property_list_add_int(fb_list, buf, fb->fb_id);
	}

	return true;
}


static void dmabuf_source_get_defaults(obs_data_t *defaults)
{
	obs_data_set_default_bool(defaults, "show_cursor", true);
	obs_data_set_default_string(defaults, "dri_card", "/dev/dri/card0");
}

static obs_properties_t *dmabuf_source_get_properties(void *data)
{
	dmabuf_source_t *ctx = data;
	blog(LOG_DEBUG, "dmabuf_source_get_properties %p", ctx);

	dmabuf_source_fblist_t stack_list = {0};

	obs_properties_t *props = obs_properties_create();
	obs_property_t *dri_device_list;

	dri_device_list = obs_properties_add_list(props, "dri_card", "DRI Card",
                                       OBS_COMBO_TYPE_LIST,
                                       OBS_COMBO_FORMAT_STRING);

	obs_property_set_modified_callback2(dri_device_list, dri_device_selected, data);

	obs_property_t *fb_list = obs_properties_add_list(
		props, "framebuffer", "Framebuffer to capture",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_properties_add_bool(props, "show_cursor",
		obs_module_text("CaptureCursor"));

	char path[32];
	for (int i = 0;; i++) {
		sprintf(path, "/dev/dri/card%d", i);
		if (access(path, F_OK) == 0)
			obs_property_list_add_string(dri_device_list, path, path);
		else
			break;
	}

	if (!ctx->fbs.resp.num_framebuffers) {
		set_visible(props, "framebuffer", false);
		set_visible(props, "show_cursor", false);
		return props;
	}

	for (int i = 0; i < ctx->fbs.resp.num_framebuffers; ++i) {
		const drmsend_framebuffer_t *fb = ctx->fbs.resp.framebuffers + i;
		char buf[128];
		sprintf(buf, "%dx%d (%#x)", fb->width, fb->height, fb->fb_id);
		obs_property_list_add_int(fb_list, buf, fb->fb_id);
	}

	return props;
}

static const char *dmabuf_source_get_name(void *data)
{
	blog(LOG_DEBUG, "dmabuf_source_get_name %p", data);
	return "DMA-BUF source";
}

static uint32_t dmabuf_source_get_width(void *data)
{
	const dmabuf_source_t *ctx = data;
	if (ctx->active_fb < 0)
		return 0;
	return ctx->fbs.resp.framebuffers[ctx->active_fb].width;
}

static uint32_t dmabuf_source_get_height(void *data)
{
	const dmabuf_source_t *ctx = data;
	if (ctx->active_fb < 0)
		return 0;
	return ctx->fbs.resp.framebuffers[ctx->active_fb].height;
}

struct obs_source_info dmabuf_input = {
	.id = "dmabuf-source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.get_name = dmabuf_source_get_name,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_DO_NOT_DUPLICATE,
	.create = dmabuf_source_create,
	.destroy = dmabuf_source_destroy,
	.video_tick = dmabuf_source_video_tick,
	.video_render = dmabuf_source_render,
	.get_width = dmabuf_source_get_width,
	.get_height = dmabuf_source_get_height,
	.get_defaults = dmabuf_source_get_defaults,
	.get_properties = dmabuf_source_get_properties,
	.update = dmabuf_source_update,
};

OBS_DECLARE_MODULE()

OBS_MODULE_USE_DEFAULT_LOCALE("linux-kmsgrab", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "libdrm/dma-buf based screen capture for linux";
}

bool obs_module_load(void)
{
	if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL && obs_get_nix_platform() != OBS_NIX_PLATFORM_WAYLAND) {
		blog(LOG_ERROR, "linux-dmabuf cannot run on non-EGL platforms");
		return false;
	}

	obs_register_source(&dmabuf_input);
	blog(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	// TODO deinit things
	blog(LOG_INFO, "plugin unloaded");
}
