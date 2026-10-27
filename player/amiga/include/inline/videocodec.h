/* Automatically generated header (sfdc 1.13)! Do not edit! */

#ifndef _INLINE_VIDEOCODEC_H
#define _INLINE_VIDEOCODEC_H

#ifndef _SFDC_VARARG_DEFINED
#define _SFDC_VARARG_DEFINED
#ifdef __HAVE_IPTR_ATTR__
typedef APTR _sfdc_vararg __attribute__((iptr));
#else
typedef ULONG _sfdc_vararg;
#endif /* __HAVE_IPTR_ATTR__ */
#endif /* _SFDC_VARARG_DEFINED */

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef VIDEOCODEC_BASE_NAME
#define VIDEOCODEC_BASE_NAME VideoCodecBase
#endif /* !VIDEOCODEC_BASE_NAME */

#define VC_OpenSessionTagList(___tags) \
      LP1(0x1e, struct VCSession *, VC_OpenSessionTagList , struct TagItem *, ___tags, a0,\
      , VIDEOCODEC_BASE_NAME)

#ifndef NO_INLINE_STDARG
#define VC_OpenSessionTags(___tag1, ...) \
    ({_sfdc_vararg _tags[] = { ___tag1, __VA_ARGS__ }; VC_OpenSessionTagList((struct TagItem *) _tags); })
#endif /* !NO_INLINE_STDARG */

#define VC_CloseSession(___session) \
      LP1NR(0x24, VC_CloseSession , struct VCSession *, ___session, a0,\
      , VIDEOCODEC_BASE_NAME)

#define VC_ImportPacket(___session, ___side, ___data, ___capacity) \
      LP4(0x2a, struct VCPacket *, VC_ImportPacket , struct VCSession *, ___session, a0, ULONG, ___side, d0, APTR, ___data, a1, ULONG, ___capacity, d1,\
      , VIDEOCODEC_BASE_NAME)

#define VC_ImportFrame(___session, ___side, ___data) \
      LP3(0x30, struct VCFrame *, VC_ImportFrame , struct VCSession *, ___session, a0, ULONG, ___side, d0, APTR, ___data, a1,\
      , VIDEOCODEC_BASE_NAME)

#define VC_SendPacket(___session, ___packet) \
      LP2(0x36, LONG, VC_SendPacket , struct VCSession *, ___session, a0, struct VCPacket *, ___packet, a1,\
      , VIDEOCODEC_BASE_NAME)

#define VC_SendFrame(___session, ___frame) \
      LP2(0x3c, LONG, VC_SendFrame , struct VCSession *, ___session, a0, struct VCFrame *, ___frame, a1,\
      , VIDEOCODEC_BASE_NAME)

#define VC_Poll(___session, ___item) \
      LP2(0x42, LONG, VC_Poll , struct VCSession *, ___session, a0, struct VCItem *, ___item, a1,\
      , VIDEOCODEC_BASE_NAME)

#define VC_ReleaseFrame(___session, ___frame) \
      LP2NR(0x48, VC_ReleaseFrame , struct VCSession *, ___session, a0, struct VCFrame *, ___frame, a1,\
      , VIDEOCODEC_BASE_NAME)

#define VC_ReleasePacket(___session, ___packet) \
      LP2NR(0x4e, VC_ReleasePacket , struct VCSession *, ___session, a0, struct VCPacket *, ___packet, a1,\
      , VIDEOCODEC_BASE_NAME)

#define VC_Flush(___session) \
      LP1(0x54, LONG, VC_Flush , struct VCSession *, ___session, a0,\
      , VIDEOCODEC_BASE_NAME)

#define VC_SetAttrsTagList(___session, ___tags) \
      LP2(0x5a, LONG, VC_SetAttrsTagList , struct VCSession *, ___session, a0, struct TagItem *, ___tags, a1,\
      , VIDEOCODEC_BASE_NAME)

#ifndef NO_INLINE_STDARG
#define VC_SetAttrsTags(___session, ___tag1, ...) \
    ({_sfdc_vararg _tags[] = { ___tag1, __VA_ARGS__ }; VC_SetAttrsTagList((___session), (struct TagItem *) _tags); })
#endif /* !NO_INLINE_STDARG */

#define VC_QueryCodec(___index, ___info) \
      LP2(0x60, LONG, VC_QueryCodec , ULONG, ___index, d0, struct VCCodecInfo *, ___info, a0,\
      , VIDEOCODEC_BASE_NAME)

#define VC_GetBackendInfo(___index, ___info) \
      LP2(0x66, LONG, VC_GetBackendInfo , ULONG, ___index, d0, struct VCInfo *, ___info, a0,\
      , VIDEOCODEC_BASE_NAME)

#define VC_DecodeImageTagList(___tags) \
      LP1(0x6c, LONG, VC_DecodeImageTagList , struct TagItem *, ___tags, a0,\
      , VIDEOCODEC_BASE_NAME)

#ifndef NO_INLINE_STDARG
#define VC_DecodeImageTags(___tag1, ...) \
    ({_sfdc_vararg _tags[] = { ___tag1, __VA_ARGS__ }; VC_DecodeImageTagList((struct TagItem *) _tags); })
#endif /* !NO_INLINE_STDARG */

#define VC_EncodeImageTagList(___tags) \
      LP1(0x72, LONG, VC_EncodeImageTagList , struct TagItem *, ___tags, a0,\
      , VIDEOCODEC_BASE_NAME)

#ifndef NO_INLINE_STDARG
#define VC_EncodeImageTags(___tag1, ...) \
    ({_sfdc_vararg _tags[] = { ___tag1, __VA_ARGS__ }; VC_EncodeImageTagList((struct TagItem *) _tags); })
#endif /* !NO_INLINE_STDARG */

#define VC_ConvertImageTagList(___tags) \
      LP1(0x78, LONG, VC_ConvertImageTagList , struct TagItem *, ___tags, a0,\
      , VIDEOCODEC_BASE_NAME)

#ifndef NO_INLINE_STDARG
#define VC_ConvertImageTags(___tag1, ...) \
    ({_sfdc_vararg _tags[] = { ___tag1, __VA_ARGS__ }; VC_ConvertImageTagList((struct TagItem *) _tags); })
#endif /* !NO_INLINE_STDARG */

#define VC_GetImageInfo(___data, ___length, ___info) \
      LP3(0x7e, LONG, VC_GetImageInfo , APTR, ___data, a0, ULONG, ___length, d0, struct VCImageInfo *, ___info, a1,\
      , VIDEOCODEC_BASE_NAME)

#endif /* !_INLINE_VIDEOCODEC_H */
