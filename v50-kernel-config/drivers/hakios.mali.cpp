/*
 * HakiosMali: IOKit service & IOUserClient exposing MediaTek MT6877 (ruby)
 * Arm Mali-G68 MC4 (Valhall v9, Job Manager) to Darwin userland (Mesa Panfrost / Metal).
 *
 * Adapted from oosexgang/xnu-kernel-mtk-helio (HakiosMali.cpp) for XNU 8796 + MT6877:
 * - Speaks Linux Panfrost DRM protocol over IOConnectCallStructMethod selectors:
 *     0x09 GEM_CLOSE, 0x40 SUBMIT, 0x41 WAIT_BO, 0x42 CREATE_BO, 0x43 MMAP_BO,
 *     0x44 GET_PARAM, 0x45 GET_BO_OFFSET, 0x48 MADVISE,
 *     0x7c HAKIOS_SURFACE_SEQ, 0x7d HAKIOS_WAIT, 0x7e HAKIOS_SURFACE, 0x7f HAKIOS_SHOW
 * - Zero-copy IOSurface import via IOSubMemoryDescriptor + 4 KiB Mali MMU mapping.
 * - Explicit D-cache clean+invalidate (FlushPoC_DcacheRegion) around non-coherent
 *   Mali-G68 DMA on cacheable IOSurface memory (ruby coh_en=0).
 * - Display & IOSurfaceLock synchronization via MiniIOSurfaceDrawn().
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" {
/* bsd/dev/hakios.mtk_mfg_mt6877.c */
bool mtk_mali_ready(void);
bool mtk_mali_parameter(uint32_t which, uint64_t * value);
uint64_t mtk_mali_address(uint64_t size);
bool mtk_mali_map(uint64_t gpu, uint64_t phys);
void mtk_mali_mapped(void);
void mtk_mali_unmap(uint64_t gpu, uint64_t size);
void mtk_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset);
uint32_t mtk_mali_submit(uint64_t chain, bool fragment);
uint64_t mtk_mali_enqueue(uint64_t chain, bool fragment);
bool mtk_mali_wait(uint64_t seq, uint64_t timeout_ns);
void FlushPoC_DcacheRegion(vm_offset_t va, unsigned length);

void HakiosMaliStart(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);

/* Telemetry exported to sysctl kern.hakios.mali in hakios.mtk_mfg_mt6877.c */
uint32_t gMaliServiceStarted;
uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown;
uint64_t gMaliLiveMade, gMaliLiveSurfaces, gMaliLiveLate;

struct mali_made { uint64_t address, size; uint32_t asked, flags; };
struct mali_made gMaliMade[32];
}

/* Linux include/uapi/drm/panfrost_drm.h & drm.h request selectors and structs */
enum {
	kDrmGemClose            = 0x09,
	kPanfrostSubmit         = 0x40,
	kPanfrostWaitBO         = 0x41,
	kPanfrostCreateBO       = 0x42,
	kPanfrostMmapBO         = 0x43,
	kPanfrostGetParam       = 0x44,
	kPanfrostGetBOOffset    = 0x45,
	kPanfrostMadvise        = 0x48,
	kHakiosSurfaceSeq       = 0x7c,         /* surface's last job seq or ~0 (pending) */
	kHakiosWait             = 0x7d,         /* wait for job seq within timeout_ns */
	kHakiosSurface          = 0x7e,         /* zero-copy IOSurface -> GPU BO */
	kHakiosShow             = 0x7f,         /* test pixels directly to display panel */
};

struct hakios_surface {
	uint32_t surface;                       /* IOSurface ID */
	uint32_t handle;                        /* kHakiosDescribe: describe only; else out: BO handle */
	uint64_t size;                          /* total mapped size (page-rounded) */
	uint32_t offset, rowBytes;              /* pixel start offset and stride */
	uint32_t width, height, pixelFormat, planes;
};
#define kHakiosDescribe        0xffffffffu

struct hakios_wait {
	uint64_t seq;
	int64_t timeout_ns;
};

struct hakios_surface_seq {
	uint32_t surface, pad;
	uint64_t seq;
};
#define HAKIOS_SEQ_PENDING     (~0ull)

struct hakios_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};

struct drm_gem_close {
	uint32_t handle, pad;
};

struct drm_panfrost_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
#define PANFROST_JD_REQ_FS      (1u << 0)

struct drm_panfrost_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns;
};

struct drm_panfrost_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};

struct drm_panfrost_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};

struct drm_panfrost_get_param {
	uint32_t param, pad;
	uint64_t value;
};

struct drm_panfrost_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};

struct drm_panfrost_madvise {
	uint32_t handle, madv, retained;
};

#define MALI_BUFFERS            4096
#define MALI_BUFFER_MAX         (256ull << 20)

struct mali_buffer {
	IOMemoryDescriptor * memory;            /* allocated BO or IOSubMemoryDescriptor over IOSurface */
	MiniIOSurface * surface;
	uint64_t address, size;
	uint64_t last_seq;                      /* last job seq referencing this BO */
};

static IOLock * gMaliLock;

static inline void
maliFlushSurfacePixels(MiniIOSurface * surface)
{
	if (surface != NULL && surface->kbase != NULL && surface->info.allocSize != 0) {
		uint64_t bytes = round_page_64(surface->info.allocSize);
		if (bytes <= MALI_BUFFER_MAX) {
			FlushPoC_DcacheRegion((vm_offset_t)(surface->kbase + PAGE_SIZE), (unsigned)bytes);
		}
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HakiosMali : public IOService
{
	OSDeclareDefaultStructors(HakiosMali);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosMali, IOService);

bool
HakiosMali::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HakiosMaliUserClient");
	setName("HakiosMali");
	registerService();
	return true;
}

void
HakiosMaliStart(void)
{
	IOService * provider = IOService::getPlatform();
	HakiosMali * gpu;

	if (gMaliServiceStarted) {
		return;
	}
	gMaliLock = IOLockAlloc();
	gpu = OSTypeAlloc(HakiosMali);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("HakiosMali: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		gpu->release();
		IOLog("HakiosMali: the service did not start\n");
		return;
	}
	gpu->release();
	gMaliServiceStarted = 1;
	IOLog("HakiosMali: service registered (Mali-G68 MC4 ready for userland)\n");
}

/*
 * Wait up to `milliseconds` for the Mali GPU to finish drawing into `surface`.
 * Called by MiniIOMobileFramebuffer before scanning out a layer and by
 * MiniIOSurface::lockCommon() before CPU reads of GPU-rendered surfaces.
 */
bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	if (surface == NULL) {
		return true;
	}
	for (uint64_t waited = 0; surface->maliSeq == HAKIOS_SEQ_PENDING && waited < milliseconds * 1000; waited += 200) {
		delay(200);
	}
	uint64_t seq = surface->maliSeq;
	if (seq == HAKIOS_SEQ_PENDING) {
		return false;
	}
	if (seq != 0) {
		if (!mtk_mali_wait(seq, milliseconds * NSEC_PER_MSEC)) {
			return false;
		}
		/* Non-coherent GPU (coh_en=0): invalidate CPU cache so display/CPU sees fresh GPU pixels */
		maliFlushSurfacePixels(surface);
		if (surface->maliSeq == seq) {
			surface->maliSeq = 0;
		}
	}
	return true;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HakiosMaliUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosMaliUserClient);

	struct mali_buffer *    fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextBuffer;
	uint32_t                fSubmits, fFailed, fRequests;

	struct mali_buffer * buffer(uint32_t handle);
	void freeBuffer(struct mali_buffer * b);
	IOReturn createBO(struct drm_panfrost_create_bo * request);
	struct mali_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle);
	IOReturn surfaceBuffer(struct hakios_surface * request);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosMaliUserClient, IOUserClient);

bool
HakiosMaliUserClient::start(IOService * provider)
{
	if (!mtk_mali_ready() || !IOUserClient::start(provider)) {
		IOLog("HakiosMali: a client could not be started\n");
		return false;
	}
	IOLog("HakiosMali: a client opened the GPU\n");
	OSIncrementAtomic((SInt32 *) &gMaliClients);
	fBuffers = IONewZero(struct mali_buffer, MALI_BUFFERS);
	return fBuffers != NULL;
}

struct mali_buffer *
HakiosMaliUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > MALI_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/* Deferred release list for buffers closed while a queued GPU job still references them */
#define MALI_LATE 512
static struct mali_buffer gMaliLate[MALI_LATE];
static uint32_t gMaliLateCount, gMaliLateMost;

static void
maliRelease(struct mali_buffer * b)
{
	if (b->surface != NULL) {
		OSAddAtomic64(-(SInt64) b->size, (volatile SInt64 *) &gMaliLiveSurfaces);
	} else {
		OSAddAtomic64(-(SInt64) b->size, (volatile SInt64 *) &gMaliLiveMade);
	}
	mtk_mali_unmap(b->address, b->size);
	b->memory->release();
	if (b->surface != NULL) {
		MiniIOSurfaceDrop(b->surface);
	}
	bzero(b, sizeof(*b));
}

static void
maliReapLate(bool wait)
{
	for (uint32_t i = 0; i < gMaliLateCount;) {
		if (mtk_mali_wait(gMaliLate[i].last_seq, wait && i == 0 ? 10ull * NSEC_PER_SEC : 0)) {
			OSAddAtomic64(-(SInt64) gMaliLate[i].size, (volatile SInt64 *) &gMaliLiveLate);
			maliRelease(&gMaliLate[i]);
			gMaliLate[i] = gMaliLate[--gMaliLateCount];
			bzero(&gMaliLate[gMaliLateCount], sizeof(gMaliLate[0]));
			wait = false;
		} else {
			i++;
		}
	}
}

void
HakiosMaliUserClient::freeBuffer(struct mali_buffer * b)
{
	maliReapLate(false);
	if (b->last_seq != 0 && !mtk_mali_wait(b->last_seq, 0)) {
		if (gMaliLateCount == MALI_LATE) {
			maliReapLate(true);
		}
		if (gMaliLateCount < MALI_LATE) {
			gMaliLate[gMaliLateCount++] = *b;
			OSAddAtomic64((SInt64) b->size, (volatile SInt64 *) &gMaliLiveLate);
			if (gMaliLateCount > gMaliLateMost) {
				gMaliLateMost = gMaliLateCount;
				if (gMaliLateMost % 64 == 1) {
					IOLog("HakiosMali: %u buffers deferred waiting for GPU jobs\n", gMaliLateMost);
				}
			}
			bzero(b, sizeof(*b));
			return;
		}
		(void)mtk_mali_wait(b->last_seq, 10ull * NSEC_PER_SEC);
	}
	maliRelease(b);
}

struct mali_buffer *
HakiosMaliUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t * handle)
{
	struct mali_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < MALI_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % MALI_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL) {
		IOLog("HakiosMali: no handles left\n");
		return NULL;
	}
	b->address = mtk_mali_address(size);
	/* Map every 4 KiB Mali page (4 per 16 KiB XNU page) into AS0 */
	for (uint64_t offset = 0; offset < size && mapped && b->address != 0; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && mtk_mali_map(b->address + offset, phys);
	}
	if (b->address == 0 || !mapped) {
		if (b->address != 0) {
			mtk_mali_unmap(b->address, size);
		}
		bzero(b, sizeof(*b));
		IOLog("HakiosMali: buffer of %llu bytes cannot be mapped into GPU MMU\n", size);
		return NULL;
	}
	mtk_mali_mapped();
	if (gMaliBuffers < 32) {
		gMaliMade[gMaliBuffers] = (struct mali_made) { b->address, size, (uint32_t) size, 0 };
	}
	OSIncrementAtomic((SInt32 *) &gMaliBuffers);
	b->size = size;
	return b;
}

IOReturn
HakiosMaliUserClient::createBO(struct drm_panfrost_create_bo * request)
{
	uint64_t size = round_page_64(request->size);
	IOBufferMemoryDescriptor * memory;
	struct mali_buffer * b;

	if (size == 0 || size > MALI_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(NULL, kIODirectionInOut | kIOMemoryPageable |
	    kIOMapWriteCombineCache, (vm_size_t) size, PAGE_SIZE);
	if (memory != NULL && memory->prepare() != kIOReturnSuccess) {
		memory->release();
		memory = NULL;
	}
	if (memory == NULL) {
		static unsigned said;

		if (said++ < 8) {
			IOLog("HakiosMali: no memory for a buffer of %llu bytes\n", size);
		}
		return kIOReturnNoMemory;
	}
	/* Push freshly zeroed lines out of CPU cache before WC / GPU use */
	{
		IOMemoryMap * passing = memory->map(kIOMapAnywhere | kIOMapDefaultCache);

		if (passing != NULL) {
			FlushPoC_DcacheRegion((vm_offset_t) passing->getVirtualAddress(), (unsigned) size);
			passing->release();
		}
	}
	b = newBuffer(memory, 0, size, &request->handle);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	OSAddAtomic64((SInt64) size, (volatile SInt64 *) &gMaliLiveMade);
	request->offset = b->address;
	mtk_mali_tag(b->address, 1, 0, 0, 0, 0, 0);
	return kIOReturnSuccess;
}

IOReturn
HakiosMaliUserClient::surfaceBuffer(struct hakios_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct mali_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size;

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > MALI_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kHakiosDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	for (uint32_t i = 0; i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	maliFlushSurfacePixels(s);
	b = newBuffer(s->memory, PAGE_SIZE, size, &request->handle);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	b->surface = s;
	OSAddAtomic64((SInt64) size, (volatile SInt64 *) &gMaliLiveSurfaces);
	mtk_mali_tag(b->address, 2, s->info.width, s->info.height, s->info.bytesPerRow, s->info.pixelFormat,
	    (uint32_t) s->info.offset);
	return kIOReturnSuccess;
}

IOReturn
HakiosMaliUserClient::clientClose(void)
{
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; fBuffers != NULL && i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL) {
			freeBuffer(&fBuffers[i]);
		}
	}
	IOLockUnlock(gMaliLock);
	if (fSubmits != 0) {
		IOLog("HakiosMali: client closed after %u job chains (%u failed)\n", fSubmits, fFailed);
	}
	terminate();
	return kIOReturnSuccess;
}

void
HakiosMaliUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct mali_buffer, MALI_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

IOReturn
HakiosMaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct mali_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gMaliLock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gMaliLock);
	return ret;
}

IOReturn
HakiosMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct drm_gem_close close;
		struct drm_panfrost_submit submit;
		struct drm_panfrost_wait_bo wait;
		struct drm_panfrost_create_bo create;
		struct drm_panfrost_mmap_bo mmap;
		struct drm_panfrost_get_param param;
		struct drm_panfrost_get_bo_offset offset;
		struct drm_panfrost_madvise madvise;
		struct hakios_show show;
		struct hakios_surface surface;
		struct hakios_wait waitseq;
		struct hakios_surface_seq sseq;
	} r;
	size_t size = args->structureInputSize;
	struct mali_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!mtk_mali_ready()) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	/* Non-blocking wait paths (do not hold gMaliLock across sleep) */
	if (selector == kHakiosWait || selector == kPanfrostWaitBO) {
		uint64_t seq = r.waitseq.seq;
		int64_t timeout = r.waitseq.timeout_ns;
		MiniIOSurface * surf = NULL;

		if (selector == kPanfrostWaitBO) {
			IOLockLock(gMaliLock);
			b = buffer(r.wait.handle);
			seq = b != NULL ? b->last_seq : 0;
			if (b != NULL && b->surface != NULL) {
				surf = MiniIOSurfaceCopy(b->surface->sid);
			}
			IOLockUnlock(gMaliLock);
			timeout = r.wait.timeout_ns;
			if (b == NULL) {
				return kIOReturnNotFound;
			}
		}
		bool done = (seq == 0) || mtk_mali_wait(seq, timeout > 0 ? (uint64_t) timeout : 0);
		if (done && surf != NULL) {
			maliFlushSurfacePixels(surf);
		}
		if (surf != NULL) {
			MiniIOSurfaceDrop(surf);
		}
		ret = done ? kIOReturnSuccess : kIOReturnTimeout;
		if (args->structureOutput != NULL && args->structureOutputSize >= size) {
			memcpy(args->structureOutput, &r, size);
			args->structureOutputSize = (uint32_t) size;
		}
		return ret;
	}

	IOLockLock(gMaliLock);
	if (fRequests++ < 32) {
		IOLog("HakiosMali: request %#x (%zu bytes)\n", selector, size);
	}
	switch (selector) {
	case kPanfrostGetParam:
		if (!mtk_mali_parameter(r.param.param, &r.param.value)) {
			ret = kIOReturnBadArgument;
		}
		break;
	case kPanfrostCreateBO:
		ret = createBO(&r.create);
		break;
	case kPanfrostMmapBO:
		ret = buffer(r.mmap.handle) != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.mmap.offset = (uint64_t) r.mmap.handle << 12;
		break;
	case kPanfrostGetBOOffset:
		b = buffer(r.offset.handle);
		ret = b != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.offset.offset = b != NULL ? b->address : 0;
		break;
	case kPanfrostSubmit: {
		uint64_t seq = 0;

		/* Flush CPU caches of any referenced IOSurfaces BEFORE enqueueing to non-coherent GPU */
		if (r.submit.jc != 0 && r.submit.bo_handles != 0) {
			for (uint32_t i = 0; i < r.submit.bo_handle_count && i < 4096; i += 64) {
				uint32_t handles[64];
				uint32_t n = r.submit.bo_handle_count - i < 64 ? r.submit.bo_handle_count - i : 64;

				if (copyin((user_addr_t) (r.submit.bo_handles + i * sizeof(uint32_t)),
				    handles, n * sizeof(uint32_t)) != 0) {
					break;
				}
				for (uint32_t k = 0; k < n; k++) {
					b = buffer(handles[k]);
					if (b != NULL && b->surface != NULL) {
						maliFlushSurfacePixels(b->surface);
					}
				}
			}
		}

		fSubmits++;
		gMaliSubmits++;
		if (r.submit.jc != 0) {
			seq = mtk_mali_enqueue(r.submit.jc, (r.submit.requirements & PANFROST_JD_REQ_FS) != 0);
		}
		if (seq != 0 && r.submit.bo_handles != 0) {
			for (uint32_t i = 0; i < r.submit.bo_handle_count && i < 4096; i += 64) {
				uint32_t handles[64];
				uint32_t n = r.submit.bo_handle_count - i < 64 ? r.submit.bo_handle_count - i : 64;

				if (copyin((user_addr_t) (r.submit.bo_handles + i * sizeof(uint32_t)),
				    handles, n * sizeof(uint32_t)) != 0) {
					break;
				}
				for (uint32_t k = 0; k < n; k++) {
					b = buffer(handles[k]);
					if (b != NULL) {
						b->last_seq = seq;
						if (b->surface != NULL) {
							b->surface->maliSeq = seq;
						}
					}
				}
			}
		}
		r.submit.jc = seq;
		break;
	}
	case kHakiosSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kHakiosSurfaceSeq: {
		MiniIOSurface * surface = MiniIOSurfaceCopy(r.sseq.surface);

		if (surface != NULL) {
			static uint32_t overDisplayed;

			if (r.sseq.seq == HAKIOS_SEQ_PENDING && surface->useCount[0] != 0 && ++overDisplayed <= 8) {
				IOLog("HakiosMali: surface %u drawn into while display holds it (%u so far)\n",
				    r.sseq.surface, overDisplayed);
			}
			surface->maliSeq = r.sseq.seq;
			wakeup((caddr_t) &surface->maliSeq);
			MiniIOSurfaceDrop(surface);
		} else {
			ret = kIOReturnNotFound;
		}
		break;
	}
	case kHakiosShow: {
		struct hakios_show * show = (struct hakios_show *) &r;
		uint64_t bytes = (uint64_t) show->rowBytes * show->height;
		uint8_t * pixels;

		if (show->width == 0 || show->width > 4096 || show->height == 0 || show->height > 4096 ||
		    show->rowBytes < 4 * show->width || (pixels = (uint8_t *) IOMallocData(bytes)) == NULL) {
			ret = kIOReturnBadArgument;
			break;
		}
		if (copyin((user_addr_t) show->pixels, pixels, bytes) != 0) {
			ret = kIOReturnVMError;
		} else {
			MiniIOMobileFramebufferShow(pixels, show->width, show->height, show->rowBytes);
			gMaliShown++;
		}
		IOFreeData(pixels, bytes);
		break;
	}
	case kPanfrostMadvise:
		r.madvise.retained = 1;
		break;
	case kDrmGemClose:
		b = buffer(r.close.handle);
		if (b != NULL) {
			freeBuffer(b);
		} else {
			ret = kIOReturnNotFound;
		}
		break;
	default:
		IOLog("HakiosMali: request %#x (%zu bytes) unsupported\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(gMaliLock);

	if (args->structureOutput != NULL && args->structureOutputSize >= size) {
		memcpy(args->structureOutput, &r, size);
		args->structureOutputSize = (uint32_t) size;
	}
	return ret;
}
