/*
 *  ReactOS W32 Subsystem
 *  Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id: vis.c,v 1.18 2004/02/22 12:25:34 navaraf Exp $
 *
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * PURPOSE:          Visibility computations
 * FILE:             subsys/win32k/ntuser/vis.c
 * PROGRAMMER:       Ge van Geldorp (ge@gse.nl)
 */

#include <win32k/win32k.h>
#include <include/painting.h>
#include <include/rect.h>
#include <include/vis.h>

#define NDEBUG
#include <win32k/debug1.h>
#include <debug.h>

HRGN FASTCALL
VIS_ComputeVisibleRegion(
   PWINDOW_OBJECT Window,
   BOOLEAN ClientArea,
   BOOLEAN ClipChildren,
   BOOLEAN ClipSiblings)
{
   HRGN VisRgn, ClipRgn;
   INT LeftOffset, TopOffset;
   PWINDOW_OBJECT PreviousWindow, CurrentWindow, CurrentSibling;

   if (!(Window->Style & WS_VISIBLE))
   {
      return NULL;
   }

   if (ClientArea)
   {
      VisRgn = UnsafeIntCreateRectRgnIndirect(&Window->ClientRect);
      LeftOffset = Window->ClientRect.left;
      TopOffset = Window->ClientRect.top;
   }
   else
   {
      VisRgn = UnsafeIntCreateRectRgnIndirect(&Window->WindowRect);
      LeftOffset = Window->WindowRect.left;
      TopOffset = Window->WindowRect.top;
   }

   /*
    * Walk through all perent windows and for each clip the visble region 
    * to the parent's client area and exclude all siblings that are over
    * our window.
    */

   PreviousWindow = Window;
   CurrentWindow = Window->Parent;
   while (CurrentWindow)
   {
      if (!(CurrentWindow->Style & WS_VISIBLE))
      {
         NtGdiDeleteObject(VisRgn);
         return NULL;
      }
      ClipRgn = UnsafeIntCreateRectRgnIndirect(&CurrentWindow->ClientRect);
      NtGdiCombineRgn(VisRgn, VisRgn, ClipRgn, RGN_AND);
      NtGdiDeleteObject(ClipRgn);

      if (ClipSiblings)
      {
         ExAcquireFastMutexUnsafe(&CurrentWindow->ChildrenListLock);
         CurrentSibling = CurrentWindow->FirstChild;
         while (CurrentSibling != PreviousWindow)
         {
            if (CurrentSibling->Style & WS_VISIBLE)
            {
               ClipRgn = UnsafeIntCreateRectRgnIndirect(&CurrentSibling->WindowRect);
               NtGdiCombineRgn(VisRgn, VisRgn, ClipRgn, RGN_DIFF);
               NtGdiDeleteObject(ClipRgn);
            }
            CurrentSibling = CurrentSibling->NextSibling;
         }
         ExReleaseFastMutexUnsafe(&CurrentWindow->ChildrenListLock);
      }

      PreviousWindow = CurrentWindow;
      CurrentWindow = CurrentWindow->Parent;
   }

   if (ClipChildren)
   {
      ExAcquireFastMutexUnsafe(&Window->ChildrenListLock);
      CurrentWindow = Window->FirstChild;
      while (CurrentWindow)
      {
         if (CurrentWindow->Style & WS_VISIBLE)
         {
            ClipRgn = UnsafeIntCreateRectRgnIndirect(&CurrentWindow->WindowRect);
            NtGdiCombineRgn(VisRgn, VisRgn, ClipRgn, RGN_DIFF);
            NtGdiDeleteObject(ClipRgn);
         }
         CurrentWindow = CurrentWindow->NextSibling;
      }
      ExReleaseFastMutexUnsafe(&Window->ChildrenListLock);
   }

   NtGdiOffsetRgn(VisRgn, -LeftOffset, -TopOffset);

   return VisRgn;
}

VOID FASTCALL
VIS_WindowLayoutChanged(
   PWINDOW_OBJECT Window,
   HRGN NewlyExposed)
{
   HRGN Temp;
   
   Temp = NtGdiCreateRectRgn(0, 0, 0, 0);
   NtGdiCombineRgn(Temp, NewlyExposed, NULL, RGN_COPY);
   if (Window->Parent != NULL)
   {
      NtGdiOffsetRgn(Temp,
                     Window->WindowRect.left - Window->Parent->ClientRect.left,
                     Window->WindowRect.top - Window->Parent->ClientRect.top);
   }
   IntRedrawWindow(Window->Parent, NULL, Temp,
                   RDW_FRAME | RDW_ERASE | RDW_INVALIDATE | 
                   RDW_ALLCHILDREN);
   NtGdiDeleteObject(Temp);
}

/* EOF */
