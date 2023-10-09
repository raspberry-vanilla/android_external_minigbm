/*
 * Copyright (C) 2021-2022 Roman Stratiienko (r.stratiienko@gmail.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "GBM-MESA-GRALLOC"

extern "C" {
#include "drv_helpers.h"
}

#include "gbm_mesa_wrapper.h"

#include "UniqueFd.h"
#include "drv_priv.h"
#include "util.h"
#include <algorithm>
#include <array>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <glob.h>
#include <iterator>
#include <linux/dma-buf.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define GBM_WRAPPER_NAME "libgbm_mesa_wrapper.so"
#define GBM_GET_OPS_SYMBOL "get_gbm_ops"

void gbm_mesa_resolve_format_and_use_flags(struct driver *drv, uint32_t format, uint64_t use_flags,
					   uint32_t *out_format, uint64_t *out_use_flags)
{
	*out_format = format;
	*out_use_flags = use_flags;
	switch (format) {
	case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED:
		/* Camera subsystem requires NV12. */
		if (use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE)) {
			*out_format = DRM_FORMAT_NV12;
		} else {
			/*HACK: See b/28671744 */
			*out_format = DRM_FORMAT_XBGR8888;
		}
		break;
	case DRM_FORMAT_FLEX_YCbCr_420_888:
		*out_format = DRM_FORMAT_NV12;
		break;
	case DRM_FORMAT_BGR565:
		/* mesa3d doesn't support BGR565 */
		*out_format = DRM_FORMAT_RGB565;
		break;
	}
}

static const uint32_t scanout_render_formats[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
						   DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
						   DRM_FORMAT_RGB565 };

static const uint32_t texture_only_formats[] = { DRM_FORMAT_NV12, DRM_FORMAT_NV21,
						 DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID };

static struct format_metadata linear_metadata = { 1, 0, DRM_FORMAT_MOD_LINEAR };

int gbm_mesa_driver_init(struct driver *drv)
{
	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &linear_metadata, BO_USE_RENDER_MASK | BO_USE_SCANOUT);

	drv_add_combinations(drv, texture_only_formats, ARRAY_SIZE(texture_only_formats),
			     &linear_metadata, BO_USE_TEXTURE_MASK | BO_USE_SCANOUT);

	drv_add_combination(drv, DRM_FORMAT_R8, &linear_metadata, BO_USE_SW_MASK | BO_USE_LINEAR);

	// Fixes android.hardware.cts.HardwareBufferTest#testCreate CTS test
	drv_add_combination(drv, DRM_FORMAT_BGR888, &linear_metadata, BO_USE_SW_MASK);

	drv_modify_combination(drv, DRM_FORMAT_NV12, &linear_metadata,
			       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);
	drv_modify_combination(drv, DRM_FORMAT_NV21, &linear_metadata, BO_USE_HW_VIDEO_ENCODER);

	/*
	 * R8 format is used for Android's HAL_PIXEL_FORMAT_BLOB and is used for JPEG snapshots
	 * from camera and input/output from hardware decoder/encoder.
	 */
	drv_modify_combination(drv, DRM_FORMAT_R8, &linear_metadata,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_HW_VIDEO_ENCODER);

	/*
	 * Android also frequently requests YV12 formats for some camera implementations
	 * (including the external provider implmenetation).
	 */
	drv_modify_combination(drv, DRM_FORMAT_YVU420_ANDROID, &linear_metadata,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);

	return drv_modify_linear_combinations(drv);
}

struct GbmMesaDriver {
	~GbmMesaDriver()
	{
		if (gbm_dev)
			wrapper->dev_destroy(gbm_dev);

		if (dl_handle)
			dlclose(dl_handle);
	}

	struct gbm_ops *wrapper = nullptr;
	struct gbm_device *gbm_dev = nullptr;
	void *dl_handle = nullptr;

	UniqueFd gbm_node_fd;
	UniqueFd gpu_node_fd;
};

struct GbmMesaDriverPriv {
	std::shared_ptr<GbmMesaDriver> gbm_mesa_drv;
};

/*
 * Check if target device has KMS.
 */
int is_kms_dev(int fd)
{
	auto res = drmModeGetResources(fd);
	if (!res)
		return false;

	bool is_kms = res->count_crtcs > 0 && res->count_connectors > 0 && res->count_encoders > 0;

	drmModeFreeResources(res);

	return is_kms;
}

/*
 * Search for a KMS device. Return opened file descriptor on success.
 */
int open_drm_dev(bool card_node, std::function<bool(int, bool, std::string)> found)
{
	glob_t glob_buf;
	memset(&glob_buf, 0, sizeof(glob_buf));
	int fd;
	const char *pattern = card_node ? "/dev/dri/card*" : "/dev/dri/renderD*";

	int ret = glob(pattern, 0, NULL, &glob_buf);
	if (ret) {
		globfree(&glob_buf);
		return -EINVAL;
	}

	for (size_t i = 0; i < glob_buf.gl_pathc; ++i) {
		fd = open(glob_buf.gl_pathv[i], O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			drv_loge("Unable to open %s with error %s", glob_buf.gl_pathv[i],
				 strerror(errno));
			continue;
		}

		drmVersionPtr drm_version;
		drm_version = drmGetVersion(fd);
		std::string drm_name(drm_version->name);
		drmFreeVersion(drm_version);

		if (found(fd, is_kms_dev(fd), drm_name))
			continue;

		close(fd);
		fd = 0;
	}

	globfree(&glob_buf);

	return 0;
}

/*
 * List of GPU which rely on separate display controller drivers,
 * For this GPUs we have to find and open /dev/cardX KMS node
 * Other GPUs can be accessed via renderD GPU node.
 */
static std::array<std::string, 6> separate_dc_gpu_list = { "v3d",      "vc4",  "etnaviv",
							   "panfrost", "lima", "freedreno" };

static bool is_separate_dc_gpu(UniqueFd *out_gpu_fd)
{
	UniqueFd gpu_fd;
	bool separate_dc = false;
	std::string gpu_name;

	open_drm_dev(false, [&](int fd, bool is_kms, std::string drm_name) -> bool {
		if (separate_dc)
			return false;

		for (const auto &name : separate_dc_gpu_list) {
			if (drm_name == std::string(name))
				separate_dc = true;
		}
		gpu_fd = UniqueFd(fd);
		gpu_name = drm_name;

		return true;
	});

	*out_gpu_fd = std::move(gpu_fd);

	drv_logi("Found GPU %s\n", gpu_name.c_str());

	return separate_dc;
}

static std::shared_ptr<GbmMesaDriver> gbm_mesa_get_or_init_driver(struct driver *drv,
								  bool mapper_sphal)
{
	std::shared_ptr<GbmMesaDriver> gbm_mesa_drv;

	if (!drv->priv) {
		gbm_mesa_drv = std::make_unique<GbmMesaDriver>();

		bool look_for_kms = is_separate_dc_gpu(&gbm_mesa_drv->gpu_node_fd);

		if (look_for_kms && !mapper_sphal) {
			drv_logi("GPU require KMSRO entry, searching for separate KMS driver...\n");
			open_drm_dev(true, [&](int fd, bool is_kms, std::string drm_name) -> bool {
				if (!is_kms || gbm_mesa_drv->gbm_node_fd)
					return false;

				gbm_mesa_drv->gbm_node_fd = UniqueFd(fd);
				drv_logi("Found KMS dev %s\n", drm_name.c_str());
				return true;
			});
			/* cardX KMS node need this otherwise composer won't be able to configure
			 * KMS state */
			if (gbm_mesa_drv->gbm_node_fd)
				drmDropMaster(gbm_mesa_drv->gbm_node_fd.Get());
			else
				drv_loge(
				    "Unable to find/open /dev/card node with KMS capabilities.\n");
		} else {
			gbm_mesa_drv->gbm_node_fd = UniqueFd(dup(gbm_mesa_drv->gpu_node_fd.Get()));
		}

		if (!gbm_mesa_drv->gbm_node_fd) {
			drv_loge("Unable to find or open DRM node");
			return nullptr;
		}

		gbm_mesa_drv->dl_handle = dlopen(GBM_WRAPPER_NAME, RTLD_NOW);
		if (gbm_mesa_drv->dl_handle == nullptr) {
			drv_loge("%s", dlerror());
			drv_loge("Unable to open '%s' shared library", GBM_WRAPPER_NAME);
			return nullptr;
		}

		auto get_gbm_ops =
		    (struct gbm_ops * (*)(void)) dlsym(gbm_mesa_drv->dl_handle, GBM_GET_OPS_SYMBOL);
		if (get_gbm_ops == nullptr) {
			drv_loge("Unable to find '%s' symbol", GBM_GET_OPS_SYMBOL);
			return nullptr;
		}

		gbm_mesa_drv->wrapper = get_gbm_ops();
		if (gbm_mesa_drv->wrapper == nullptr) {
			drv_loge("Unable to get wrapper ops");
			return nullptr;
		}

		gbm_mesa_drv->gbm_dev =
		    gbm_mesa_drv->wrapper->dev_create(gbm_mesa_drv->gbm_node_fd.Get());
		if (!gbm_mesa_drv->gbm_dev) {
			drv_loge("Unable to create gbm_mesa driver");
			return nullptr;
		}

		auto priv = new GbmMesaDriverPriv();
		priv->gbm_mesa_drv = gbm_mesa_drv;
		drv->priv = priv;
	} else {
		gbm_mesa_drv = ((GbmMesaDriverPriv *)drv->priv)->gbm_mesa_drv;
	}

	return gbm_mesa_drv;
}

void gbm_mesa_driver_close(struct driver *drv)
{
	if (drv->priv) {
		delete (GbmMesaDriverPriv *)(drv->priv);
		drv->priv = nullptr;
	}
}

struct GbmMesaBoPriv {
	~GbmMesaBoPriv()
	{
		if (gbm_bo) {
			auto wr = drv->wrapper;
			wr->free(gbm_bo);
		}
	}

	std::shared_ptr<GbmMesaDriver> drv;
	uint32_t map_stride = 0;
	UniqueFd fds[DRV_MAX_PLANES];
	struct gbm_bo *gbm_bo = nullptr;
};

static void gbm_mesa_inode_to_handle(struct bo *bo)
{
	// DRM handles are used as unique buffer keys
	// Since we are not relying on DRM, provide fstat->inode instead
	auto priv = (GbmMesaBoPriv *)bo->priv;

	for (size_t plane = 0; plane < bo->meta.num_planes; plane++) {
		struct stat sb;
		fstat(priv->fds[plane].Get(), &sb);
		bo->handles[plane].u64 = sb.st_ino;
	}
}

int gbm_mesa_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
		       uint64_t use_flags)
{
	/* For some ARM SOCs, if no more free CMA available, buffer can be allocated in VRAM but HWC
	 * won't be able to display it directly, using GPU for compositing */
	bool scanout_strong = false;
	bool bo_layout_ready = false;
	uint32_t size_align = 1;
	int err = 0;

	auto drv = gbm_mesa_get_or_init_driver(bo->drv, false);
	if (drv == nullptr) {
		drv_loge("Failed to init gbm driver");
		return -EINVAL;
	}

	auto wr = drv->wrapper;

	struct alloc_args alloc_args = {
		.gbm = drv->gbm_dev,
		.width = width,
		.height = height,
		.drm_format = wr->get_gbm_format(format) ? format : 0,
		.force_linear = (use_flags & BO_USE_SW_MASK) != 0,
		.needs_map_stride = (use_flags & BO_USE_SW_MASK) != 0,
		.use_scanout = (use_flags & BO_USE_SCANOUT) != 0,
	};

	/* Alignment for RPI4 CSI camera. Since we do not care about other cameras, keep this
	 * globally for now.
	 * TODO: Create/use constraints table for camera/codecs */
	if (use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE)) {
		scanout_strong = true;
		alloc_args.use_scanout = true;
		alloc_args.width = ALIGN(alloc_args.width, 32);
		size_align = 4096;
	}

	if (alloc_args.drm_format == 0) {
		/* Always use linear for spoofed format allocations. */
		drv_bo_from_format(bo, alloc_args.width, 1, alloc_args.height, format);
		bo_layout_ready = true;
		bo->meta.total_size = ALIGN(bo->meta.total_size, size_align);
		alloc_args.drm_format = DRM_FORMAT_R8;
		alloc_args.width = bo->meta.total_size;
		alloc_args.height = 1;
		alloc_args.force_linear = true;

		drv_logv("Unable to allocate 0x%08x format, allocate as 1D buffer", format);
	}

	if (alloc_args.drm_format == DRM_FORMAT_R8 && alloc_args.height == 1) {
		/* Some mesa drivers may not support 1D allocations.
		 * Use 2D texture with 4096 width instead.
		 */
		alloc_args.needs_map_stride = false;
		alloc_args.height = DIV_ROUND_UP(alloc_args.width, 4096);
		alloc_args.width = 4096;
		drv_logv("Allocate 1D buffer as %dx%d R8 2D texture", alloc_args.width,
			 alloc_args.height);
	}

	err = wr->alloc(&alloc_args);

	if (err && !scanout_strong) {
		drv_loge("Failed to allocate for scanout, trying non-scanout");
		alloc_args.use_scanout = false;
		err = wr->alloc(&alloc_args);
	}

	if (err) {
		drv_loge("Failed to allocate buffer");
		return err;
	}

	if (!bo_layout_ready)
		drv_bo_from_format(bo, alloc_args.out_stride, 1, alloc_args.height, format);

	drv_logv("Allocated: %dx%d, stride: %d, map_stride: %d", width, height,
		 alloc_args.out_stride, alloc_args.out_map_stride);

	auto priv = new GbmMesaBoPriv();
	for (size_t plane = 0; plane < bo->meta.num_planes; plane++) {
		priv->fds[plane] = UniqueFd(alloc_args.out_fd);

	}

	priv->map_stride = alloc_args.out_map_stride;
	bo->meta.format_modifier = alloc_args.out_modifier;

	bo->priv = priv;
	priv->drv = drv;

	gbm_mesa_inode_to_handle(bo);

	return 0;
}

int gbm_mesa_bo_import(struct bo *bo, struct drv_import_fd_data *data)
{
	if (bo->priv) {
		drv_loge("%s bo isn't empty", __func__);
		return -EINVAL;
	}
	auto priv = new GbmMesaBoPriv();
	for (size_t plane = 0; plane < bo->meta.num_planes; plane++) {
		priv->fds[plane] = UniqueFd(dup(data->fds[plane]));
	}

	if (data->use_flags & BO_USE_SW_MASK) {
		// Mapping require importing by gbm_mesa
		auto drv = gbm_mesa_get_or_init_driver(bo->drv, true);
		auto wr = drv->wrapper;

		uint32_t s_format = data->format;
		int s_height = data->height;
		int s_width = data->width;
		if (wr->get_gbm_format(s_format) == 0) {
			s_width = bo->meta.total_size;
			s_height = 1;
			s_format = DRM_FORMAT_R8;
		}

		priv->drv = drv;
		priv->gbm_bo = wr->import(drv->gbm_dev, data->fds[0], s_width, s_height,
					  data->strides[0], data->format_modifier, s_format);
	}

	bo->priv = priv;

	gbm_mesa_inode_to_handle(bo);

	return 0;
}

int gbm_mesa_bo_destroy(struct bo *bo)
{
	if (bo->priv) {
		delete (GbmMesaBoPriv *)(bo->priv);
		bo->priv = nullptr;
	}
	return 0;
}

int gbm_mesa_bo_get_plane_fd(struct bo *bo, size_t plane)
{
	return dup(((GbmMesaBoPriv *)bo->priv)->fds[plane].Get());
}

void *gbm_mesa_bo_map(struct bo *bo, struct vma *vma, uint32_t map_flags)
{
	auto drv = gbm_mesa_get_or_init_driver(bo->drv, true);
	auto wr = drv->wrapper;

	vma->length = bo->meta.total_size;

	auto priv = (GbmMesaBoPriv *)bo->priv;
	assert(priv->gbm_bo != nullptr);

	void *buf = MAP_FAILED;

	uint32_t s_format = bo->meta.format;
	int s_width = bo->meta.width;
	int s_height = bo->meta.height;
	if (wr->get_gbm_format(s_format) == 0) {
		s_width = bo->meta.total_size;
		s_height = 1;
	}

	wr->map(priv->gbm_bo, s_width, s_height, &buf, &vma->priv);

	return buf;
}

int gbm_mesa_bo_unmap(struct bo *bo, struct vma *vma)
{
	auto drv = gbm_mesa_get_or_init_driver(bo->drv, true);
	auto wr = drv->wrapper;

	auto priv = (GbmMesaBoPriv *)bo->priv;
	assert(priv->gbm_bo != nullptr);
	assert(vma->priv != nullptr);
	wr->unmap(priv->gbm_bo, vma->priv);
	vma->priv = nullptr;
	return 0;
}

uint32_t gbm_mesa_bo_get_map_stride(struct bo *bo)
{
	auto priv = (GbmMesaBoPriv *)bo->priv;

	return priv->map_stride;
}
