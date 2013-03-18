/*
 * Copyright © 2013 Siarhei Siamashka <siarhei.siamashka@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pixman.h>

#include "xorgVersion.h"
#include "xf86_OSproc.h"
#include "xf86.h"
#include "xf86drm.h"
#include "dri2.h"
#include "damage.h"
#include "fb.h"

#include "fbdev_priv.h"
#include "sunxi_disp.h"
#include "sunxi_x_g2d.h"

/*
 * The code below is borrowed from "xserver/fb/fbwindow.c"
 */

static void
xCopyWindowProc(DrawablePtr pSrcDrawable,
                 DrawablePtr pDstDrawable,
                 GCPtr pGC,
                 BoxPtr pbox,
                 int nbox,
                 int dx,
                 int dy,
                 Bool reverse, Bool upsidedown, Pixel bitplane, void *closure)
{
    FbBits *src;
    FbStride srcStride;
    int srcBpp;
    int srcXoff, srcYoff;
    FbBits *dst;
    FbStride dstStride;
    int dstBpp;
    int dstXoff, dstYoff;
    ScreenPtr pScreen = pDstDrawable->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    sunxi_disp_t *disp = SUNXI_DISP(pScrn);

    fbGetDrawable(pSrcDrawable, src, srcStride, srcBpp, srcXoff, srcYoff);
    fbGetDrawable(pDstDrawable, dst, dstStride, dstBpp, dstXoff, dstYoff);

    if (srcBpp == 32 && dstBpp == 32 &&
        disp->framebuffer_addr == (void *)src &&
        disp->framebuffer_addr == (void *)dst &&
        (dy + srcYoff != dstYoff || dx + srcXoff + 1 >= dstXoff))
    {
        while (nbox--) {
            sunxi_g2d_blit_a8r8g8b8(disp,
                pbox->x1 + dstXoff, pbox->y1 + dstYoff,
                pbox->x1 + dx + srcXoff, pbox->y1 + dy + srcYoff,
                pbox->x2 - pbox->x1,
                pbox->y2 - pbox->y1);
            pbox++;
        }
    }
    else {
        while (nbox--) {
            fbBlt(src + (pbox->y1 + dy + srcYoff) * srcStride,
                  srcStride,
                  (pbox->x1 + dx + srcXoff) * srcBpp,
                  dst + (pbox->y1 + dstYoff) * dstStride,
                  dstStride,
                  (pbox->x1 + dstXoff) * dstBpp,
                  (pbox->x2 - pbox->x1) * dstBpp,
                  (pbox->y2 - pbox->y1),
              GXcopy, FB_ALLONES, dstBpp, reverse, upsidedown);
            pbox++;
        }
    }

    fbFinishAccess(pDstDrawable);
    fbFinishAccess(pSrcDrawable);
}

static void
xCopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)
{
    RegionRec rgnDst;
    int dx, dy;

    PixmapPtr pPixmap = fbGetWindowPixmap(pWin);
    DrawablePtr pDrawable = &pPixmap->drawable;

    dx = ptOldOrg.x - pWin->drawable.x;
    dy = ptOldOrg.y - pWin->drawable.y;
    RegionTranslate(prgnSrc, -dx, -dy);

    RegionNull(&rgnDst);

    RegionIntersect(&rgnDst, &pWin->borderClip, prgnSrc);

#ifdef COMPOSITE
    if (pPixmap->screen_x || pPixmap->screen_y)
        RegionTranslate(&rgnDst, -pPixmap->screen_x, -pPixmap->screen_y);
#endif

    miCopyRegion(pDrawable, pDrawable,
                 0, &rgnDst, dx, dy, xCopyWindowProc, 0, 0);

    RegionUninit(&rgnDst);
    fbValidateDrawable(&pWin->drawable);
}

/*****************************************************************************/

static void
xCopyNtoN(DrawablePtr pSrcDrawable,
          DrawablePtr pDstDrawable,
          GCPtr pGC,
          BoxPtr pbox,
          int nbox,
          int dx,
          int dy,
          Bool reverse, Bool upsidedown, Pixel bitplane, void *closure)
{
    CARD8 alu = pGC ? pGC->alu : GXcopy;
    FbBits pm = pGC ? fbGetGCPrivate(pGC)->pm : FB_ALLONES;
    FbBits *src;
    FbStride srcStride;
    int srcBpp;
    int srcXoff, srcYoff;
    FbBits *dst;
    FbStride dstStride;
    int dstBpp;
    int dstXoff, dstYoff;
    ScreenPtr pScreen = pDstDrawable->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    sunxi_disp_t *disp = SUNXI_DISP(pScrn);
    Bool use_g2d;

    fbGetDrawable(pSrcDrawable, src, srcStride, srcBpp, srcXoff, srcYoff);
    fbGetDrawable(pDstDrawable, dst, dstStride, dstBpp, dstXoff, dstYoff);

    use_g2d = disp->framebuffer_addr == (void *)src &&
              disp->framebuffer_addr == (void *)dst &&
              (dy + srcYoff != dstYoff || dx + srcXoff + 1 >= dstXoff);

    while (nbox--) {
        if (use_g2d) {
            sunxi_g2d_blit_a8r8g8b8(disp,
                pbox->x1 + dstXoff, pbox->y1 + dstYoff,
                pbox->x1 + dx + srcXoff, pbox->y1 + dy + srcYoff,
                pbox->x2 - pbox->x1,
                pbox->y2 - pbox->y1);
        }
        else if (!reverse && !upsidedown) {
            pixman_blt((uint32_t *) src, (uint32_t *) dst, srcStride, dstStride,
                 srcBpp, dstBpp, (pbox->x1 + dx + srcXoff),
                 (pbox->y1 + dy + srcYoff), (pbox->x1 + dstXoff),
                 (pbox->y1 + dstYoff), (pbox->x2 - pbox->x1),
                 (pbox->y2 - pbox->y1));
        }
        else {
            fbBlt(src + (pbox->y1 + dy + srcYoff) * srcStride,
                  srcStride,
                  (pbox->x1 + dx + srcXoff) * srcBpp,
                  dst + (pbox->y1 + dstYoff) * dstStride,
                  dstStride,
                  (pbox->x1 + dstXoff) * dstBpp,
                  (pbox->x2 - pbox->x1) * dstBpp,
                  (pbox->y2 - pbox->y1), alu, pm, dstBpp, reverse, upsidedown);
        }
        pbox++;
    }

    fbFinishAccess(pDstDrawable);
    fbFinishAccess(pSrcDrawable);
}

static RegionPtr
xCopyArea(DrawablePtr pSrcDrawable,
         DrawablePtr pDstDrawable,
         GCPtr pGC,
         int xIn, int yIn, int widthSrc, int heightSrc, int xOut, int yOut)
{
    CARD8 alu = pGC ? pGC->alu : GXcopy;
    FbBits pm = pGC ? fbGetGCPrivate(pGC)->pm : FB_ALLONES;

    if (pm == FB_ALLONES && alu == GXcopy && 
        pSrcDrawable->bitsPerPixel == pDstDrawable->bitsPerPixel &&
        pSrcDrawable->bitsPerPixel == 32)
    {
        return miDoCopy(pSrcDrawable, pDstDrawable, pGC, xIn, yIn,
                    widthSrc, heightSrc, xOut, yOut, xCopyNtoN, 0, 0);
    }
    return fbCopyArea(pSrcDrawable,
                      pDstDrawable,
                      pGC,
                      xIn, yIn, widthSrc, heightSrc, xOut, yOut);
}

static Bool
xCreateGC(GCPtr pGC)
{
    ScreenPtr pScreen = pGC->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    SunxiG2D *self = SUNXI_G2D(pScrn);
    Bool result;

    if (!fbCreateGC(pGC))
        return FALSE;

    if (!self->pGCOps) {
        self->pGCOps = calloc(1, sizeof(GCOps));
        memcpy(self->pGCOps, pGC->ops, sizeof(GCOps));

        /* Add our own hook for CopyArea function */
        self->pGCOps->CopyArea = xCopyArea;
    }
    pGC->ops = self->pGCOps;

    return TRUE;
}

/*****************************************************************************/

SunxiG2D *SunxiG2D_Init(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    sunxi_disp_t *disp = SUNXI_DISP(pScrn);
    SunxiG2D *private;

    if (!disp || disp->fd_g2d < 0) {
        xf86DrvMsg(pScreen->myNum, X_INFO,
            "No sunxi-g2d hardware detected (check /dev/disp and /dev/g2d)\n");
        return NULL;
    }

    private = calloc(1, sizeof(SunxiG2D));
    if (!private) {
        xf86DrvMsg(pScreen->myNum, X_INFO,
            "SunxiG2D_Init: calloc failed\n");
        return NULL;
    }

    /* Wrap the current CopyWindow function */
    private->CopyWindow = pScreen->CopyWindow;
    pScreen->CopyWindow = xCopyWindow;

    /* Wrap the current CreateGC function */
    private->CreateGC = pScreen->CreateGC;
    pScreen->CreateGC = xCreateGC;

    return private;
}

void SunxiG2D_Close(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    SunxiG2D *private = SUNXI_G2D(pScrn);

    pScreen->CopyWindow = private->CopyWindow;
    pScreen->CreateGC   = private->CreateGC;

    if (private->pGCOps) {
        free(private->pGCOps);
    }
}
