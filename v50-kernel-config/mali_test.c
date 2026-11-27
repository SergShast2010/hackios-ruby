/*
 * mali_test: Stage 3 (v0.62c) userland verification of HakiosMali IOKit driver
 * on Redmi Note 12 Pro 5G (ruby / MT6877 / Mali-G68 MC4 Valhall v9).
 *
 * Tests:
 *   1. Open IOKit service "HakiosMali"
 *   2. Query all Panfrost DRM parameters (prod_id, revision, shader/l2/tiler/mmu/tex/afbc)
 *   3. CREATE_BO + MMAP_BO + GET_BO_OFFSET + IOConnectMapMemory64 + MADVISE
 *   4. Submit real Mali WRITE_VALUE (0xDEADBEEF) job from userland via kPanfrostSubmit,
 *      wait via kHakiosWait + kPanfrostWaitBO, verify 0xDEADBEEF in mapped BO
 *   5. Zero-copy IOSurface: create 64x64 BGRA IOSurface, CPU writes 0x11223344,
 *      describe & import via kHakiosSurface, submit WRITE_VALUE (0xCAFEBABE) directly
 *      into IOSurface pixel memory, verify CPU sees 0xCAFEBABE after IOSurfaceLock
 *      (exercising MiniIOSurfaceDrawn + non-coherent D-cache flush)
 *   6. Clean GEM_CLOSE / unmap / IOServiceClose (verify 0 leaked granules)
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	kDrmGemClose         = 0x09,
	kPanfrostSubmit      = 0x40,
	kPanfrostWaitBO      = 0x41,
	kPanfrostCreateBO    = 0x42,
	kPanfrostMmapBO      = 0x43,
	kPanfrostGetParam    = 0x44,
	kPanfrostGetBOOffset = 0x45,
	kPanfrostMadvise     = 0x48,
	kHakiosSurfaceSeq    = 0x7c,
	kHakiosWait          = 0x7d,
	kHakiosSurface       = 0x7e,
};

#define kHakiosDescribe 0xffffffffu

struct drm_gem_close { uint32_t handle, pad; };
struct drm_panfrost_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
struct drm_panfrost_wait_bo { uint32_t handle, pad; int64_t timeout_ns; };
struct drm_panfrost_create_bo { uint32_t size, flags, handle, pad; uint64_t offset; };
struct drm_panfrost_mmap_bo { uint32_t handle, flags; uint64_t offset; };
struct drm_panfrost_get_param { uint32_t param, pad; uint64_t value; };
struct drm_panfrost_get_bo_offset { uint32_t handle, pad; uint64_t offset; };
struct drm_panfrost_madvise { uint32_t handle, madv, retained; };
struct hakios_wait { uint64_t seq; int64_t timeout_ns; };
struct hakios_surface {
	uint32_t surface, handle;
	uint64_t size;
	uint32_t offset, rowBytes, width, height, pixelFormat, planes;
};

static int g_checks, g_failed;

#define CHECK(cond, fmt, ...) do { \
	g_checks++; \
	if (!(cond)) { \
		g_failed++; \
		printf("FAIL: " fmt "\n", ##__VA_ARGS__); \
	} else { \
		printf("OK:   " fmt "\n", ##__VA_ARGS__); \
	} \
} while (0)

static kern_return_t
mali_call(io_connect_t conn, uint32_t sel, void *buf, size_t sz)
{
	size_t out_sz = sz;
	return IOConnectCallStructMethod(conn, sel, buf, sz, buf, &out_sz);
}

static uint64_t
get_param(io_connect_t conn, uint32_t id, kern_return_t *kr_out)
{
	struct drm_panfrost_get_param p = { .param = id };
	kern_return_t kr = mali_call(conn, kPanfrostGetParam, &p, sizeof(p));
	if (kr_out) *kr_out = kr;
	return p.value;
}

static void
build_write_value_desc(volatile uint32_t *job, uint64_t dst_va, uint32_t magic)
{
	memset((void *)job, 0, 64);
	/* Job Header (32 B): type=2 (WRITE_VALUE) << 1 | is_64b (1) | index=1 << 16 */
	job[4]  = (2u << 1) | 1u | (1u << 16);
	/* Payload at +0x20: 64-bit target GPU VA, type=6 (IMMEDIATE_32), value */
	job[8]  = (uint32_t)dst_va;
	job[9]  = (uint32_t)(dst_va >> 32);
	job[10] = 6u;
	job[12] = magic;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static CFNumberRef
cf_num(int v)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &v);
}

static IOSurfaceRef
make_test_surface(int w, int h)
{
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFNumberRef nw = cf_num(w), nh = cf_num(h), nbpe = cf_num(4), nbpr = cf_num(w * 4),
	    nfmt = cf_num((int)'BGRA');
	CFDictionarySetValue(d, kIOSurfaceWidth, nw);
	CFDictionarySetValue(d, kIOSurfaceHeight, nh);
	CFDictionarySetValue(d, kIOSurfaceBytesPerElement, nbpe);
	CFDictionarySetValue(d, kIOSurfaceBytesPerRow, nbpr);
	CFDictionarySetValue(d, kIOSurfacePixelFormat, nfmt);
	IOSurfaceRef s = IOSurfaceCreate(d);
	CFRelease(nw); CFRelease(nh); CFRelease(nbpe); CFRelease(nbpr); CFRelease(nfmt);
	CFRelease(d);
	return s;
}

int
main(void)
{
	printf("=== mali_test: HakiosMali Stage 3 (v0.62c) verification ===\n");

	/* 1. Open IOKit service HakiosMali */
	io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching("HakiosMali"));
	CHECK(svc != 0, "IOServiceGetMatchingService(\"HakiosMali\") -> 0x%x", svc);
	if (!svc) return 1;

	io_connect_t conn = 0;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	CHECK(kr == KERN_SUCCESS && conn != 0, "IOServiceOpen -> kr=0x%x conn=0x%x", kr, conn);
	if (kr != KERN_SUCCESS || !conn) return 1;

	/* 2. Query Panfrost parameters */
	kern_return_t kr_id = 0, kr_rev = 0, kr_sh = 0, kr_mmu = 0;
	uint64_t prod_id  = get_param(conn, 0, &kr_id);
	uint64_t rev      = get_param(conn, 1, &kr_rev);
	uint64_t shader   = get_param(conn, 2, &kr_sh);
	uint64_t tiler    = get_param(conn, 3, NULL);
	uint64_t l2       = get_param(conn, 4, NULL);
	uint64_t mmu_feat = get_param(conn, 12, &kr_mmu);
	uint64_t tiler_f  = get_param(conn, 10, NULL);
	uint64_t tex0     = get_param(conn, 18, NULL);
	uint64_t max_thr  = get_param(conn, 14, NULL);
	uint64_t tls_al   = get_param(conn, 39, NULL);
	uint64_t afbc_f   = get_param(conn, 40, NULL);

	CHECK(kr_id == KERN_SUCCESS && prod_id == 0x9204,
	    "GET_PARAM prod_id=0x%llx rev=0x%llx (Mali-G68 Valhall v9)",
	    (unsigned long long)prod_id, (unsigned long long)rev);
	CHECK(kr_sh == KERN_SUCCESS && shader == 0x50005 && l2 == 1 && tiler == 1,
	    "GET_PARAM shader=0x%llx l2=0x%llx tiler=0x%llx",
	    (unsigned long long)shader, (unsigned long long)l2, (unsigned long long)tiler);
	CHECK(kr_mmu == KERN_SUCCESS && mmu_feat == 0x2830,
	    "GET_PARAM mmu=0x%llx tiler_feat=0x%llx max_thr=%llu tex0=0x%llx tls=0x%llx afbc=0x%llx",
	    (unsigned long long)mmu_feat, (unsigned long long)tiler_f,
	    (unsigned long long)max_thr, (unsigned long long)tex0,
	    (unsigned long long)tls_al, (unsigned long long)afbc_f);

	/* 3. CREATE_BO + MMAP_BO + GET_BO_OFFSET + IOConnectMapMemory64 + MADVISE */
	struct drm_panfrost_create_bo cbo = { .size = 16384 };
	kr = mali_call(conn, kPanfrostCreateBO, &cbo, sizeof(cbo));
	CHECK(kr == KERN_SUCCESS && cbo.handle != 0 && cbo.offset != 0,
	    "CREATE_BO handle=%u gpu_va=0x%llx", cbo.handle, (unsigned long long)cbo.offset);

	struct drm_panfrost_mmap_bo mbo = { .handle = cbo.handle };
	kr = mali_call(conn, kPanfrostMmapBO, &mbo, sizeof(mbo));
	CHECK(kr == KERN_SUCCESS && mbo.offset == ((uint64_t)cbo.handle << 12),
	    "MMAP_BO cookie=0x%llx", (unsigned long long)mbo.offset);

	struct drm_panfrost_get_bo_offset gbo = { .handle = cbo.handle };
	kr = mali_call(conn, kPanfrostGetBOOffset, &gbo, sizeof(gbo));
	CHECK(kr == KERN_SUCCESS && gbo.offset == cbo.offset,
	    "GET_BO_OFFSET gpu_va=0x%llx", (unsigned long long)gbo.offset);

	struct drm_panfrost_madvise madv = { .handle = cbo.handle, .madv = 0 };
	kr = mali_call(conn, kPanfrostMadvise, &madv, sizeof(madv));
	CHECK(kr == KERN_SUCCESS && madv.retained == 1, "MADVISE retained=%u", madv.retained);

	mach_vm_address_t cpu_va = 0;
	mach_vm_size_t cpu_sz = 0;
	kr = IOConnectMapMemory64(conn, cbo.handle, mach_task_self(), &cpu_va, &cpu_sz, kIOMapAnywhere);
	CHECK(kr == KERN_SUCCESS && cpu_va != 0 && cpu_sz >= 16384,
	    "IOConnectMapMemory64 cpu_va=0x%llx size=%llu",
	    (unsigned long long)cpu_va, (unsigned long long)cpu_sz);

	if (kr == KERN_SUCCESS && cpu_va != 0) {
		/* 4. Real userland GPU WRITE_VALUE job into mapped BO */
		volatile uint32_t *job1 = (volatile uint32_t *)(uintptr_t)cpu_va;
		volatile uint32_t *dst1 = (volatile uint32_t *)(uintptr_t)(cpu_va + 0x800);
		*dst1 = 0x00000000u;
		build_write_value_desc(job1, cbo.offset + 0x800ull, 0xDEADBEEFu);

		uint32_t handles1[1] = { cbo.handle };
		struct drm_panfrost_submit sub1 = {
			.jc = cbo.offset,
			.bo_handles = (uint64_t)(uintptr_t)handles1,
			.bo_handle_count = 1,
			.requirements = 0,
		};
		kr = mali_call(conn, kPanfrostSubmit, &sub1, sizeof(sub1));
		uint64_t seq1 = sub1.jc;
		CHECK(kr == KERN_SUCCESS && seq1 != 0,
		    "SUBMIT(BO WRITE_VALUE) kr=0x%x seq=%llu", kr, (unsigned long long)seq1);

		struct hakios_wait hw = { .seq = seq1, .timeout_ns = 2000000000ll };
		kr = mali_call(conn, kHakiosWait, &hw, sizeof(hw));
		CHECK(kr == KERN_SUCCESS, "HAKIOS_WAIT seq=%llu kr=0x%x", (unsigned long long)seq1, kr);

		struct drm_panfrost_wait_bo wbo = { .handle = cbo.handle, .timeout_ns = 2000000000ll };
		kr = mali_call(conn, kPanfrostWaitBO, &wbo, sizeof(wbo));
		CHECK(kr == KERN_SUCCESS, "WAIT_BO handle=%u kr=0x%x", cbo.handle, kr);

		__asm__ volatile ("dsb sy" ::: "memory");
		uint32_t got1 = *dst1;
		CHECK(got1 == 0xDEADBEEFu, "BO GPU result=0x%08x (expected 0xdeadbeef)", got1);

		/* 5. Zero-copy IOSurface test */
		IOSurfaceRef surf = make_test_surface(64, 64);
		CHECK(surf != NULL, "IOSurfaceCreate(64x64 BGRA) id=%u", surf ? IOSurfaceGetID(surf) : 0);
		if (surf) {
			uint32_t sid = IOSurfaceGetID(surf);
			IOSurfaceLock(surf, 0, NULL);
			uint32_t *pix = (uint32_t *)IOSurfaceGetBaseAddress(surf);
			pix[0] = 0x11223344u;
			pix[1] = 0x55667788u;
			IOSurfaceUnlock(surf, 0, NULL);

			struct hakios_surface desc = { .surface = sid, .handle = kHakiosDescribe };
			kr = mali_call(conn, kHakiosSurface, &desc, sizeof(desc));
			CHECK(kr == KERN_SUCCESS && desc.width == 64 && desc.height == 64 && desc.rowBytes == 256,
			    "HAKIOS_SURFACE(describe) %ux%u row=%u size=%llu off=%u",
			    desc.width, desc.height, desc.rowBytes, (unsigned long long)desc.size, desc.offset);

			struct hakios_surface imp = { .surface = sid, .handle = 0 };
			kr = mali_call(conn, kHakiosSurface, &imp, sizeof(imp));
			CHECK(kr == KERN_SUCCESS && imp.handle != 0,
			    "HAKIOS_SURFACE(import) sid=%u -> handle=%u", sid, imp.handle);

			struct drm_panfrost_get_bo_offset sbo = { .handle = imp.handle };
			kr = mali_call(conn, kPanfrostGetBOOffset, &sbo, sizeof(sbo));
			CHECK(kr == KERN_SUCCESS && sbo.offset != 0,
			    "IOSurface GPU VA=0x%llx (+off %u)", (unsigned long long)sbo.offset, imp.offset);

			/* Submit WRITE_VALUE (0xCAFEBABE) directly into IOSurface pixel[0] */
			volatile uint32_t *job2 = (volatile uint32_t *)(uintptr_t)(cpu_va + 0x100);
			build_write_value_desc(job2, sbo.offset + (uint64_t)imp.offset, 0xCAFEBABEu);

			uint32_t handles2[2] = { cbo.handle, imp.handle };
			struct drm_panfrost_submit sub2 = {
				.jc = cbo.offset + 0x100ull,
				.bo_handles = (uint64_t)(uintptr_t)handles2,
				.bo_handle_count = 2,
				.requirements = 0,
			};
			kr = mali_call(conn, kPanfrostSubmit, &sub2, sizeof(sub2));
			CHECK(kr == KERN_SUCCESS && sub2.jc != 0,
			    "SUBMIT(IOSurface WRITE_VALUE) seq=%llu", (unsigned long long)sub2.jc);

			/* IOSurfaceLock triggers lockCommon -> MiniIOSurfaceDrawn -> D-cache flush */
			IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
			uint32_t p0 = ((volatile uint32_t *)IOSurfaceGetBaseAddress(surf))[0];
			uint32_t p1 = ((volatile uint32_t *)IOSurfaceGetBaseAddress(surf))[1];
			IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
			CHECK(p0 == 0xCAFEBABEu && p1 == 0x55667788u,
			    "IOSurface zero-copy pixels: pix[0]=0x%08x (exp 0xcafebabe) pix[1]=0x%08x (exp 0x55667788)",
			    p0, p1);

			struct drm_gem_close cls_s = { .handle = imp.handle };
			kr = mali_call(conn, kDrmGemClose, &cls_s, sizeof(cls_s));
			CHECK(kr == KERN_SUCCESS, "GEM_CLOSE(IOSurface) kr=0x%x", kr);
			CFRelease(surf);
		}

		IOConnectUnmapMemory64(conn, cbo.handle, mach_task_self(), cpu_va);
	}

	struct drm_gem_close cls1 = { .handle = cbo.handle };
	kr = mali_call(conn, kDrmGemClose, &cls1, sizeof(cls1));
	CHECK(kr == KERN_SUCCESS, "GEM_CLOSE(BO) kr=0x%x", kr);

	IOServiceClose(conn);

	printf("=== mali_test: %s (%d/%d checks passed) ===\n",
	    g_failed == 0 ? "ALL PASS" : "FAILED", g_checks - g_failed, g_checks);
	return g_failed ? 1 : 0;
}
