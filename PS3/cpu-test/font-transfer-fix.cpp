// SPDX-License-Identifier: GPL-2.0-or-later
// Font-only correction for the installed SDL PSL1GHT scaled-blit backend.
#include <rsx/rsx.h>
extern "C" void __real_rsxSetTransferScaleSurface(gcmContextData *,const gcmTransferScale *,const gcmTransferSurface *);
extern "C" void __wrap_rsxSetTransferScaleSurface(gcmContextData *context,const gcmTransferScale *scale,const gcmTransferSurface *surface) {
    // This app's sole copied texture is a 192x144 ARGB8888 font atlas.
    // SDL supplies a 12x24 glyph and integer atlas coordinates. The RSX
    // command requires 12.4 fixed-point origins and full source dimensions.
    if(scale->pitch==192*4 && scale->inW==12 && scale->inH==24 && scale->inX<=180 && scale->inY<=120) {
        gcmTransferScale fixed=*scale;
        fixed.inX=rsxGetFixedUint16((f32)scale->inX);
        fixed.inY=rsxGetFixedUint16((f32)scale->inY);
        fixed.inW=192; fixed.inH=144;
        __real_rsxSetTransferScaleSurface(context,&fixed,surface);
    } else {
        __real_rsxSetTransferScaleSurface(context,scale,surface);
    }
}
