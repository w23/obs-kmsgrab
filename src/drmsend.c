#include "drmsend.h"

#include <xf86drm.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drmMode.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define LOG_PREFIX "obs-drmsend: "

#define ERR(fmt, ...) fprintf(stderr, LOG_PREFIX fmt "\n", ##__VA_ARGS__)
#define MSG(fmt, ...) fprintf(stdout, LOG_PREFIX fmt "\n", ##__VA_ARGS__)

static void printUsage(const char *name)
{
	MSG("usage: %s /dev/dri/card socket_filename", name);
}

typedef struct {
	drmsend_response_t response;
	int fb_fds[OBS_DRMSEND_MAX_FRAMEBUFFERS * 4];
} response_data_t;

static int responseSend(const char *sockname, response_data_t *data) {
	int sockfd = -1;
	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	{
		struct sockaddr_un addr;
		addr.sun_family = AF_UNIX;
		if (strlen(sockname) >= sizeof(addr.sun_path)) {
			MSG("Socket filename '%s' is too long, max %d",
			    sockname, (int)sizeof(addr.sun_path));
			return 0;
		}

		strcpy(addr.sun_path, sockname);
		if (-1 == connect(sockfd, (const struct sockaddr *)&addr,
				  sizeof(addr))) {
			MSG("Cannot connect to unix socket: %d", errno);
			return 0;
		}
	}

	data->response.tag = OBS_DRMSEND_TAG;

	struct msghdr msg = {0};

	struct iovec io = {
		.iov_base = &data->response,
		.iov_len = sizeof(data->response),
	};
	msg.msg_iov = &io;
	msg.msg_iovlen = 1;

	const int fb_size = sizeof(int) * data->response.num_fds;
	char cmsg_buf[CMSG_SPACE(fb_size)];

	msg.msg_control = cmsg_buf;
	msg.msg_controllen = sizeof cmsg_buf;

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(fb_size);
	memcpy(CMSG_DATA(cmsg), data->fb_fds, fb_size);

	const ssize_t sent = sendmsg(sockfd, &msg, 0);

	if (sent < 0) {
		perror("cannot sendmsg");
		goto cleanup;
	}

	MSG("sent %d bytes", (int)sent);

cleanup:
	close(sockfd);
	return 1;
}

const char *program_name;

int responseAppendFramebuffer(response_data_t *data, const drmModeFB2Ptr drmfb, int drmfd) {
	// Skip duplicates
	for (int i = 0; i < data->response.num_framebuffers; ++i) {
		if (data->response.framebuffers[i].fb_id == drmfb->fb_id) {
			MSG("Duplicate fb_id %#x found", drmfb->fb_id);
			return 0;
		}
	}

	MSG("\t\tfb%d: %#x %ux%u fourcc=%#x modifier=%#lx flags=%#x",
			data->response.num_framebuffers, drmfb->fb_id, drmfb->width, drmfb->height, drmfb->pixel_format, drmfb->modifier, drmfb->flags);

	if (data->response.num_framebuffers == OBS_DRMSEND_MAX_FRAMEBUFFERS) {
		ERR("Too many framebuffers, max %d", OBS_DRMSEND_MAX_FRAMEBUFFERS);
		return -1;
	}

	drmsend_framebuffer_t *fb = data->response.framebuffers + data->response.num_framebuffers;

	fb->planes = 0;
	int fb_fds[4] = {-1, -1, -1, -1};
	for (int i = 0; i < 4 && drmfb->handles[i]; ++i) {
		// Check whether this handle has been already acquired
		int j;
		for (j = 0; j < i; ++j) {
			if (drmfb->handles[i] == drmfb->handles[j]) {
				fb_fds[i] = fb_fds[j];
				break;
			}
		}

		// Not found, need to acquire fd for handle
		if (j == i) {
			const int ret = drmPrimeHandleToFD(drmfd, drmfb->handles[i], O_RDONLY, fb_fds + i);
			if (ret != 0 || fb_fds[i] == -1) {
				ERR("Cannot get fd for fb %#x handle %#x: %s (%d)", drmfb->fb_id, drmfb->handles[i], strerror(errno), errno);
				// TODO close already open handle fds
				return -1;
			}
		}

		MSG("\t\t\t%d: handle=%#x(%d) pitch=%u offset=%u", i, drmfb->handles[i], fb_fds[i], drmfb->pitches[i], drmfb->offsets[i]);

		fb->offsets[i] = drmfb->offsets[i];
		fb->pitches[i] = drmfb->pitches[i];

		++fb->planes;
	}

	if (fb->planes == 0) {
		ERR("\t\t\tNo valid FB handles were found for fb %#x", drmfb->fb_id);
		ERR("\t\t\tPossible reason: not permitted to get FB handles. Do `sudo setcap cap_sys_admin+ep %s`", program_name);
		return -1;
	}

	fb->width = drmfb->width;
	fb->height = drmfb->height;
	fb->fourcc = drmfb->pixel_format;
	fb->modifiers = drmfb->modifier;

	// Copy only unique fds
	int unique_fds = 0;
	for (int i = 0; i < fb->planes; ++i) {
		int j;
		for (j = 0; j < i && fb_fds[i] != fb_fds[j]; ++j);

		fb->fd_indexes[i] = data->response.num_fds + j;

		if (j == i) {
			data->fb_fds[fb->fd_indexes[i]] = fb_fds[i];
			++unique_fds;
		}
	}
	data->response.num_fds += unique_fds;

	++data->response.num_framebuffers;
	return 0;
}

static void readDrmData(int drmfd, response_data_t *data) {
	drmModePlaneResPtr planes = drmModeGetPlaneResources(drmfd);
	if (!planes) {
		ERR("Cannot get drm planes: %s (%d)", strerror(errno), errno);
		return;
	}

	MSG("DRM planes %d:", planes->count_planes);
	for (uint32_t i = 0; i < planes->count_planes; ++i) {
		drmModePlanePtr plane = drmModeGetPlane(drmfd, planes->planes[i]);
		if (!plane) {
			ERR("Cannot get drmModePlanePtr for plane %#x: %s (%d)",
					planes->planes[i], strerror(errno), errno);
			continue;
		}

		MSG("\t%d: fb_id=%#x", i, plane->fb_id);

		if (plane->fb_id) {
			drmModeFB2Ptr drmfb = drmModeGetFB2(drmfd, plane->fb_id);
			if (!drmfb) {
				ERR("Cannot get drmModeFB2Ptr for fb %#x: %s (%d)",
						plane->fb_id, strerror(errno), errno);
			} else {
				responseAppendFramebuffer(data, drmfb, drmfd);
				drmModeFreeFB2(drmfb);
			}
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(planes);
}

int main(int argc, const char *argv[])
{
	if (argc < 3) {
		printUsage(argv[0]);
		return 1;
	}

	program_name = argv[0];
	const char *card = argv[1];
	const char *sockname = argv[2];

	MSG("Opening card %s", card);
	const int drmfd = open(card, O_RDONLY);
	if (drmfd < 0) {
		perror("Cannot open card");
		return 1;
	}

	if (0 != drmSetClientCap(drmfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
		perror("Cannot tell drm to expose all planes; the rest will very likely fail");
	}

	response_data_t data = {0};

	readDrmData(drmfd, &data);
	responseSend(sockname, &data);

	close(drmfd);
	return 0;
}
