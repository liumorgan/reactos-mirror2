/*
 * PROJECT:         ReactOS Win32k Subsystem
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            win32k/objects/line.c
 * PURPOSE:         Line functions
 * PROGRAMMERS:     ...
 */

#include <win32k.h>

#define NDEBUG
#include <debug.h>

// Some code from the WINE project source (www.winehq.com)

// Should use Fx in Point
//
BOOL FASTCALL
IntGdiMoveToEx(DC      *dc,
               int     X,
               int     Y,
               LPPOINT Point,
               BOOL    BypassPath)
{
    BOOL  PathIsOpen;
    PDC_ATTR pdcattr = dc->pdcattr;
    if ( Point )
    {
        if ( pdcattr->ulDirty_ & DIRTY_PTLCURRENT ) // Double hit!
        {
            Point->x = pdcattr->ptfxCurrent.x; // ret prev before change.
            Point->y = pdcattr->ptfxCurrent.y;
            IntDPtoLP ( dc, Point, 1);         // Reconvert back.
        }
        else
        {
            Point->x = pdcattr->ptlCurrent.x;
            Point->y = pdcattr->ptlCurrent.y;
        }
    }
    pdcattr->ptlCurrent.x = X;
    pdcattr->ptlCurrent.y = Y;
    pdcattr->ptfxCurrent = pdcattr->ptlCurrent;
    CoordLPtoDP(dc, &pdcattr->ptfxCurrent); // Update fx
    pdcattr->ulDirty_ &= ~(DIRTY_PTLCURRENT|DIRTY_PTFXCURRENT|DIRTY_STYLESTATE);

    if (BypassPath) return TRUE;

    PathIsOpen = PATH_IsPathOpen(dc->dclevel);

    if ( PathIsOpen )
        return PATH_MoveTo ( dc );

    return TRUE;
}

// Should use Fx in pt
//
VOID FASTCALL
IntGetCurrentPositionEx(PDC dc, LPPOINT pt)
{
    PDC_ATTR pdcattr = dc->pdcattr;

    if ( pt )
    {
        if (pdcattr->ulDirty_ & DIRTY_PTFXCURRENT)
        {
            pdcattr->ptfxCurrent = pdcattr->ptlCurrent;
            CoordLPtoDP(dc, &pdcattr->ptfxCurrent); // Update fx
            pdcattr->ulDirty_ &= ~(DIRTY_PTFXCURRENT|DIRTY_STYLESTATE);
        }
        pt->x = pdcattr->ptlCurrent.x;
        pt->y = pdcattr->ptlCurrent.y;
    }
}

BOOL FASTCALL
IntGdiLineTo(DC  *dc,
             int XEnd,
             int YEnd)
{
    SURFACE *psurf;
    BOOL      Ret = TRUE;
    PBRUSH pbrLine;
    RECTL     Bounds;
    POINT     Points[2];
    PDC_ATTR pdcattr = dc->pdcattr;

    if (PATH_IsPathOpen(dc->dclevel))
    {
        Ret = PATH_LineTo(dc, XEnd, YEnd);
        if (Ret)
        {
            // FIXME: PATH_LineTo should maybe do this? No
            pdcattr->ptlCurrent.x = XEnd;
            pdcattr->ptlCurrent.y = YEnd;
            pdcattr->ptfxCurrent = pdcattr->ptlCurrent;
            CoordLPtoDP(dc, &pdcattr->ptfxCurrent); // Update fx
            pdcattr->ulDirty_ &= ~(DIRTY_PTLCURRENT|DIRTY_PTFXCURRENT|DIRTY_STYLESTATE);
        }
        return Ret;
    }
    else
    {
        psurf = dc->dclevel.pSurface;
        if (NULL == psurf)
        {
            EngSetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }

        Points[0].x = pdcattr->ptlCurrent.x;
        Points[0].y = pdcattr->ptlCurrent.y;
        Points[1].x = XEnd;
        Points[1].y = YEnd;

        IntLPtoDP(dc, Points, 2);

        /* The DCOrg is in device coordinates */
        Points[0].x += dc->ptlDCOrig.x;
        Points[0].y += dc->ptlDCOrig.y;
        Points[1].x += dc->ptlDCOrig.x;
        Points[1].y += dc->ptlDCOrig.y;

        Bounds.left = min(Points[0].x, Points[1].x);
        Bounds.top = min(Points[0].y, Points[1].y);
        Bounds.right = max(Points[0].x, Points[1].x);
        Bounds.bottom = max(Points[0].y, Points[1].y);

        /* Get BRUSH from current pen. */
        pbrLine = dc->dclevel.pbrLine;
        ASSERT(pbrLine);

        if (!(pbrLine->flAttrs & BR_IS_NULL))
        {
            Ret = IntEngLineTo(&psurf->SurfObj,
                               dc->rosdc.CombinedClip,
                               &dc->eboLine.BrushObject,
                               Points[0].x, Points[0].y,
                               Points[1].x, Points[1].y,
                               &Bounds,
                               ROP2_TO_MIX(pdcattr->jROP2));
        }

    }

    if (Ret)
    {
        pdcattr->ptlCurrent.x = XEnd;
        pdcattr->ptlCurrent.y = YEnd;
        pdcattr->ptfxCurrent = pdcattr->ptlCurrent;
        CoordLPtoDP(dc, &pdcattr->ptfxCurrent); // Update fx
        pdcattr->ulDirty_ &= ~(DIRTY_PTLCURRENT|DIRTY_PTFXCURRENT|DIRTY_STYLESTATE);
    }

    return Ret;
}

BOOL FASTCALL
IntGdiPolyBezier(DC      *dc,
                 LPPOINT pt,
                 DWORD   Count)
{
    BOOL ret = FALSE; // Default to FAILURE

    if ( PATH_IsPathOpen(dc->dclevel) )
    {
        return PATH_PolyBezier ( dc, pt, Count );
    }

    /* We'll convert it into line segments and draw them using Polyline */
    {
        POINT *Pts;
        INT nOut;

        Pts = GDI_Bezier ( pt, Count, &nOut );
        if ( Pts )
        {
            ret = IntGdiPolyline(dc, Pts, nOut);
            ExFreePoolWithTag(Pts, TAG_BEZIER);
        }
    }

    return ret;
}

BOOL FASTCALL
IntGdiPolyBezierTo(DC      *dc,
                   LPPOINT pt,
                   DWORD  Count)
{
    BOOL ret = FALSE; // Default to failure
    PDC_ATTR pdcattr = dc->pdcattr;

    if ( PATH_IsPathOpen(dc->dclevel) )
        ret = PATH_PolyBezierTo ( dc, pt, Count );
    else /* We'll do it using PolyBezier */
    {
        POINT *npt;
        npt = ExAllocatePoolWithTag(PagedPool,
                                    sizeof(POINT) * (Count + 1),
                                    TAG_BEZIER);
        if ( npt )
        {
            npt[0].x = pdcattr->ptlCurrent.x;
            npt[0].y = pdcattr->ptlCurrent.y;
            memcpy(npt + 1, pt, sizeof(POINT) * Count);
            ret = IntGdiPolyBezier(dc, npt, Count+1);
            ExFreePoolWithTag(npt, TAG_BEZIER);
        }
    }
    if ( ret )
    {
        pdcattr->ptlCurrent.x = pt[Count-1].x;
        pdcattr->ptlCurrent.y = pt[Count-1].y;
        pdcattr->ptfxCurrent = pdcattr->ptlCurrent;
        CoordLPtoDP(dc, &pdcattr->ptfxCurrent); // Update fx
        pdcattr->ulDirty_ &= ~(DIRTY_PTLCURRENT|DIRTY_PTFXCURRENT|DIRTY_STYLESTATE);
    }

    return ret;
}

BOOL FASTCALL
IntGdiPolyline(DC      *dc,
               LPPOINT pt,
               int     Count)
{
    SURFACE *psurf;
    BRUSH *pbrLine;
    LPPOINT Points;
    BOOL Ret = TRUE;
    LONG i;
    PDC_ATTR pdcattr = dc->pdcattr;

    psurf = dc->dclevel.pSurface;
    if (!psurf)
    {
        return FALSE;
    }

    if (PATH_IsPathOpen(dc->dclevel))
        return PATH_Polyline(dc, pt, Count);

    DC_vPrepareDCsForBlit(dc, dc->rosdc.CombinedClip->rclBounds,
                            NULL, dc->rosdc.CombinedClip->rclBounds);

    if (pdcattr->ulDirty_ & (DIRTY_FILL | DC_BRUSH_DIRTY))
        DC_vUpdateFillBrush(dc);

    if (pdcattr->ulDirty_ & (DIRTY_LINE | DC_PEN_DIRTY))
        DC_vUpdateLineBrush(dc);

    /* Get BRUSHOBJ from current pen. */
    pbrLine = dc->dclevel.pbrLine;
    ASSERT(pbrLine);

    if (!(pbrLine->flAttrs & BR_IS_NULL))
    {
        Points = EngAllocMem(0, Count * sizeof(POINT), GDITAG_TEMP);
        if (Points != NULL)
        {
            RtlCopyMemory(Points, pt, Count * sizeof(POINT));
            IntLPtoDP(dc, Points, Count);

            /* Offset the array of points by the DC origin */
            for (i = 0; i < Count; i++)
            {
                Points[i].x += dc->ptlDCOrig.x;
                Points[i].y += dc->ptlDCOrig.y;
            }

            Ret = IntEngPolyline(&psurf->SurfObj,
                                 dc->rosdc.CombinedClip,
                                 &dc->eboLine.BrushObject,
                                 Points,
                                 Count,
                                 ROP2_TO_MIX(pdcattr->jROP2));

            EngFreeMem(Points);
        }
        else
        {
            Ret = FALSE;
        }
    }

    DC_vFinishBlit(dc, NULL);

    return Ret;
}

BOOL FASTCALL
IntGdiPolylineTo(DC      *dc,
                 LPPOINT pt,
                 DWORD   Count)
{
    BOOL ret = FALSE; // Default to failure
    PDC_ATTR pdcattr = dc->pdcattr;

    if (PATH_IsPathOpen(dc->dclevel))
    {
        ret = PATH_PolylineTo(dc, pt, Count);
    }
    else /* Do it using Polyline */
    {
        POINT *pts = ExAllocatePoolWithTag(PagedPool,
                                           sizeof(POINT) * (Count + 1),
                                           TAG_SHAPE);
        if ( pts )
        {
            pts[0].x = pdcattr->ptlCurrent.x;
            pts[0].y = pdcattr->ptlCurrent.y;
            memcpy( pts + 1, pt, sizeof(POINT) * Count);
            ret = IntGdiPolyline(dc, pts, Count + 1);
            ExFreePoolWithTag(pts, TAG_SHAPE);
        }
    }
    if ( ret )
    {
        pdcattr->ptlCurrent.x = pt[Count-1].x;
        pdcattr->ptlCurrent.y = pt[Count-1].y;
        pdcattr->ptfxCurrent = pdcattr->ptlCurrent;
        CoordLPtoDP(dc, &pdcattr->ptfxCurrent); // Update fx
        pdcattr->ulDirty_ &= ~(DIRTY_PTLCURRENT|DIRTY_PTFXCURRENT|DIRTY_STYLESTATE);
    }

    return ret;
}


BOOL FASTCALL
IntGdiPolyPolyline(DC      *dc,
                   LPPOINT pt,
                   PULONG  PolyPoints,
                   DWORD   Count)
{
    ULONG i;
    LPPOINT pts;
    PULONG pc;
    BOOL ret = FALSE; // Default to failure
    pts = pt;
    pc = PolyPoints;

    if (PATH_IsPathOpen(dc->dclevel))
        return PATH_PolyPolyline( dc, pt, PolyPoints, Count );

    for (i = 0; i < Count; i++)
    {
        ret = IntGdiPolyline ( dc, pts, *pc );
        if (ret == FALSE)
        {
            return ret;
        }
        pts+=*pc++;
    }

    return ret;
}

/******************************************************************************/

BOOL
APIENTRY
NtGdiLineTo(HDC  hDC,
            int  XEnd,
            int  YEnd)
{
    DC *dc;
    BOOL Ret;
    RECT rcLockRect ;

    dc = DC_LockDc(hDC);
    if (!dc)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (dc->dctype == DC_TYPE_INFO)
    {
        DC_UnlockDc(dc);
        /* Yes, Windows really returns TRUE in this case */
        return TRUE;
    }

    rcLockRect.left = dc->pdcattr->ptlCurrent.x;
    rcLockRect.top = dc->pdcattr->ptlCurrent.y;
    rcLockRect.right = XEnd;
    rcLockRect.bottom = YEnd;

    IntLPtoDP(dc, &rcLockRect, 2);

    /* The DCOrg is in device coordinates */
    rcLockRect.left += dc->ptlDCOrig.x;
    rcLockRect.top += dc->ptlDCOrig.y;
    rcLockRect.right += dc->ptlDCOrig.x;
    rcLockRect.bottom += dc->ptlDCOrig.y;

    DC_vPrepareDCsForBlit(dc, rcLockRect, NULL, rcLockRect);

    if (dc->pdcattr->ulDirty_ & (DIRTY_LINE | DC_PEN_DIRTY))
        DC_vUpdateLineBrush(dc);

    Ret = IntGdiLineTo(dc, XEnd, YEnd);

    DC_vFinishBlit(dc, NULL);

    DC_UnlockDc(dc);
    return Ret;
}

BOOL
APIENTRY
NtGdiPolyDraw(
    IN HDC hdc,
    IN LPPOINT lppt,
    IN LPBYTE lpbTypes,
    IN ULONG cCount)
{
    PDC dc;
    PDC_ATTR pdcattr;
    POINT *line_pts = NULL, *line_pts_old, *bzr_pts = NULL, bzr[4];
    INT num_pts, num_bzr_pts, space, space_old, size;
    ULONG i;
    BOOL result = FALSE;

    dc = DC_LockDc(hdc);
    if (!dc) return FALSE;
    pdcattr = dc->pdcattr;

    if (pdcattr->ulDirty_ & (DIRTY_FILL | DC_BRUSH_DIRTY))
       DC_vUpdateFillBrush(dc);

    if (pdcattr->ulDirty_ & (DIRTY_LINE | DC_PEN_DIRTY))
       DC_vUpdateLineBrush(dc);

    if (!cCount)
    {
       DC_UnlockDc(dc);
       return TRUE;
    }

    _SEH2_TRY
    {
        ProbeArrayForRead(lppt, sizeof(POINT), cCount, sizeof(LONG));
        ProbeArrayForRead(lpbTypes, sizeof(BYTE), cCount, sizeof(BYTE));

        if (PATH_IsPathOpen(dc->dclevel))
        {
           result = PATH_PolyDraw(dc, (const POINT *)lppt, (const BYTE *)lpbTypes, cCount);
           _SEH2_LEAVE;
        }

        /* Check for valid point types */
        for (i = 0; i < cCount; i++)
        {
           switch (lpbTypes[i])
           {
           case PT_MOVETO:
           case PT_LINETO | PT_CLOSEFIGURE:
           case PT_LINETO:
               break;
           case PT_BEZIERTO:
               if((i + 2 < cCount) && (lpbTypes[i + 1] == PT_BEZIERTO) &&
                  ((lpbTypes[i + 2] & ~PT_CLOSEFIGURE) == PT_BEZIERTO))
               {
                   i += 2;
                   break;
               }
           default:
               _SEH2_LEAVE;
           }
        }

        space = cCount + 300;
        line_pts = ExAllocatePoolWithTag(PagedPool, space * sizeof(POINT), TAG_SHAPE);
        num_pts = 1;

        line_pts[0].x = pdcattr->ptlCurrent.x;
        line_pts[0].y = pdcattr->ptlCurrent.y;

        for ( i = 0; i < cCount; i++ )
        {
           switch (lpbTypes[i])
           {
           case PT_MOVETO:
               if (num_pts >= 2) IntGdiPolyline( dc, line_pts, num_pts );
               num_pts = 0;
               line_pts[num_pts++] = lppt[i];
               break;
           case PT_LINETO:
           case (PT_LINETO | PT_CLOSEFIGURE):
               line_pts[num_pts++] = lppt[i];
               break;
           case PT_BEZIERTO:
               bzr[0].x = line_pts[num_pts - 1].x;
               bzr[0].y = line_pts[num_pts - 1].y;
               RtlCopyMemory( &bzr[1], &lppt[i], 3 * sizeof(POINT) );

               if ((bzr_pts = GDI_Bezier( bzr, 4, &num_bzr_pts )))
               {
                   size = num_pts + (cCount - i) + num_bzr_pts;
                   if (space < size)
                   {
                      space_old = space;
                      space = size * 2;
                      line_pts_old = line_pts;
                      line_pts = ExAllocatePoolWithTag(PagedPool, space * sizeof(POINT), TAG_SHAPE);
                      if (!line_pts) _SEH2_LEAVE;
                      RtlCopyMemory(line_pts, line_pts_old, space_old * sizeof(POINT));
                      ExFreePoolWithTag(line_pts_old, TAG_SHAPE);
                   }
                   RtlCopyMemory( &line_pts[num_pts], &bzr_pts[1], (num_bzr_pts - 1) * sizeof(POINT) );
                   num_pts += num_bzr_pts - 1;
                   ExFreePoolWithTag(bzr_pts, TAG_BEZIER);
               }
               i += 2;
               break;
           }
           if (lpbTypes[i] & PT_CLOSEFIGURE) line_pts[num_pts++] = line_pts[0];
        }

        if (num_pts >= 2) IntGdiPolyline( dc, line_pts, num_pts );
        IntGdiMoveToEx( dc, line_pts[num_pts - 1].x, line_pts[num_pts - 1].y, NULL, TRUE );
        ExFreePoolWithTag(line_pts, TAG_SHAPE);
        result = TRUE;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastNtError(_SEH2_GetExceptionCode());
    }
    _SEH2_END;

    DC_UnlockDc(dc);

    return result;
}

/*
 * @implemented
 */
BOOL
APIENTRY
NtGdiMoveTo(
    IN HDC hdc,
    IN INT x,
    IN INT y,
    OUT OPTIONAL LPPOINT pptOut)
{
    PDC dc;
    BOOL Ret;
    POINT Point;

    dc = DC_LockDc(hdc);
    if (!dc) return FALSE;

    Ret = IntGdiMoveToEx(dc, x, y, &Point, TRUE);

    if (pptOut)
    {
       _SEH2_TRY
       {
           ProbeForWrite( pptOut, sizeof(POINT), 1);
           RtlCopyMemory( pptOut, &Point, sizeof(POINT));
       }
       _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
       {
           SetLastNtError(_SEH2_GetExceptionCode());
           Ret = FALSE;
       }
       _SEH2_END;
    }
    DC_UnlockDc(dc);

    return Ret;
}

/* EOF */
