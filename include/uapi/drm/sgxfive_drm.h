/* SPDX-License-Identifier: (GPL-2.0-only WITH Linux-syscall-note) OR MIT */
/*
 * include/uapi/drm/sgxfive_drm.h - the sgxfive render-node interface for the
 * PowerVR SGX545 in Intel Cedarview.
 *
 * Draft: not a stable interface until it is merged upstream together with its
 * open-source userspace.
 */
#ifndef SGXFIVE_DRM_UAPI_H
#define SGXFIVE_DRM_UAPI_H

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * DOC: sgxfive UAPI
 *
 * The sgxfive render node gives an unprivileged process GEM buffer objects,
 * its own GPU virtual address spaces (VM contexts), contexts and job
 * submission with explicit synchronisation through DRM syncobjs. The kernel
 * owns the SGX firmware: no ioctl takes or returns a physical address, a
 * firmware address or an address inside the kernel's alias region.
 *
 * Rules for every argument struct, as in the PowerVR Rogue UAPI:
 *
 *  - All members are naturally aligned and the struct is padded to a multiple
 *    of 8 bytes, so i686 and x86-64 share one layout. Padding is explicit,
 *    named ``_padding_<hex offset>``, and must be zero; this is always checked.
 *  - Every ``flags`` field has a ``*_FLAGS_MASK``. Bits outside the mask are
 *    reserved and rejected with -EINVAL.
 *  - Structs reached through &struct drm_sgxfive_obj_array are versioned by
 *    their stride: a shorter (older) element is zero-extended, a longer (newer)
 *    one is accepted only if its unknown tail is zero. An array with a
 *    non-zero count needs a stride of 1 to %DRM_SGXFIVE_OBJ_ARRAY_MAX_STRIDE
 *    bytes; any other stride is -EINVAL.
 *  - GPU virtual addresses are carried as __u64 and must be below 4 GiB.
 */

/**
 * struct drm_sgxfive_obj_array - An array of objects passed by pointer.
 */
struct drm_sgxfive_obj_array {
	/** @stride: Size of one element as userspace knows it. Used for versioning. */
	__u32 stride;

	/** @count: Number of elements in the array. */
	__u32 count;

	/** @array: User pointer to the first element. */
	__u64 array;
};

/**
 * DRM_SGXFIVE_OBJ_ARRAY() - Literal of type &struct drm_sgxfive_obj_array.
 * @cnt: Number of elements pointed to by @ptr.
 * @ptr: Pointer to the start of a C array.
 */
#define DRM_SGXFIVE_OBJ_ARRAY(cnt, ptr) \
	{ .stride = sizeof((ptr)[0]), .count = (cnt), .array = (__u64)(uintptr_t)(ptr) }

/* The largest &drm_sgxfive_obj_array.stride the kernel accepts, in bytes. */
#define DRM_SGXFIVE_OBJ_ARRAY_MAX_STRIDE	256

/**
 * DOC: sgxfive IOCTL numbers
 *
 * 0x0a to 0x0f are reserved for future ioctls.
 */
#define DRM_SGXFIVE_DEV_QUERY			0x00
#define DRM_SGXFIVE_CREATE_BO			0x01
#define DRM_SGXFIVE_GET_BO_MMAP_OFFSET		0x02
#define DRM_SGXFIVE_CREATE_VM_CONTEXT		0x03
#define DRM_SGXFIVE_DESTROY_VM_CONTEXT		0x04
#define DRM_SGXFIVE_VM_MAP			0x05
#define DRM_SGXFIVE_VM_UNMAP			0x06
#define DRM_SGXFIVE_CREATE_CONTEXT		0x07
#define DRM_SGXFIVE_DESTROY_CONTEXT		0x08
#define DRM_SGXFIVE_SUBMIT_JOBS			0x09

/*
 * SGXFIVE_IOCTL() - Build an sgxfive IOCTL number from its index, one of
 * DRM_IOR/DRM_IOW/DRM_IOWR and the args struct name without its
 * ``drm_sgxfive_`` prefix and ``_args`` suffix. Not for direct use.
 */
#define SGXFIVE_IOCTL(_ioctl, _mode, _data) \
	_mode(DRM_COMMAND_BASE + DRM_SGXFIVE_##_ioctl, struct drm_sgxfive_##_data##_args)

#define DRM_IOCTL_SGXFIVE_DEV_QUERY \
	SGXFIVE_IOCTL(DEV_QUERY, DRM_IOWR, dev_query)
#define DRM_IOCTL_SGXFIVE_CREATE_BO \
	SGXFIVE_IOCTL(CREATE_BO, DRM_IOWR, create_bo)
#define DRM_IOCTL_SGXFIVE_GET_BO_MMAP_OFFSET \
	SGXFIVE_IOCTL(GET_BO_MMAP_OFFSET, DRM_IOWR, get_bo_mmap_offset)
#define DRM_IOCTL_SGXFIVE_CREATE_VM_CONTEXT \
	SGXFIVE_IOCTL(CREATE_VM_CONTEXT, DRM_IOWR, create_vm_context)
#define DRM_IOCTL_SGXFIVE_DESTROY_VM_CONTEXT \
	SGXFIVE_IOCTL(DESTROY_VM_CONTEXT, DRM_IOW, destroy_vm_context)
#define DRM_IOCTL_SGXFIVE_VM_MAP \
	SGXFIVE_IOCTL(VM_MAP, DRM_IOW, vm_map)
#define DRM_IOCTL_SGXFIVE_VM_UNMAP \
	SGXFIVE_IOCTL(VM_UNMAP, DRM_IOW, vm_unmap)

/**
 * DOC: sgxfive DEV_QUERY
 *
 * One extensible query ioctl. &struct drm_sgxfive_dev_query_args.type names
 * the output struct. With a NULL pointer only the size the kernel knows is
 * returned; a smaller size gets a truncated copy; a larger one gets the known
 * struct and the size is set to what was written. Arrays inside query structs
 * follow the same rule through their &struct drm_sgxfive_obj_array.
 */

/**
 * enum drm_sgxfive_dev_query - Values for &drm_sgxfive_dev_query_args.type.
 *
 * Append only.
 */
enum drm_sgxfive_dev_query {
	/** @DRM_SGXFIVE_DEV_QUERY_GPU_INFO: &struct drm_sgxfive_dev_query_gpu_info. */
	DRM_SGXFIVE_DEV_QUERY_GPU_INFO = 0,
	/** @DRM_SGXFIVE_DEV_QUERY_FW_INFO: &struct drm_sgxfive_dev_query_fw_info. */
	DRM_SGXFIVE_DEV_QUERY_FW_INFO,
	/** @DRM_SGXFIVE_DEV_QUERY_HEAP_INFO: &struct drm_sgxfive_dev_query_heap_info. */
	DRM_SGXFIVE_DEV_QUERY_HEAP_INFO,
	/** @DRM_SGXFIVE_DEV_QUERY_JOB_INFO: &struct drm_sgxfive_dev_query_job_info. */
	DRM_SGXFIVE_DEV_QUERY_JOB_INFO,
	/** @DRM_SGXFIVE_DEV_QUERY_RUNTIME_INFO: &struct drm_sgxfive_dev_query_runtime_info. */
	DRM_SGXFIVE_DEV_QUERY_RUNTIME_INFO,
};

/*
 * DRM_SGXFIVE_QUIRK_SAMPLE_CACHE: the host bridge is below revision 4.
 * Textures must be in the TEXTURE heap, and the other heaps use the REV1
 * layout.
 */
#define DRM_SGXFIVE_QUIRK_SAMPLE_CACHE		(1ULL << 0)
#define DRM_SGXFIVE_QUIRKS_MASK			DRM_SGXFIVE_QUIRK_SAMPLE_CACHE

/*
 * The firmware has answered a misc-info request: core_id_sw,
 * core_revision_sw and &drm_sgxfive_dev_query_fw_info.ddk_version are
 * valid; until then they are 0. Only the debug group's dri/N/sgx/miscinfo
 * file sends that request, so in a kernel built without
 * CONFIG_DRM_GMA500_SGXFIVE_DEBUG the flag stays clear.
 */
#define DRM_SGXFIVE_GPU_INFO_SW_VALID		(1U << 0)

/**
 * struct drm_sgxfive_dev_query_gpu_info - The GPU core.
 *
 * Answered from registers read at probe and the firmware's last misc-info
 * reply; the query never boots the firmware.
 */
struct drm_sgxfive_dev_query_gpu_info {
	/** @core_id: The CORE_ID register, 0x01150000 on SGX545. */
	__u32 core_id;

	/** @core_revision: The CORE_REVISION register. */
	__u32 core_revision;

	/** @core_id_sw: The firmware's core id (545), if %DRM_SGXFIVE_GPU_INFO_SW_VALID. */
	__u32 core_id_sw;

	/** @core_revision_sw: The firmware's core revision, as @core_id_sw. */
	__u32 core_revision_sw;

	/** @quirks: ``DRM_SGXFIVE_QUIRK_*`` bits. */
	__u64 quirks;

	/** @host_bridge_rev: PCI revision of 00:00.0, which selects the quirks. */
	__u32 host_bridge_rev;

	/** @flags: ``DRM_SGXFIVE_GPU_INFO_*`` bits. */
	__u32 flags;
};

/**
 * enum drm_sgxfive_fw_state - Values for &drm_sgxfive_dev_query_fw_info.state.
 */
enum drm_sgxfive_fw_state {
	/** @DRM_SGXFIVE_FW_STATE_OFF: Not booted (lazy boot) or powered off. */
	DRM_SGXFIVE_FW_STATE_OFF = 0,
	/** @DRM_SGXFIVE_FW_STATE_ON: Booted and accepting commands. */
	DRM_SGXFIVE_FW_STATE_ON,
	/** @DRM_SGXFIVE_FW_STATE_IDLE: Idle with clocks gated; the next job wakes it. */
	DRM_SGXFIVE_FW_STATE_IDLE,
};

/**
 * struct drm_sgxfive_dev_query_fw_info - The firmware the kernel loaded.
 *
 * Userspace may refuse a firmware build it does not know.
 */
struct drm_sgxfive_dev_query_fw_info {
	/**
	 * @ddk_version: The firmware's version, major << 16 | minor << 8 |
	 * branch, as its misc-info reply gives it; 0 until it has given one
	 * (see %DRM_SGXFIVE_GPU_INFO_SW_VALID).
	 */
	__u32 ddk_version;

	/** @ddk_build: The firmware's build number, from the firmware file. */
	__u32 ddk_build;

	/** @build_options: The firmware's build options word, from the firmware file. */
	__u32 build_options;

	/** @fw_format: format_version of sgx545_ukernel.fw. */
	__u32 fw_format;

	/** @state: &enum drm_sgxfive_fw_state, informational only. */
	__u32 state;

	/** @_padding_14: Reserved, zero. */
	__u32 _padding_14;
};

/**
 * enum drm_sgxfive_heap_id - Index into the HEAP_INFO array.
 *
 * Every id is always reported. The kernel supports one layout, the REV1
 * layout of the silicon with %DRM_SGXFIVE_QUIRK_SAMPLE_CACHE (host bridge
 * revision below 4), and reports it whatever the revision; other silicon is
 * not supported. A heap that a later layout lacks would have size 0.
 * Userspace never hardcodes heap addresses.
 */
enum drm_sgxfive_heap_id {
	/** @DRM_SGXFIVE_HEAP_GENERAL: Surfaces, background objects, streams. */
	DRM_SGXFIVE_HEAP_GENERAL = 0,
	/** @DRM_SGXFIVE_HEAP_TEXTURE: Textures; they must lie here under QUIRK_SAMPLE_CACHE. */
	DRM_SGXFIVE_HEAP_TEXTURE,
	/** @DRM_SGXFIVE_HEAP_PDS_PIXEL: Pixel PDS code and data (PDS_EXEC_BASE). */
	DRM_SGXFIVE_HEAP_PDS_PIXEL,
	/** @DRM_SGXFIVE_HEAP_PDS_VERTEX: Vertex PDS code and data (TA work). */
	DRM_SGXFIVE_HEAP_PDS_VERTEX,
	/** @DRM_SGXFIVE_HEAP_USSE_PIXEL: Pixel USSE code, USE_CODE_BASE_0..9 windows. */
	DRM_SGXFIVE_HEAP_USSE_PIXEL,
	/** @DRM_SGXFIVE_HEAP_USSE_VERTEX: Vertex USSE code, USE_CODE_BASE_10..13. */
	DRM_SGXFIVE_HEAP_USSE_VERTEX,
	/** @DRM_SGXFIVE_HEAP_KERNEL_CODE: Kernel alias of firmware code; reserved. */
	DRM_SGXFIVE_HEAP_KERNEL_CODE,
	/** @DRM_SGXFIVE_HEAP_KERNEL_DATA: Kernel alias of firmware data; reserved. */
	DRM_SGXFIVE_HEAP_KERNEL_DATA,
	/** @DRM_SGXFIVE_HEAP_COUNT: Number of ids known to this header. */
	DRM_SGXFIVE_HEAP_COUNT,
	/* Add new heaps above DRM_SGXFIVE_HEAP_COUNT only. */
};

/* The heap is the kernel's: VM_MAP into it fails with -EINVAL. */
#define DRM_SGXFIVE_HEAP_RESERVED		(1U << 0)
/*
 * The heap base is a USE code base: a DOUTU reaches code within the 512 KiB
 * window selected by its CBASE field, counted from the heap base.
 */
#define DRM_SGXFIVE_HEAP_FIXED_WINDOWS		(1U << 1)
#define DRM_SGXFIVE_HEAP_FLAGS_MASK \
	(DRM_SGXFIVE_HEAP_RESERVED | DRM_SGXFIVE_HEAP_FIXED_WINDOWS)

/**
 * struct drm_sgxfive_heap - One GPU virtual address range.
 */
struct drm_sgxfive_heap {
	/** @base: First GPU virtual address of the heap. */
	__u64 base;

	/** @size: Size in bytes; 0 if the heap is absent on this device. */
	__u64 size;

	/** @flags: ``DRM_SGXFIVE_HEAP_*`` bits. */
	__u32 flags;

	/** @page_size_log2: Log2 of the GPU page size, 12. */
	__u32 page_size_log2;
};

/**
 * struct drm_sgxfive_dev_query_heap_info - The heaps of every VM context.
 */
struct drm_sgxfive_dev_query_heap_info {
	/**
	 * @heaps: Array of &struct drm_sgxfive_heap indexed by
	 * &enum drm_sgxfive_heap_id. With a NULL array the kernel sets count and
	 * stride to what it knows. Otherwise it writes the first count heaps it
	 * knows, each cut to the stride or zero-filled up to it, and sets count
	 * to the number written.
	 */
	struct drm_sgxfive_obj_array heaps;
};

/* Supported job types, as bits of &drm_sgxfive_dev_query_job_info.job_types. */
#define DRM_SGXFIVE_JOB_TYPE_TRANSFER_BIT	(1ULL << 0)

/**
 * struct drm_sgxfive_dev_query_job_info - Limits of SUBMIT_JOBS.
 */
struct drm_sgxfive_dev_query_job_info {
	/** @job_types: ``DRM_SGXFIVE_JOB_TYPE_*_BIT`` bits; 0 while jobs are not supported. */
	__u64 job_types;

	/** @max_jobs: Maximum jobs per SUBMIT_JOBS call. */
	__u32 max_jobs;

	/** @max_sync_ops: Maximum sync operations per job. */
	__u32 max_sync_ops;

	/** @transfer_cmd_size: Largest &struct drm_sgxfive_transfer_cmd known. */
	__u32 transfer_cmd_size;

	/**
	 * @max_updates: Maximum entries in &drm_sgxfive_job.updates. 0: memory
	 * updates are not supported and any update list is rejected.
	 */
	__u32 max_updates;
};

/**
 * struct drm_sgxfive_dev_query_runtime_info - Fixed rendering parameters.
 */
struct drm_sgxfive_dev_query_runtime_info {
	/** @num_pixel_partitions: Pixel partitions the kernel programs, 31. */
	__u32 num_pixel_partitions;

	/** @tile_width: Tile width in pixels, 32. */
	__u32 tile_width;

	/** @tile_height: Tile height in pixels, 16. */
	__u32 tile_height;

	/** @_padding_c: Reserved, zero. */
	__u32 _padding_c;
};

/**
 * struct drm_sgxfive_dev_query_args - Arguments for %DRM_IOCTL_SGXFIVE_DEV_QUERY.
 */
struct drm_sgxfive_dev_query_args {
	/** @type: &enum drm_sgxfive_dev_query. */
	__u32 type;

	/** @size: [IN] Size of the buffer at @pointer. [OUT] Bytes written or needed. */
	__u32 size;

	/** @pointer: User pointer to the output struct; NULL to query its size. */
	__u64 pointer;
};

/**
 * DOC: sgxfive CREATE_BO and GET_BO_MMAP_OFFSET
 *
 * Buffer objects are shmem GEM objects with pages below 4 GiB. GPU access
 * attributes are per mapping (VM_MAP), not per object. A BO can be mmap()ed,
 * write-combined, on the render node or through a dma-buf exported from it,
 * only if it was created with %DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS: for any other
 * BO GET_BO_MMAP_OFFSET fails with -EINVAL and mmap() of a dma-buf exported
 * from it with -EACCES. Handles are released with the core
 * %DRM_IOCTL_GEM_CLOSE.
 */
#define DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS		(1ULL << 0)
/* Reserved until the cache coherency of GPU page-table entries is known: -EINVAL. */
#define DRM_SGXFIVE_BO_CPU_CACHED		(1ULL << 1)
#define DRM_SGXFIVE_BO_FLAGS_MASK		DRM_SGXFIVE_BO_ALLOW_CPU_ACCESS

/**
 * struct drm_sgxfive_create_bo_args - Arguments for %DRM_IOCTL_SGXFIVE_CREATE_BO.
 */
struct drm_sgxfive_create_bo_args {
	/** @size: [IN/OUT] Size in bytes, non-zero; on return, rounded up to 4 KiB. */
	__u64 size;

	/** @handle: [OUT] GEM handle. */
	__u32 handle;

	/** @_padding_c: Reserved, zero. */
	__u32 _padding_c;

	/** @flags: [IN] ``DRM_SGXFIVE_BO_*`` bits within %DRM_SGXFIVE_BO_FLAGS_MASK. */
	__u64 flags;
};

/**
 * struct drm_sgxfive_get_bo_mmap_offset_args - Arguments for
 * %DRM_IOCTL_SGXFIVE_GET_BO_MMAP_OFFSET.
 */
struct drm_sgxfive_get_bo_mmap_offset_args {
	/** @handle: [IN] GEM handle of a CPU-accessible BO; -ENOENT if there is none. */
	__u32 handle;

	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;

	/** @offset: [OUT] Fake offset for mmap() on the render node. */
	__u64 offset;
};

/**
 * DOC: sgxfive VM contexts, VM_MAP and VM_UNMAP
 *
 * A VM context is one GPU address space (one SGX page directory). The client
 * manages it: VM_MAP places a range of a BO at an address of its choosing,
 * 4 KiB aligned and inside one heap that is not %DRM_SGXFIVE_HEAP_RESERVED.
 * Overlapping an existing mapping fails with -EEXIST. The mapping is in the
 * page tables when the ioctl returns. VM_UNMAP removes exactly one whole
 * mapping (-ENOENT otherwise); its pages stay pinned until the GPU can no
 * longer reach them. An open file has at most
 * %DRM_SGXFIVE_FILE_MAX_VM_CONTEXTS VM contexts at a time; CREATE_VM_CONTEXT
 * beyond that fails with -ENOSPC.
 */

/* VM contexts one open file may hold at a time. */
#define DRM_SGXFIVE_FILE_MAX_VM_CONTEXTS	32

/**
 * struct drm_sgxfive_create_vm_context_args - Arguments for
 * %DRM_IOCTL_SGXFIVE_CREATE_VM_CONTEXT.
 */
struct drm_sgxfive_create_vm_context_args {
	/** @handle: [OUT] VM context handle. */
	__u32 handle;

	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;
};

/**
 * struct drm_sgxfive_destroy_vm_context_args - Arguments for
 * %DRM_IOCTL_SGXFIVE_DESTROY_VM_CONTEXT.
 */
struct drm_sgxfive_destroy_vm_context_args {
	/** @handle: [IN] VM context handle. */
	__u32 handle;

	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;
};

/* The GPU may only read the mapping (SGX PTE READONLY); writes fault. */
#define DRM_SGXFIVE_VM_MAP_READ_ONLY		(1U << 0)
#define DRM_SGXFIVE_VM_MAP_FLAGS_MASK		DRM_SGXFIVE_VM_MAP_READ_ONLY

/**
 * struct drm_sgxfive_vm_map_args - Arguments for %DRM_IOCTL_SGXFIVE_VM_MAP.
 */
struct drm_sgxfive_vm_map_args {
	/** @vm_context_handle: [IN] VM context. */
	__u32 vm_context_handle;

	/** @flags: [IN] ``DRM_SGXFIVE_VM_MAP_*`` bits. */
	__u32 flags;

	/** @device_addr: [IN] GPU address: 4 KiB aligned, non-zero, in one user heap. */
	__u64 device_addr;

	/** @handle: [IN] GEM handle of the BO; -ENOENT if there is none. */
	__u32 handle;

	/** @_padding_14: Reserved, zero. */
	__u32 _padding_14;

	/** @offset: [IN] Offset into the BO, 4 KiB aligned. */
	__u64 offset;

	/** @size: [IN] Non-zero multiple of 4 KiB, within the BO and the heap. */
	__u64 size;
};

/**
 * struct drm_sgxfive_vm_unmap_args - Arguments for %DRM_IOCTL_SGXFIVE_VM_UNMAP.
 */
struct drm_sgxfive_vm_unmap_args {
	/** @vm_context_handle: [IN] VM context. */
	__u32 vm_context_handle;

	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;

	/** @device_addr: [IN] Start of the mapping. */
	__u64 device_addr;

	/** @size: [IN] Size of the mapping; must match it exactly. */
	__u64 size;
};

#if defined(__cplusplus)
}
#endif

#endif /* SGXFIVE_DRM_UAPI_H */
